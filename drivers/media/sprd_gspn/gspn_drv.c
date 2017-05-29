/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
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

#include "gspn_sync.h"
#include <linux/sprd_iommu.h>

GSPN_CONTEXT_T *g_gspn_ctx = NULL;// used to print log and debug when kernel crash, not used to pass parameters
static int stat = 0;
void *gsp_memcpy(void *k_dst, void *k_src, size_t n)
{
    const char *src = k_src;
    char *dst = k_dst;

    /* Simple, byte oriented memcpy. */
    while (n--)
        *dst++ = *src++;

    return k_dst;
}
/*
gspn_kcmd_fence_pre_process
description: create release fence for every layer of CMD, copy this release fence back to user
                 collect all acq fence together
warning: function should be protect by gspn_ctx->fence_create_sema
*/
static GSPN_ERR_CODE_E gspn_kcmd_fence_pre_process(GSPN_CORE_T *core, GSPN_KCMD_INFO_T *kcmd)
{
    GSPN_ERR_CODE_E ret = GSPN_NO_ERR;

    if(kcmd->src_cmd.misc_info.async_flag) {
        ret = gspn_layer_list_fence_process(kcmd, core->timeline);
        if (ret) {
            GSPN_LOGE("gspn layer list fence process failed\n");
        }
    }

    return ret;
}


/*
gspn_kcmd_list_fence_pre_process
description: create release fence for every layer of each KCMD in kcmd_list
                 collect all acq fence fd of KCMD together
                 copy release fd back to user space
*/
static GSPN_ERR_CODE_E gspn_kcmd_list_fence_pre_process(GSPN_CORE_T *core,
                                                        struct list_head *kcmd_list)
{
    GSPN_KCMD_INFO_T *kcmd = container_of(kcmd_list->next, GSPN_KCMD_INFO_T, list);
    GSPN_ERR_CODE_E ret = GSPN_NO_ERR;
    if (core == NULL) {
        GSPN_LOGE("core is null pointer\n");
        return -1;
    }
    if(kcmd->src_cmd.misc_info.async_flag) {
        down(&core->fence_create_sema);
        list_for_each_entry(kcmd, kcmd_list, list) {
            ret = gspn_kcmd_fence_pre_process(core, kcmd);
            if(ret) {
                /*destory all these release fence, that just create before*/
                GSPN_LOGE("fence_pre_process failed!\n");
                break;
            }
        }
        up(&core->fence_create_sema);
    }
    return ret;
}




/*
gspn_get_kcmd_from_empty_list
description: get n KCMD from empty list, clean them, and link them together as a frame
*/
static GSPN_ERR_CODE_E gspn_get_kcmd_from_empty_list(GSPN_CONTEXT_T *gspn_ctx,struct list_head *kcmd_list,uint32_t n)
{
    GSPN_KCMD_INFO_T *kcmd = NULL;
    struct list_head *pos = NULL;
    struct list_head frame_list;
    int32_t i = 0;

    INIT_LIST_HEAD(kcmd_list);
    INIT_LIST_HEAD(&frame_list);

    //get the really count of empty_list
    i = 0;
    list_for_each(pos, &gspn_ctx->empty_list) {
        kcmd = list_first_entry(pos, GSPN_KCMD_INFO_T, list);
        i++;
    }
    if(i != gspn_ctx->empty_list_cnt) {
        GSPN_LOGW("empty_list_cnt:%d, real_cnt:%d!\n",gspn_ctx->empty_list_cnt,i);
    }
    if(i < n) {
        GSPN_LOGE("not enough empty kcmd, need:%d/real_cnt:%d!\n",n,i);
        return GSPN_K_NOT_ENOUGH_EMPTY_KCMD_ERR;
    }

    i = 0;
    while(i < n) {
        kcmd = list_first_entry(&gspn_ctx->empty_list, GSPN_KCMD_INFO_T, list);
        list_del(&kcmd->list);
        memset(kcmd,0,sizeof(GSPN_KCMD_INFO_T));  //need to be dong 0417
        kcmd->src_cmd.l0_info.acq_fen_fd = -1;
        kcmd->src_cmd.l0_info.rls_fen_fd = -1;
        kcmd->src_cmd.l1_info.acq_fen_fd = -1;
        kcmd->src_cmd.l1_info.rls_fen_fd = -1;
        kcmd->src_cmd.l2_info.acq_fen_fd = -1;
        kcmd->src_cmd.l2_info.rls_fen_fd = -1;
        kcmd->src_cmd.l3_info.acq_fen_fd = -1;
        kcmd->src_cmd.l3_info.rls_fen_fd = -1;
        kcmd->src_cmd.des1_info.acq_fen_fd = -1;
        kcmd->src_cmd.des1_info.rls_fen_fd = -1;
        kcmd->src_cmd.des2_info.acq_fen_fd = -1;
        kcmd->src_cmd.des2_info.rls_fen_fd = -1;
        list_add_tail(&kcmd->list_frame, &frame_list);
        list_add_tail(&kcmd->list, kcmd_list);
        gspn_ctx->empty_list_cnt --;
        i++;
    }
    list_del(&frame_list);
    return GSPN_NO_ERR;
}

/*
gspn_put_list_to_empty_list
description: put KCMD list to fill list
*/
GSPN_ERR_CODE_E gspn_put_list_to_empty_list(GSPN_CONTEXT_T *gspn_ctx,
                                            struct list_head *new_list)
{
    struct list_head *pos = NULL;
    struct list_head *next = NULL;
    int32_t cnt = 0;

    down(&gspn_ctx->empty_list_sema);
    /*get the really count of empty_list*/
    cnt = 0;
    list_for_each(pos, &gspn_ctx->empty_list) {
        cnt++;
    }
    if(cnt != gspn_ctx->empty_list_cnt) {
        GSPN_LOGW("empty_list_cnt:%d,cnt:%d!\n",gspn_ctx->empty_list_cnt,cnt);
    }
    list_for_each_safe(pos, next, new_list) {
        list_del(pos);
        list_add_tail(pos, &gspn_ctx->empty_list);
        gspn_ctx->empty_list_cnt++;
    }
    up(&gspn_ctx->empty_list_sema);
    GSPN_LOGI("empty cnt: %d\n", gspn_ctx->empty_list_cnt);
    wake_up_interruptible_all(&(gspn_ctx->empty_list_wq));
    return GSPN_NO_ERR;
}

static GSPN_CORE_T *gspn_core_select(GSPN_CONTEXT_T *gspn_ctx)
{
    GSPN_CORE_T *core = NULL;
    GSPN_CORE_MNG_T *mng = NULL;
    uint32_t i = 0;
    uint32_t num = 1;

    if (gspn_ctx != NULL)
        mng = &gspn_ctx->core_mng;
    else {
        GSPN_LOGE("can't core select with null gspn ctx\n");
        return NULL;
    }

    if (mng == NULL) {
        GSPN_LOGE("core mng is null\n");
        return NULL;
    }

    for(i = 0; i < mng->core_cnt; i++) {
        if (num > mng->cores[i].fill_list_cnt) {
            num = mng->cores[i].fill_list_cnt;
            core = &mng->cores[i];
        }
    }
    if(core == NULL) {
        GSPN_LOGW("do not find idle core, hardcode core 0\n");
        core = &mng->cores[0];
    }
    if (core == NULL)
        GSPN_LOGE("fatal error no idle core\n");

    return core;
}

