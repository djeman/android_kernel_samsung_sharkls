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
/*
gspn_kcmd_leak_check
description: in ioctl, if user process killed after get kcmd from empty but before put to fill,
                 the kcmd maybe dissociate from managed list, maybe there is the other scenario exsit,
                 that cause kcmd leak, this function check this case and recover it.
*/
static void gspn_kcmd_leak_check(GSPN_CONTEXT_T *gspn_ctx)
{
    int32_t i = 0;
    GSPN_CORE_T *core = NULL;
    /*if empty-list-cnt not equal the total-kcmd-cnt, reinitialize the kcmd list*/
    if(gspn_ctx->empty_list_cnt != GSPN_KCMD_MAX) {
        GSPN_LOGW("empty cnt %d != total cnt %d\n", gspn_ctx->empty_list_cnt, GSPN_KCMD_MAX);

        /*cmd list initialize*/
        INIT_LIST_HEAD(&gspn_ctx->empty_list);
        i = 0;
        while(i < GSPN_KCMD_MAX) {
            list_add_tail(&gspn_ctx->kcmds[i].list, &gspn_ctx->empty_list);
            i++;
        }
        for (i = 0; i < gspn_ctx->core_mng.core_cnt; i++) {
            INIT_LIST_HEAD(&core->fill_list);
            core->fill_list_cnt = 0;
            sema_init(&core->fill_list_sema, 1);
        }
        gspn_ctx->empty_list_cnt = GSPN_KCMD_MAX;
        sema_init(&gspn_ctx->empty_list_sema, 1);
        init_waitqueue_head(&gspn_ctx->empty_list_wq);
        init_waitqueue_head(&gspn_ctx->sync_done_wq);
        INIT_LIST_HEAD(&gspn_ctx->core_mng.dissociation_list);
    }
}

static void gspn_suspend_process(GSPN_CONTEXT_T *gspn_ctx)
{
    GSPN_KCMD_INFO_T *kcmd = NULL;
    GSPN_CORE_T **core = &gspn_ctx->core_mng.cores;
    struct list_head temp_list;
    uint32_t record_cnt = 0;
    uint32_t real_cnt = 0;
    int32_t i = 0;

    if(gspn_ctx->suspend_flag) {
        /*temp_list = fill_list*/
        for (i = 0; i < gspn_ctx->core_mng.core_cnt; i++) {
            down(&core[i]->fill_list_sema);
            list_add_tail(&temp_list, &core[i]->fill_list);
            list_del(&core[i]->fill_list);
            INIT_LIST_HEAD(&core[i]->fill_list);
            record_cnt = core[i]->fill_list_cnt;
            core[i]->fill_list_cnt = 0;
            up(&core[i]->fill_list_sema);
        }

        GSPN_LOGI("get list from fill-list ok.\n");

        list_for_each_entry(kcmd, &temp_list, list) {
            if(kcmd->src_cmd.misc_info.async_flag) {
                /*put all acq fence */
                gspn_layer_list_acquire_fence_free(kcmd->acq_fence,
                                                    &kcmd->acq_fence_cnt);
                for (i = 0; i < gspn_ctx->core_mng.core_cnt; i++) {
                    /*signal release fence*/
                    down(&core[i]->fence_create_sema);
                    gspn_ctx->remain_async_cmd_cnt--;
                    gspn_sync_fence_signal(core[i]->timeline);
                    up(&core[i]->fence_create_sema);
                }
            } else {
                if (kcmd->done_cnt == NULL) {
                    (*(kcmd->done_cnt))++;
                } else {
                    GSPN_LOGE("done_cnt is null\n");
                }
                wake_up_interruptible_all(&(gspn_ctx->sync_done_wq));
            }
            real_cnt++;
        }
        GSPN_LOGI("fill-list fence process ok.\n");

        if(real_cnt != record_cnt) {
            GSPN_LOGW("real cnt %d != %d!\n",real_cnt,record_cnt);
        }

        /*add temp_list to empty_list*/
        gspn_put_list_to_empty_list(gspn_ctx,&temp_list);
        GSPN_LOGI("put fill-list to empty-list ok.\n");

        /*if all cores done, notify suspend thread*/
        if(GSPN_ALL_CORE_FREE(gspn_ctx)) {
            /*if all core free, and dissociation list is not empty, move it to empty list*/
            if(gspn_ctx->core_mng.dissociation_list.next != &gspn_ctx->core_mng.dissociation_list) {
                gspn_put_list_to_empty_list(gspn_ctx,&gspn_ctx->core_mng.dissociation_list);
                INIT_LIST_HEAD(&gspn_ctx->core_mng.dissociation_list);
            }

            gspn_kcmd_leak_check(gspn_ctx);
            GSPN_LOGI("suspend done.\n");

            up(&gspn_ctx->suspend_clean_done_sema);
        }
    }
}


