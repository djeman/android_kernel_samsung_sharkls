/*
 * FM Radio driver for RDS with SPREADTRUM SC2331FM Radio chip
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

#include "fmdrv_rds_parser.h"
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include "fmdrv.h"
#include "fmdrv_main.h"

/*
 * rds_event_set
 * To set rds event, and user space can use this flag to juge
 * which event happened
 * If success return 0, else return error code
 */
static signed int rds_event_set(unsigned short *events, signed int event_mask)
{
	FMR_ASSERT(events);
	*events |= event_mask;
	return 0;
}

static signed int rds_event_del(unsigned short *events, signed int event_mask)
{
	FMR_ASSERT(events);
	*events &= ~(event_mask);
	return 0;
}

/*
* rds_flag_set
* To set rds event flag, and user space can use this flag to juge which event
* happened
* If success return 0, else return error code
*/
static signed int rds_flag_set(unsigned int *flags, signed int flag_mask)
{
	FMR_ASSERT(flags);
	*flags |= flag_mask;
	return 0;
}

/*
 * Group types which contain this information:
 * TA(Traffic Program) code 0A 0B 14B 15B
 */
void rds_get_eon_ta(RDS_marlin_group *grp)
{
	unsigned char ta_tp;
	unsigned short pi_on;
	if (grp->block[3].crc_flag != CRC_OK)
		return;
	/*bit3: TA ON    bit4: TP ON */
	ta_tp = (unsigned char)(((grp->block[1].data[0] & (1 << 4)) >> 4) | 
			((grp->block[2].data[0] & (1 << 3)) << 1));
	pi_on = grp->block[3].data[1];
	/* need add some code to adapter google upper layer  here */
}

/*
 * EON = Enhanced Other Networks information
 * Group types which contain this information: EON : 14A
 * variant code is in blockB low 4 bits
 */
void rds_get_eon(RDS_marlin_group *grp)
{
	unsigned short pi_on;
	if (grp->block[2].crc_flag != CRC_OK || grp->block[3].crc_flag != CRC_OK)
		return;
	/* if the upper Layer true */
	pi_on = grp->block[3].data[1];
}

/*
 * PTYN = Programme TYpe Name
 * From Group 10A, it's a 8 character description transmitted in two 10A group
 * block 2 bit0 is PTYN segment address.
 * block3 and block4 is PTYN text character
 */
void rds_get_ptyn(RDS_marlin_group *grp)
{
	unsigned char seg_addr = (grp->block[1].data[0] & 0x01);
	unsigned char ptyn[4], i, step;
	memcpy((void *)&ptyn[0], (void *)grp->block[2].word, 2);
	memcpy((void *)&ptyn[2], (void *)grp->block[3].word, 2);
	for (i = 0; i < 2; i++) {
		step = i >> 1;
		/* update seg_addr[0,1] if blockC/D is reliable data */
		if (grp->block[2].crc_flag == CRC_OK && grp->block[3].crc_flag == CRC_OK) {
			/* it's a new PTYN*/
			if (memcmp((void *)&ptyn[seg_addr * 4 + step], (void *)
				(ptyn + step), 2) != 0)
				memcpy((void *)&ptyn[seg_addr * 4 + step],
				(void *)(ptyn + step), 2);
		}
	}
}

/*
 * EWS = Coding of Emergency Warning Systems
 *    EWS inclued belows:
 *	unsigned char data_5b;
 *	unsigned short data_16b_1;
 *	unsigned short data_16b_2;
 */
void rds_get_ews(RDS_marlin_group *grp)
{
	unsigned char data_5b;
	unsigned short data_16b_1;
	unsigned short data_16b_2;
	data_5b = (grp->block[1].data[0] & 0x1F);
	data_16b_1 = grp->block[2].word;
	data_16b_2 = grp->block[3].word;
}