static void gspn_kcmdl_mmu_id_set(struct GSPN_CORE_T *core,
        struct list_head *list)
{
    GSPN_KCMD_INFO_T *kcmd = NULL;
    if (core == NULL || list == NULL) {
        GSPN_LOGE("can't set kcmd mmu id with null");
        return;
    }

    list_for_each_entry(kcmd, list, list) {
        switch (core->core_id) {
        case 0:
            kcmd->mmu_id = ION_GSP0;
            break;
        case 1:
            kcmd->mmu_id = ION_GSP1;
            break;
        default:
            kcmd->mmu_id = -1;
            break;
        }
    }
}

/*
gspn_put_list_to_fill_list
description: put KCMD list to fill list
*/
static GSPN_ERR_CODE_E gspn_put_list_to_fill_list(GSPN_CONTEXT_T *gspn_ctx,
                                                struct list_head *new_list,
                                                GSPN_CORE_T *core)
{
    struct list_head *pos = NULL;
    struct list_head *next = NULL;
    int32_t cnt = 0;
    GSPN_KCMD_INFO_T *kcmd = NULL;

    down(&core->fill_list_sema);
    /*get the really count of fill_list*/
    cnt = 0;
    list_for_each(pos, &core->fill_list) {
        cnt++;
    }
    if(cnt != core->fill_list_cnt) {
        GSPN_LOGW("fill_list_cnt:%d,cnt:%d!\n", core->fill_list_cnt,cnt);
    }


    list_for_each_safe(pos, next, new_list) {
        list_del(pos);
        kcmd = list_entry(pos, GSPN_KCMD_INFO_T, list);
        get_monotonic_boottime(&kcmd->start_time);
        list_add_tail(pos, &core->fill_list);
        core->fill_list_cnt++;
    }
    up(&core->fill_list_sema);


    /*add to fill list success, wake work thread to process*/
    up(&gspn_ctx->wake_work_thread_sema);
    return GSPN_NO_ERR;
}

/*
func:gspn_enable_set
desc:config des-layer enable and scale enable according to the run mode and scl_seq.
*/
static GSPN_ERR_CODE_E gspn_enable_set(GSPN_KCMD_INFO_T *kcmd)
{
    kcmd->src_cmd.misc_info.scale_en = 0;
    if(kcmd->src_cmd.misc_info.run_mod == 0) {
        kcmd->src_cmd.des2_info.layer_en = 0;
        if(kcmd->src_cmd.l0_info.layer_en == 1
           ||kcmd->src_cmd.l1_info.layer_en == 1
           ||kcmd->src_cmd.l2_info.layer_en == 1
           ||kcmd->src_cmd.l3_info.layer_en == 1) {
            kcmd->src_cmd.des1_info.layer_en = 1;
        } else {
            GSPN_LOGE("all src layer are not enable!\n");
            return GSPN_K_PARAM_CHK_ERR;
        }

        if(kcmd->src_cmd.misc_info.scale_seq == 0) {
            if(kcmd->src_cmd.l0_info.clip_size.w  != kcmd->src_cmd.des1_info.scale_out_size.w
               ||kcmd->src_cmd.l0_info.clip_size.h  != kcmd->src_cmd.des1_info.scale_out_size.h) {
                kcmd->src_cmd.misc_info.scale_en = 1;
            }
        } else {
            if(kcmd->src_cmd.des1_info.work_src_size.w != kcmd->src_cmd.des1_info.scale_out_size.w
               ||kcmd->src_cmd.des1_info.work_src_size.h != kcmd->src_cmd.des1_info.scale_out_size.h) {
                kcmd->src_cmd.misc_info.scale_en = 1;
            }
        }
    } else {
        if(kcmd->src_cmd.l1_info.layer_en == 1
           ||kcmd->src_cmd.l2_info.layer_en == 1
           ||kcmd->src_cmd.l3_info.layer_en == 1) {
            kcmd->src_cmd.des1_info.layer_en = 1;
        } else {
            kcmd->src_cmd.des1_info.layer_en = 0;
        }

        if(kcmd->src_cmd.l0_info.layer_en == 1) {
            kcmd->src_cmd.des2_info.layer_en = 1;
            if(kcmd->src_cmd.l0_info.clip_size.w  != kcmd->src_cmd.des2_info.scale_out_size.w
               ||kcmd->src_cmd.l0_info.clip_size.h  != kcmd->src_cmd.des2_info.scale_out_size.h) {
                kcmd->src_cmd.misc_info.scale_en = 1;
            }
        } else {
            kcmd->src_cmd.des2_info.layer_en = 0;
        }
    }
    return GSPN_NO_ERR;
}

static struct dma_buf *sprd_ion_get_dmabuf(int fd)
{
    struct dma_buf *dmabuf = NULL;

    pr_debug("%s, fd: %d\n", __func__, fd);
    if (fd < 0) {
        pr_err("%s, fd=%d less than zero\n", __func__, fd);
        return dmabuf;
    }

    dmabuf = dma_buf_get(fd);
    if (IS_ERR(dmabuf)) {
        pr_err("%s, dmabuf=0x%lx dma_buf_get error!\n",
            __func__, (unsigned long)dmabuf);
        return NULL;
    }

    return dmabuf;
}


/*
func: gspn_get_dmabuf
description:translate every layer's buffer-fd to DMA-buffer-pointer,
            for the fd is only valid in user process context.
*/
static GSPN_ERR_CODE_E gspn_get_dmabuf(GSPN_KCMD_INFO_T *kcmd)
{
#define GSPN_Lx_GET_DMABUF(lx_info)\
    do {\
        if(lx_info.layer_en == 1 && lx_info.addr_info.share_fd > 0) { \
            lx_info.addr_info.dmabuf = (void *)sprd_ion_get_dmabuf(lx_info.addr_info.share_fd);\
            if(0 == lx_info.addr_info.dmabuf) { \
                GSPN_LOGE("get dma buffer failed!lx_info.addr_info.share_fd = %d\n", lx_info.addr_info.share_fd); \
                return GSPN_K_GET_DMABUF_BY_FD_ERR; \
            } \
            GSPN_LOGI("fd=%d, dmabuf=%p\n",lx_info.addr_info.share_fd,lx_info.addr_info.dmabuf); \
        }\
    } while(0)
    GSPN_Lx_GET_DMABUF(kcmd->src_cmd.des1_info);
    GSPN_Lx_GET_DMABUF(kcmd->src_cmd.des2_info);
    GSPN_Lx_GET_DMABUF(kcmd->src_cmd.l0_info);
    GSPN_Lx_GET_DMABUF(kcmd->src_cmd.l1_info);
    GSPN_Lx_GET_DMABUF(kcmd->src_cmd.l2_info);
    GSPN_Lx_GET_DMABUF(kcmd->src_cmd.l3_info);

    return GSPN_NO_ERR;
}

