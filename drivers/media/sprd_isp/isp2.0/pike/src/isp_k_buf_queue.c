/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/uaccess.h>
#include <linux/sprd_mm.h>
#include <video/sprd_isp.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include "isp_reg.h"
#include "isp_drv.h"

void isp_buf_queue_init(struct isp_buf_queue *queue)
{
	if (NULL == queue) {
		printk("isp_buf_queue_init: queue is null error.\n");
		return;
	}

	memset((void*)queue, 0, sizeof(struct isp_buf_queue));
	queue->write = &queue->node[0];
	queue->read  = &queue->node[0];
	queue->cnt = 0;
}

int32_t isp_buf_queue_write(struct isp_buf_queue *queue, struct isp_buf_node *node)
{
	if (NULL == queue || NULL == node) {
		printk("isp_buf_queue_write: queue or node is null error %p %p\n",
			queue, node);
		return -1;
	}

	if (ISP_BUF_QUEUE_LENGTH == queue->cnt) {
		return -1;
	}

	*queue->write++ = *node;
	if (queue->write > &queue->node[ISP_BUF_QUEUE_LENGTH-1]) {
		queue->write = &queue->node[0];
	}
	queue->cnt++;


	return 0;
}

int32_t isp_buf_queue_read(struct isp_buf_queue *queue, struct isp_buf_node *node)
{
	int	ret = 0;

	if (NULL == queue || NULL == node) {
		printk("isp_buf_queue_read: queue or node is null error %p %p\n",
			queue, node);
		return -1;
	}

	if (0 == queue->cnt) {
		return -1;
	}

	*node = *queue->read++;
	if (queue->read > &queue->node[ISP_BUF_QUEUE_LENGTH-1]) {
		queue->read = &queue->node[0];
	}
	queue->cnt--;
	return ret;
}

int32_t isp_k_switch_bq_buf(struct isp_k_private *isp_private, struct isp_node *node)
{
	int32_t ret = 0;
	if (node->irq_val1 & BIT_18) {
		ret = isp_k_binging_switch_bq_buf(isp_private);
		if (ret)
			node->irq_val1 &= ~BIT_18;
	} else if (node->irq_val1 & BIT_9) {
		ret = isp_k_rawaem_switch_bq_buf(isp_private);
		if (ret)
			node->irq_val1 &= ~BIT_9;
	}

	return ret;
}

int isp_k_create_bq_thread(struct isp_k_private *isp_private)
{
	isp_private->is_rawae_thread_stop = 0;
	sema_init(&isp_private->rawae_thread_sem, 0);
	isp_private->rawae_thread = kthread_run(isp_k_rawaem_thread_loop, isp_private, "isp_rawaem_thread");
	if (IS_ERR(isp_private->rawae_thread)) {
		printk("isp_create_rawae_thread error!\n");
		return -1;
	}

	return 0;
}

int isp_k_stop_bq_thread(struct isp_k_private *isp_private)
{
	int cnt = 0;

	if (isp_private->rawae_thread) {
		isp_private->is_rawae_thread_stop = 1;
		up(&isp_private->rawae_thread_sem);
		if (0 != isp_private->is_rawae_thread_stop) {
			while (cnt < 500) {
				cnt++;
				if (0 == isp_private->is_rawae_thread_stop)
					break;
				msleep(1);
			}
		}
		isp_private->rawae_thread = NULL;
	}

	return 0;
}

static int32_t isp_k_bq_enqueue_buf(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	int32_t ret = 0;
	struct isp_buf_node bq_node;

	ret = copy_from_user((void *)&bq_node, param->property_param, sizeof(struct isp_buf_node));
	if (0 != ret) {
		printk("isp_k_bq_enqueue_buf: copy_from_user error, ret = 0x%x\n", (uint32_t)ret);
		return -1;
	}

	switch (bq_node.type) {
	case ISP_NODE_TYPE_BINNING4AWB:
	case ISP_NODE_TYPE_AE_RESERVED:
		ret = isp_k_binning_enqueue_buf(isp_private, &bq_node);
		break;
	case ISP_NODE_TYPE_RAWAEM:
		ret = isp_k_rawaem_enqueue_buf(isp_private, &bq_node);
		break;
	default:
		printk("isp_k_bq_enqueue_buf: fail cmd id:%d, not supported.\n", bq_node.type);
		break;
	}

	return ret;
}

static int32_t isp_k_bq_dequeue_buf(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	int32_t ret = 0;
	struct isp_buf_node bq_node;

	ret = copy_from_user((void *)&bq_node, param->property_param, sizeof(struct isp_buf_node));
	if (0 != ret) {
		printk("isp_k_bq_dequeue_buf: copy_from_user error, ret = 0x%x\n", (uint32_t)ret);
		return -1;
	}

	switch (bq_node.type) {
	case ISP_NODE_TYPE_BINNING4AWB:
		ret = isp_k_binning_dequeue_buf(isp_private, &bq_node);
		break;
	case ISP_NODE_TYPE_RAWAEM:
		ret = isp_k_rawaem_dequeue_buf(isp_private, &bq_node);
		break;
	default:
		printk("isp_k_bq_dequeue_buf: fail cmd id:%d, not supported.\n", bq_node.type);
		break;
	}

	ret = copy_to_user(param->property_param, (void*)&bq_node, sizeof(struct isp_buf_node));
	if (0 != ret) {
		ret = -1;
		printk("isp_k_bq_dequeue_buf: copy error, ret=0x%x\n", (uint32_t)ret);
	}

	return ret;
}

static int32_t isp_k_bq_init(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	isp_buf_queue_init(&isp_private->ae_irq_queue);
	isp_buf_queue_init(&isp_private->ae_user_queue);
	isp_private->is_ae_reserved_buf = 0;
	isp_private->is_rawae_schedule = 0;
	sema_init(&isp_private->rawae_schedule_sem, 0);

	return 0;
}


int32_t isp_k_cfg_buf_queue(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	int32_t ret = 0;

	if (!param) {
		printk("isp_k_cfg_buf_queue: param is null error.\n");
		return -1;
	}

	if (NULL == param->property_param) {
		printk("isp_k_cfg_buf_queue: property_param is null error.\n");
		return -1;
	}

	switch (param->property) {
	case ISP_PRO_BUFQUEUE_ENQUEUE_BUF:
		ret = isp_k_bq_enqueue_buf(param, isp_private);
		break;
	case ISP_PRO_BUFQUEUE_DEQUEUE_BUF:
		ret = isp_k_bq_dequeue_buf(param, isp_private);
		break;
	case ISP_PRO_BUFQUEUE_INIT:
		ret = isp_k_bq_init(param, isp_private);
		break;
	default:
		printk("isp_k_cfg_buf_queue: fail cmd id:%d, not supported.\n", param->property);
		break;
	}

	return ret;
}