void rfd_get_rtplus(RDS_marlin_group *grp)
{
	unsigned char content_type, s_marker, l_marker;
	bool running;

	running = ((grp->block[1].data[0] & 0x08) != 0) ? 1 : 0;
	if (grp->block[1].crc_flag == CRC_OK && grp->block[2].crc_flag == CRC_OK) {
		content_type = (((grp->block[1].data[0] & 0x07) << 3) + (grp->block[2].data[1] >> 5));
		s_marker = (((grp->block[2].data[1] & 0x1F) << 1) + (grp->block[2].data[0] >> 7));
		l_marker = ((grp->block[2].data[0] & 0x7F) >> 1);
	}
	if (grp->block[2].crc_flag == CRC_OK && grp->block[3].crc_flag == CRC_OK) {
		content_type = (((grp->block[2].data[0] & 0x01) << 5) + (grp->block[3].data[1] >> 3));
		s_marker = ((grp->block[3].data[0] >> 5) + (grp->block[3].data[0] & 0x07) << 3);
		l_marker = (grp->block[3].data[0] & 0x1f);
	}
}

/*
 * ODA = Open Data Applications
 */
void rds_get_oda(RDS_marlin_group *grp)
{
	rfd_get_rtplus(grp);
}

/*
 * TDC = Transparent Data Channel
 */
void rds_get_tdc(RDS_marlin_group *grp)
{
	unsigned char chnl_num, len, tdc_seg[4];

	/* block C can be BlockC_C */
	/* unrecoverable block 3,or ERROR in block 4, discard this group */
	if (grp->block[2].crc_flag != CRC_OK || grp->block[3].crc_flag != CRC_OK)
		return;

	/* read TDChannel number */
	chnl_num = grp->block[2].data[0] & 0x1f;

	if ((grp->grp_type & GRP_VER_MASK) == GRP_VER_A) {
		memcpy(tdc_seg, grp->block[2].word, 2);
		len = 2;
	}

	memcpy(tdc_seg +  len, grp->block[3].word, 2);
	len += 2;
}

/*
 * CT = Programe Clock time
 */
void rds_get_ct(RDS_marlin_group *grp)
{
	unsigned int temp1, temp2, mjd;
	unsigned short month, day, year, hour, minute;
	unsigned char sense, offset;

	if (grp->block[2].crc_flag != CRC_OK || 
			grp->block[3].crc_flag != CRC_OK)
		return;

	offset = (grp->block[3].data[0] & 0x1F);
	sense = (grp->block[3].data[0] & 0x20) >> 5;
	minute = (grp->block[3].word & 0x0FC0) >> 6;

	temp1 = (grp->block[2].data[0] & 0x01);
	temp2 = (grp->block[3].data[1] & 0xF0);
	hour = ((temp1 << 4) | (temp2 >> 4));

	temp1 = (grp->block[2].data[0] & 0x03);
	temp2 = (grp->block[2].word & 0xFFFE);
	mjd = ((temp1 << 15) | (temp2 >> 1));

	year = (mjd * 100 - 1507820) / 36525;
	month = (mjd - 10000 - 149561000 - year * 3652500) / 306001;

	temp1 = (36525 * year) / 100;
	temp2 = (306001 * month) / 10000;
	day = mjd - 14956 - temp1 - temp2;
	
	/* set RDS EVENT FLAG  in here */
	fmdev->rds_data.CT.month = month;
	fmdev->rds_data.CT.day = day;
	fmdev->rds_data.CT.year = year;
	fmdev->rds_data.CT.hour = hour;
	fmdev->rds_data.CT.local_time_offset_half_hour = offset;
	fmdev->rds_data.CT.local_time_offset_signbit = sense;

	if (fmdev->rds_data.CT.minute != minute) {
		fmdev->rds_data.CT.minute = minute;
		rds_event_set(&(fmdev->rds_data.event_status),
			RDS_EVENT_UTCDATETIME);
	}
}

/*
 *
 */
void rds_get_oda_aid(RDS_marlin_group *buf) {}

