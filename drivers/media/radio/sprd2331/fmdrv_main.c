/*
 * FM Radio  driver  with SPREADTRUM SC2331FM Radio chip
 *
 * Copyright (c) 2015 Spreadtrum
 * Author: Songhe Wei <songhe.wei@spreadtrum.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


/* driver definitions */
#define DRIVER_AUTHOR "Songhe Wei<songhe.wei@spreadtrum.com>"
#define DRIVER_DESC "SDIO radio driver for marlin2 FM Radio Receivers,"\
"share SDIO with BT/WIFI"

#include <linux/miscdevice.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/ioctl.h>
#include <linux/err.h>
#include <linux/errno.h>
#include  <linux/module.h>
#include  <linux/completion.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include "fmdrv.h"
#include "../../../misc/sdiodev/sdio_dev.h"
#include "../../../misc/mdbg/mdbg_sdio.h"
#include "fmdrv_ops.h"
#include "fmdrv_main.h"
#include "fmdrv_rds_parser.h"


#ifdef CONFIG_OF
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#endif

/* #define FM_VERSION	"v0.0" */
#define FM_CHANNEL_WRITE 5
#define FM_CHANNEL_READ 10
#define FM_WRITE_SIZE	(64)
#define FM_READ_SIZE	(128)

bool read_flag;

struct fmdrv_ops *fmdev;

static void receive_tasklet(unsigned long arg)
{
	unsigned char *pdata;/* the data from SDIO is event data*/
	read_flag = 1;
	pdata = fmdev->read_buf;

	if (*pdata == '1') {
		pdata = pdata + 2;
		rds_parser(pdata);
	}
	/* seek event before: status,RSSI,SNR,Freq */
	/* seek event after: 4,status,RSSI,SNR,Freq */
	else if ((*pdata == '4') ||
		((fmdev->seek_flag == 1) && (*pdata == '3')) ||
		((fmdev->seek_flag == 1) && (*pdata == '0'))) {
		fmdev->seek_flag = 0;
		fm_pr("RX before seektask_completion=0x%x",
			fmdev->seektask_completion.done);
		complete(&fmdev->seektask_completion);
		fm_pr("RX after seektask_completion=0x%x",
			fmdev->seektask_completion.done);
	} else {
		fm_pr("receive_tasklet before completion=0x%x",
			fmdev->completed.done);
		complete(&fmdev->completed);
		fm_pr("receive_tasklet after completion=0x%x",
			fmdev->completed.done);
	}
}

/* ssize_t */
int fm_read_rds_data(struct file *filp, char __user *buf,
	size_t count, loff_t *pos)
{
	int timeout = -1;
	int ret;

	pr_info("(FM_RDS) fm start to read RDS data\n");
	if (filp->f_flags & O_NONBLOCK) {
		timeout = 0;
		pr_err("fm_read_rds_data NON BLOCK!!!\n");
		return -EWOULDBLOCK;
	}

	if (timeout < 0) {
		/* wait forever */
		ret = wait_event_interruptible((fmdev->rds_han.rx_queue),
			((fmdev->rds_han.new_data_flag) == 1));
		if (ret) {
			pr_err("(FM RDS)wait_event_interruptible < 0\n");
			return -EINTR;
		}

	}
	fmdev->rds_data.rt_data.textlength =
		strlen(fmdev->rds_data.rt_data.textdata[3]);

	if (copy_to_user(buf, &(fmdev->rds_data), sizeof(fmdev->rds_data))) {
		fm_pr("fm_read_rds_data ret value is -eFAULT\n");
		return -EFAULT;
	}

	pr_info("(fm drs) fm event is %x\n", fmdev->rds_data.event_status);
	fmdev->rds_data.event_status = 0;
	return sizeof(fmdev->rds_data);
}

int parse_at_fm_cmd(uint32 *freq_found)
{
	int comma_cou = 0, comma_all = 0;
	int i = 0;
	int cmdstart = 0;
	int len = 0;
	char *cur_ptr;
	char num_str[6] = {0};
	int result = 0;
	int ret = 0;
	cur_ptr = fmdev->read_buf;
	read_flag = 0;

	if ((*cur_ptr == '0') || (*cur_ptr == '3'))
		/* seek event before: status,RSSI,SNR,Freq */
		comma_all = 3;
	else if (*cur_ptr == '4')
		/* seek event after: 4,status,RSSI,SNR,Freq */
		comma_all = 4;

	for (i = 0; i < 32 && cur_ptr[i] != '\0'; i++) {
		if (cur_ptr[i] == ',')
			comma_cou++;
		if (comma_cou == comma_all) {
			cmdstart = i;
			break;
		}
	}
	if ((comma_cou < comma_all) || (comma_cou > comma_all)) {
		fm_pr("seek fmdev->read_buf: %s error!", fmdev->read_buf);
		return -EFAULT;
	}
	for (i = 0, cmdstart++; cmdstart < 32 && cur_ptr[cmdstart] != '\0'
		&& cur_ptr[cmdstart] != ','; i++, cmdstart++) {
		if (cur_ptr[cmdstart] >= '0' && cur_ptr[cmdstart] <= '9')
			num_str[i] = cur_ptr[cmdstart];
		else if (cur_ptr[cmdstart] == ' ')
			break;
	}
	len = strlen(num_str);
	cur_ptr = num_str;
	result = cur_ptr[0] - '0';
	for (i = 1; i < len; i++)
		result = result * 10 + cur_ptr[i] - '0';
	*freq_found = result;
	fm_pr("fm seek event have come freq=%d", result);

	return 0;
}