static void gspn_recalculate_timeout(GSPN_CONTEXT_T *gspn_ctx)
{
    struct timespec startTime = {-1UL,-1UL};// the earliest start core
    struct timespec currentTime = {-1UL,-1UL};
    long long elapse = 0;
    int32_t i = 0;

    if(gspn_ctx->core_mng.cores[0].fill_list_cnt == 0
    || gspn_ctx->core_mng.cores[0].fill_list_cnt == 0) {
        if(GSPN_ALL_CORE_FREE(gspn_ctx)) {
            gspn_ctx->timeout = 0xffffffff;// set to max to wait cmd come
            GSPN_LOGI("fill-list empty and all cores free, set time to infinite.\n");
            return;
        }
    } else {
        if(gspn_ctx->core_mng.free_cnt > 0) {
            gspn_ctx->timeout = 1;
            GSPN_LOGI("fill-list not empty and have core free, wake work thread to process.\n");
            up(&gspn_ctx->wake_work_thread_sema);
            return;
        }
    }

    /*get the earliest start time*/
    i = 0;
    while (i < gspn_ctx->core_mng.core_cnt) {
        if ((gspn_ctx->core_mng.cores[i].status == GSPN_CORE_BUSY)
           &&(gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_sec < startTime.tv_sec
              ||(gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_sec == startTime.tv_sec
                 &&gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_nsec < startTime.tv_nsec))) {
            startTime.tv_sec = gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_sec;
            startTime.tv_nsec = gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_nsec;
             GSPN_LOGE("status busy enter!gspn_ctx->core_mng.cores[i].status = %d",
                     gspn_ctx->core_mng.cores[i].status);
        }
        i++;
    }
    i = 0;
    while (i < gspn_ctx->core_mng.core_cnt) {
        if (gspn_ctx->core_mng.cores[i].status == GSPN_CORE_BUSY) {
        get_monotonic_boottime(&currentTime);
        /*re-calculate the earliest timeout*/
        elapse = (currentTime.tv_sec - startTime.tv_sec)*1000000000 + currentTime.tv_nsec - startTime.tv_nsec;
        GSPN_LOGI("elapse = %lld, currentTime.tv_nsec = %ld, startTime.tv_nsec = %ld"
                "currentTime.tv_sec = %ld, startTime.tv_sec = %ld",
            elapse,currentTime.tv_nsec, startTime.tv_nsec, currentTime.tv_sec, startTime.tv_sec);
        if(elapse < MS_TO_NS(GSPN_CMD_EXE_TIMEOUT)) {
            gspn_ctx->timeout = NS_TO_US(MS_TO_NS(GSPN_CMD_EXE_TIMEOUT) - elapse);
            GSPN_LOGI("set timeout %u us.\n", gspn_ctx->timeout);
        } else { /*time is up*/
            gspn_ctx->timeout = 0;
            //GSPN_TryToRecover(i,gspn_ctx->core_mng.cores[i].ctl_reg_base);//recovery core i
            //up(gspn_ctx->wake_work_thread_sema);
            GSPN_LOGI("time up! set 0 to leave this case to timeout_process().\n");
        }
    }
    i++;
  }
}