/*
 * rt == Radio Text
 * Group types which contain this information: 2A 2B
 * 2A: address in block2 last 4bits, Text in block3 and block4
 * 2B: address in block2 last 4bits, Text in block4(16bits)
 */
static unsigned char test_old_flag = 0x02;

void rds_get_rt(RDS_marlin_group *grp)
{
	unsigned char addr = (grp->block[1].data[0] & 0x0F);
	unsigned char text_flag = (grp->block[1].data[0] & 0x10);
	
	fm_pr("RT Text A/B Flag is %d", text_flag);
	if (text_flag != test_old_flag) {
		memset(fmdev->rds_data.rt_data.textdata[3] , ' ', 64);
		fmdev->rds_data.rt_data.textdata[3][63] = '\0';
		test_old_flag = text_flag;
	}

	if (grp->grp_type == 0x2A) { 
		fmdev->rds_data.rt_data.textdata[3][addr * 4] = 
						grp->block[2].data[1];
		fmdev->rds_data.rt_data.textdata[3][addr * 4 + 1] = 
						grp->block[2].data[0];
		fmdev->rds_data.rt_data.textdata[3][addr * 4 + 2] = 
						grp->block[3].data[1];
		fmdev->rds_data.rt_data.textdata[3][addr * 4 + 3] = 
						grp->block[3].data[0];
	/* group type = 2B */
	} else {
		fmdev->rds_data.rt_data.textdata[3][addr * 2] = 
							grp->block[3].data[1];
		fmdev->rds_data.rt_data.textdata[3][addr * 2 + 1] =  
							grp->block[3].data[0];
	}

	rds_event_set(&(fmdev->rds_data.event_status),
			RDS_EVENT_LAST_RADIOTEXT);
	fm_pr("RT is %s", fmdev->rds_data.rt_data.textdata[3]);
}

/*
 * PIN = Programme Item Number
 */
void rds_get_pin(RDS_marlin_group *grp)
{
	struct RDS_PIN {
		unsigned char day;
		unsigned char hour;
		unsigned char minute;
	} rds_pin;

	if (grp->block[3].crc_flag != CRC_OK)
		return;
	rds_pin.day = ((grp->block[3].data[1] & 0xF8) >> 3);
	rds_pin.hour = (grp->block[3].data[1] & 0x07) << 2 | ((grp->block[3].data[0] & 0xC0) >> 6);
	rds_pin.minute = (grp->block[3].data[0] & 0x3F);
}

/*
 * SLC = Slow Labelling codes from group 1A, block3
 * LA 0 0 0 OPC ECC
 */
void rds_get_slc(RDS_marlin_group *grp)
{
	unsigned char variant_code, paging;
	unsigned char ecc_code = 0;

	if (grp->block[2].crc_flag != CRC_OK)
		return;
	/* take bit12 ~ bit14 of block3 as variant code */
	variant_code = ((grp->block[2].data[1] & 0x70) >> 4);
	if (variant_code == 0) {
		ecc_code = grp->block[2].data[0];
		paging = (grp->block[2].data[1] & 0x0F);
	}
	fmdev->rds_data.extend_country_code = ecc_code;
}

/*
 * Group types which contain this information: 0A 0B
 * PS = Programme Service name
 * block2 last 2bit stard for address, block4 16bits meaning ps.
 */
void rds_get_ps(RDS_marlin_group *grp)
{
	unsigned char index = (grp->block[1].data[0] & 0x03) * 2;
	fmdev->rds_data.ps_data.addr_cnt = index;
	fmdev->rds_data.ps_data.PS[3][index] = grp->block[3].data[1];
	fmdev->rds_data.ps_data.PS[3][index + 1] = grp->block[3].data[0];
	rds_event_set(&(fmdev->rds_data.event_status),
				RDS_EVENT_PROGRAMNAME);
	fm_pr("The PS is %s", fmdev->rds_data.ps_data.PS[3]);
}

unsigned short rds_get_freq(void)
{
	return 0;
}

