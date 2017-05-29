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
#include <asm/cacheflush.h>
#include "isp_reg.h"
#include "isp_drv.h"

static int32_t isp_k_afl_statistic(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	int32_t ret = 0;

	if(0x00 == isp_private->anti_flicker_buf_phys_addr) {
		ret = -1;
		printk("isp_k_afl_statistic: buffer is null error\n");
	}

	return ret;
}

static int32_t isp_k_afl_bypass(struct isp_io_param *param)
{
	int32_t ret = 0;
	uint32_t bypass = 0;

	ret = copy_from_user((void *)&bypass, param->property_param, sizeof(bypass));
	if (0 != ret) {
		printk("isp_k_afl_bypass: copy_from_user error, ret = 0x%x\n", (uint32_t)ret);
		return -1;
	}

	if (bypass) {
		REG_OWR(ISP_ANTI_FLICKER_PARAM0, BIT_0);
	} else {
		REG_MWR(ISP_ANTI_FLICKER_PARAM0, BIT_0, 0);
	}

	return ret;
}


static int32_t isp_k_anti_flicker_block(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	int32_t ret = 0;
	struct isp_dev_anti_flicker_info_v1 afl_info;

	memset(&afl_info, 0x00, sizeof(afl_info));

	ret = copy_from_user((void *)&afl_info, param->property_param, sizeof(afl_info));
	if (0 != ret) {
		printk("isp_k_anti_flicker_block: copy error, ret=0x%x\n", (uint32_t)ret);
		return -1;
	}

	REG_MWR(ISP_ANTI_FLICKER_PARAM0, BIT_0, afl_info.bypass);

	REG_MWR(ISP_ANTI_FLICKER_PARAM0, BIT_1, afl_info.mode << 1);

	REG_MWR(ISP_ANTI_FLICKER_PARAM0, 0xF << 2, afl_info.skip_frame_num << 2);

	REG_MWR(ISP_ANTI_FLICKER_PARAM1, 0xF , afl_info.line_step);

	REG_MWR(ISP_ANTI_FLICKER_PARAM1, 0xFF << 8, afl_info.frame_num << 8);

	REG_MWR(ISP_ANTI_FLICKER_PARAM1, 0xFFFF << 16, afl_info.vheight << 16);

	REG_MWR(ISP_ANTI_FLICKER_COL_POS, 0xFFFF , afl_info.start_col);

	REG_MWR(ISP_ANTI_FLICKER_COL_POS, 0xFFFF << 16, afl_info.end_col << 16);

	REG_WR(ISP_ANTI_FLICKER_DDR_INIT_ADDR, isp_private->anti_flicker_buf_phys_addr);

	return ret;
}

static int32_t isp_k_anti_flicker_transaddr(struct isp_io_param *param,
			struct isp_k_private *isp_private)
{
	int32_t ret = 0;
	uint32_t addr = 0;

	ret = copy_from_user((void *)&addr, param->property_param, sizeof(uint32_t));

	isp_private->anti_flicker_buf_phys_addr = addr;

	printk("anti flicker phys addr 0x%x,\n",isp_private->anti_flicker_buf_phys_addr);
	return ret;
}

int32_t isp_k_cfg_anti_flicker(struct isp_io_param *param, struct isp_k_private *isp_private)
{
	int32_t ret = 0;

	if (!param) {
		printk("isp_k_cfg_anti_flicker: param is null error.\n");
		return -1;
	}

	if (NULL == param->property_param) {
		printk("isp_k_cfg_anti_flicker: property_param is null error.\n");
		return -1;
	}

	switch (param->property) {
	case ISP_PRO_ANTI_FLICKER_BLOCK:
		ret = isp_k_anti_flicker_block(param, isp_private);
		break;
	case ISP_PRO_ANTI_FLICKER_STATISTIC:
		ret = isp_k_afl_statistic(param, isp_private);
		break;
	case ISP_PRO_ANTI_FLICKER_BYPASS:
		ret = isp_k_afl_bypass(param);
		break;
	case ISP_PRO_ANTI_FLICKER_TRANSADDR:
		ret = isp_k_anti_flicker_transaddr(param, isp_private);
		break;
	default:
		printk("isp_k_cfg_anti_flicker: fail cmd id:%d, not supported.\n", param->property);
		break;
	}

	return ret;
}