static GSPN_ERR_CODE_E gspn_iommu_map(GSPN_CONTEXT_T *gspn_ctx,GSPN_KCMD_INFO_T *kcmd)
{
    struct ion_addr_data data;

#define GSPN_LAYER_MAP(lx_info)\
    if((lx_info).layer_en == 1 &&  (lx_info).addr_info.share_fd) {\
        data.dmabuf = (lx_info).addr_info.dmabuf;\
        data.is_need_iova = true; \
        data.fd_buffer = -1;\
        if(sprd_ion_get_addr(kcmd->mmu_id, &data)) {\
            GSPN_LOGE("map failed!\n");\
            return GSPN_K_IOMMU_MAP_ERR;\
        }\
        if(data.iova_enabled) {\
            (lx_info).addr.plane_y = data.iova_addr;\
            GSPN_LOGI("dma buf virt address: %lx,  share_fd: %d \n", data.iova_addr,  (lx_info).addr_info.share_fd);\
        } else {\
            (lx_info).addr.plane_y = data.phys_addr;\
            GSPN_LOGI("dma buf phy address: %x\n", data.phys_addr);\
        }\
        (lx_info).addr.plane_u= (lx_info).addr.plane_y + (lx_info).addr_info.uv_offset;\
        (lx_info).addr.plane_v= (lx_info).addr.plane_y + (lx_info).addr_info.v_offset;\
    }

    GSPN_LAYER_MAP(kcmd->src_cmd.l0_info);
    GSPN_LAYER_MAP(kcmd->src_cmd.l1_info);
    GSPN_LAYER_MAP(kcmd->src_cmd.l2_info);
    GSPN_LAYER_MAP(kcmd->src_cmd.l3_info);
    GSPN_LAYER_MAP(kcmd->src_cmd.des1_info);
    GSPN_LAYER_MAP(kcmd->src_cmd.des2_info);

    return GSPN_NO_ERR;
}

static void sprd_ion_put_dmabuf(struct dma_buf *dmabuf)
{
    dma_buf_put(dmabuf);
}

static int gspn_iommu_unmap(GSPN_CONTEXT_T *gspn_ctx,GSPN_KCMD_INFO_T *kcmd)
{
    struct ion_addr_data data;
#define GSPN_LAYER_UNMAP(lx_info)\
    if((lx_info).layer_en == 1  && (lx_info).addr_info.share_fd)  {\
        data.dmabuf = (lx_info).addr_info.dmabuf;\
        data.is_need_iova = true;\
        data.fd_buffer = -1;\
        sprd_ion_free_addr(kcmd->mmu_id, &data);\
        sprd_ion_put_dmabuf(data.dmabuf);\
    }
    GSPN_LAYER_UNMAP(kcmd->src_cmd.l0_info);
    GSPN_LAYER_UNMAP(kcmd->src_cmd.l1_info);
    GSPN_LAYER_UNMAP(kcmd->src_cmd.l2_info);
    GSPN_LAYER_UNMAP(kcmd->src_cmd.l3_info);
    GSPN_LAYER_UNMAP(kcmd->src_cmd.des1_info);
    GSPN_LAYER_UNMAP(kcmd->src_cmd.des2_info);
    return 0;
}


/*
gspn_frame_post_process
description:if frame list done, move these kcmds from dissociation list to empty list
*/
static int32_t gspn_frame_post_process(GSPN_CONTEXT_T *gspn_ctx, GSPN_KCMD_INFO_T *kcmd)
{
    int32_t ret = 1;
    struct list_head frame_list;
    if(gspn_ctx->suspend_flag == 0) {
        list_add_tail(&frame_list, &kcmd->list_frame);
        list_for_each_entry(kcmd, &frame_list, list_frame) {
            if(kcmd->done_flag == 0) {
                ret = 0;
                break;
            }
        }

        if(ret == 1) { // all done
            GSPN_LOGI("frame done.\n");
            list_for_each_entry(kcmd, &frame_list, list_frame) {
                 GSPN_LOGI("kcmd->list = %p, kcmd = %p\n", &kcmd->list, kcmd);
                 list_del(&kcmd->list);//break from dissociation_list
                 list_add_tail(&kcmd->list, &gspn_ctx->empty_list);
                 gspn_ctx->empty_list_cnt++;
                 up(&gspn_ctx->empty_list_sema);
                 wake_up_interruptible_all(&(gspn_ctx->empty_list_wq));
                 GSPN_LOGI("empty cnt: %d\n", gspn_ctx->empty_list_cnt);
            }
        }
        list_del(&frame_list);
    } else {
        INIT_LIST_HEAD(&frame_list);
        list_del(&kcmd->list);//break from dissociation_list
        list_add_tail(&kcmd->list, &frame_list);
        gspn_put_list_to_empty_list(gspn_ctx,&frame_list);
    }
    return ret;
}