void rds_get_af_method(unsigned char AFH, unsigned char AFL)
{
	static signed short pre_af_num;
	unsigned char  indx, indx2, num;
	fm_pr("af code is %d and %d", AFH, AFL);
	if (AFH >= RDS_AF_NUM_1 && AFH <= RDS_AF_NUM_25) {
		if (AFH == RDS_AF_NUM_1) {
			fmdev->rds_data.af_data.ismethod_a = RDS_AF_M_A;
			fmdev->rds_data.af_data.AF_NUM = 1;
		}
		/* have got af number */
		fmdev->rds_data.af_data.isafnum_get = 0;
		pre_af_num = AFH - 224;
		if (pre_af_num != fmdev->rds_data.af_data.AF_NUM)
			fmdev->rds_data.af_data.AF_NUM = pre_af_num;
		else
			fmdev->rds_data.af_data.isafnum_get = 1;
		if ((AFL < 205) && (AFL > 0)) {
			fmdev->rds_data.af_data.AF[0][0] = AFL + 875;
			/* convert to 100KHz */
#ifdef SPRD_FM_50KHZ_SUPPORT
			fmdev->rds_data.af_data.AF[0][0] *= 10;
#endif
			if ((fmdev->rds_data.af_data.AF[0][0]) !=
				(fmdev->rds_data.af_data.AF[1][0])) {
				fmdev->rds_data.af_data.AF[1][0] =
					fmdev->rds_data.af_data.AF[0][0];
			} else {
				if (fmdev->rds_data.af_data.AF[1][0] !=
					rds_get_freq())
					fmdev->rds_data.af_data.ismethod_a = 1;
				else
					fmdev->rds_data.af_data.ismethod_a = 0;
			}

			/*only one AF handle */
			if ((fmdev->rds_data.af_data.isafnum_get) &&
				(fmdev->rds_data.af_data.AF_NUM == 1)) {
				fmdev->rds_data.af_data.addr_cnt = 0xFF;
			}
		}
	} else if ((fmdev->rds_data.af_data.isafnum_get) &&
		(fmdev->rds_data.af_data.addr_cnt != 0xFF)) {
		/*AF Num correct */
		num = fmdev->rds_data.af_data.AF_NUM;
		num = num >> 1;
		/* WCN_DBG(FM_DBG | RDSC, "RetrieveGroup0 +num:%d\n", num); */
		/* Put AF freq fm_s32o buffer and check if AF freq is repeat again */
		for (indx = 1; indx < (num + 1); indx++) {
			if ((AFH == (fmdev->rds_data.af_data.AF[0][2*num-1]))
				&& (AFL ==
				(fmdev->rds_data.af_data.AF[0][2*indx]))) {
				pr_info("AF same as");
				break;
			} else if (!(fmdev->rds_data.af_data.AF[0][2 * indx-1])) {
				/*convert to 100KHz */
				fmdev->rds_data.af_data.AF[0][2*indx-1] =
					AFH + 875;
				fmdev->rds_data.af_data.AF[0][2*indx] =
					AFL + 875;
#ifdef MTK_FM_50KHZ_SUPPORT
				fmdev->rds_data.af_data.AF[0][2*indx-1] *= 10;
				fmdev->rds_data.af_data.AF[0][2*indx] *= 10;
#endif
				break;
			}
		}
		num = fmdev->rds_data.af_data.AF_NUM;
		if (num <= 0)
			return;
		if ((fmdev->rds_data.af_data.AF[0][num-1]) == 0)
			return;
		num = num >> 1;
		for (indx = 1; indx < num; indx++) {
			for (indx2 = indx + 1; indx2 < (num + 1); indx2++) {
				AFH = fmdev->rds_data.af_data.AF[0][2*indx-1];
				AFL = fmdev->rds_data.af_data.AF[0][2*indx];
				if (AFH > (fmdev->rds_data.af_data.AF[0][2*indx2
					-1])) {
					fmdev->rds_data.af_data.AF[0][2*indx-1]
					= fmdev->rds_data.af_data.AF[0][2
					*indx2-1];
					fmdev->rds_data.af_data.AF[0][2*indx] =
					fmdev->rds_data.af_data.AF[0][2*indx2];
					fmdev->rds_data.af_data.AF[0][2*indx2-1]
						= AFH;
					fmdev->rds_data.af_data.AF[0][2*indx2]
						= AFL;
				} else if (AFH == (fmdev->rds_data.af_data.AF[0][2
					*indx2-1])) {
					if (AFL > (fmdev->rds_data.af_data.AF[0]
						[2*indx2])) {
						fmdev->rds_data.af_data.AF[0][2
							*indx-1]
						= fmdev->rds_data.af_data
						.AF[0][2*indx2-1];
						fmdev->rds_data.af_data.AF[0][2
							*indx] = fmdev->rds_data
							.af_data.AF[0][2*indx2];
						fmdev->rds_data.af_data.AF[0][2*
							indx2-1] = AFH;
						fmdev->rds_data.af_data.AF[0][2
							*indx2] = AFL;
					}
				}
			}
		}

		/* arrange frequency from low to high:end */
		/* compare AF buff0 and buff1 data:start */
		num = fmdev->rds_data.af_data.AF_NUM;
		indx2 = 0;
		for (indx = 0; indx < num; indx++) {
			if ((fmdev->rds_data.af_data.AF[1][indx]) ==
				(fmdev->rds_data.af_data.AF[0][indx])) {
				if (fmdev->rds_data.af_data.AF[1][indx] != 0)
					indx2++;
			} else
				fmdev->rds_data.af_data.AF[1][indx] =
				fmdev->rds_data.af_data.AF[0][indx];
		}

		/* compare AF buff0 and buff1 data:end */
		if (indx2 == num) {
			fmdev->rds_data.af_data.addr_cnt = 0xFF;
			rds_event_set(&(fmdev->rds_data.event_status),
						RDS_EVENT_AF_LIST);
			for (indx = 0; indx < num; indx++) {
				if ((fmdev->rds_data.af_data.AF[1][indx]) == 0) {
					fmdev->rds_data.af_data.addr_cnt = 0x0F;
					rds_event_del(&(fmdev->rds_data.event_status),
							RDS_EVENT_AF_LIST);
				}
			}
		} else
			fmdev->rds_data.af_data.addr_cnt = 0x0F;
	}
}