/*
gspn_fill_kcmd
description: copy CMD to KCMD
*/
static GSPN_ERR_CODE_E gspn_fill_kcmd(struct list_head *kcmd_list,
                                        GSPN_CMD_INFO_T __user *cmd,
                                        uint32_t *done_cnt)
{
    GSPN_KCMD_INFO_T *kcmd = NULL;
    int32_t ret = GSPN_NO_ERR;

    list_for_each_entry(kcmd, kcmd_list, list) {
        ret = copy_from_user((void*)&kcmd->src_cmd, (void __user *)cmd, sizeof(GSPN_CMD_INFO_T));
        if(ret) {
            GSPN_LOGE("copy from user err!ret=%d. cmd:%p\n",ret,cmd);
            ret = GSPN_K_COPY_FROM_USER_ERR;
            return ret;
        }

        kcmd->ucmd = (GSPN_CMD_INFO_T __user *)cmd;
        kcmd->pid = current->pid;
        kcmd->done_cnt = done_cnt;

        /*config destnation layer enable, scale enable*/
        ret = gspn_enable_set(kcmd);
        if(ret) {
            return ret;
        }

        /*translate every layer's buffer-fd to DMA-buffer-pointer,
        for the fd is only valid in user process context*/
        ret = gspn_get_dmabuf(kcmd);
        if(ret) {
            return ret;
        }

        cmd++;
    }
    return ret;
}

#if 0
uint32_t  __attribute__((weak)) sci_get_chip_id(void)
{
    if (NULL == g_gspn_ctx) {
        return -1;
    }

    GSPN_LOGI("GSPN local read chip id, *(%p) == %x \n",
              (void*)(g_gspn_ctx->core_mng.chip_id_addr),REG_GET(g_gspn_ctx->core_mng.chip_id_addr+0xFC));
    return REG_GET(g_gspn_ctx->core_mng.chip_id_addr);
}

static uint32_t  gspn_get_chip_id(void)
{
    uint32_t adie_chip_id = sci_get_chip_id();

    if (NULL == g_gspn_ctx) {
        return -1;
    }

    if((adie_chip_id & 0xffff0000) < 0x50000000) {
        GSPN_LOGW("chip id 0x%08x is invalidate, try to get it by reading reg directly!\n", adie_chip_id);
        adie_chip_id = REG_GET(g_gspn_ctx->core_mng.chip_id_addr);
        if((adie_chip_id & 0xffff0000) < 0x50000000) {
            GSPN_LOGW("chip id 0x%08x from reg is invalidate too!\n", adie_chip_id);
        }
    }
    GSPN_LOGI("return chip id 0x%08x \n", adie_chip_id);
    return adie_chip_id;
}
#else
uint32_t  __attribute__((weak)) sci_get_chip_id(void)
{
    GSPN_LOGI("GSPN local read chip id, *(%p) == %x \n",
              (void*)(SPRD_AONAPB_BASE+0xFC),REG_GET(SPRD_AONAPB_BASE+0xFC));
    return REG_GET(SPRD_AONAPB_BASE+0xFC);
}

static uint32_t  gspn_get_chip_id(void)
{
    uint32_t adie_chip_id = sci_get_chip_id();
    if((adie_chip_id & 0xffff0000) < 0x50000000) {
        GSPN_LOGW("chip id 0x%08x is invalidate, try to get it by reading reg directly!\n", adie_chip_id);
        adie_chip_id = REG_GET(SPRD_AONAPB_BASE+0xFC);
        if((adie_chip_id & 0xffff0000) < 0x50000000) {
            GSPN_LOGW("chip id 0x%08x from reg is invalidate too!\n", adie_chip_id);
        }
    }
    GSPN_LOGI("return chip id 0x%08x \n", adie_chip_id);
    return adie_chip_id;
}
#endif
static GSPN_CAPABILITY_T* gspn_cfg_capability(GSPN_CONTEXT_T *gspn_ctx)
{
    uint32_t adie_chip_id = 0;

    if(gspn_ctx->capability.magic != GSPN_CAPABILITY_MAGIC_NUMBER) {  // not initialized
        memset((void*)&gspn_ctx->capability,0,sizeof(gspn_ctx->capability));
        gspn_ctx->capability.max_layer_cnt = 4;
        gspn_ctx->capability.max_yuvLayer_cnt = 1;
        gspn_ctx->capability.max_scaleLayer_cnt = 1;
        gspn_ctx->capability.seq0_scale_range_up=256;// 1/4~4
        gspn_ctx->capability.seq0_scale_range_down=1;// 1/16~4
        gspn_ctx->capability.seq1_scale_range_up=64;// 1/4~4
        gspn_ctx->capability.seq1_scale_range_down=4;// 1/16~4
        gspn_ctx->capability.scale_updown_sametime=1;
        gspn_ctx->capability.src_yuv_xywh_even_limit=1;
        gspn_ctx->capability.max_video_size=2;
        gspn_ctx->capability.addr_type_support=GSPN_ADDR_TYPE_IOVIRTUAL;
        gspn_ctx->capability.std_support_in = GSPN_STD_BT601_FULL | GSPN_STD_BT601_NARROW;
        gspn_ctx->capability.std_support_out = GSPN_STD_BT601_FULL | GSPN_STD_BT601_NARROW;
        adie_chip_id = gspn_get_chip_id();
        switch(adie_chip_id&0xFFFF0000) {
            case 0x96310000:/*sharkLT8*/
                gspn_ctx->capability.version = 0x10;
                gspn_ctx->capability.block_alpha_limit=1;
                gspn_ctx->capability.max_video_size=1;
                /*on sharkLT8, the max gspn clock 256MHz,
                blending throughput one pixel one clock on output side.
                scaling throughput is 0.8 pixel one clock on output side.*/
                gspn_ctx->capability.max_throughput=256;
                break;
            default:
                gspn_ctx->capability.version = 0x10;
                gspn_ctx->capability.block_alpha_limit=1;
                gspn_ctx->capability.max_video_size=1;
                /*on sharkLT8, the max gspn clock 256MHz,
                blending throughput one pixel one clock on output side.
                scaling throughput is 0.8 pixel one clock on output side.*/
                gspn_ctx->capability.max_throughput=256;
                GSPN_LOGI("info:a new chip id, be treated as sharkLT8!\n");
                break;
        }

#ifndef CONFIG_SPRD_IOMMU
        //gspn_ctx->capability.buf_type_support = GSPN_ADDR_TYPE_PHYSICAL;
#endif
        gspn_ctx->capability.crop_min.w=gspn_ctx->capability.crop_min.h=4;
        gspn_ctx->capability.out_min.w=gspn_ctx->capability.out_min.h=4;
        gspn_ctx->capability.crop_max.w=gspn_ctx->capability.crop_max.h=8191;
        gspn_ctx->capability.out_max.w=gspn_ctx->capability.out_max.h=8191;
        gspn_ctx->capability.magic = GSPN_CAPABILITY_MAGIC_NUMBER;
    }
    return &gspn_ctx->capability;
}