/*
func:gspn_calc_exe_time
desc:calc and return the execute time of core, in us unit.
*/
static uint32_t gspn_calc_exe_time(GSPN_KCMD_INFO_T *kcmd)
{
    struct timespec startTime = {-1UL,-1UL};// the earliest start core
    struct timespec currentTime = {-1UL,-1UL};
    long long elapse = 0;

    get_monotonic_boottime(&currentTime);
    startTime.tv_sec = kcmd->start_time.tv_sec;
    startTime.tv_nsec = kcmd->start_time.tv_nsec;
    elapse = (currentTime.tv_sec - startTime.tv_sec)*1000000000 + currentTime.tv_nsec - startTime.tv_nsec;
    return NS_TO_US(elapse);
}

static void gspn_kcmd_post_process(GSPN_CONTEXT_T *gspn_ctx)
{
    GSPN_KCMD_INFO_T *kcmd = NULL;
    GSPN_CTL_REG_T *base = NULL;
    GSPN_CORE_T *core = NULL;
    int32_t index = -1;
    int32_t i = 0;
    GSPN_LOGI("enter.gspn_ctx->core_mng.core_cnt = %d\n", gspn_ctx->core_mng.core_cnt);

    while (i < gspn_ctx->core_mng.core_cnt) {
        core = &gspn_ctx->core_mng.cores[i];
        kcmd = core->current_kcmd;
        base = core->ctl_reg_base;
        index = core->current_kcmd_sub_cmd_index;
        if(kcmd != NULL && !GSPN_MOD1_BUSY_GET(base) && !GSPN_MOD2_BUSY_GET(base)) {
            GSPN_LOGI("core %d done.\n", i);

            /*unbind kcmd and core*/
            core->current_kcmd = NULL;
            core->status = GSPN_CORE_FREE;
            gspn_ctx->core_mng.free_cnt++;
            if(-1 < index && index < GSPN_SPLIT_PARTS_MAX) { // split case
                /*unbind sub_cmd and core*/
                kcmd->sub_cmd_occupied_core[index] = NULL;
                core->current_kcmd_sub_cmd_index = -1;

                /*set kcmd part done, and check all parts done*/
                kcmd->sub_cmd_done_cnt++;
                GSPN_LOGI("%d/%d done.\n", kcmd->sub_cmd_done_cnt, kcmd->sub_cmd_total);
                if(kcmd->sub_cmd_done_cnt < kcmd->sub_cmd_total) {
                    i++;
                    continue;//only part of kcmd done, so kcmd can't put to empty
                }
            } else { /*whole kcmd done*/
                GSPN_LOGI("whole kcmd done.\n");
                GSPN_LOGP("cost %d us.\n", gspn_calc_exe_time(kcmd));
                kcmd->occupied_core = NULL;
            }

            kcmd->done_flag = 1;
            gspn_iommu_unmap(gspn_ctx,kcmd);/*must before gspn disable*/
            GSPN_CoreDisable(core);
            /*sync process*/
            if(kcmd->src_cmd.misc_info.async_flag) {
                /*signal release fence*/
                down(&core->fence_create_sema);
                gspn_ctx->remain_async_cmd_cnt--;
                gspn_sync_fence_signal(core->timeline);
                up(&core->fence_create_sema);
            } else {
                GSPN_LOGI("wake up sync ioctl process, done_cnt:%d.\n", *(kcmd->done_cnt));
                if (kcmd->done_cnt != NULL)
                    (*(kcmd->done_cnt))++;
                wake_up_interruptible_all(&(gspn_ctx->sync_done_wq));
            }

            /*if frame list done, move these kcmds from dissociation list to empty list*/
            gspn_frame_post_process(gspn_ctx,kcmd);
        }

        i++;
    }

}

/*
gspn_core_bind
description: occupy a free core and bind it with a kcmd
*/
static GSPN_ERR_CODE_E gspn_core_bind(GSPN_CORE_T *core, GSPN_KCMD_INFO_T *kcmd)
{
    GSPN_ERR_CODE_E ret = GSPN_NO_ERR;

    if(core == NULL || core->status != GSPN_CORE_FREE) {
        ret = GSPN_K_GET_FREE_CORE_ERR;
        GSPN_LOGE("gspn core bind failed\n");
    } else {
        /*bind core and kcmd*/
        core->status = GSPN_CORE_OCCUPIED;
        core->current_kcmd = kcmd;
        core->current_kcmd_sub_cmd_index = -1;
        kcmd->occupied_core = core;
        kcmd->sub_cmd_done_cnt = 0;
        kcmd->sub_cmd_total = 0;
    }
    return ret;
}