/*
 * Group types which contain this information: 0A
 * AF = Alternative Frequencies
 * af infomation in block 3
 */
void rds_get_af(RDS_marlin_group *grp)
{
	if (grp->block[2].crc_flag != CRC_OK)
		return;
	rds_get_af_method(grp->block[2].data[1], grp->block[2].data[0]);
	fmdev->rds_data.af_data.AF[1][24] = 0;
}

/*
 * Group types which contain this information: 0A 0B 15B
 * TA = Traffic Announcement
 */
void rds_get_ta_di_ms(RDS_marlin_group *grp)
{
	unsigned char ta, ms, di;

	ta = (grp->block[1].data[0] & (1 << 4)) >> 4;
	fmdev->rds_data.RDSFLAG.TA = ta;
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_TA);

	if ((fmdev->rds_data.switch_tp) && 
			(fmdev->rds_data.RDSFLAG.TP) && 
			!(fmdev->rds_data.RDSFLAG.TA))
        	rds_event_set(&(fmdev->rds_data.event_status), 
					RDS_EVENT_TAON_OFF);

	ms = (grp->block[1].data[0] & (1 << 3)) >> 3;
	fmdev->rds_data.RDSFLAG.music = ms;
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_MUSIC);

	di = (grp->block[1].data[0] & 0x0F);
	fmdev->rds_data.RDSFLAG.music = (di & 0x01);
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_MUSIC);

	fmdev->rds_data.RDSFLAG.artificial_head = (di & 0x02) >> 1;
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_ARTIFICIAL_HEAD);

	fmdev->rds_data.RDSFLAG.compressed = (di & 0x04) >> 2;
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_COMPRESSED);

	fmdev->rds_data.RDSFLAG.dynamic_pty = (di & 0x08) >> 3;
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_DYNAMIC_PTY);

	rds_event_set(&(fmdev->rds_data.event_status), 
				RDS_EVENT_FLAGS);
}