void gspn_kcmd_list_fence_destroy(struct list_head *kcmd_list)
{
    GSPN_KCMD_INFO_T *kcmd;
    struct gspn_layer_list_fence_data list_data;

    if (kcmd_list == NULL) {
        GSPN_LOGE("kcmd_list is null, so can't destroy\n");
        return;
    }

    list_for_each_entry(kcmd, kcmd_list, list) {
        list_data.rls_cnt = &kcmd->rls_fence_cnt;
        list_data.acq_cnt = &kcmd->acq_fence_cnt;
        list_data.rls_fen_arr = kcmd->rls_fence;
        list_data.acq_fen_arr = kcmd->acq_fence;
        gspn_layer_list_fence_free(&list_data);
    }
}


static long gspn_drv_ioctl(struct file *pfile,
                           uint32_t cmd,
                           unsigned long arg)
{
    int32_t ret = -GSPN_NO_ERR;
    uint32_t param_size = _IOC_SIZE(cmd);
    uint32_t ctl_code = _IOC_NR(cmd);
    uint32_t n = (_IOC_TYPE(cmd)&GSPN_IO_CNT_MASK);// CMD[n]
    uint32_t async_flag = (_IOC_TYPE(cmd)&GSPN_IO_ASYNC_MASK)?1:0;
    uint32_t done_cnt = 0;// used for sync call
    GSPN_CONTEXT_T *gspn_ctx = (GSPN_CONTEXT_T*)pfile->private_data;
    GSPN_CORE_T *core = NULL;
    struct list_head kcmd_list;
    GSPN_LOGI("<<<pid:0x%08x,cmd:0x%08x, io code:0x%x, param_size:%d, type:0x%02x, arg:%ld, async_flag = %d\n",
              current->pid, cmd, _IOC_NR(cmd), param_size, _IOC_TYPE(cmd), arg, async_flag);
    if(gspn_ctx == NULL||arg == 0) {
        GSPN_LOGE("ioctl failed!gspn_ctx:%p,arg:%ld\n",gspn_ctx,arg);
        return -EINVAL;
    }

    switch (ctl_code) {
        case GSPN_CTL_CODE_GET_CAPABILITY:
            if (param_size>=sizeof(GSPN_CAPABILITY_T)) {
                GSPN_CAPABILITY_T *cap=gspn_cfg_capability(gspn_ctx);
                if(arg & MEM_OPS_ADDR_ALIGN_MASK || (ulong)cap & MEM_OPS_ADDR_ALIGN_MASK) {
                    GSPN_LOGW("copy_to_user use none 8B alignment address!");
                }
                ret=copy_to_user((void __user *)arg,(const void*)cap,sizeof(GSPN_CAPABILITY_T));
                if(ret) {
                    GSPN_LOGE("get gspn capability failed in copy_to_user !\n");
                    ret = GSPN_K_COPY_TO_USER_ERR;
                    break;
                }
                GSPN_LOGI("get gspn capability success in copy_to_user \n");
            } else {
                GSPN_LOGE("get gsp capability, buffer is too small,come:%u,need:%zu!",
                          param_size,sizeof(GSPN_CAPABILITY_T));
                ret = GSPN_K_COPY_TO_USER_ERR;
                break;
            }
            break;

        case GSPN_CTL_CODE_SET_PARAM: {
            if(n > GSPN_CMD_ARRAY_MAX || param_size < sizeof(GSPN_CMD_INFO_T)) {
                /*if this happen, we don't destory acq fence,
                 * because we don't know the number of the array*/
                GSPN_LOGE("ioctl failed!%d>%d or %d<%zu!\n",
                          n,GSPN_CMD_ARRAY_MAX,param_size,sizeof(GSPN_CMD_INFO_T));
                return -EINVAL;
            }
            GSPN_LOGI("set cmd: %d\n", stat);
            stat++;
            ret = wait_event_interruptible_timeout(gspn_ctx->empty_list_wq,
                                                   (gspn_ctx->empty_list_cnt>=n ||gspn_ctx->suspend_flag),
                                                   msecs_to_jiffies(500));

            if (ret<=0) { /*ret<0:interrupted by singal ; ret==0: timeout;*/
                GSPN_LOGE("wait condition failed!ret:%d,n:%d,empty:%d,suspend:%d\n",
                          ret,n,gspn_ctx->empty_list_cnt,gspn_ctx->suspend_flag);
                GSPN_LOGE("set cmd: %d\n", stat);
                GSPN_DumpReg(&gspn_ctx->core_mng, 0);
            } else {
                /*GSPN_LOGI("wait condition success!ret:%d,n:%d,empty:%d,suspend:%d, gspn_ctx = 0x%x\n",
                          ret,n,gspn_ctx->empty_list_cnt,gspn_ctx->suspend_flag, gspn_ctx);*/

                down(&gspn_ctx->empty_list_sema);
                if(gspn_ctx->suspend_flag || gspn_ctx->empty_list_cnt < n) {
                    up(&gspn_ctx->empty_list_sema);
                } else {
                    /*get n kcmd from empty list*/
                    ret = gspn_get_kcmd_from_empty_list(gspn_ctx,&kcmd_list,n);
                    up(&gspn_ctx->empty_list_sema);
                    if(!ret) {
                        /*cfg kcmd,copy from user CMD*/
                        GSPN_LOGI("empty cnt: %d\n", gspn_ctx->empty_list_cnt);
                        ret = gspn_fill_kcmd(&kcmd_list,(GSPN_CMD_INFO_T*)arg,&done_cnt);
                        if (!ret) {
                            core = gspn_core_select(gspn_ctx);
                            gspn_kcmdl_mmu_id_set(core, &kcmd_list);
                            /*create release fence for every layer of each KCMD in kcmd_list*/
                            if (async_flag) {
                                ret = gspn_kcmd_list_fence_pre_process(core, &kcmd_list);
                                if (ret) {
                                    GSPN_LOGE("kcmd list fence create failed\n");
                                    goto cfg_exit2; 
                                }
                            }
                            /*put kcmd_list to fill list*/
                            ret = gspn_put_list_to_fill_list(gspn_ctx, &kcmd_list, core);
                            if(!ret) {
                                /*if it's sync cmd, wait it done*/
                                if(async_flag == 0) {
                                    ret = wait_event_interruptible_timeout(gspn_ctx->sync_done_wq,
                                            (done_cnt>=n ||gspn_ctx->suspend_flag),
                                            msecs_to_jiffies(1000));
                                    if (ret <= 0) { /*ret<0:interrupted by singal ; ret==0: timeout;*/
                                        GSPN_LOGE("pid:0x%08x wait frame done failed! ret:%d,n:%d,done_cnt:%d,suspend:%d\n"
                                                , current->pid,ret,n,done_cnt,gspn_ctx->suspend_flag);
                                    } else {
                                        /*GSPN_LOGI("pid:0x%08x wait frame done success! ret:%d,n:%d,done_cnt:%d,suspend:%d\n"
                                                , current->pid, ret, n, done_cnt, gspn_ctx->suspend_flag);*/
                                        ret = 0;
                                    }
                                }
                                break;
                            } else {
                                goto cfg_exit2;
                            }
                        } else {
                            goto cfg_exit2;
                        }
                    } else {
                        /*rel fence have not been created yet, just return*/
                        break;
                    }
                cfg_exit2:
                    if (async_flag) {
                        gspn_kcmd_list_fence_destroy(&kcmd_list);
                    }
                    gspn_put_list_to_empty_list(gspn_ctx, &kcmd_list);
                }
            }
        }
        break;
        default:
            ret = GSPN_K_CTL_CODE_ERR;
            GSPN_LOGE("unknown cmd!\n");
            break;
    }


    GSPN_LOGI("pid:0x%08x,cmd:0x%08x, io number:0x%x, param_size:%d,type:0x%02x,arg:%ld>>>\n",
              current->pid, cmd, _IOC_NR(cmd), param_size,_IOC_TYPE(cmd),arg);
    return ret;
}