static void gspn_coef_gen_and_cfg(GSPN_CONTEXT_T *gspn_ctx,GSPN_KCMD_INFO_T *kcmd)
{
    uint32_t src_w = 0, src_h = 0;
    uint32_t dst_w = 0,dst_h = 0;
    uint32_t order_v = 0, order_h = 0;
    uint32_t ver_tap, hor_tap;
    uint32_t *ret_coef = NULL;
    int i = 0;
    uint32_t *pSrcCoef  = NULL;
    uint32_t *pDstCoef = NULL;

    if(kcmd->src_cmd.misc_info.scale_en == 1) {
        src_w = kcmd->src_cmd.l0_info.clip_size.w;
        src_h = kcmd->src_cmd.l0_info.clip_size.h;
        if(kcmd->src_cmd.misc_info.run_mod == 0) {
            dst_w = kcmd->src_cmd.des1_info.scale_out_size.w;
            dst_h = kcmd->src_cmd.des1_info.scale_out_size.h;
            if(kcmd->src_cmd.misc_info.scale_seq) {
                src_w = kcmd->src_cmd.des1_info.work_src_size.w;
                src_h = kcmd->src_cmd.des1_info.work_src_size.h;
            }
        } else {
            dst_w = kcmd->src_cmd.des2_info.scale_out_size.w;
            dst_h = kcmd->src_cmd.des2_info.scale_out_size.h;
        }

        switch (kcmd->src_cmd.misc_info.htap4_en)
        {
        case 0:  //8 4
            hor_tap = 8;
            ver_tap = 4;
            break;
        case 1: // 4  4
            hor_tap = 4;
            ver_tap = 4;
            break;
        case 2: // 6 4
            hor_tap = 6;
            ver_tap = 4;
            break;
        case 3: //  2  4
            hor_tap = 2;
            ver_tap = 4;
            break;
        case 4:  // 8  2
            hor_tap = 8;
            ver_tap = 2;
            break;
        case 5: //  4  2
            hor_tap = 4;
            ver_tap = 2;
            break;
        case 6:  // 6  2
            hor_tap = 6;
            ver_tap = 2;
            break;
        case 7:  // 2  2
            hor_tap = 2;
            ver_tap = 2;
            break;
        default:
            hor_tap = 8;
            ver_tap = 4;
            break;
        }

        if(dst_h*4 < src_h && src_h <= dst_h*8) {
            order_v = 1;
        } else if(dst_h*8 < src_h && src_h <= dst_h*16) {
            order_v = 2;
        }
        if(dst_w*4 < src_w && src_w <= dst_w*8) {
            order_h= 1;
        } else if(dst_w*8 < src_w && src_w <= dst_w*16) {
            order_h = 2;
        }
        src_w = src_w >> order_h;
        src_h = src_h >> order_v;
        ret_coef = gspn_gen_block_scaler_coef(gspn_ctx,
                                              src_w, src_h,
                                              kcmd->src_cmd.des1_info.scale_out_size.w,
                                              kcmd->src_cmd.des1_info.scale_out_size.h,
                                              hor_tap, ver_tap);
        GSPN_LOGI("kcmd_addr = 0x%p\n", kcmd);
        /*config the coef to register of the bind core of kcmd*/
        if(kcmd->occupied_core) {
            if((ulong)ret_coef & MEM_OPS_ADDR_ALIGN_MASK) {
                GSPN_LOGI("memcpy use none 8B alignment address!\n");
            }
            GSPN_LOGI("ctl_reg_base = %p, coef_tab = %p, kcmd->occupied_core->ctl_reg_base = %p,"
                    "kcmd->ctl_reg_base->coef_tab = %p\n",
                    gspn_ctx->core_mng.cores->ctl_reg_base,
                    &gspn_ctx->core_mng.cores->ctl_reg_base->coef_tab,
                    kcmd->occupied_core->ctl_reg_base,
                    &kcmd->occupied_core->ctl_reg_base->coef_tab);
            //gsp_memcpy((void*)&(kcmd->occupied_core->ctl_reg_base->coef_tab),(void*)ret_coef,48*4);
            {
                i = 0;
                pSrcCoef  = ret_coef;
                pDstCoef = (uint32_t *)&(kcmd->occupied_core->ctl_reg_base->coef_tab);
                for(i = 0; i < 48 ; i++)
                {
                    *pDstCoef++ = *pSrcCoef++;
                }
            }
        } else {
            GSPN_LOGE("kcmd->occupied_core is NULL, can't get coef register addr!");
        }
    }
}