void parse_at_fm_num(int *num)
{
	int i = 0;
	int len = 0;
	char *cur_ptr;
	int result = 0;
	int ret = 0;

	cur_ptr = fmdev->read_buf;
	read_flag = 0;
	len = strlen(cur_ptr);
	result = cur_ptr[0] - '0';
	for (i = 1; i < len; i++)
		result = result * 10 + cur_ptr[i] - '0';
	*num = result;
	fm_pr("fm num event have come num = %d", result);

	return;
}

int fm_open(struct inode *inode, struct file *filep)
{
	fm_pr("start open SPRD fm module...");
	return 0;
}

void fm_sdio_read(void)
{
	memset(fmdev->read_buf, 0, FM_READ_SIZE);
	fmdev->rcv_len = sdio_dev_get_chn_datalen(FM_CHANNEL_READ);
	if (fmdev->rcv_len <= 0) {
		fm_err("FM_CHANNEL_READ len err\n");
		return;
	}
	if (fmdev->rcv_len > FM_READ_SIZE)
		fm_err("The read data len:%d, beyond max read:%d",
		fmdev->rcv_len, FM_READ_SIZE);
	sdio_dev_read(FM_CHANNEL_READ, fmdev->read_buf, &fmdev->rcv_len);
	fm_pr("* fmdev->read_buf: %s *\n", fmdev->read_buf);
	return;
}

int fm_sdio_write(char *buffer, uint32 size)
{
	sdio_dev_write(FM_CHANNEL_WRITE, buffer, size);
	printk_ratelimited(KERN_INFO "%s size: %d\n", __func__, size);
	return size;
}

void fm_read(void)
{
	fm_sdio_read();
	tasklet_schedule(&fmdev->rx_task);
	return;
}
EXPORT_SYMBOL_GPL(fm_read);

int fm_write_buf(unsigned char *array)
{
	int cnt = 0;
	int len = 0;
	int ret = 0;

	fm_pr("fm start wake marlin\n");
	ret = set_marlin_wakeup(FM_CHANNEL_WRITE, 0x3);
	if (ret) {
		fm_pr("fm write_buf wakeup marlin failed!!!\n");
		return ret;
	}
	cnt = 0;
	read_flag = 0;
	len = strlen(array);
	fm_sdio_write(array, len);
	fm_pr("* fmdev->write_buf: %s *\n", array);
	return ret;
}

int fm_write(unsigned char *array)
{
	unsigned long timeleft;
	int ret = 0;

	init_completion(&fmdev->completed);
	fm_pr("fm_write_completion done=0X%x", fmdev->completed.done);
	ret = fm_write_buf(array);
	if (ret) {
		fm_pr("fm_write failed!!!\n");
		return -EFAULT;
	}
	timeleft = wait_for_completion_timeout(&fmdev->completed,
		FM_DRV_TX_TIMEOUT);
	fm_pr("fm_write_after_completion done=0X%x", fmdev->completed.done);
	if (!timeleft) {
		fm_err("Timeout, %d\n", ETIMEDOUT);
		return -ETIMEDOUT;
	} else {
		fm_pr("success!");
		return 0;
	}
}

/*
* open cmd :at+fm=0,freq
* open event:status
*
*/
int fm_powerup(void *arg)
{
	struct fm_tune_parm parm;
	int ret = 0;

	fmdev->seek_flag = 0;
	if (copy_from_user(&parm, arg, sizeof(parm))) {
		fm_pr("fm powerup 's ret value is -eFAULT\n");
		return -EFAULT;
	}
	parm.freq *= 10;
	sprintf(fmdev->write_buf, "at+fm=0\r\n");
	ret = fm_write(fmdev->write_buf);

	return ret;
}

/*
* close cmd :at+fm=12
* close event:status
*
*/
int fm_powerdown(void)
{
	int ret = 0;
	sprintf(fmdev->write_buf, "at+fm=12\r\n");
	ret = fm_write(fmdev->write_buf);
	return ret;
}