/*
 * Group types which contain this information: all
 * TP = Traffic Program identification
 */
void rds_get_tp(RDS_marlin_group *grp)
{
	unsigned char tp;
	tp = (grp->block[1].data[1] & (1 << 2)) >> 2;
	fmdev->rds_data.RDSFLAG.TP = tp;
	rds_flag_set(&(fmdev->rds_data.RDSFLAG.flag_status), 
				RDS_FLAG_IS_TP);
	rds_event_set(&(fmdev->rds_data.event_status), 
				RDS_EVENT_FLAGS);
}

/*
 * Group types which contain this information: all
 * block2:Programme Type code = 5 bits($)
 *         #### ##$$ $$$# ####
 */
void rds_get_pty(RDS_marlin_group *grp)
{
	unsigned short pty = ((grp->block[1].data[0] >> 5) |
		((grp->block[1].data[1] & 0x3) << 3));

	fmdev->rds_data.PTY = pty;
	rds_event_set(&(fmdev->rds_data.event_status), 
				RDS_EVENT_PTY_CODE);
}

/*
 * Group types which contain this information: all
 * Read PI code from the group. grp_typeA: block 1 and block3,
 * grp_type B: block3
 */
void rds_get_pi_code(RDS_marlin_group *grp)
{
	/* pi_code for version A, pi_code_b for version B */
	unsigned short pi_code = 0, pi_code_b = 0;

	if (grp->block[0].crc_flag == CRC_OK)
		pi_code = grp->block[0].word;

	if ((grp->grp_type & GRP_VER_MASK) == GRP_VER_B && 
			grp->block[2].crc_flag == CRC_OK)
		pi_code = grp->block[2].word;

	if (pi_code == 0 && pi_code_b != 0)
		pi_code = pi_code_b;

	/* send pi_code value to global and copy to user space in read rds interface */
	if (pi_code != 0) {
		fmdev->rds_data.PI = pi_code;
		rds_event_set(&(fmdev->rds_data.event_status), 
				RDS_EVENT_PI_CODE);
	}
}

/*
 * Block 1: PIcode(16bit)+CRC
 * Block 2 : Group type code(4bit)
 * B0 version(1bit 0:version A; 1:version B)
 * TP(1bit)+ PTY(5 bits)
 * @ buffer point to the start of Block 1
 * Block3: 16bits + 10bits
 * Block4: 16bits + 10bits
 * rds_get_group_type from Block2
 */
void rds_get_group_type(RDS_marlin_group *grp)
{
	if (grp->block[1].crc_flag != CRC_OK) {
		grp->grp_type = GRP_TYPE_INVALID;
		return;
	}

	grp->grp_type = (grp->block[1].data[1] & GRP_TYPE_MASK);

	/* 0:version A, 1: version B */
	if (grp->block[1].data[1] & GRP_VER_BIT)
		grp->grp_type |= GRP_VER_B;
	else
		grp->grp_type |= GRP_VER_A;
}

/*                 p here
 * string: 1,     1,16551,1,11,1,57725,1,26400
 * hex: 1,       1,0x40A7,1,0xB,1,0xE17D,1,0x6720
 */
int string_to_hex(unsigned char *buf_src, RDS_marlin_group *grp)
{
	unsigned int c[4]; /* crc data */
	unsigned int b[4]; /* block data */
	int ret, i;

	ret = sscanf(buf_src, "%d,%d,%d,%d,%d,%d,%d,%d",
		&c[0], &b[0], &c[1], &b[1], &c[2], &b[2], &c[3], &b[3]);
	if (ret != 8) {
		fm_pr("invalid buffer received, elements count: %d\n", ret);
		return 0;
	}

	for (i=0; i<4; i++) {
		grp->block[i].crc_flag = c[i];
		grp->block[i].word = b[i] & 0xFFFF;
	}

	return 1;
}