/*
gspn_reg_cfg
description: only support single core
*/
static GSPN_ERR_CODE_E gspn_reg_cfg(GSPN_CORE_MNG_T *core_mng, GSPN_KCMD_INFO_T *kcmd, GSPN_CAPABILITY_T *capability)
{
    int32_t ret = 0;
    ret = GSPN_InfoCfg(kcmd,-1);
    //GSPN_DumpReg(core_mng, kcmd->occupied_core->core_id);  //hl add for test
    if(ret==3 && capability->version == 0x10) {
        /*on sharklT8, scaling up and down on different direction will rise err flag,
        but case can be executed successfully.*/
    } else if(ret) {
        GSPN_DumpReg(core_mng, kcmd->occupied_core->core_id);
        return (ret < 0) ? GSPN_K_PARAM_CHK_ERR : (GSPN_ERR_CODE_E)ret;
    }
    if(kcmd->occupied_core) {
        kcmd->occupied_core->status = GSPN_CORE_CONFIGURED;
    }
    return GSPN_NO_ERR;
}


static int32_t gspn_wait_fence(GSPN_KCMD_INFO_T *kcmd)
{
    uint32_t i = 0;
    int32_t ret = 0;
    long timeout = GSPN_WAIT_FENCE_TIMEOUT;
    GSPN_LOGI("enter.\n");
    for(i=0; i<kcmd->acq_fence_cnt; i++) {
        ret = sync_fence_wait(kcmd->acq_fence[i],timeout);
        GSPN_LOGI("Wait Fence %d/%d %s.\n",i,kcmd->acq_fence_cnt,ret?"failed":"success");
        if(ret) {
            timeout = 1;//timeout occurs, decrease the next timeout
        }
        sync_fence_put(kcmd->acq_fence[i]);
    }
    GSPN_LOGI("exit.\n");
    return ret;
}


static GSPN_ERR_CODE_E gspn_kcmd_pre_process(GSPN_CONTEXT_T *gspn_ctx)
{
    GSPN_ERR_CODE_E ret = GSPN_NO_ERR;
    GSPN_KCMD_INFO_T *kcmd = NULL;
    GSPN_CORE_T *core = NULL;
    int i = 0;

    //INIT_LIST_HEAD(&gspn_ctx->core_mng.dissociation_list);
    for (i = 0; i <  gspn_ctx->core_mng.core_cnt; i++) {
        core = &gspn_ctx->core_mng.cores[i];
        if(core->fill_list_cnt > 0) {
            if(core->status == GSPN_CORE_FREE) {
                GSPN_LOGI("fetch a kcmd from fill-list.\n");

                /*fetch a kcmd from fill list head, and add it to dissociation list tail*/
                down(&core->fill_list_sema);
                kcmd = list_first_entry(&core->fill_list, GSPN_KCMD_INFO_T, list);
                list_del(&kcmd->list);
                core->fill_list_cnt--;
                up(&core->fill_list_sema);
                list_add_tail(&kcmd->list, &gspn_ctx->core_mng.dissociation_list); //hl change

                ret = gspn_core_bind(core ,kcmd);
                if(ret) {
                    GSPN_LOGE("bind core failed!");
                    return ret;
                } else 
                    gspn_ctx->core_mng.free_cnt--;

                ret = GSPN_CoreEnable(gspn_ctx,kcmd->occupied_core->core_id);
                if(ret) {
                    GSPN_LOGE("core enable failed!");
                    return ret;
                }

                ret = gspn_iommu_map(gspn_ctx,kcmd);
                if(ret) {
                    GSPN_LOGE("iommu map failed!");
                    return ret;
                }

                gspn_coef_gen_and_cfg(gspn_ctx,kcmd);
                ret = gspn_reg_cfg(&gspn_ctx->core_mng,kcmd,&gspn_ctx->capability);
                if(ret) {
                    GSPN_LOGE("reg cfg failed!");
                    return ret;
                }

                if(kcmd->src_cmd.misc_info.async_flag == 1) {
                    ret = gspn_wait_fence(kcmd);
                    if(ret) {
                        GSPN_LOGE("wait fence failed!");
                        return ret;
                    }
                }
                ret = GSPN_Trigger(&gspn_ctx->core_mng,kcmd->occupied_core->core_id);
                if(ret) {
                    GSPN_LOGE("trigger failed!\n");
                    return ret;
                } else {
                    GSPN_LOGI("trigger success!\n");
                    kcmd->occupied_core->status = GSPN_CORE_BUSY;
                    kcmd->occupied_core->current_kcmd_sub_cmd_index = -1;
                }
            }
        }
    }
    return ret;
}