static int32_t gspn_drv_open(struct inode *node, struct file *pfile)
{
    struct miscdevice *miscdev = NULL;
    GSPN_CONTEXT_T *gspn_ctx = NULL;
    GSPN_LOGI("enter. pid:0x%08x\n",current->pid);

    miscdev = pfile->private_data;
    if (NULL == miscdev) {
        GSPN_LOGE("open failed!miscdev == NULL!\n");
        return -EINVAL;
    }
    gspn_ctx = container_of(miscdev, GSPN_CONTEXT_T, dev);
    if (NULL == gspn_ctx) {
        GSPN_LOGE("open failed!gspn_ctx == NULL!\n");
        return -EINVAL;
    }
    pfile->private_data = (void*)gspn_ctx;
    return GSPN_NO_ERR;
}

static int32_t gspn_drv_release(struct inode *node, struct file *pfile)
{
    GSPN_LOGI("enter. pid:0x%08x\n",current->pid);
    pfile->private_data = NULL;
    return GSPN_NO_ERR;
}

static int atoi(const char *str)
{
    int value = 0;
    while(*str>='0' && *str<='9') {
        value *= 10;
        value += *str - '0';
        str++;
    }
    return value;
}
static ssize_t gspn_drv_write(struct file *pfile, const char __user * u_data, size_t cnt, loff_t *cnt_ret)
{
    GSPN_CONTEXT_T *gspn_ctx = (GSPN_CONTEXT_T*)pfile->private_data;
    uint32_t i = 0;
    if(gspn_ctx == NULL) {
        GSPN_LOGE("gspn_ctx is NULL! pid:0x%08x\n",current->pid);
        return -ENODEV;
    }

    i = atoi(u_data);
    GSPN_PRINTK("enter. u_data:%s == %d\n", u_data, i);
    if(i<255) {
        gspn_ctx->log_level = i;
    }
    return 1;
}

static ssize_t gspn_drv_read(struct file *pfile, char __user *u_data, size_t cnt, loff_t *cnt_ret)
{
    char rt_word[32]= {0};
    GSPN_CONTEXT_T *gspn_ctx = (GSPN_CONTEXT_T*)pfile->private_data;

    if(gspn_ctx == NULL) {
        GSPN_LOGE("gspn_ctx is NULL! pid:0x%08x\n",current->pid);
        return -ENODEV;
    }

    *cnt_ret = 0;
    *cnt_ret += sprintf(rt_word + *cnt_ret, "log level:%d.\n",gspn_ctx->log_level);
    return copy_to_user(u_data, (void*)rt_word, (ulong)*cnt_ret);
}

static struct file_operations gspn_drv_fops = {
    .owner          = THIS_MODULE,
    .open           = gspn_drv_open,
    .write          = gspn_drv_write,
    .read           = gspn_drv_read,
    .unlocked_ioctl = gspn_drv_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = gspn_drv_ioctl,
#endif
    .release        = gspn_drv_release
};

static int gspn_parse_dt_reg(struct device_node *np,uint32_t idx,uint32_t** reg_base,const char *name)
{
    struct resource res;
    int32_t ret = 0;

    ret = of_address_to_resource(np, idx, &res);
    if (ret) {
        GSPN_LOGE("failed to parse %s reg base!\n",name);
        return -1;
    }
    *reg_base = (uint32_t*)ioremap_nocache(res.start, resource_size(&res));
    if(*reg_base == NULL) {
        GSPN_LOGE("failed to ioremap %s reg base!\n",name);
        return -1;
    }
    GSPN_LOGI("%s pa:%08x va:%p\n", name, (uint32_t)res.start,*reg_base);
    return 0;
}

static int gspn_parse_dt_clk(struct device_node *np,struct clk **ppclk,const char *name)
{
    *ppclk = of_clk_get_by_name(np,name);
    if (IS_ERR(*ppclk)) {
        GSPN_LOGE("failed to get %s!\n",name);
        return -EINVAL;
    }
    //GSPN_LOGI("get %s success!\n",name);
    return 0;
}

static int gspn_parse_dt_number(struct device_node *np,uint32_t *pNumber,const char *name)
{
    uint32_t data = 0;
    int32_t ret = 0;

    /*get and check core id*/
    ret = of_property_read_u32(np, name, &data);
    if (ret) {
        GSPN_LOGE("get %s err!\n",name);
    } else {
        *pNumber = data;
    }
    return ret;
}