/*
 * rds_parser
 * Block0: PI code(16bits)
 * Block1: Group type(4bits), B0=version code(1bit),
 * TP=traffic program code(1bit),
 * PTY=program type code(5bits), other(5bits)
 * @getfreq - function pointer, AF need get current freq
 * Theoretically F
 * Block2 + CRC(10 bits)
 * Block3(16 bits) + CRC(10 bits)
 * Block4(16 bits) + CRC(10 bits)
 * From marlin2 chip, the data stream is like below:
 * One Group = CRC_Flag(8bit)+Block1(16bits)
 *           + CRC_Flag(8bit)+Block2(16bits)
 *           + CRC_Flag(8bit)+Block3(16bits)
 *           + CRC_Flag(8bit)+Block4(16bits)
 */
void rds_parser(unsigned char *buffer)
{
	RDS_marlin_group grp;

	if (string_to_hex(buffer, &grp) == 0)
		return;

	rds_get_group_type(&grp);
	fm_pr("group type is : 0x%x", grp.grp_type);

	rds_get_pi_code(&grp);
	if (grp.grp_type != GRP_TYPE_INVALID) {
		rds_get_tp(&grp);
		rds_get_pty(&grp);
	}

	switch (grp.grp_type) {
	case GRP_TYPE_INVALID:
		fm_pr("invalid group type\n");
		break;
	/* Basic tuning and switching information only */
	case 0x0A:
		rds_get_ta_di_ms(&grp);
		rds_get_af(&grp);
		rds_get_ps(&grp);
		break;
	case 0x0B:
		rds_get_ta_di_ms(&grp);
		rds_get_ps(&grp);
		break;
	/* Programme Item Number and slow labelling codes only */
	case 0x1A:
		rds_get_slc(&grp);
		rds_get_pin(&grp);
		break;
	/* Programme Item Number */
	case 0x1B:
		rds_get_pin(&grp);
		break;
	/* RadioText only */
	case 0x2A:
	case 0x2B:
		rds_get_rt(&grp);
		break;
	/* Applications Identification for ODA only */
	case 0x3A:
		rds_get_oda_aid(&grp);
		break;
	/* Clock-time and date only */
	case 0x4A:
		rds_get_ct(&grp);
		break;
	/* Transparent Data Channels (32 channels) or ODA */
	case 0x5A:
	case 0x5B:
		rds_get_tdc(&grp);
		break;
	/* gency Warning System or ODA */
	case 0x9a:
		rds_get_ews(&grp);
		break;
	/* Programme Type Name */
	case 0xAA:
		rds_get_ptyn(&grp);
		break;
	/* Enhanced Other Networks information only */
	case 0xEA:
		rds_get_eon(&grp);
		break;
	case 0xEB:
		rds_get_eon_ta(&grp);
		break;
	/* Fast switching information */
	case 0xFB:
		rds_get_ta_di_ms(&grp);
		break;
	/* ODA (Open Data Applications) group availability signaled in type 3A groups*/
	case 0x3B:
	case 0x4B:
	case 0x6A:
	case 0x6B:
	case 0x7A:
	case 0x7B:
	case 0x8A:
	case 0x8B:
	case 0x9B:
	case 0xAB:
	case 0xBA:
	case 0xBB:
	case 0xCA:
	case 0xCB:
	case 0xDB:
	case 0xDA:
		rds_get_oda(&grp);
		break;

	default:
		fm_pr("rds group type[0x%x] not to be processed", grp.grp_type);
		break;
	}

	if (fmdev->rds_data.event_status != 0) {
		fmdev->rds_han.new_data_flag = 1;
		wake_up_interruptible(&fmdev->rds_han.rx_queue);
	}
}