/*
* tune cmd :at+fm=1,freq
* tune event:status,RSSI,SNR,Freq
*
*/
int fm_tune(void *arg)
{
	struct fm_tune_parm parm;
	int ret = 0;
	if (copy_from_user(&parm, arg, sizeof(parm))) {
		fm_pr("fm tune 's ret value is -eFAULT\n");
		return -EFAULT;
	}
	parm.freq *= 10;
	sprintf(fmdev->write_buf, "at+fm=1,%d\r\n", parm.freq);
	ret = fm_write(fmdev->write_buf);
	return ret;
}

/*
* seek cmd :at+fm=4,freq,drection
*seek event:status,RSSI,SNR,Freq
*
*/
int fm_seek(void *arg)
{
	struct fm_seek_parm parm;
	unsigned long timeleft;
	int ret = 0;
	if (copy_from_user(&parm, arg, sizeof(parm))) {
		fm_pr("fm seek 's ret value is -eFAULT\n");
		return -EFAULT;
	}
	pr_info("FM seek freq =%d", parm.freq);
	parm.freq *= 10;
	sprintf(fmdev->write_buf, "at+fm=4,%d,%d\r\n",
		parm.freq, parm.seekdir);
	ret = fm_write_buf(fmdev->write_buf);
	fmdev->seek_flag = 1;

	init_completion(&fmdev->seektask_completion);
	timeleft = wait_for_completion_timeout(&fmdev->seektask_completion,
		FM_DRV_RX_SEEK_TIMEOUT);
	if (!timeleft) {
		pr_err("(fmdrv) %s(): seek Timeout(%d sec), fm seek end !\n",
		__func__, jiffies_to_msecs(FM_DRV_RX_SEEK_TIMEOUT) / 1000);
		/*-110*/
		return -ETIMEDOUT;
	}

	ret = parse_at_fm_cmd(&parm.freq);
	if (ret < 0) {
		fm_err("fm seek receive buf error!\n");
		return ret;
	}
	parm.freq /= 10;
	if (copy_to_user(arg, &parm, sizeof(parm)))
		ret = -EFAULT;
	fmdev->rcv_len = 0;
	return ret;
}

/*
* getrssi cmd :at+fm=13
*getrssi event:RSSI
*
*/
int fm_getrssi(void *arg)
{
	int ret = 0;
	int rssi = 0;

	sprintf(fmdev->write_buf, "at+fm=13\r\n");
	ret = fm_write(fmdev->write_buf);
	if (read_flag == 1)
		parse_at_fm_num(&rssi);
	else {
		fm_pr("fm getrssi can not get read buf\n");
		return -EFAULT;
	}
	pr_info("FM get rssi = %d\n", rssi);
	if (copy_to_user(arg, &rssi, sizeof(rssi)))
		ret = -EFAULT;

	return ret;
}

int fm_mute(void *arg)
{
	int mute = 0;
	if (copy_from_user(&mute, arg, sizeof(mute))) {
		fm_pr("fm mute 's ret value is -eFAULT\n");
		return -EFAULT;
	}
	fm_pr("fm_mute: %d", mute);
	return 0;
}

int fm_rds_onoff(void *arg)
{
	unsigned short is_on;
	int ret = 0;
	if (copy_from_user(&is_on, arg, sizeof(is_on))) {
		fm_pr("fm rds_onoff 's ret value is -eFAULT\n");
		return -EFAULT;
	}
	sprintf(fmdev->write_buf, "at+fm=6,%d\r\n", is_on);
	ret = fm_write(fmdev->write_buf);
	return ret;
}

int __init init_fm_driver(void)
{
	int ret = 0;

	fmdev = kzalloc(sizeof(struct fmdrv_ops), GFP_KERNEL);
	if (!fmdev)
		return -ENOMEM;
	init_completion(&fmdev->completed);
	init_completion(&fmdev->seektask_completion);

	fmdev->read_buf =  kzalloc(FM_READ_SIZE, GFP_KERNEL);
	sdiodev_readchn_init(FM_CHANNEL_READ, fm_read, 0);

	ret = fm_device_init_driver();

	tasklet_init(&fmdev->rx_task, receive_tasklet, (unsigned long)fmdev);

	/* RDS init*/
	fmdev->rds_han.new_data_flag = 0;
	init_waitqueue_head(&fmdev->rds_han.rx_queue);

	return ret;
}

void __exit exit_fm_driver(void)
{
	fm_device_exit_driver();
	tasklet_kill(&fmdev->rx_task);
	kfree(fmdev->read_buf);
	fmdev->read_buf = NULL;
	kfree(fmdev);
	fmdev = NULL;
}

module_init(init_fm_driver);
module_exit(exit_fm_driver);
/* ------------- Module Info ------------- */
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_LICENSE("GPL");
MODULE_VERSION(FM_VERSION);