static void gspn_timeout_process(GSPN_CONTEXT_T *gspn_ctx)
{
    struct timespec startTime = {-1UL,-1UL};// the earliest start core
    struct timespec currentTime = {-1UL,-1UL};
    long long elapse = 0;
    int32_t i = 0;

    get_monotonic_boottime(&currentTime);
    i = 0;
    while(i<gspn_ctx->core_mng.core_cnt) {
        if((gspn_ctx->core_mng.cores[i].current_kcmd != NULL/*means module enabled, or register read only get zero*/)
           &&(GSPN_MOD1_BUSY_GET(gspn_ctx->core_mng.cores[i].ctl_reg_base)//(gspn_ctx->core_mng.cores[i].status == GSPN_CORE_BUSY)
              ||GSPN_MOD2_BUSY_GET(gspn_ctx->core_mng.cores[i].ctl_reg_base))) {
            startTime.tv_sec = gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_sec;
            startTime.tv_nsec = gspn_ctx->core_mng.cores[i].current_kcmd->start_time.tv_nsec;

            elapse = (currentTime.tv_sec - startTime.tv_sec)*1000000000 + currentTime.tv_nsec - startTime.tv_nsec;
            if(elapse < 0 || elapse >= MS_TO_NS(GSPN_CMD_EXE_TIMEOUT)) {
                GSPN_LOGE("core%d timeout, try to recovery.\n",i);
                GSPN_DumpReg(&gspn_ctx->core_mng, i);
                /*recovery this core*/
                GSPN_TryToRecover(gspn_ctx, i);
            }
        }
        i++;
    }
}

static void gspn_coef_cache_init(GSPN_CONTEXT_T *gspn_ctx)
{
    uint32_t i = 0;

    GSPN_LOGI("enter!\n");
    if(gspn_ctx->cache_coef_init_flag == 0) {
        i = 0;
        INIT_LIST_HEAD(&gspn_ctx->coef_list);
        while(i < GSPN_COEF_CACHE_MAX) {
            list_add_tail(&gspn_ctx->coef_cache[i].list, &gspn_ctx->coef_list);
            i++;
        }
        gspn_ctx->cache_coef_init_flag = 1;
    }
}

int gspn_work_thread(void *data)
{
    GSPN_CONTEXT_T *gspn_ctx = (GSPN_CONTEXT_T *)data;
    int32_t ret = 0;

    if(gspn_ctx == NULL) {
        GSPN_LOGE("gspn_ctx == NULL, return!\n");
        return GSPN_K_CREATE_THREAD_ERR;
    }
    gspn_coef_cache_init(gspn_ctx);

    while(1) {
        if(gspn_ctx->suspend_flag == 1) {
            gspn_suspend_process(gspn_ctx);
        }
        gspn_recalculate_timeout(gspn_ctx);
        //ret = down_timeout(&gspn_ctx->wake_work_thread_sema, US_TO_JIFFIES(gspn_ctx->timeout));
        ret = down_timeout(&gspn_ctx->wake_work_thread_sema, US_TO_JIFFIES(0xffffffff));
        sema_init(&gspn_ctx->wake_work_thread_sema,0);
        if(ret == -ETIME) { // time out
            gspn_timeout_process(gspn_ctx);
        }
        gspn_kcmd_post_process(gspn_ctx); // free cores before assign job to them
        if(gspn_ctx->suspend_flag == 0) {
            ret = gspn_kcmd_pre_process(gspn_ctx); // get kcmds from filled list and assign them to cores
            if(ret) {
                GSPN_LOGE("pre process err!\n");
                up(&gspn_ctx->wake_work_thread_sema);// leave the err kcmd to post process
            }
        }
    }
    return 0;
}