/*
gspn_parse_dt_init_core
description: parse dt structure and initialize core structure
*/
static int gspn_parse_dt_init_core(GSPN_CONTEXT_T *gspn_ctx)
{
    struct device_node *np = NULL, *nchd = NULL;
    GSPN_CORE_T *core = NULL;
    uint32_t nr = 0;
    int32_t ret = 0, i = 0;

    /* param check */
    if(gspn_ctx == NULL ||gspn_ctx->gspn_of_dev == NULL||gspn_ctx->gspn_of_dev->of_node == NULL) {
        GSPN_LOGE("parameter err!\n");
        return -EINVAL;
    }
    np = gspn_ctx->gspn_of_dev->of_node;
    nr = of_get_child_count(np);
    if (nr == 0 || nr > GSPN_CORE_MAX) {
        GSPN_LOGE("get dt child count err!\n");
        return (!nr ? -ENODEV : -EINVAL);
    }

    /* alloc core structure */
    //core = kzalloc(nr*sizeof(GSPN_CORE_T), GFP_KERNEL);
    core = vzalloc(nr*sizeof(GSPN_CORE_T));
    if (core == NULL) {
        GSPN_LOGE("failed to alloc gspn core structure!\n");
        return -ENOMEM;
    }

    /* parse ap interrupt reg base */
    ret = gspn_parse_dt_reg(np,0,&gspn_ctx->core_mng.int_reg_base,"sprd,glb_int_base");
    /*parse gspn clock enable ctl reg base */
    ret |= gspn_parse_dt_reg(np,1,&gspn_ctx->core_mng.enable_reg_base,"sprd,gspn_en_base");
    /*parse gspn soft reset ctl reg base */
    ret |= gspn_parse_dt_reg(np,2,&gspn_ctx->core_mng.reset_reg_base,"sprd,gspn_rst_base");
    /*parse gspn soft reset ctl reg base */
    ret |= gspn_parse_dt_reg(np,3,&gspn_ctx->core_mng.chip_id_addr,"sprd,ap_chipid_addr");
    if (ret) {
        GSPN_LOGE(" core%d parse int/en/rst reg err!\n",i);
        goto error;
    }

    /* parse each core params */
    i = 0;
    for_each_child_of_node(np, nchd) {
        /* get and check core id */
        ret = gspn_parse_dt_number(nchd, &core[i].core_id, "sprd,core_id");
        if (ret || i != core[i].core_id) {
            GSPN_LOGE("core_id order err, i %d, data %d.\n",i,core[i].core_id);
            goto error;
        }

        /* alloc CMDQ reg structure,
        this memory must be phy, if it's virtual, it's hard to map it to iommu*/
        core[i].cmdq = kzalloc(GSPN_CMDQ_ARRAY_MAX*sizeof(GSPN_CMD_REG_T), GFP_KERNEL);
        if (core[i].cmdq == NULL) {
            GSPN_LOGE("core%d, failed to alloc CMDQ array!\n",i);
            goto error;
        }

        ret = gspn_parse_dt_number(nchd,&core[i].gspn_en_rst_bit,"sprd,gspn_en_rst_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].mmu_en_rst_bit,"sprd,mmu_en_rst_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].auto_gate_bit,"sprd,auto_gate_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].force_gate_bit,"sprd,force_gate_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].emc_en_bit,"sprd,emc_en_bit");
       // ret |= gspn_parse_dt_number(nchd,&core[i].noc_auto_bit,"sprd,noc_auto_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].noc_force_bit,"sprd,noc_force_bit");
       // ret |= gspn_parse_dt_number(nchd,&core[i].mtx_auto_bit,"sprd,mtx_auto_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].mtx_force_bit,"sprd,mtx_force_bit");
        ret |= gspn_parse_dt_number(nchd,&core[i].mtx_en_bit,"sprd,mtx_en_bit");
        if (ret) {
            GSPN_LOGE("core%d parse bit number err!\n",i);
            goto error;
        }
        /*parse core[n] module reg base */
        ret = gspn_parse_dt_reg(nchd,0,(uint32_t**)&core[i].ctl_reg_base,"sprd,spn_ctl_reg");
        /*parse core[n] iommu ctl reg base */
        ret |= gspn_parse_dt_reg(nchd,1,&core[i].iommu_ctl_reg_base,"sprd,mmu_ctl_reg");
        /*parse core[n] clock source select ctl reg base */
        ret |= gspn_parse_dt_reg(nchd,2,&core[i].clk_select_reg_base,"sprd,gspn_clk_sel");
        /*parse core[n] clock auto-gate reg base */
        ret |= gspn_parse_dt_reg(nchd,3,&core[i].auto_gate_reg_base,"sprd,gspn_auto_gate");
        /*parse core[n] emc clock reg base */
        ret |= gspn_parse_dt_reg(nchd,4,&core[i].emc_reg_base,"sprd,gspn_emc_reg");
        if (ret) {
            GSPN_LOGE("core%d failed to get reg base!\n",i);
            goto error;
        } 

        /* parse core[n] irq number */
        core[i].interrupt_id = irq_of_parse_and_map(nchd, i);
        if (core[i].interrupt_id == 0) {
            GSPN_LOGE("core%d failed to parse irq number!\n",i);
            ret = -EINVAL;
            goto error;
        }
        GSPN_LOGI("core%d, irq:%d,\n",i,core[i].interrupt_id);

        ret = gspn_parse_dt_clk(nchd,&core[i].mtx_clk,GSPN_MTX_CLOCK_NAME);/* parse core[n] emc clock */
        ret |= gspn_parse_dt_clk(nchd,&core[i].niu_clk,GSPN_NIU_CLOCK_NAME);/* parse core[n] gspn clock parent */
        ret |= gspn_parse_dt_clk(nchd,&core[i].disp_clk,GSPN_DISP_CLOCK_NAME);/* parse core[n] gspn clock parent */
        ret |= gspn_parse_dt_clk(nchd,&core[i].gspn_clk_parent,GSPN_CLOCK_PARENT3);/* parse core[n] gspn clock parent */
        ret |= gspn_parse_dt_clk(nchd,&core[i].mtx_clk_parent,GSPN_MTX_CLOCK_PARENT_NAME);/* parse core[n] emc clock parent */
        ret |= gspn_parse_dt_clk(nchd,&core[i].niu_clk_parent,GSPN_NIU_CLOCK_PARENT_NAME);/* parse core[n] gspn clock parent */
        ret |= gspn_parse_dt_clk(nchd,&core[i].emc_clk,GSPN_EMC_CLOCK_NAME);/* parse core[n] gspn clock parent */
        if (i == 0) {
            ret |= gspn_parse_dt_clk(nchd,&core[i].gspn_clk,GSPN0_CLOCK_NAME);/* parse core[n] gspn clock */
            ret |= gspn_parse_dt_clk(nchd,&core[i].mmu_clk,GSPN0_MMU_CLOCK_NAME);/* parse core[n] gspn clock parent */
        }
        else {
            ret |= gspn_parse_dt_clk(nchd,&core[i].gspn_clk,GSPN1_CLOCK_NAME);/* parse core[n] gspn clock */
            ret |= gspn_parse_dt_clk(nchd,&core[i].mmu_clk,GSPN1_MMU_CLOCK_NAME);/* parse core[n] gspn clock parent */
        }
        if(ret) {
            GSPN_LOGE("core%d failed to get clock!\n",i);
            goto error;
        }

        //core[i].status = GSPN_CORE_UNINITIALIZED; //hl changed
        core[i].status = GSPN_CORE_FREE; 
        core[i].current_kcmd = NULL;
        core[i].current_kcmd_sub_cmd_index = -1;
        i++;
    }
    gspn_ctx->core_mng.core_cnt = nr;
    gspn_ctx->core_mng.free_cnt = nr;
    gspn_ctx->core_mng.cores = core;
    GSPN_LOGI("parse success, exit.\n");
    return 0;

error:
    if(core) {
        i = 0;
        while(i < nr) {
            if(core[i].cmdq) {
                kfree(core[i].cmdq);
                core[i].cmdq = NULL;
            }
        }
        vfree(core);
        core = NULL;
    }

    return ret;
}


/*
gspn_clock_init
description: only set parents, clock enable implement in gspn_module_enable()
*/
static int32_t gspn_clock_init(GSPN_CONTEXT_T *gspn_ctx)
{
    int ret = 0, i = 0;

    while(i<gspn_ctx->core_mng.core_cnt) {
        ret = clk_set_parent(gspn_ctx->core_mng.cores[i].gspn_clk, gspn_ctx->core_mng.cores[i].gspn_clk_parent);
        if(ret) {
            GSPN_LOGE("core%d failed to set gspn clk parent!\n",i);
            return ret;
        } else {
            GSPN_LOGI("core%d set gspn clk parent success!\n",i);
        }

        ret = clk_set_parent(gspn_ctx->core_mng.cores[i].mmu_clk, gspn_ctx->core_mng.cores[i].mmu_clk_parent);
        if(ret) {
            GSPN_LOGE("core%d failed to set mtx clk parent!\n",i);
            return ret;
        } else {
            GSPN_LOGI("core%d set mtx clk parent success!\n",i);
        }

        ret = clk_set_parent(gspn_ctx->core_mng.cores[i].mtx_clk, gspn_ctx->core_mng.cores[i].mtx_clk_parent);
        if(ret) {
            GSPN_LOGE("core%d failed to set mtx clk parent!\n",i);
            return ret;
        } else {
            GSPN_LOGI("core%d set mtx clk parent success!\n",i);
        }

        ret = clk_set_parent(gspn_ctx->core_mng.cores[i].niu_clk, gspn_ctx->core_mng.cores[i].niu_clk_parent);
        if(ret) {
            GSPN_LOGE("core%d failed to set disp clk parent!\n",i);
            return ret;
        } else {
            GSPN_LOGI("core%d set mtx clk parent success!\n",i);
        }

        ret = clk_set_parent(gspn_ctx->core_mng.cores[i].disp_clk, gspn_ctx->core_mng.cores[i].emc_clk);
        if(ret) {
            GSPN_LOGE("core%d failed to set mtx clk parent!\n",i);
            return ret;
        } else {
            GSPN_LOGI("core%d set mtx clk parent success!\n",i);
        }

        i++;
    }
    GSPN_CLK_GATE_ENABLE(&gspn_ctx->core_mng);
    return 0;
}

static int32_t gspn_suspend_common(GSPN_CONTEXT_T *gspn_ctx)
{
    int32_t ret = 0;
    int32_t i = 0;
    gspn_ctx->suspend_flag = 1;
    up(&gspn_ctx->wake_work_thread_sema);
    wake_up_interruptible_all(&(gspn_ctx->empty_list_wq));
    wake_up_interruptible_all(&(gspn_ctx->sync_done_wq));

    /*in case of GSPN is processing now, wait it finish*/
    ret = down_timeout(&gspn_ctx->suspend_clean_done_sema,msecs_to_jiffies(500));
    if(ret) {
        GSPN_LOGE("wait suspend clean done sema failed, ret: %d\n",ret);
        while(i<gspn_ctx->core_mng.core_cnt) {
            GSPN_DumpReg(&gspn_ctx->core_mng, i);
            i++;
        }
    } else {
        GSPN_LOGI("wait suspend clean done sema success. \n");
    }
    return ret;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void gspn_early_suspend(struct early_suspend* es)
{
    GSPN_CONTEXT_T *gspn_ctx = NULL;

    GSPN_LOGI("enter.\n");

    gspn_ctx = container_of(es, GSPN_CONTEXT_T, earlysuspend);
    if (NULL == gspn_ctx) {
        GSPN_LOGE("suspend failed! gspn_ctx == NULL!\n");
        return;
    }
    gspn_suspend_common(gspn_ctx);
    REG_OR((gspn_ctx->core_mng).cores[0].auto_gate_reg_base,(gspn_ctx->core_mng).cores[0].noc_force_bit);
}

static void gspn_late_resume(struct early_suspend* es)
{
    GSPN_CONTEXT_T *gspn_ctx = NULL;

    GSPN_PRINTK("enter.\n");
    gspn_ctx = container_of(es, GSPN_CONTEXT_T, earlysuspend);
    if (NULL == gspn_ctx) {
        GSPN_LOGE("resume failed!gspn_ctx == NULL!\n");
        return;
    }

    gspn_ctx->gsp_coef_force_calc = 1;
    GSPN_CLK_GATE_ENABLE(&gspn_ctx->core_mng);

    /*only on sharkLT8, gsp/gspn select*/
    if(gspn_ctx->core_mng.module_sel_reg_base && gspn_ctx->capability.version == 0x10) {
        REG_SET(gspn_ctx->core_mng.module_sel_reg_base, 1);
    }
    gspn_ctx->suspend_flag = 0;
}

#else
static int32_t gspn_suspend(struct platform_device *pdev,pm_message_t state)
{
    GSPN_CONTEXT_T *gspn_ctx = NULL;
    int32_t ret = 0;

    GSPN_PRINTK("enter.\n");
    gspn_ctx = platform_get_drvdata(pdev);
    if (NULL == gspn_ctx) {
        GSPN_LOGE("suspend failed! gspn_ctx == NULL!\n");
        return -EINVAL;
    }
    ret = gspn_suspend_common(gspn_ctx);
    return ret;
}

static int32_t gspn_resume(struct platform_device *pdev)
{
    GSPN_CONTEXT_T *gspn_ctx = NULL;
    GSPN_PRINTK("enter\n");

    gspn_ctx = (GSPN_CONTEXT_T *)platform_get_drvdata(pdev);
    if (NULL == gspn_ctx) {
        GSPN_LOGE("resume failed!gspn_ctx == NULL!\n");
        return -EINVAL;
    }

    gspn_ctx->gsp_coef_force_calc = 1;
    GSPN_CLK_GATE_ENABLE(&gspn_ctx->core_mng);
    gspn_ctx->suspend_flag = 0;
    return 0;
}

#endif


/*
func:gspn_async_init
desc:init list, create work thread, prepare for async feature
*/
static int32_t gspn_async_init(GSPN_CONTEXT_T *gspn_ctx)
{
    int32_t i = 0;
    struct gspn_sync_timeline* tl = NULL;
    GSPN_CORE_T *core = NULL;
    /*cmd list initialize*/
    INIT_LIST_HEAD(&gspn_ctx->empty_list);

    while(i < GSPN_KCMD_MAX) {
        list_add_tail(&gspn_ctx->kcmds[i].list, &gspn_ctx->empty_list);
        i++;
    }
    /*gspn context initialized*/
    gspn_ctx->empty_list_cnt = GSPN_KCMD_MAX;
    init_waitqueue_head(&gspn_ctx->empty_list_wq);
    init_waitqueue_head(&gspn_ctx->sync_done_wq);

    sema_init(&gspn_ctx->wake_work_thread_sema,0);
    sema_init(&gspn_ctx->empty_list_sema, 1);
    sema_init(&gspn_ctx->suspend_clean_done_sema,0);

    /*gspn all cores initialized*/
    for (i = 0; i < gspn_ctx->core_mng.core_cnt; i++) {
        core = &gspn_ctx->core_mng.cores[i];
        INIT_LIST_HEAD(&core->fill_list);
        core->fill_list_cnt = 0;
        sema_init(&core->fill_list_sema, 1);
        sema_init(&core->fence_create_sema,1);
        tl = gspn_sync_timeline_create("gspn_timeline");
        if (tl == NULL) {
            GSPN_LOGE("create timeline failed!\n");
            return -1;
        }
        core->timeline = tl;
    }

    gspn_ctx->work_thread = kthread_run(gspn_work_thread, (void *)gspn_ctx, "gspn_work_thread");
    if (IS_ERR(gspn_ctx->work_thread)) {
        GSPN_LOGE("create gspn work thread failed! %ld\n",PTR_ERR(gspn_ctx->work_thread));
        gspn_ctx->work_thread = NULL;
        goto error;
    }
    gspn_ctx->remain_async_cmd_cnt = 0;

    return 0;

error:
    gspn_sync_timeline_destroy(core->timeline);
    return -1;
}



static int32_t gspn_drv_probe(struct platform_device *pdev)
{
    GSPN_CONTEXT_T *gspn_ctx = NULL;
    int32_t ret = 0;
    int32_t i = 0;


    /*params check*/
    if(pdev == NULL) {
        GSPN_LOGE("pdev is null!!\n");
        return -EINVAL;
    }

    /*alloc ctx*/
    //gspn_ctx =kzalloc(sizeof(GSPN_CONTEXT_T), GFP_KERNEL);
    gspn_ctx =vzalloc(sizeof(GSPN_CONTEXT_T));
    if (NULL == gspn_ctx) {
        GSPN_LOGE("alloc GSPN_CONTEXT_T failed!\n");
        return -ENOMEM;
    }
    g_gspn_ctx = gspn_ctx;

    /*ctx initialize*/
    gspn_ctx->log_level = 0;
    gspn_ctx->gspn_of_dev = &(pdev->dev);
    ret = gspn_parse_dt_init_core(gspn_ctx);
    if (ret) {
        goto exit;
    }

    /*chip id check*/
    gspn_cfg_capability(gspn_ctx);
    if(gspn_ctx->capability.magic != GSPN_CAPABILITY_MAGIC_NUMBER) {
        GSPN_LOGE("chip id not match, maybe there is only GSP not GSPN!\n");
        goto exit;
    }

    /*only on sharkLT8, gsp/gspn select*/
    if(gspn_ctx->core_mng.module_sel_reg_base && gspn_ctx->capability.version == 0x10) {
        REG_SET(gspn_ctx->core_mng.module_sel_reg_base, 1);
    }

    ret = gspn_clock_init(gspn_ctx);
    if (ret) {
        goto exit;
    }

    INIT_LIST_HEAD(&gspn_ctx->core_mng.dissociation_list);

    ret = gspn_async_init(gspn_ctx);
    if (ret) {
        goto exit;
    }

    /*misc dev register*/
    gspn_ctx->dev.minor = MISC_DYNAMIC_MINOR;
    gspn_ctx->dev.name = "sprd_gspn";
    gspn_ctx->dev.fops = &gspn_drv_fops;
    ret = misc_register(&gspn_ctx->dev);
    if (ret) {
        GSPN_LOGE("gspn cannot register miscdev (%d)\n",ret);
        goto exit;
    }

    /*request irq*/
    i = 0;
    while(i<gspn_ctx->core_mng.core_cnt) {
        ret = request_irq(gspn_ctx->core_mng.cores[i].interrupt_id,
                          gspn_irq_handler,
                          0,//IRQF_SHARED
                          "GSPN",
                          gspn_ctx);
        if (ret) {
            GSPN_LOGE("core%d could not request irq %d!\n",i,gspn_ctx->core_mng.cores[i].interrupt_id);
            goto exit1;
        }
        i++;
    }


#ifdef CONFIG_HAS_EARLYSUSPEND
    gspn_ctx->earlysuspend.suspend = gspn_early_suspend;
    gspn_ctx->earlysuspend.resume  = gspn_late_resume;
    gspn_ctx->earlysuspend.level   = EARLY_SUSPEND_LEVEL_STOP_DRAWING;
    register_early_suspend(&gspn_ctx->earlysuspend);
#endif

    platform_set_drvdata(pdev, gspn_ctx);

    GSPN_LOGI("gspn probe success.\n");
    return ret;

exit1:
    misc_deregister(&gspn_ctx->dev);

exit:
    if(gspn_ctx->work_thread) {
        kthread_stop(gspn_ctx->work_thread);
        gspn_ctx->work_thread = NULL;
    }

    if (NULL != gspn_ctx) {
        if (NULL != gspn_ctx->core_mng.cores) {
            i = 0;
            while(i < gspn_ctx->core_mng.core_cnt) {
                if(gspn_ctx->core_mng.cores[i].cmdq) {
                    kfree(gspn_ctx->core_mng.cores[i].cmdq);
                    gspn_ctx->core_mng.cores[i].cmdq = NULL;
                }
            }
            vfree(gspn_ctx->core_mng.cores);
            gspn_ctx->core_mng.cores = NULL;
        }
        vfree(gspn_ctx);
        gspn_ctx = NULL;
    }

    // TODO: should set to 0 after probe to close log
    //gspn_ctx->log_level = 0;

    return ret;
}

static int32_t gspn_drv_remove(struct platform_device *pdev)
{
    int i = 0;
    GSPN_CONTEXT_T *gspn_ctx = NULL;
    pr_info("%s[%d] enter.\n",__func__,__LINE__);

    gspn_ctx = platform_get_drvdata(pdev);
    if (NULL == gspn_ctx) {
        return -EINVAL;
    }

    if(gspn_ctx->work_thread) {
        kthread_stop(gspn_ctx->work_thread);
        gspn_ctx->work_thread = NULL;
    }

    i = 0;
    while(i<gspn_ctx->core_mng.core_cnt) {
        free_irq(gspn_ctx->core_mng.cores[i].interrupt_id, gspn_irq_handler);
        i++;
    }

    misc_deregister(&gspn_ctx->dev);
    if(gspn_ctx->core_mng.cores) {
        i = 0;
        while(i < gspn_ctx->core_mng.core_cnt) {
            if(gspn_ctx->core_mng.cores[i].cmdq) {
                kfree(gspn_ctx->core_mng.cores[i].cmdq);
                gspn_ctx->core_mng.cores[i].cmdq = NULL;
            }
        }
        vfree(gspn_ctx->core_mng.cores);
        gspn_ctx->core_mng.cores = NULL;
    }

    vfree(gspn_ctx);
    gspn_ctx = NULL;
    platform_set_drvdata(pdev, gspn_ctx);
    pr_info("%s[%d] exit.\n",__func__,__LINE__);
    return 0;
}

static const struct of_device_id sprdgspn_dt_ids[] = {
    { .compatible = "sprd,gspn", },
    {}
};

static struct platform_driver gspn_driver = {
    .probe = gspn_drv_probe,
    .remove = gspn_drv_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
    .suspend = gspn_suspend,
    .resume = gspn_resume,
#endif
    .driver =
    {
        .owner = THIS_MODULE,
        .name = "sprd_gspn",
        .of_match_table = of_match_ptr(sprdgspn_dt_ids),
    }
};

static int32_t __init gspn_drv_init(void)
{
    pr_info("%s[%d] enter! \n",__func__,__LINE__);

    if (platform_driver_register(&gspn_driver) != 0) {
        pr_info("%s[%d] gspn platform driver register Failed!\n",__func__,__LINE__);
        return -1;
    } else {
        pr_info("%s[%d] gspn platform driver registered successful!\n",__func__,__LINE__);
    }
    return 0;
}

static void gspn_drv_exit(void)
{
    platform_driver_unregister(&gspn_driver);
    pr_info("%s[%d] gspn platform driver unregistered!\n",__func__,__LINE__);
}

module_init(gspn_drv_init);
module_exit(gspn_drv_exit);

MODULE_DESCRIPTION("GSPN Driver");
MODULE_LICENSE("GPL");
