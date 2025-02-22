/*
 * drivers/amlogic/amports/video.c
 *
 * Copyright (C) 2015 Amlogic, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
*/

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/fs.h>

#include <linux/string.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/amlogic/major.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/ctype.h>
#include <linux/amlogic/amports/ptsserv.h>
#include <linux/amlogic/amports/timestamp.h>
#include <linux/amlogic/amports/tsync.h>

#include <linux/amlogic/amports/vframe.h>
#include <linux/amlogic/amports/vframe_provider.h>
#include <linux/amlogic/amports/vframe_receiver.h>
#include <linux/amlogic/amports/amstream.h>
#include <linux/amlogic/vout/vout_notify.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/clk.h>
#include <linux/amlogic/gpio-amlogic.h>
#include <linux/amlogic/canvas/canvas.h>
#include <linux/amlogic/canvas/canvas_mgr.h>
#include <linux/dma-mapping.h>
#include <linux/dma-contiguous.h>
#include <linux/switch.h>

#include "amports_priv.h"

#ifdef CONFIG_GE2D_KEEP_FRAME
#include <linux/amlogic/ge2d/ge2d.h>
#include <linux/amlogic/canvas/canvas_mgr.h>
#endif
#if defined(CONFIG_AM_VECM)
#include <linux/amlogic/amvecm/amvecm.h>
#endif
#include "vdec_reg.h"
/* #include <linux/amlogic/ppmgr/ppmgr_status.h> */

#ifdef CONFIG_PM
#include <linux/delay.h>
#include <linux/pm.h>
#endif

#include "arch/register.h"

/* #include <plat/fiq_bridge.h> */
/*
#include <asm/fiq.h>
*/
#include <linux/uaccess.h>

#include "amports_config.h"


/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
#include <linux/amlogic/vpu.h>
/* #endif */
#include "videolog.h"
#ifdef CONFIG_AM_VIDEOCAPTURE
#include "amvideocap_priv.h"
#endif
#ifdef CONFIG_AM_VIDEO_LOG
#define AMLOG
#endif
#include "amlog.h"
MODULE_AMLOG(LOG_LEVEL_ERROR, 0, LOG_DEFAULT_LEVEL_DESC, LOG_MASK_DESC);

#include "vpp.h"
#include "linux/amlogic/tvin/tvin_v4l2.h"
#ifdef CONFIG_VSYNC_RDMA
#define DISPLAY_CANVAS_BASE_INDEX2   0x10
#define DISPLAY_CANVAS_MAX_INDEX2    0x15
#include "rdma.h"
#endif
#include <linux/amlogic/amports/video_prot.h>
#include "video.h"

#include <linux/amlogic/codec_mm/codec_mm.h>
#define MEM_NAME "video-keep"
#ifdef CONFIG_GE2D_KEEP_FRAME
/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */
/* #include <mach/mod_gate.h> */
/* #endif */
/* #include "mach/meson-secure.h" */
#endif

#if 1
/*TODO for logo*/
struct platform_resource_s {
	char name[10];
	int mem_start;
	int mem_end;
};
#endif
static int debugflags;
static int output_fps;
static u32 omx_pts;
static int omx_pts_interval_upper = 11000;
static int omx_pts_interval_lower = -5500;


bool omx_secret_mode = false;
#define DEBUG_FLAG_FFPLAY	(1<<0)
#define DEBUG_FLAG_CALC_PTS_INC	(1<<1)

#define RECEIVER_NAME "amvideo"

static s32 amvideo_poll_major;

static int video_receiver_event_fun(int type, void *data, void *);

static const struct vframe_receiver_op_s video_vf_receiver = {
	.event_cb = video_receiver_event_fun
};

static struct vframe_receiver_s video_vf_recv;

#define RECEIVER4OSD_NAME "amvideo4osd"
static int video4osd_receiver_event_fun(int type, void *data, void *);

static const struct vframe_receiver_op_s video4osd_vf_receiver = {
	.event_cb = video4osd_receiver_event_fun
};

static struct vframe_receiver_s video4osd_vf_recv;

static struct vframe_provider_s *osd_prov;

#define DRIVER_NAME "amvideo"
#define MODULE_NAME "amvideo"
#define DEVICE_NAME "amvideo"

#ifdef CONFIG_AML_VSYNC_FIQ_ENABLE
#define FIQ_VSYNC
#endif

/* #define SLOW_SYNC_REPEAT */
/* #define INTERLACE_FIELD_MATCH_PROCESS */
bool disable_slow_sync = 0;

#ifdef INTERLACE_FIELD_MATCH_PROCESS
#define FIELD_MATCH_THRESHOLD  10
static int field_matching_count;
#endif

#define M_PTS_SMOOTH_MAX 45000
#define M_PTS_SMOOTH_MIN 2250
#define M_PTS_SMOOTH_ADJUST 900
static u32 underflow;
static u32 next_peek_underflow;

#define VIDEO_ENABLE_STATE_IDLE       0
#define VIDEO_ENABLE_STATE_ON_REQ     1
#define VIDEO_ENABLE_STATE_ON_PENDING 2
#define VIDEO_ENABLE_STATE_OFF_REQ    3

static DEFINE_SPINLOCK(video_onoff_lock);
static int video_onoff_state = VIDEO_ENABLE_STATE_IDLE;
static DEFINE_SPINLOCK(video2_onoff_lock);
static int video2_onoff_state = VIDEO_ENABLE_STATE_IDLE;

#ifdef FIQ_VSYNC
#define BRIDGE_IRQ INT_TIMER_C
#define BRIDGE_IRQ_SET() WRITE_CBUS_REG(ISA_TIMERC, 1)
#endif

#define RESERVE_CLR_FRAME

#if 1	/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */

#define VD1_MEM_POWER_ON() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&delay_work_lock, flags); \
		vpu_delay_work_flag &= ~VPU_DELAYWORK_MEM_POWER_OFF_VD1; \
		spin_unlock_irqrestore(&delay_work_lock, flags); \
		switch_vpu_mem_pd_vmod(VPU_VIU_VD1, VPU_MEM_POWER_ON); \
		switch_vpu_mem_pd_vmod(VPU_AFBC_DEC, VPU_MEM_POWER_ON); \
		switch_vpu_mem_pd_vmod(VPU_DI_POST, VPU_MEM_POWER_ON); \
	} while (0)
#define VD2_MEM_POWER_ON() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&delay_work_lock, flags); \
		vpu_delay_work_flag &= ~VPU_DELAYWORK_MEM_POWER_OFF_VD2; \
		spin_unlock_irqrestore(&delay_work_lock, flags); \
		switch_vpu_mem_pd_vmod(VPU_VIU_VD2, VPU_MEM_POWER_ON); \
	} while (0)
#define VD1_MEM_POWER_OFF() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&delay_work_lock, flags); \
		vpu_delay_work_flag |= VPU_DELAYWORK_MEM_POWER_OFF_VD1; \
		vpu_mem_power_off_count = VPU_MEM_POWEROFF_DELAY; \
		spin_unlock_irqrestore(&delay_work_lock, flags); \
	} while (0)
#define VD2_MEM_POWER_OFF() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&delay_work_lock, flags); \
		vpu_delay_work_flag |= VPU_DELAYWORK_MEM_POWER_OFF_VD2; \
		vpu_mem_power_off_count = VPU_MEM_POWEROFF_DELAY; \
		spin_unlock_irqrestore(&delay_work_lock, flags); \
	} while (0)

#if HAS_VPU_PROT
#define PROT_MEM_POWER_ON() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&delay_work_lock, flags); \
		vpu_delay_work_flag &= ~VPU_DELAYWORK_MEM_POWER_OFF_PROT; \
		spin_unlock_irqrestore(&delay_work_lock, flags); \
		switch_vpu_mem_pd_vmod(VPU_PIC_ROT2, VPU_MEM_POWER_ON); \
		switch_vpu_mem_pd_vmod(VPU_PIC_ROT3, VPU_MEM_POWER_ON); \
	} while (0)
#define PROT_MEM_POWER_OFF() \
	do { \
		unsigned long flags; \
		video_prot_gate_off(); \
		spin_lock_irqsave(&delay_work_lock, flags); \
		vpu_delay_work_flag |= VPU_DELAYWORK_MEM_POWER_OFF_PROT; \
		vpu_mem_power_off_count = VPU_MEM_POWEROFF_DELAY; \
		spin_unlock_irqrestore(&delay_work_lock, flags); \
	} while (0)
#else
#define PROT_MEM_POWER_ON()
#define PROT_MEM_POWER_OFF()
#endif
#else
#define VD1_MEM_POWER_ON()
#define VD2_MEM_POWER_ON()
#define PROT_MEM_POWER_ON()
#define VD1_MEM_POWER_OFF()
#define VD2_MEM_POWER_OFF()
#define PROT_MEM_POWER_OFF()
#endif

#define VIDEO_LAYER_ON() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&video_onoff_lock, flags); \
		video_onoff_state = VIDEO_ENABLE_STATE_ON_REQ; \
		video_enabled = 1;\
		spin_unlock_irqrestore(&video_onoff_lock, flags); \
	} while (0)

#define VIDEO_LAYER_OFF() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&video_onoff_lock, flags); \
		video_onoff_state = VIDEO_ENABLE_STATE_OFF_REQ; \
		video_enabled = 0;\
		spin_unlock_irqrestore(&video_onoff_lock, flags); \
	} while (0)

#define VIDEO_LAYER2_ON() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&video2_onoff_lock, flags); \
		video2_onoff_state = VIDEO_ENABLE_STATE_ON_REQ; \
		spin_unlock_irqrestore(&video2_onoff_lock, flags); \
	} while (0)

#define VIDEO_LAYER2_OFF() \
	do { \
		unsigned long flags; \
		spin_lock_irqsave(&video2_onoff_lock, flags); \
		video2_onoff_state = VIDEO_ENABLE_STATE_OFF_REQ; \
		spin_unlock_irqrestore(&video2_onoff_lock, flags); \
	} while (0)

#if HAS_VPU_PROT
#define EnableVideoLayer()  \
	do { \
		if (get_vpu_mem_pd_vmod(VPU_VIU_VD1) == VPU_MEM_POWER_DOWN || \
			get_vpu_mem_pd_vmod(VPU_PIC_ROT2) ==\
				VPU_MEM_POWER_DOWN || \
			READ_VCBUS_REG(VPU_PROT3_CLK_GATE) == 0) { \
			PROT_MEM_POWER_ON(); \
			video_prot_gate_on(); \
			video_prot.video_started = 1; \
			video_prot.angle_changed = 1; \
		} \
		VD1_MEM_POWER_ON(); \
		VIDEO_LAYER_ON(); \
	} while (0)
#else
#define EnableVideoLayer()  \
	do { \
		VD1_MEM_POWER_ON(); \
		VIDEO_LAYER_ON(); \
	} while (0)
#endif
#ifdef TV_3D_FUNCTION_OPEN
#define EnableVideoLayer2()  \
	do { \
		VD2_MEM_POWER_ON(); \
		SET_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off, \
		VPP_VD2_PREBLEND | VPP_PREBLEND_EN | \
		(0x1ff << VPP_VD2_ALPHA_BIT)); \
	} while (0)
#else
#define EnableVideoLayer2()  \
	do { \
		VD2_MEM_POWER_ON(); \
		VIDEO_LAYER2_ON(); \
	} while (0)
#endif
#define VSYNC_EnableVideoLayer2()  \
	do { \
		VD2_MEM_POWER_ON(); \
		VSYNC_WR_MPEG_REG(VPP_MISC + cur_dev->vpp_off, \
		READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off) |\
		VPP_VD2_PREBLEND | (0x1ff << VPP_VD2_ALPHA_BIT)); \
	} while (0)

#define DisableVideoLayer() \
	do { \
		CLEAR_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off, \
		VPP_VD1_PREBLEND|VPP_VD2_PREBLEND|\
		VPP_VD2_POSTBLEND|VPP_VD1_POSTBLEND); \
		VIDEO_LAYER_OFF(); \
		VD1_MEM_POWER_OFF(); \
		PROT_MEM_POWER_OFF(); \
		video_prot.video_started = 0; \
		if (debug_flag & DEBUG_FLAG_BLACKOUT) {  \
			pr_info("DisableVideoLayer()\n"); \
		} \
	} while (0)

#if 1	/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
#define DisableVideoLayer_NoDelay() \
	do { \
		CLEAR_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off, \
		VPP_VD1_PREBLEND|VPP_VD2_PREBLEND|\
		VPP_VD2_POSTBLEND|VPP_VD1_POSTBLEND); \
		if (debug_flag & DEBUG_FLAG_BLACKOUT) {  \
			pr_info("DisableVideoLayer_NoDelay()\n"); \
		} \
	} while (0)
#else
#define DisableVideoLayer_NoDelay() DisableVideoLayer()
#endif
#ifdef TV_3D_FUNCTION_OPEN
#define DisableVideoLayer2() \
	do { \
		CLEAR_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off, \
		VPP_VD2_PREBLEND | VPP_PREBLEND_EN | \
		(0x1ff << VPP_VD2_ALPHA_BIT)); \
		VD2_MEM_POWER_OFF(); \
	} while (0)
#else
#define DisableVideoLayer2() \
	do { \
		CLEAR_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off, \
		VPP_VD2_PREBLEND | (0x1ff << VPP_VD2_ALPHA_BIT)); \
		VD2_MEM_POWER_OFF(); \
	} while (0)
#endif
#define DisableVideoLayer_PREBELEND() \
	do { CLEAR_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off, \
		VPP_VD1_PREBLEND|VPP_VD2_PREBLEND); \
		if (debug_flag & DEBUG_FLAG_BLACKOUT) {  \
			pr_info("DisableVideoLayer_PREBELEND()\n"); \
		} \
	} while (0)

#ifndef CONFIG_AM_VIDEO2
#define DisableVPP2VideoLayer() \
	CLEAR_VCBUS_REG_MASK(VPP2_MISC, \
		VPP_VD1_PREBLEND|VPP_VD2_PREBLEND|\
		VPP_VD2_POSTBLEND|VPP_VD1_POSTBLEND);

#endif
/*********************************************************/
static struct switch_dev video1_state_sdev = {
/* android video layer switch device */
	.name = "video_layer1",
};


#define VOUT_TYPE_TOP_FIELD 0
#define VOUT_TYPE_BOT_FIELD 1
#define VOUT_TYPE_PROG      2

#define VIDEO_DISABLE_NONE    0
#define VIDEO_DISABLE_NORMAL  1
#define VIDEO_DISABLE_FORNEXT 2

#define MAX_ZOOM_RATIO 300

#if 1	/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */

#define VPP_PREBLEND_VD_V_END_LIMIT 2304
#else
#define VPP_PREBLEND_VD_V_END_LIMIT 1080
#endif

#define DUR2PTS(x) ((x) - ((x) >> 4))
#define DUR2PTS_RM(x) ((x) & 0xf)
#define PTS2DUR(x) (((x) << 4) / 15)

#ifdef VIDEO_PTS_CHASE
static int vpts_chase;
static int av_sync_flag;
static int vpts_chase_counter;
static int vpts_chase_pts_diff;
#endif

#define DEBUG_FLAG_BLACKOUT     0x1
#define DEBUG_FLAG_PRINT_TOGGLE_FRAME 0x2
#define DEBUG_FLAG_PRINT_RDMA                0x4
#define DEBUG_FLAG_LOG_RDMA_LINE_MAX         0x100
#define DEBUG_FLAG_TOGGLE_SKIP_KEEP_CURRENT  0x10000
#define DEBUG_FLAG_TOGGLE_FRAME_PER_VSYNC    0x20000
#define DEBUG_FLAG_RDMA_WAIT_1		     0x40000
#define DEBUG_FLAG_VSYNC_DONONE                0x80000
#define DEBUG_FLAG_GOFIELD_MANUL             0x100000
static int debug_flag;

/* DEBUG_FLAG_BLACKOUT; */

static int vsync_enter_line_max;
static int vsync_exit_line_max;

#ifdef CONFIG_VSYNC_RDMA
static int vsync_rdma_line_max;
#endif

static unsigned int process_3d_type;

#ifdef TV_3D_FUNCTION_OPEN
/* toggle_3d_fa_frame is for checking the vpts_expire  in 2 vsnyc */
static int toggle_3d_fa_frame = 1;
/*the pause_one_3d_fl_frame is for close
the A/B register switch in every sync at pause mode. */
static int pause_one_3d_fl_frame;
MODULE_PARM_DESC(pause_one_3d_fl_frame, "\n pause_one_3d_fl_frame\n");
module_param(pause_one_3d_fl_frame, uint, 0664);

enum toggle_out_fl_frame_e {
	OUT_FA_A_FRAME,
	OUT_FA_BANK_FRAME,
	OUT_FA_B_FRAME
};

static unsigned int video_3d_format;
static unsigned int mvc_flag;
static unsigned int force_3d_scaler = 3;
static int mode_3d_changed;
static int last_mode_3d;
#endif

#ifdef TV_REVERSE
bool reverse = false;
#endif

const char video_dev_id[] = "amvideo-dev";

const char video_dev_id2[] = "amvideo-dev2";

int onwaitendframe = 0;
struct video_dev_s {
	int vpp_off;
	int viu_off;
};
struct video_dev_s video_dev[2] = {
	{0x1d00 - 0x1d00, 0x1a00 - 0x1a00},
	{0x1900 - 0x1d00, 0x1e00 - 0x1a00}
};

struct video_dev_s *cur_dev = &video_dev[0];

static int cur_dev_idx;

#ifdef CONFIG_PM
struct video_pm_state_s {
	int event;
	u32 vpp_misc;
	int mem_pd_vd1;
	int mem_pd_vd2;
	int mem_pd_di_post;
	int mem_pd_prot2;
	int mem_pd_prot3;
};

#endif

static DEFINE_MUTEX(video_module_mutex);
static DEFINE_SPINLOCK(lock);
static u32 frame_par_ready_to_set, frame_par_force_to_set;
static u32 vpts_remainder;
static int video_property_changed;
static u32 video_notify_flag;
static int enable_video_discontinue_report = 1;

#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
static u32 video_scaler_mode;
static int content_top = 0, content_left = 0, content_w = 0, content_h;
static int scaler_pos_changed;
#endif
static struct amvideocap_req *capture_frame_req;
static struct video_prot_s video_prot;
static u32 video_angle;
u32 get_video_angle(void)
{
	return video_angle;
}
EXPORT_SYMBOL(get_video_angle);
#if HAS_VPU_PROT
static u32 use_prot;
u32 get_prot_status(void)
{
	return video_prot.status;
}
EXPORT_SYMBOL(get_prot_status);
#endif
static inline ulong keep_phy_addr(unsigned long addr)
{
	return addr;
}
static int free_alloced_keep_buffer(void);
static int alloc_keep_buffer(void);
static int _video_set_disable(u32 val);

int video_property_notify(int flag)
{
	video_property_changed = flag;
	return 0;
}

#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
int video_scaler_notify(int flag)
{
	video_scaler_mode = flag;
	video_property_changed = true;
	return 0;
}

u32 amvideo_get_scaler_para(int *x, int *y, int *w, int *h, u32 *ratio)
{
	*x = content_left;
	*y = content_top;
	*w = content_w;
	*h = content_h;
	/* *ratio = 100; */
	return video_scaler_mode;
}

void amvideo_set_scaler_para(int x, int y, int w, int h, int flag)
{
	mutex_lock(&video_module_mutex);
	if (w < 2)
		w = 0;
	if (h < 2)
		h = 0;
	if (flag) {
		if ((content_left != x) || (content_top != y)
		    || (content_w != w) || (content_h != h))
			scaler_pos_changed = 1;
		content_left = x;
		content_top = y;
		content_w = w;
		content_h = h;
	} else
		vpp_set_video_layer_position(x, y, w, h);
	video_property_changed = true;
	mutex_unlock(&video_module_mutex);
	return;
}

u32 amvideo_get_scaler_mode(void)
{
	return video_scaler_mode;
}
#endif

bool to_notify_trick_wait = false;
/* display canvas */
#define DISPLAY_CANVAS_BASE_INDEX 0x60

#ifdef CONFIG_VSYNC_RDMA
static struct vframe_s *cur_rdma_buf;
/*
void vsync_rdma_config(void);
void vsync_rdma_config_pre(void);
bool is_vsync_rdma_enable(void);
void start_rdma(void);
void enable_rdma_log(int flag);
*/
static int enable_rdma_log_count;

bool rdma_enable_pre = false;

static u32 disp_canvas_index[2][6] = {
	{
	 DISPLAY_CANVAS_BASE_INDEX,
	 DISPLAY_CANVAS_BASE_INDEX + 1,
	 DISPLAY_CANVAS_BASE_INDEX + 2,
	 DISPLAY_CANVAS_BASE_INDEX + 3,
	 DISPLAY_CANVAS_BASE_INDEX + 4,
	 DISPLAY_CANVAS_BASE_INDEX + 5,
	 },
	{
	 DISPLAY_CANVAS_BASE_INDEX2,
	 DISPLAY_CANVAS_BASE_INDEX2 + 1,
	 DISPLAY_CANVAS_BASE_INDEX2 + 2,
	 DISPLAY_CANVAS_BASE_INDEX2 + 3,
	 DISPLAY_CANVAS_BASE_INDEX2 + 4,
	 DISPLAY_CANVAS_BASE_INDEX2 + 5,
	 }
};

static u32 disp_canvas[2][2];
static u32 rdma_canvas_id;
static u32 next_rdma_canvas_id = 1;

#define DISPBUF_TO_PUT_MAX  8
static struct vframe_s *dispbuf_to_put[DISPBUF_TO_PUT_MAX];
static int dispbuf_to_put_num;
#else
static u32 disp_canvas_index[6] = {
	DISPLAY_CANVAS_BASE_INDEX,
	DISPLAY_CANVAS_BASE_INDEX + 1,
	DISPLAY_CANVAS_BASE_INDEX + 2,
	DISPLAY_CANVAS_BASE_INDEX + 3,
	DISPLAY_CANVAS_BASE_INDEX + 4,
	DISPLAY_CANVAS_BASE_INDEX + 5,
};

static u32 disp_canvas[2];
#endif

static u32 post_canvas;


unsigned long keep_y_addr, keep_u_addr, keep_v_addr;
static int keep_video_on;

#define Y_BUFFER_SIZE   0x400000	/* for 1920*1088 */
#define U_BUFFER_SIZE   0x100000	/* compatible with NV21 */
#define V_BUFFER_SIZE   0x80000



/* zoom information */
static u32 zoom_start_x_lines;
static u32 zoom_end_x_lines;
static u32 zoom_start_y_lines;
static u32 zoom_end_y_lines;

static u32 ori_start_x_lines;
static u32 ori_end_x_lines;
static u32 ori_start_y_lines;
static u32 ori_end_y_lines;

/* wide settings */
static u32 wide_setting;

/* black out policy */
#if defined(CONFIG_JPEGLOGO)
static u32 blackout;
#else
static u32 blackout = 1;
#endif
static u32 force_blackout;

/* disable video */
static u32 disable_video = VIDEO_DISABLE_NONE;
static u32 video_enabled;
/* show first frame*/
static bool show_first_frame_nosync;
/* static bool first_frame=false; */

/* test screen*/
static u32 test_screen;

/* video frame repeat count */
static u32 frame_repeat_count;

/* vout */
static const struct vinfo_s *vinfo;

/* config */
static struct vframe_s *cur_dispbuf;
static struct vframe_s vf_local;
static u32 vsync_pts_inc;
static u32 vsync_pts_inc_scale;
static u32 vsync_pts_inc_scale_base = 1;
static u32 vsync_pts_inc_upint;
static u32 vsync_pts_inc_adj;
static u32 vsync_pts_125;
static u32 vsync_pts_112;
static u32 vsync_pts_101;
static u32 vsync_pts_100;
static u32 vsync_freerun;
static u32 vsync_slow_factor = 1;

/* frame rate calculate */
static u32 last_frame_count;
static u32 frame_count;
static u32 new_frame_count;
static u32 last_frame_time;
static u32 timer_count;
static u32 vsync_count;
static struct vpp_frame_par_s *cur_frame_par, *next_frame_par;
static struct vpp_frame_par_s frame_parms[2];

/* vsync pass flag */
static u32 wait_sync;

#ifdef FIQ_VSYNC
static bridge_item_t vsync_fiq_bridge;
#endif

/* trickmode i frame*/
u32 trickmode_i = 0;

/* trickmode ff/fb */
u32 trickmode_fffb = 0;
atomic_t trickmode_framedone = ATOMIC_INIT(0);
atomic_t video_sizechange = ATOMIC_INIT(0);
atomic_t video_unreg_flag = ATOMIC_INIT(0);
atomic_t video_pause_flag = ATOMIC_INIT(0);
int trickmode_duration = 0;
int trickmode_duration_count = 0;
u32 trickmode_vpts = 0;
/* last_playback_filename */
char file_name[512];

/* video freerun mode */
#define FREERUN_NONE    0	/* no freerun mode */
#define FREERUN_NODUR   1	/* freerun without duration */
#define FREERUN_DUR     2	/* freerun with duration */
static u32 freerun_mode;
static u32 slowsync_repeat_enable;

void set_freerun_mode(int mode)
{
	freerun_mode = mode;
}
EXPORT_SYMBOL(set_freerun_mode);

static const enum f2v_vphase_type_e vpp_phase_table[4][3] = {
	{F2V_P2IT, F2V_P2IB, F2V_P2P},	/* VIDTYPE_PROGRESSIVE */
	{F2V_IT2IT, F2V_IT2IB, F2V_IT2P},	/* VIDTYPE_INTERLACE_TOP */
	{F2V_P2IT, F2V_P2IB, F2V_P2P},
	{F2V_IB2IT, F2V_IB2IB, F2V_IB2P}	/* VIDTYPE_INTERLACE_BOTTOM */
};

static const u8 skip_tab[6] = { 0x24, 0x04, 0x68, 0x48, 0x28, 0x08 };

/* wait queue for poll */
static wait_queue_head_t amvideo_trick_wait;

/* wait queue for poll */
static wait_queue_head_t amvideo_sizechange_wait;

#if 1				/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
#define VPU_DELAYWORK_VPU_CLK            1
#define VPU_DELAYWORK_MEM_POWER_OFF_VD1  2
#define VPU_DELAYWORK_MEM_POWER_OFF_VD2  4
#define VPU_DELAYWORK_MEM_POWER_OFF_PROT 8
#define VPU_VIDEO_LAYER1_CHANGED		16

#define VPU_MEM_POWEROFF_DELAY           100
static struct work_struct vpu_delay_work;
static int vpu_clk_level;
static DEFINE_SPINLOCK(delay_work_lock);
static int vpu_delay_work_flag;
static int vpu_mem_power_off_count;
#endif

static u32 vpts_ref;
static u32 video_frame_repeat_count;
static u32 smooth_sync_enable;
static u32 hdmi_in_onvideo;
#ifdef CONFIG_AM_VIDEO2
static int video_play_clone_rate = 60;
static int android_clone_rate = 30;
static int noneseamless_play_clone_rate = 5;
#endif
void safe_disble_videolayer(void)
{
#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
	if (video_scaler_mode)
		DisableVideoLayer_PREBELEND();
	else
		DisableVideoLayer();
#else
	DisableVideoLayer();
#endif
}

#ifdef CONFIG_GE2D_KEEP_FRAME
static int display_canvas_y_dup;
static int display_canvas_u_dup;
static int display_canvas_v_dup;
static struct ge2d_context_s *ge2d_video_context;
static int ge2d_videotask_init(void)
{
	const char *keep_owner = "keepframe";
	if (ge2d_video_context == NULL)
		ge2d_video_context = create_ge2d_work_queue();

	if (ge2d_video_context == NULL) {
		pr_info("create_ge2d_work_queue video task failed\n");
		return -1;
	}
	if (!display_canvas_y_dup)
		display_canvas_y_dup = canvas_pool_map_alloc_canvas(keep_owner);
	if (!display_canvas_u_dup)
		display_canvas_u_dup = canvas_pool_map_alloc_canvas(keep_owner);
	if (!display_canvas_v_dup)
		display_canvas_v_dup = canvas_pool_map_alloc_canvas(keep_owner);
	pr_info("create_ge2d_work_queue video task ok\n");

	return 0;
}

static int ge2d_videotask_release(void)
{
	if (ge2d_video_context) {
		destroy_ge2d_work_queue(ge2d_video_context);
		ge2d_video_context = NULL;
	}
	if (display_canvas_y_dup)
		canvas_pool_map_free_canvas(display_canvas_y_dup);
	if (display_canvas_u_dup)
		canvas_pool_map_free_canvas(display_canvas_u_dup);
	if (display_canvas_v_dup)
		canvas_pool_map_free_canvas(display_canvas_v_dup);

	return 0;
}

static int ge2d_store_frame_YUV444(u32 cur_index)
{
	u32 y_index, des_index, src_index;
	struct canvas_s cs, cd;
	ulong yaddr;
	u32 ydupindex;
	struct config_para_ex_s ge2d_config;
	memset(&ge2d_config, 0, sizeof(struct config_para_ex_s));

	ydupindex = display_canvas_y_dup;

	pr_info("ge2d_store_frame_YUV444 cur_index:s:0x%x\n", cur_index);
	/* pr_info("ge2d_store_frame cur_index:d:0x%x\n", canvas_tab[0]); */
	y_index = cur_index & 0xff;
	canvas_read(y_index, &cs);

	yaddr = keep_phy_addr(keep_y_addr);
	canvas_config(ydupindex,
		      (ulong) yaddr,
		      cs.width, cs.height, CANVAS_ADDR_NOWRAP, cs.blkmode);

	canvas_read(ydupindex, &cd);
	src_index = y_index;
	des_index = ydupindex;

	pr_info("ge2d_canvas_dup ADDR srcy[0x%lx] des[0x%lx]\n", cs.addr,
	       cd.addr);

	ge2d_config.alu_const_color = 0;
	ge2d_config.bitmask_en = 0;
	ge2d_config.src1_gb_alpha = 0;

	ge2d_config.src_planes[0].addr = cs.addr;
	ge2d_config.src_planes[0].w = cs.width;
	ge2d_config.src_planes[0].h = cs.height;

	ge2d_config.dst_planes[0].addr = cd.addr;
	ge2d_config.dst_planes[0].w = cd.width;
	ge2d_config.dst_planes[0].h = cd.height;

	ge2d_config.src_para.canvas_index = src_index;
	ge2d_config.src_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.src_para.format = GE2D_FORMAT_M24_YUV444;
	ge2d_config.src_para.fill_color_en = 0;
	ge2d_config.src_para.fill_mode = 0;
	ge2d_config.src_para.color = 0;
	ge2d_config.src_para.top = 0;
	ge2d_config.src_para.left = 0;
	ge2d_config.src_para.width = cs.width;
	ge2d_config.src_para.height = cs.height;

	ge2d_config.dst_para.canvas_index = des_index;
	ge2d_config.dst_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.dst_para.format = GE2D_FORMAT_M24_YUV444;
	ge2d_config.dst_para.fill_color_en = 0;
	ge2d_config.dst_para.fill_mode = 0;
	ge2d_config.dst_para.color = 0;
	ge2d_config.dst_para.top = 0;
	ge2d_config.dst_para.left = 0;
	ge2d_config.dst_para.width = cs.width;
	ge2d_config.dst_para.height = cs.height;

	if (ge2d_context_config_ex(ge2d_video_context, &ge2d_config) < 0) {
		pr_info("ge2d_context_config_ex failed\n");
		return -1;
	}

	stretchblt_noalpha(ge2d_video_context, 0, 0, cs.width, cs.height,
			   0, 0, cs.width, cs.height);

	return 0;
}

/* static u32 canvas_tab[1]; */
static int ge2d_store_frame_NV21(u32 cur_index)
{
	u32 y_index, u_index, des_index, src_index;
	struct canvas_s cs0, cs1, cd;
	ulong yaddr, uaddr;
	u32 ydupindex, udupindex;
	struct config_para_ex_s ge2d_config;
	memset(&ge2d_config, 0, sizeof(struct config_para_ex_s));

	ydupindex = display_canvas_y_dup;
	udupindex = display_canvas_u_dup;

	pr_info("ge2d_store_frame_NV21 cur_index:s:0x%x\n", cur_index);

	/* pr_info("ge2d_store_frame cur_index:d:0x%x\n", canvas_tab[0]); */
	yaddr = keep_phy_addr(keep_y_addr);
	uaddr = keep_phy_addr(keep_u_addr);

	y_index = cur_index & 0xff;
	u_index = (cur_index >> 8) & 0xff;

	canvas_read(y_index, &cs0);
	canvas_read(u_index, &cs1);
	canvas_config(ydupindex,
		      (ulong) yaddr,
		      cs0.width, cs0.height, CANVAS_ADDR_NOWRAP, cs0.blkmode);
	canvas_config(udupindex,
		      (ulong) uaddr,
		      cs1.width, cs1.height, CANVAS_ADDR_NOWRAP, cs1.blkmode);

	canvas_read(ydupindex, &cd);
	src_index = ((y_index & 0xff) | ((u_index << 8) & 0x0000ff00));
	des_index = ((ydupindex & 0xff) | ((udupindex << 8) & 0x0000ff00));

	pr_info("ge2d_store_frame d:0x%x\n", des_index);

	ge2d_config.alu_const_color = 0;
	ge2d_config.bitmask_en = 0;
	ge2d_config.src1_gb_alpha = 0;

	ge2d_config.src_planes[0].addr = cs0.addr;
	ge2d_config.src_planes[0].w = cs0.width;
	ge2d_config.src_planes[0].h = cs0.height;
	ge2d_config.src_planes[1].addr = cs1.addr;
	ge2d_config.src_planes[1].w = cs1.width;
	ge2d_config.src_planes[1].h = cs1.height;

	ge2d_config.dst_planes[0].addr = cd.addr;
	ge2d_config.dst_planes[0].w = cd.width;
	ge2d_config.dst_planes[0].h = cd.height;

	ge2d_config.src_para.canvas_index = src_index;
	ge2d_config.src_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.src_para.format = GE2D_FORMAT_M24_NV21;
	ge2d_config.src_para.fill_color_en = 0;
	ge2d_config.src_para.fill_mode = 0;
	ge2d_config.src_para.color = 0;
	ge2d_config.src_para.top = 0;
	ge2d_config.src_para.left = 0;
	ge2d_config.src_para.width = cs0.width;
	ge2d_config.src_para.height = cs0.height;

	ge2d_config.dst_para.canvas_index = des_index;
	ge2d_config.dst_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.dst_para.format = GE2D_FORMAT_M24_NV21;
	ge2d_config.dst_para.fill_color_en = 0;
	ge2d_config.dst_para.fill_mode = 0;
	ge2d_config.dst_para.color = 0;
	ge2d_config.dst_para.top = 0;
	ge2d_config.dst_para.left = 0;
	ge2d_config.dst_para.width = cs0.width;
	ge2d_config.dst_para.height = cs0.height;

	if (ge2d_context_config_ex(ge2d_video_context, &ge2d_config) < 0) {
		pr_info("ge2d_context_config_ex failed\n");
		return -1;
	}

	stretchblt_noalpha(ge2d_video_context, 0, 0, cs0.width, cs0.height,
			   0, 0, cs0.width, cs0.height);

	return 0;
}

/* static u32 canvas_tab[1]; */
static int ge2d_store_frame_YUV420(u32 cur_index)
{
	u32 y_index, u_index, v_index;
	struct canvas_s cs, cd;
	ulong yaddr, uaddr, vaddr;
	u32 ydupindex, udupindex, vdupindex;
	struct config_para_ex_s ge2d_config;
	memset(&ge2d_config, 0, sizeof(struct config_para_ex_s));

	ydupindex = display_canvas_y_dup;
	udupindex = display_canvas_u_dup;
	vdupindex = display_canvas_v_dup;

	pr_info("ge2d_store_frame_YUV420 cur_index:s:0x%x\n", cur_index);
	/* operation top line */
	/* Y data */
	ge2d_config.alu_const_color = 0;
	ge2d_config.bitmask_en = 0;
	ge2d_config.src1_gb_alpha = 0;

	y_index = cur_index & 0xff;
	canvas_read(y_index, &cs);
	ge2d_config.src_planes[0].addr = cs.addr;
	ge2d_config.src_planes[0].w = cs.width;
	ge2d_config.src_planes[0].h = cs.height;
	ge2d_config.src_planes[1].addr = 0;
	ge2d_config.src_planes[1].w = 0;
	ge2d_config.src_planes[1].h = 0;
	ge2d_config.src_planes[2].addr = 0;
	ge2d_config.src_planes[2].w = 0;
	ge2d_config.src_planes[2].h = 0;

	yaddr = keep_phy_addr(keep_y_addr);
	canvas_config(ydupindex,
		      (ulong) yaddr,
		      cs.width, cs.height, CANVAS_ADDR_NOWRAP, cs.blkmode);
	canvas_read(ydupindex, &cd);
	ge2d_config.dst_planes[0].addr = cd.addr;
	ge2d_config.dst_planes[0].w = cd.width;
	ge2d_config.dst_planes[0].h = cd.height;
	ge2d_config.dst_planes[1].addr = 0;
	ge2d_config.dst_planes[1].w = 0;
	ge2d_config.dst_planes[1].h = 0;
	ge2d_config.dst_planes[2].addr = 0;
	ge2d_config.dst_planes[2].w = 0;
	ge2d_config.dst_planes[2].h = 0;

	ge2d_config.src_key.key_enable = 0;
	ge2d_config.src_key.key_mask = 0;
	ge2d_config.src_key.key_mode = 0;
	ge2d_config.src_key.key_color = 0;

	ge2d_config.src_para.canvas_index = y_index;
	ge2d_config.src_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.src_para.format = GE2D_FMT_S8_Y;
	ge2d_config.src_para.fill_color_en = 0;
	ge2d_config.src_para.fill_mode = 0;
	ge2d_config.src_para.x_rev = 0;
	ge2d_config.src_para.y_rev = 0;
	ge2d_config.src_para.color = 0;
	ge2d_config.src_para.top = 0;
	ge2d_config.src_para.left = 0;
	ge2d_config.src_para.width = cs.width;
	ge2d_config.src_para.height = cs.height;

	ge2d_config.dst_para.canvas_index = ydupindex;
	ge2d_config.dst_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.dst_para.format = GE2D_FMT_S8_Y;
	ge2d_config.dst_para.fill_color_en = 0;
	ge2d_config.dst_para.fill_mode = 0;
	ge2d_config.dst_para.x_rev = 0;
	ge2d_config.dst_para.y_rev = 0;
	ge2d_config.dst_xy_swap = 0;
	ge2d_config.dst_para.color = 0;
	ge2d_config.dst_para.top = 0;
	ge2d_config.dst_para.left = 0;
	ge2d_config.dst_para.width = cs.width;
	ge2d_config.dst_para.height = cs.height;

	if (ge2d_context_config_ex(ge2d_video_context, &ge2d_config) < 0) {
		pr_info("++ge2d configing error.\n");
		return -1;
	}
	stretchblt_noalpha(ge2d_video_context, 0, 0, cs.width, cs.height, 0, 0,
			   cs.width, cs.height);

	/* U data */
	ge2d_config.alu_const_color = 0;
	ge2d_config.bitmask_en = 0;
	ge2d_config.src1_gb_alpha = 0;

	u_index = (cur_index >> 8) & 0xff;
	canvas_read(u_index, &cs);
	ge2d_config.src_planes[0].addr = cs.addr;
	ge2d_config.src_planes[0].w = cs.width;
	ge2d_config.src_planes[0].h = cs.height;
	ge2d_config.src_planes[1].addr = 0;
	ge2d_config.src_planes[1].w = 0;
	ge2d_config.src_planes[1].h = 0;
	ge2d_config.src_planes[2].addr = 0;
	ge2d_config.src_planes[2].w = 0;
	ge2d_config.src_planes[2].h = 0;

	uaddr = keep_phy_addr(keep_u_addr);
	canvas_config(udupindex,
		      (ulong) uaddr,
		      cs.width, cs.height, CANVAS_ADDR_NOWRAP, cs.blkmode);
	canvas_read(udupindex, &cd);
	ge2d_config.dst_planes[0].addr = cd.addr;
	ge2d_config.dst_planes[0].w = cd.width;
	ge2d_config.dst_planes[0].h = cd.height;
	ge2d_config.dst_planes[1].addr = 0;
	ge2d_config.dst_planes[1].w = 0;
	ge2d_config.dst_planes[1].h = 0;
	ge2d_config.dst_planes[2].addr = 0;
	ge2d_config.dst_planes[2].w = 0;
	ge2d_config.dst_planes[2].h = 0;

	ge2d_config.src_key.key_enable = 0;
	ge2d_config.src_key.key_mask = 0;
	ge2d_config.src_key.key_mode = 0;
	ge2d_config.src_key.key_color = 0;

	ge2d_config.src_para.canvas_index = u_index;
	ge2d_config.src_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.src_para.format = GE2D_FMT_S8_CB;
	ge2d_config.src_para.fill_color_en = 0;
	ge2d_config.src_para.fill_mode = 0;
	ge2d_config.src_para.x_rev = 0;
	ge2d_config.src_para.y_rev = 0;
	ge2d_config.src_para.color = 0;
	ge2d_config.src_para.top = 0;
	ge2d_config.src_para.left = 0;
	ge2d_config.src_para.width = cs.width;
	ge2d_config.src_para.height = cs.height;

	ge2d_config.dst_para.canvas_index = udupindex;
	ge2d_config.dst_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.dst_para.format = GE2D_FMT_S8_CB;
	ge2d_config.dst_para.fill_color_en = 0;
	ge2d_config.dst_para.fill_mode = 0;
	ge2d_config.dst_para.x_rev = 0;
	ge2d_config.dst_para.y_rev = 0;
	ge2d_config.dst_xy_swap = 0;
	ge2d_config.dst_para.color = 0;
	ge2d_config.dst_para.top = 0;
	ge2d_config.dst_para.left = 0;
	ge2d_config.dst_para.width = cs.width;
	ge2d_config.dst_para.height = cs.height;

	if (ge2d_context_config_ex(ge2d_video_context, &ge2d_config) < 0) {
		pr_info("++ge2d configing error.\n");
		return -1;
	}
	stretchblt_noalpha(ge2d_video_context, 0, 0, cs.width, cs.height, 0, 0,
			   cs.width, cs.height);

	/* operation top line */
	/* V data */
	ge2d_config.alu_const_color = 0;
	ge2d_config.bitmask_en = 0;
	ge2d_config.src1_gb_alpha = 0;

	v_index = (cur_index >> 16) & 0xff;
	canvas_read(v_index, &cs);
	ge2d_config.src_planes[0].addr = cs.addr;
	ge2d_config.src_planes[0].w = cs.width;
	ge2d_config.src_planes[0].h = cs.height;
	ge2d_config.src_planes[1].addr = 0;
	ge2d_config.src_planes[1].w = 0;
	ge2d_config.src_planes[1].h = 0;
	ge2d_config.src_planes[2].addr = 0;
	ge2d_config.src_planes[2].w = 0;
	ge2d_config.src_planes[2].h = 0;

	vaddr = keep_phy_addr(keep_v_addr);
	canvas_config(vdupindex,
		      (ulong) vaddr,
		      cs.width, cs.height, CANVAS_ADDR_NOWRAP, cs.blkmode);
	ge2d_config.dst_planes[0].addr = cd.addr;
	ge2d_config.dst_planes[0].w = cd.width;
	ge2d_config.dst_planes[0].h = cd.height;
	ge2d_config.dst_planes[1].addr = 0;
	ge2d_config.dst_planes[1].w = 0;
	ge2d_config.dst_planes[1].h = 0;
	ge2d_config.dst_planes[2].addr = 0;
	ge2d_config.dst_planes[2].w = 0;
	ge2d_config.dst_planes[2].h = 0;

	ge2d_config.src_key.key_enable = 0;
	ge2d_config.src_key.key_mask = 0;
	ge2d_config.src_key.key_mode = 0;
	ge2d_config.src_key.key_color = 0;

	ge2d_config.src_para.canvas_index = v_index;
	ge2d_config.src_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.src_para.format = GE2D_FMT_S8_CR;
	ge2d_config.src_para.fill_color_en = 0;
	ge2d_config.src_para.fill_mode = 0;
	ge2d_config.src_para.x_rev = 0;
	ge2d_config.src_para.y_rev = 0;
	ge2d_config.src_para.color = 0;
	ge2d_config.src_para.top = 0;
	ge2d_config.src_para.left = 0;
	ge2d_config.src_para.width = cs.width;
	ge2d_config.src_para.height = cs.height;

	ge2d_config.dst_para.canvas_index = vdupindex;
	ge2d_config.dst_para.mem_type = CANVAS_TYPE_INVALID;
	ge2d_config.dst_para.format = GE2D_FMT_S8_CR;
	ge2d_config.dst_para.fill_color_en = 0;
	ge2d_config.dst_para.fill_mode = 0;
	ge2d_config.dst_para.x_rev = 0;
	ge2d_config.dst_para.y_rev = 0;
	ge2d_config.dst_xy_swap = 0;
	ge2d_config.dst_para.color = 0;
	ge2d_config.dst_para.top = 0;
	ge2d_config.dst_para.left = 0;
	ge2d_config.dst_para.width = cs.width;
	ge2d_config.dst_para.height = cs.height;

	if (ge2d_context_config_ex(ge2d_video_context, &ge2d_config) < 0) {
		pr_info("++ge2d configing error.\n");
		return -1;
	}
	stretchblt_noalpha(ge2d_video_context, 0, 0, cs.width, cs.height, 0, 0,
			   cs.width, cs.height);
	return 0;
}

static void ge2d_keeplastframe_block(int cur_index, int format)
{
	/* u32 cur_index; */
	u32 y_index, u_index, v_index;
#ifdef CONFIG_VSYNC_RDMA
	u32 y_index2, u_index2, v_index2;
#endif

	mutex_lock(&video_module_mutex);

#ifdef CONFIG_VSYNC_RDMA
	y_index = disp_canvas_index[0][0];
	y_index2 = disp_canvas_index[1][0];
	u_index = disp_canvas_index[0][1];
	u_index2 = disp_canvas_index[1][1];
	v_index = disp_canvas_index[0][2];
	v_index2 = disp_canvas_index[1][2];
#else
	/* cur_index = READ_VCBUS_REG(VD1_IF0_CANVAS0 + cur_dev->viu_off); */
	y_index = cur_index & 0xff;
	u_index = (cur_index >> 8) & 0xff;
	v_index = (cur_index >> 16) & 0xff;
#endif

	switch (format) {
	case GE2D_FORMAT_M24_YUV444:
		ge2d_store_frame_YUV444(cur_index);
		canvas_update_addr(y_index, keep_phy_addr(keep_y_addr));
#ifdef CONFIG_VSYNC_RDMA
		canvas_update_addr(y_index2, keep_phy_addr(keep_y_addr));
#endif
		break;
	case GE2D_FORMAT_M24_NV21:
		ge2d_store_frame_NV21(cur_index);
		canvas_update_addr(y_index, keep_phy_addr(keep_y_addr));
		canvas_update_addr(u_index, keep_phy_addr(keep_u_addr));
#ifdef CONFIG_VSYNC_RDMA
		canvas_update_addr(y_index2, keep_phy_addr(keep_y_addr));
		canvas_update_addr(u_index2, keep_phy_addr(keep_u_addr));
#endif
		break;
	case GE2D_FORMAT_M24_YUV420:
		ge2d_store_frame_YUV420(cur_index);
		canvas_update_addr(y_index, keep_phy_addr(keep_y_addr));
		canvas_update_addr(u_index, keep_phy_addr(keep_u_addr));
		canvas_update_addr(v_index, keep_phy_addr(keep_v_addr));
#ifdef CONFIG_VSYNC_RDMA
		canvas_update_addr(y_index2, keep_phy_addr(keep_y_addr));
		canvas_update_addr(u_index2, keep_phy_addr(keep_u_addr));
		canvas_update_addr(v_index2, keep_phy_addr(keep_v_addr));
#endif
		break;
	default:
		break;
	}
	mutex_unlock(&video_module_mutex);

}

#endif

/*********************************************************/
static inline struct vframe_s *video_vf_peek(void)
{
	return vf_peek(RECEIVER_NAME);
}

static inline struct vframe_s *video_vf_get(void)
{
	struct vframe_s *vf = NULL;
	vf = vf_get(RECEIVER_NAME);

	if (vf) {
		video_notify_flag |= VIDEO_NOTIFY_PROVIDER_GET;
		atomic_set(&vf->use_cnt, 1);
		/*always to 1,for first get from vfm provider */

#ifdef TV_3D_FUNCTION_OPEN
		/*can be moved to h264mvc.c */
		if ((vf->type & VIDTYPE_MVC)
		    && (process_3d_type & MODE_3D_ENABLE) && vf->trans_fmt) {
			vf->type = VIDTYPE_PROGRESSIVE | VIDTYPE_VIU_FIELD;
			process_3d_type |= MODE_3D_MVC;
			mvc_flag = 1;
		} else {
			process_3d_type &= (~MODE_3D_MVC);
			mvc_flag = 0;
		}
#endif
	}
	return vf;

}

static int vf_get_states(struct vframe_states *states)
{
	int ret = -1;
	unsigned long flags;
	struct vframe_provider_s *vfp;
	vfp = vf_get_provider(RECEIVER_NAME);
	spin_lock_irqsave(&lock, flags);
	if (vfp && vfp->ops && vfp->ops->vf_states)
		ret = vfp->ops->vf_states(states, vfp->op_arg);
	spin_unlock_irqrestore(&lock, flags);
	return ret;
}

static inline void video_vf_put(struct vframe_s *vf)
{
	struct vframe_provider_s *vfp = vf_get_provider(RECEIVER_NAME);
	if (vfp && atomic_dec_and_test(&vf->use_cnt)) {
		vf_put(vf, RECEIVER_NAME);
		video_notify_flag |= VIDEO_NOTIFY_PROVIDER_PUT;
	}
}

int ext_get_cur_video_frame(struct vframe_s **vf, int *canvas_index)
{
	if (cur_dispbuf == NULL)
		return -1;
	atomic_inc(&cur_dispbuf->use_cnt);
	*canvas_index = READ_VCBUS_REG(VD1_IF0_CANVAS0 + cur_dev->viu_off);
	*vf = cur_dispbuf;
	return 0;
}

int ext_put_video_frame(struct vframe_s *vf)
{
	if (vf == &vf_local)
		return 0;
	video_vf_put(vf);
	return 0;
}

int ext_register_end_frame_callback(struct amvideocap_req *req)
{
	mutex_lock(&video_module_mutex);
	capture_frame_req = req;
	mutex_unlock(&video_module_mutex);
	return 0;
}

#ifdef CONFIG_AM_VIDEOCAPTURE
static int ext_frame_capture_poll(int endflags)
{
	mutex_lock(&video_module_mutex);
	if (capture_frame_req && capture_frame_req->callback) {
		struct vframe_s *vf;
		int index;
		int ret;
		struct amvideocap_req *req = capture_frame_req;
		ret = ext_get_cur_video_frame(&vf, &index);
		if (!ret) {
			req->callback(req->data, vf, index);
			capture_frame_req = NULL;
		}
	}
	mutex_unlock(&video_module_mutex);
	return 0;
}
#endif
static void vpp_settings_h(struct vpp_frame_par_s *framePtr)
{
	struct vppfilter_mode_s *vpp_filter = &framePtr->vpp_filter;
	u32 r1, r2, r3;
#ifdef TV_3D_FUNCTION_OPEN
	u32 x_lines;
#endif
	r1 = framePtr->VPP_hsc_linear_startp - framePtr->VPP_hsc_startp;
	r2 = framePtr->VPP_hsc_linear_endp - framePtr->VPP_hsc_startp;
	r3 = framePtr->VPP_hsc_endp - framePtr->VPP_hsc_startp;
#ifdef SUPER_SCALER_OPEN
	if (framePtr->supscl_path == sup0_pp_sp1_scpath)
		r3 >>= framePtr->supsc1_hori_ratio;
#endif
#ifdef TV_3D_FUNCTION_OPEN
	x_lines = zoom_end_x_lines / (framePtr->hscale_skip_count + 1);
	if (process_3d_type & MODE_3D_OUT_TB) {
		/* vd1 and vd2 do pre blend */
		VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_H_START_END,
				((zoom_start_x_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | (((zoom_end_x_lines) &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_BLEND_VD2_H_START_END,
				((zoom_start_x_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | (((zoom_end_x_lines) &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_H_START_END +
				cur_dev->vpp_off,
				((framePtr->VPP_hsc_startp & VPP_VD_SIZE_MASK)
				<< VPP_VD1_START_BIT) |
				((framePtr->VPP_hsc_endp & VPP_VD_SIZE_MASK)
				<< VPP_VD1_END_BIT));
	} else if (process_3d_type & MODE_3D_OUT_LR) {
		/* vd1 and vd2 do pre blend */
		VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_H_START_END,
				((zoom_start_x_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | (((x_lines >> 1) &
				VPP_VD_SIZE_MASK) <<
				VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_BLEND_VD2_H_START_END,
				((((x_lines + 1) >> 1) & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | ((x_lines &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_H_START_END +
				cur_dev->vpp_off,
				((framePtr->VPP_hsc_startp & VPP_VD_SIZE_MASK)
				<< VPP_VD1_START_BIT) |
				((framePtr->VPP_hsc_endp & VPP_VD_SIZE_MASK)
				<< VPP_VD1_END_BIT));
	} else
#endif
	{

		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_H_START_END +
				cur_dev->vpp_off,
				((framePtr->VPP_hsc_startp & VPP_VD_SIZE_MASK)
				<< VPP_VD1_START_BIT) |
				((framePtr->VPP_hsc_endp & VPP_VD_SIZE_MASK)
				<< VPP_VD1_END_BIT));

		VSYNC_WR_MPEG_REG(VPP_BLEND_VD2_H_START_END + cur_dev->vpp_off,
			((framePtr->VPP_hd_start_lines_ &
			VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT) |
			((framePtr->VPP_hd_end_lines_ &
			VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
	}
	VSYNC_WR_MPEG_REG(VPP_HSC_REGION12_STARTP + cur_dev->vpp_off,
			(0 << VPP_REGION1_BIT) |
			((r1 & VPP_REGION_MASK) << VPP_REGION2_BIT));

	VSYNC_WR_MPEG_REG(VPP_HSC_REGION34_STARTP + cur_dev->vpp_off,
			((r2 & VPP_REGION_MASK) << VPP_REGION3_BIT) |
			((r3 & VPP_REGION_MASK) << VPP_REGION4_BIT));
	VSYNC_WR_MPEG_REG(VPP_HSC_REGION4_ENDP + cur_dev->vpp_off, r3);

	VSYNC_WR_MPEG_REG(VPP_HSC_START_PHASE_STEP + cur_dev->vpp_off,
			vpp_filter->vpp_hf_start_phase_step);

	VSYNC_WR_MPEG_REG(VPP_HSC_REGION1_PHASE_SLOPE + cur_dev->vpp_off,
			vpp_filter->vpp_hf_start_phase_slope);

	VSYNC_WR_MPEG_REG(VPP_HSC_REGION3_PHASE_SLOPE + cur_dev->vpp_off,
			vpp_filter->vpp_hf_end_phase_slope);

	VSYNC_WR_MPEG_REG(VPP_LINE_IN_LENGTH + cur_dev->vpp_off,
			framePtr->VPP_line_in_length_);
	VSYNC_WR_MPEG_REG(VPP_PREBLEND_H_SIZE + cur_dev->vpp_off,
			framePtr->VPP_line_in_length_);
}

static void vpp_settings_v(struct vpp_frame_par_s *framePtr)
{
	struct vppfilter_mode_s *vpp_filter = &framePtr->vpp_filter;
	u32 r, afbc_enble_flag;
#ifdef TV_3D_FUNCTION_OPEN
	u32 y_lines;
#endif
	r = framePtr->VPP_vsc_endp - framePtr->VPP_vsc_startp;
	afbc_enble_flag = 0;
	if (is_meson_gxbb_cpu())
		afbc_enble_flag = READ_VCBUS_REG(AFBC_ENABLE) & 0x100;
	if ((vpp_filter->vpp_vsc_start_phase_step > 0x1000000)
		&& afbc_enble_flag)
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_V_START_END +
			cur_dev->vpp_off, ((framePtr->VPP_vsc_startp &
			VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT)
			| (((framePtr->VPP_vsc_endp + 1) & VPP_VD_SIZE_MASK) <<
			VPP_VD1_END_BIT));
	else
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_V_START_END +
			cur_dev->vpp_off, ((framePtr->VPP_vsc_startp &
			VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT)
			| ((framePtr->VPP_vsc_endp & VPP_VD_SIZE_MASK) <<
			VPP_VD1_END_BIT));

#ifdef TV_3D_FUNCTION_OPEN
	y_lines = zoom_end_y_lines / (framePtr->vscale_skip_count + 1);
	if (process_3d_type & MODE_3D_OUT_TB) {
		VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_V_START_END,
				((zoom_start_y_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | (((y_lines >> 1) &
				VPP_VD_SIZE_MASK) <<
				VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(
				VPP_BLEND_VD2_V_START_END,
				((((y_lines + 1) >> 1) & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) |
				((y_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_END_BIT));
	} else if (process_3d_type & MODE_3D_OUT_LR) {
		VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_V_START_END,
				((zoom_start_y_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | ((zoom_end_y_lines &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_BLEND_VD2_V_START_END,
				((zoom_start_y_lines & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | ((zoom_end_y_lines &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
	} else
#endif
	{
		if ((framePtr->VPP_post_blend_vd_v_end_ -
			framePtr->VPP_post_blend_vd_v_start_ + 1) >
			VPP_PREBLEND_VD_V_END_LIMIT) {
			VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_V_START_END +
				cur_dev->vpp_off,
				((framePtr->VPP_post_blend_vd_v_start_
				& VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) |
				((framePtr->VPP_post_blend_vd_v_end_ &
					VPP_VD_SIZE_MASK)
				<< VPP_VD1_END_BIT));
		} else {
			VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_V_START_END +
				cur_dev->vpp_off,
				((0 & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) |
				(((VPP_PREBLEND_VD_V_END_LIMIT - 1) &
				VPP_VD_SIZE_MASK) <<
				VPP_VD1_END_BIT));
		}
		VSYNC_WR_MPEG_REG(VPP_BLEND_VD2_V_START_END + cur_dev->vpp_off,
				(((framePtr->VPP_vd_end_lines_ / 2) &
				VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT) |
				(((framePtr->VPP_vd_end_lines_) &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
	}
	VSYNC_WR_MPEG_REG(VPP_VSC_REGION12_STARTP + cur_dev->vpp_off, 0);
	VSYNC_WR_MPEG_REG(VPP_VSC_REGION34_STARTP + cur_dev->vpp_off,
			  ((r & VPP_REGION_MASK) << VPP_REGION3_BIT) |
			  ((r & VPP_REGION_MASK) << VPP_REGION4_BIT));
#ifdef SUPER_SCALER_OPEN
	if (framePtr->supscl_path == sup0_pp_sp1_scpath)
		r >>= framePtr->supsc1_vert_ratio;
#endif
	VSYNC_WR_MPEG_REG(VPP_VSC_REGION4_ENDP + cur_dev->vpp_off, r);

	VSYNC_WR_MPEG_REG(VPP_VSC_START_PHASE_STEP + cur_dev->vpp_off,
			vpp_filter->vpp_vsc_start_phase_step);
}

#ifdef TV_3D_FUNCTION_OPEN

static void zoom_get_horz_pos(struct vframe_s *vf, u32 vpp_3d_mode, u32 *ls,
			      u32 *le, u32 *rs, u32 *re)
{
	u32 crop_sx, crop_ex, crop_sy, crop_ey;
	vpp_get_video_source_crop(&crop_sy, &crop_sx, &crop_ey, &crop_ex);

	switch (vpp_3d_mode) {
	case VPP_3D_MODE_LR:
		/*half width,double height */
		*ls = zoom_start_x_lines;
		*le = zoom_end_x_lines;
		*rs = *ls + (vf->width >> 1);
		*re = *le + (vf->width >> 1);
		if (process_3d_type & MODE_3D_OUT_LR) {
			*ls = zoom_start_x_lines;
			*le = zoom_end_x_lines >> 1;
			*rs = *ls + (vf->width >> 1);
			*re = *le + (vf->width >> 1);
		}
		break;
	case VPP_3D_MODE_TB:
	case VPP_3D_MODE_LA:
	case VPP_3D_MODE_FA:
	default:
		if (vf->trans_fmt == TVIN_TFMT_3D_FP) {
			*ls = vf->left_eye.start_x + crop_sx;
			*le = vf->left_eye.start_x + vf->left_eye.width -
				crop_ex - 1;
			*rs = vf->right_eye.start_x + crop_sx;
			*re = vf->right_eye.start_x + vf->right_eye.width -
				crop_ex - 1;
		} else if (process_3d_type & MODE_3D_OUT_LR) {
			*ls = zoom_start_x_lines;
			*le = zoom_end_x_lines >> 1;
			*rs = *ls;
			*re = *le;
			/* *rs = *ls + (vf->width); */
			/* *re = *le + (vf->width); */
		} else {
			*ls = *rs = zoom_start_x_lines;
			*le = *re = zoom_end_x_lines;
		}
		break;
	}

	return;
}

static void zoom_get_vert_pos(struct vframe_s *vf, u32 vpp_3d_mode, u32 *ls,
			u32 *le, u32 *rs, u32 *re)
{
	u32 crop_sx, crop_ex, crop_sy, crop_ey, height;
	vpp_get_video_source_crop(&crop_sy, &crop_sx, &crop_ey, &crop_ex);

	if (vf->type & VIDTYPE_INTERLACE)
		height = vf->height >> 1;
	else
		height = vf->height;

	switch (vpp_3d_mode) {
	case VPP_3D_MODE_TB:
		if (vf->trans_fmt == TVIN_TFMT_3D_FP) {
			if (vf->type & VIDTYPE_INTERLACE) {
				/*if input is interlace vertical
				crop will be reduce by half */
				*ls =
				    (vf->left_eye.start_y +
				     (crop_sy >> 1)) >> 1;
				*le =
				    ((vf->left_eye.start_y +
				      vf->left_eye.height -
				      (crop_ey >> 1)) >> 1) - 1;
				*rs =
				    (vf->right_eye.start_y +
				     (crop_sy >> 1)) >> 1;
				*re =
				    ((vf->right_eye.start_y +
				      vf->left_eye.height -
				      (crop_ey >> 1)) >> 1) - 1;
			} else {
				*ls = vf->left_eye.start_y + (crop_sy >> 1);
				*le = vf->left_eye.start_y +
					vf->left_eye.height -
					(crop_ey >> 1) - 1;
				*rs = vf->right_eye.start_y + (crop_sy >> 1);
				*re =
				    vf->right_eye.start_y +
				    vf->left_eye.height - (crop_ey >> 1) - 1;
			}
		} else {
			if ((vf->type & VIDTYPE_VIU_FIELD)
			    && (vf->type & VIDTYPE_INTERLACE)) {
				*ls = zoom_start_y_lines >> 1;
				*le = zoom_end_y_lines >> 1;
				*rs = *ls + (height >> 1);
				*re = *le + (height >> 1);

			} else if (vf->type & VIDTYPE_INTERLACE) {
				*ls = zoom_start_y_lines >> 1;
				*le = zoom_end_y_lines >> 1;
				*rs = *ls + height;
				*re = *le + height;

			} else {
				/* same width,same height */
				*ls = zoom_start_y_lines >> 1;
				*le = zoom_end_y_lines >> 1;
				*rs = *ls + (height >> 1);
				*re = *le + (height >> 1);
			}
		}
		if ((process_3d_type & MODE_3D_TO_2D_MASK)
		    || (process_3d_type & MODE_3D_OUT_LR)) {
			/* same width,half height */
			*ls = zoom_start_y_lines;
			*le = zoom_end_y_lines;
			*rs = zoom_start_y_lines + (height >> 1);
			*re = zoom_end_y_lines + (height >> 1);
		}
		break;
	case VPP_3D_MODE_LR:
		/* half width,double height */
		*ls = *rs = zoom_start_y_lines >> 1;
		*le = *re = zoom_end_y_lines >> 1;
		if ((process_3d_type & MODE_3D_TO_2D_MASK)
		    || (process_3d_type & MODE_3D_OUT_LR)) {
			/*half width ,same height */
			*ls = *rs = zoom_start_y_lines;
			*le = *re = zoom_end_y_lines;
		}
		break;
	case VPP_3D_MODE_FA:
		/*same width same heiht */
		if ((process_3d_type & MODE_3D_TO_2D_MASK)
		    || (process_3d_type & MODE_3D_OUT_LR)) {
			*ls = *rs = zoom_start_y_lines;
			*le = *re = zoom_end_y_lines;
		} else {
			*ls = *rs = (zoom_start_y_lines + crop_sy) >> 1;
			*le = *re = (zoom_end_y_lines + crop_ey) >> 1;
		}
		break;
	case VPP_3D_MODE_LA:
		*ls = *rs = zoom_start_y_lines;
		if ((process_3d_type & MODE_3D_LR_SWITCH)
		    || (process_3d_type & MODE_3D_TO_2D_R))
			*ls = *rs = zoom_start_y_lines + 1;
		if (process_3d_type & MODE_3D_TO_2D_L)
			*ls = *rs = zoom_start_y_lines;
		*le = *re = zoom_end_y_lines;
		if ((process_3d_type & MODE_3D_OUT_FA_MASK)
		    || (process_3d_type & MODE_3D_OUT_TB)
		    || (process_3d_type & MODE_3D_OUT_LR)) {
			*rs = zoom_start_y_lines + 1;
			*ls = zoom_start_y_lines;
			/* *le = zoom_end_y_lines; */
			/* *re = zoom_end_y_lines; */
		}
		break;
	default:
		*ls = *rs = zoom_start_y_lines;
		*le = *re = zoom_end_y_lines;
		break;
	}

	return;
}

#endif
static void zoom_display_horz(int hscale)
{
	u32 ls, le, rs, re;
#ifdef TV_3D_FUNCTION_OPEN
	if (process_3d_type & MODE_3D_ENABLE) {
		zoom_get_horz_pos(cur_dispbuf, cur_frame_par->vpp_3d_mode, &ls,
				  &le, &rs, &re);
	} else {
		ls = rs = zoom_start_x_lines;
		le = re = zoom_end_x_lines;
	}
#else
	ls = rs = zoom_start_x_lines;
	le = re = zoom_end_x_lines;
#endif
	VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_X0 + cur_dev->viu_off,
			  (ls << VDIF_PIC_START_BIT) |
			  (le << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_X0 + cur_dev->viu_off,
			  (ls / 2 << VDIF_PIC_START_BIT) |
			  (le / 2 << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_X1 + cur_dev->viu_off,
			  (rs << VDIF_PIC_START_BIT) |
			  (re << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_X1 + cur_dev->viu_off,
			  (rs / 2 << VDIF_PIC_START_BIT) |
			  (re / 2 << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VIU_VD1_FMT_W + cur_dev->viu_off,
			  (((zoom_end_x_lines - zoom_start_x_lines +
			     1) >> hscale) << VD1_FMT_LUMA_WIDTH_BIT) |
			  (((zoom_end_x_lines / 2 - zoom_start_x_lines / 2 +
			     1) >> hscale) << VD1_FMT_CHROMA_WIDTH_BIT));

	if (get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB) {
		int l_aligned;
		int r_aligned;
		if (zoom_start_x_lines > 0) {
			l_aligned = round_down(ori_start_x_lines, 32);
			r_aligned = round_up(ori_end_x_lines, 32);
		} else {
			l_aligned = round_down(zoom_start_x_lines, 32);
			r_aligned = round_up(zoom_end_x_lines, 32);
		}
		VSYNC_WR_MPEG_REG(AFBC_VD_CFMT_W,
			  ((r_aligned - l_aligned) << 16) |
			  (r_aligned / 2 - l_aligned / 2));

		VSYNC_WR_MPEG_REG(AFBC_MIF_HOR_SCOPE,
			  ((l_aligned / 32) << 16) |
			  ((r_aligned / 32) - 1));


		VSYNC_WR_MPEG_REG(AFBC_PIXEL_HOR_SCOPE,
			  ((zoom_start_x_lines - l_aligned) << 16) |
			  (zoom_end_x_lines - l_aligned));

		VSYNC_WR_MPEG_REG(AFBC_SIZE_IN,
			  (VSYNC_RD_MPEG_REG(AFBC_SIZE_IN) & 0xffff) |
			  ((r_aligned - l_aligned) << 16));
	}

	VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_X0,
			  (ls << VDIF_PIC_START_BIT) |
			  (le << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_X0,
			  (ls / 2 << VDIF_PIC_START_BIT) |
			  (le / 2 << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_X1,
			  (rs << VDIF_PIC_START_BIT) |
			  (re << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_X1,
			  (rs / 2 << VDIF_PIC_START_BIT) |
			  (re / 2 << VDIF_PIC_END_BIT));

	VSYNC_WR_MPEG_REG(VIU_VD2_FMT_W + cur_dev->viu_off,
			  (((zoom_end_x_lines - zoom_start_x_lines +
			     1) >> hscale) << VD1_FMT_LUMA_WIDTH_BIT) |
			  (((zoom_end_x_lines / 2 - zoom_start_x_lines / 2 +
			     1) >> hscale) << VD1_FMT_CHROMA_WIDTH_BIT));
}

static void zoom_display_vert(void)
{

	u32 ls, le, rs, re;
#ifdef TV_3D_FUNCTION_OPEN

	if (process_3d_type & MODE_3D_ENABLE) {
		zoom_get_vert_pos(cur_dispbuf, cur_frame_par->vpp_3d_mode, &ls,
				  &le, &rs, &re);
	} else {
		ls = rs = zoom_start_y_lines;
		le = re = zoom_end_y_lines;
	}
#else
	ls = rs = zoom_start_y_lines;
	le = re = zoom_end_y_lines;

#endif

	if ((cur_dispbuf) && (cur_dispbuf->type & VIDTYPE_MVC)) {
		VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_Y0 + cur_dev->viu_off,
				(ls * 2 << VDIF_PIC_START_BIT) |
				(le * 2 << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_Y0 + cur_dev->viu_off,
				((ls) << VDIF_PIC_START_BIT) |
				((le) << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_Y0,
				(ls * 2 << VDIF_PIC_START_BIT) |
				(le * 2 << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_Y0,
				((ls) << VDIF_PIC_START_BIT) |
				((le) << VDIF_PIC_END_BIT));
	} else {
		VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_Y0 + cur_dev->viu_off,
				(ls << VDIF_PIC_START_BIT) |
				(le << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_Y0 + cur_dev->viu_off,
				((ls / 2) << VDIF_PIC_START_BIT) |
				((le / 2) << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_Y1 + cur_dev->viu_off,
				(rs << VDIF_PIC_START_BIT) |
				(re << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_Y1 + cur_dev->viu_off,
				((rs / 2) << VDIF_PIC_START_BIT) |
				((re / 2) << VDIF_PIC_END_BIT));
#ifdef TV_3D_FUNCTION_OPEN
		/* vd2 */
		VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_Y0,
				(ls << VDIF_PIC_START_BIT) |
				(le << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_Y0,
				((ls / 2) << VDIF_PIC_START_BIT) |
				((le / 2) << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_Y1,
				(rs << VDIF_PIC_START_BIT) |
				(re << VDIF_PIC_END_BIT));

		VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_Y1,
				((rs / 2) << VDIF_PIC_START_BIT) |
				((re / 2) << VDIF_PIC_END_BIT));
#endif
	}

	if (get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB) {
		int t_aligned;
		int b_aligned;
		if (zoom_start_y_lines > 0) {
			t_aligned = round_down(zoom_start_y_lines, 4);
			b_aligned = round_up(zoom_end_y_lines, 4);
		} else {
			t_aligned = round_down(zoom_start_y_lines, 4);
			b_aligned = round_up(zoom_end_y_lines, 4);
		}
		VSYNC_WR_MPEG_REG(AFBC_VD_CFMT_H,
		    b_aligned - t_aligned);

		VSYNC_WR_MPEG_REG(AFBC_MIF_VER_SCOPE,
		    ((t_aligned / 4) << 16) |
		    ((b_aligned / 4) - 1));

		VSYNC_WR_MPEG_REG(AFBC_PIXEL_VER_SCOPE,
		    ((zoom_start_y_lines - t_aligned) << 16) |
		    (zoom_end_y_lines - t_aligned));

		VSYNC_WR_MPEG_REG(AFBC_SIZE_IN,
		    (VSYNC_RD_MPEG_REG(AFBC_SIZE_IN) & 0xffff0000) |
		    (b_aligned - t_aligned));
	}
}

#ifdef TV_3D_FUNCTION_OPEN
/* judge the out mode is 240:LBRBLRBR  or 120:LRLRLR */
static void judge_3d_fa_out_mode(void)
{
	if ((process_3d_type & MODE_3D_OUT_FA_MASK)
	    && pause_one_3d_fl_frame == 2)
		toggle_3d_fa_frame = OUT_FA_B_FRAME;
	else if ((process_3d_type & MODE_3D_OUT_FA_MASK)
		 && pause_one_3d_fl_frame == 1)
		toggle_3d_fa_frame = OUT_FA_A_FRAME;
	else if ((process_3d_type & MODE_3D_OUT_FA_MASK)
		 && pause_one_3d_fl_frame == 0) {
		/* toggle_3d_fa_frame  determine
		the out frame is L or R or blank */
		if ((process_3d_type & MODE_3D_OUT_FA_L_FIRST)) {
			if (0 == vsync_count % 2)
				toggle_3d_fa_frame = OUT_FA_A_FRAME;
			else
				toggle_3d_fa_frame = OUT_FA_B_FRAME;
		} else if ((process_3d_type & MODE_3D_OUT_FA_R_FIRST)) {
			if (0 == vsync_count % 2)
				toggle_3d_fa_frame = OUT_FA_B_FRAME;
			else
				toggle_3d_fa_frame = OUT_FA_A_FRAME;
		} else if ((process_3d_type & MODE_3D_OUT_FA_LB_FIRST)) {
			if (0 == vsync_count % 4)
				toggle_3d_fa_frame = OUT_FA_A_FRAME;
			else if (2 == vsync_count % 4)
				toggle_3d_fa_frame = OUT_FA_B_FRAME;
			else
				toggle_3d_fa_frame = OUT_FA_BANK_FRAME;
		} else if ((process_3d_type & MODE_3D_OUT_FA_RB_FIRST)) {
			if (0 == vsync_count % 4)
				toggle_3d_fa_frame = OUT_FA_B_FRAME;
			else if (2 == vsync_count % 4)
				toggle_3d_fa_frame = OUT_FA_A_FRAME;
			else
				toggle_3d_fa_frame = OUT_FA_BANK_FRAME;
		}
	} else
		toggle_3d_fa_frame = OUT_FA_A_FRAME;
}

#endif

u32 property_changed_true = 0;
static void vsync_toggle_frame(struct vframe_s *vf)
{
	u32 first_picture = 0;
	unsigned long flags;
	frame_count++;
	ori_start_x_lines = 0;
	ori_end_x_lines = vf->width - 1;
	ori_start_y_lines = 0;
	ori_end_y_lines = vf->height - 1;
	if (debug_flag & DEBUG_FLAG_PRINT_TOGGLE_FRAME)
		pr_info("%s()\n", __func__);

	if (trickmode_i || trickmode_fffb)
		trickmode_duration_count = trickmode_duration;

	if (vf->early_process_fun) {
		if (vf->early_process_fun(vf->private_data, vf) == 1) {
			/* video_property_changed = true; */
			first_picture = 1;
		}
	} else {
		if (READ_VCBUS_REG(DI_IF1_GEN_REG) & 0x1) {
			/* disable post di */
			VSYNC_WR_MPEG_REG(DI_POST_CTRL, 0x3 << 30);
			VSYNC_WR_MPEG_REG(DI_POST_SIZE,
					  (32 - 1) | ((128 - 1) << 16));
			VSYNC_WR_MPEG_REG(DI_IF1_GEN_REG,
					  READ_VCBUS_REG(DI_IF1_GEN_REG) &
					  0xfffffffe);
		}
	}

	timer_count = 0;
	if ((vf->width == 0) && (vf->height == 0)) {
		amlog_level(LOG_LEVEL_ERROR,
			    "Video: invalid frame dimension\n");
		return;
	}
	if ((cur_dispbuf) && (cur_dispbuf != &vf_local) && (cur_dispbuf != vf)
	    && (video_property_changed != 2)) {
		if (cur_dispbuf->source_type == VFRAME_SOURCE_TYPE_OSD) {
			if (osd_prov && osd_prov->ops && osd_prov->ops->put) {
				osd_prov->ops->put(cur_dispbuf,
						   osd_prov->op_arg);
				if (debug_flag & DEBUG_FLAG_BLACKOUT) {
					pr_info(
					"[v4o]pre vf is osd,put it\n");
				}
			}
			first_picture = 1;
			if (debug_flag & DEBUG_FLAG_BLACKOUT) {
				pr_info(
				"[v4o] pre vf is osd, clear it to NULL\n");
			}
		} else {
			new_frame_count++;
#ifdef CONFIG_VSYNC_RDMA
			if (is_vsync_rdma_enable()) {
#ifdef RDMA_RECYCLE_ORDERED_VFRAMES
				if (dispbuf_to_put_num < DISPBUF_TO_PUT_MAX) {
					dispbuf_to_put[dispbuf_to_put_num] =
					    cur_dispbuf;
					dispbuf_to_put_num++;
				} else
					video_vf_put(cur_dispbuf);
#else
				if (cur_rdma_buf == cur_dispbuf) {
					dispbuf_to_put[0] = cur_dispbuf;
					dispbuf_to_put_num = 1;
				} else
					video_vf_put(cur_dispbuf);
#endif
			} else {
				int i;
				for (i = 0; i < dispbuf_to_put_num; i++) {
					if (dispbuf_to_put[i]) {
						video_vf_put(
							dispbuf_to_put[i]);
						dispbuf_to_put[i] = NULL;
					}
					dispbuf_to_put_num = 0;
				}
				video_vf_put(cur_dispbuf);
			}
#else
			video_vf_put(cur_dispbuf);
#endif
		}

	} else
		first_picture = 1;

	if (video_property_changed) {
		property_changed_true = 2;
		video_property_changed = false;
		first_picture = 1;
	}
	if (property_changed_true > 0) {
		property_changed_true--;
		first_picture = 1;
	}

	if (debug_flag & DEBUG_FLAG_BLACKOUT) {
		if (first_picture) {
			pr_info
			    ("[video4osd] first %s picture {%d,%d} pts:%x,\n",
			     (vf->source_type ==
			      VFRAME_SOURCE_TYPE_OSD) ? "OSD" : "", vf->width,
			     vf->height, vf->pts);
		}
	}
	/* switch buffer */
	post_canvas = vf->canvas0Addr;

	if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB) &&
		(vf->type & VIDTYPE_COMPRESS)) {
		VSYNC_WR_MPEG_REG(AFBC_HEAD_BADDR, vf->canvas0Addr>>4);
		VSYNC_WR_MPEG_REG(AFBC_BODY_BADDR, vf->canvas1Addr>>4);
	} else if ((VSYNC_RD_MPEG_REG(DI_IF1_GEN_REG) & 0x1) == 0) {
#ifdef CONFIG_VSYNC_RDMA
		canvas_copy(vf->canvas0Addr & 0xff,
			    disp_canvas_index[rdma_canvas_id][0]);
		canvas_copy((vf->canvas0Addr >> 8) & 0xff,
			    disp_canvas_index[rdma_canvas_id][1]);
		canvas_copy((vf->canvas0Addr >> 16) & 0xff,
			    disp_canvas_index[rdma_canvas_id][2]);
		canvas_copy(vf->canvas1Addr & 0xff,
			    disp_canvas_index[rdma_canvas_id][3]);
		canvas_copy((vf->canvas1Addr >> 8) & 0xff,
			    disp_canvas_index[rdma_canvas_id][4]);
		canvas_copy((vf->canvas1Addr >> 16) & 0xff,
			    disp_canvas_index[rdma_canvas_id][5]);

		VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[rdma_canvas_id][0]);
#ifndef TV_3D_FUNCTION_OPEN
		VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS1 + cur_dev->viu_off,
				  disp_canvas[rdma_canvas_id][0]);
		VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[rdma_canvas_id][1]);
		VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 + cur_dev->viu_off,
				  disp_canvas[rdma_canvas_id][1]);
#else
		VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[rdma_canvas_id][0]);
		if (cur_frame_par && (cur_frame_par->vpp_2pic_mode == 1)) {
			VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[rdma_canvas_id][0]);
			VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[rdma_canvas_id][0]);
		} else {
			VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[rdma_canvas_id][1]);
			VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[rdma_canvas_id][1]);
		}
#endif
		/* VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1,
		disp_canvas[rdma_canvas_id][1]); */
		next_rdma_canvas_id = rdma_canvas_id ? 0 : 1;
#if HAS_VPU_PROT
		if (has_vpu_prot()) {
			if (use_prot) {
				video_prot.prot2_canvas =
				    disp_canvas[rdma_canvas_id][0] & 0xff;
				video_prot.prot3_canvas =
				    (disp_canvas[rdma_canvas_id][0] >> 8) &
				    0xff;
				VSYNC_WR_MPEG_REG_BITS(VPU_PROT2_DDR,
						       video_prot.prot2_canvas,
						       0, 8);
				VSYNC_WR_MPEG_REG_BITS(VPU_PROT3_DDR,
						       video_prot.prot3_canvas,
						       0, 8);
			}
		}
#endif
#else
		canvas_copy(vf->canvas0Addr & 0xff, disp_canvas_index[0]);
		canvas_copy((vf->canvas0Addr >> 8) & 0xff,
			    disp_canvas_index[1]);
		canvas_copy((vf->canvas0Addr >> 16) & 0xff,
			    disp_canvas_index[2]);
		canvas_copy(vf->canvas1Addr & 0xff, disp_canvas_index[3]);
		canvas_copy((vf->canvas1Addr >> 8) & 0xff,
			    disp_canvas_index[4]);
		canvas_copy((vf->canvas1Addr >> 16) & 0xff,
			    disp_canvas_index[5]);
#ifndef TV_3D_FUNCTION_OPEN
		VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[0]);
		VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS1 + cur_dev->viu_off,
				  disp_canvas[0]);
		VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[1]);
		VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 + cur_dev->viu_off,
				  disp_canvas[1]);
#else
		VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[0]);
		VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS0 + cur_dev->viu_off,
				  disp_canvas[0]);
		if (cur_frame_par && (cur_frame_par->vpp_2pic_mode == 1)) {
			VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[0]);
			VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[0]);
		} else {
			VSYNC_WR_MPEG_REG(VD1_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[1]);
			VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 + cur_dev->viu_off,
					  disp_canvas[1]);
		}
		/* VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS0 +
		cur_dev->viu_off, disp_canvas[0]); */
		/* VSYNC_WR_MPEG_REG(VD2_IF0_CANVAS1 +
		cur_dev->viu_off, disp_canvas[1]); */
#endif
#if HAS_VPU_PROT
		if (has_vpu_prot()) {
			if (use_prot) {
				video_prot.prot2_canvas = disp_canvas_index[0];
				video_prot.prot3_canvas = disp_canvas_index[1];
				VSYNC_WR_MPEG_REG_BITS(VPU_PROT2_DDR,
						       video_prot.prot2_canvas,
						       0, 8);
				VSYNC_WR_MPEG_REG_BITS(VPU_PROT3_DDR,
						       video_prot.prot3_canvas,
						       0, 8);
			}
		}
#endif
#endif
	}
	/* set video PTS */
	if (cur_dispbuf != vf) {
		if (vf->source_type != VFRAME_SOURCE_TYPE_OSD) {
			if (vf->pts != 0) {
				amlog_mask(LOG_MASK_TIMESTAMP,
				"vpts to: 0x%x, scr: 0x%x, abs_scr: 0x%x\n",
					   vf->pts, timestamp_pcrscr_get(),
					   READ_MPEG_REG(SCR_HIU));

				timestamp_vpts_set(vf->pts);
			} else if (cur_dispbuf) {
				amlog_mask(LOG_MASK_TIMESTAMP,
				"vpts inc: 0x%x, scr: 0x%x, abs_scr: 0x%x\n",
					   timestamp_vpts_get() +
					   DUR2PTS(cur_dispbuf->duration),
					   timestamp_pcrscr_get(),
					   READ_MPEG_REG(SCR_HIU));

				timestamp_vpts_inc(DUR2PTS
						   (cur_dispbuf->duration));

				vpts_remainder +=
				    DUR2PTS_RM(cur_dispbuf->duration);
				if (vpts_remainder >= 0xf) {
					vpts_remainder -= 0xf;
					timestamp_vpts_inc(-1);
				}
			}
		} else {
			first_picture = 1;
			if (debug_flag & DEBUG_FLAG_BLACKOUT) {
				pr_info(
				"[v4o] cur vframe is osd, do not set PTS\n");
			}
		}
		vf->type_backup = vf->type;
	}

	/* enable new config on the new frames */
	if ((first_picture) ||
	    (cur_dispbuf->bufWidth != vf->bufWidth) ||
	    (cur_dispbuf->width != vf->width) ||
	    (cur_dispbuf->height != vf->height) ||
#ifdef TV_3D_FUNCTION_OPEN
	    ((process_3d_type & MODE_3D_AUTO) &&
	     (cur_dispbuf->trans_fmt != vf->trans_fmt)) ||
#endif
	    (cur_dispbuf->ratio_control != vf->ratio_control) ||
	    ((cur_dispbuf->type_backup & VIDTYPE_INTERLACE) !=
	     (vf->type_backup & VIDTYPE_INTERLACE)) ||
	    (cur_dispbuf->type != vf->type)
#if HAS_VPU_PROT
	    || (cur_dispbuf->video_angle != vf->video_angle)
	    || video_prot.angle_changed
#endif
	    ) {
		atomic_inc(&video_sizechange);
		wake_up_interruptible(&amvideo_sizechange_wait);
		amlog_mask(LOG_MASK_FRAMEINFO,
			   "%s %dx%d  ar=0x%x\n",
			   ((vf->type & VIDTYPE_TYPEMASK) ==
			    VIDTYPE_INTERLACE_TOP) ? "interlace-top"
			   : ((vf->type & VIDTYPE_TYPEMASK)
			      == VIDTYPE_INTERLACE_BOTTOM)
			   ? "interlace-bottom" : "progressive", vf->width,
			   vf->height, vf->ratio_control);
#ifdef TV_3D_FUNCTION_OPEN
		amlog_mask(LOG_MASK_FRAMEINFO,
			   "%s trans_fmt=%u\n", __func__, vf->trans_fmt);

#endif
		next_frame_par = (&frame_parms[0] == next_frame_par) ?
		    &frame_parms[1] : &frame_parms[0];
#if HAS_VPU_PROT
		if (has_vpu_prot()) {
			if (use_prot) {
				struct vframe_s tmp_vf = *vf;
				video_prot.angle = vf->video_angle;
				if ((first_picture) || video_prot.angle_changed
				    || (cur_dispbuf->video_angle !=
					vf->video_angle
					|| cur_dispbuf->width != vf->width
					|| cur_dispbuf->height != vf->height)) {
					u32 angle_orientation = 0;
					video_prot_init(&video_prot, &tmp_vf);
					angle_orientation = vf->video_angle;
					video_prot_set_angle(&video_prot,
							     angle_orientation);
					video_prot.angle = angle_orientation;
					video_prot.status =
					    angle_orientation % 2;
					video_prot.angle_changed = 0;
					if ((debug_flag & DEBUG_FLAG_BLACKOUT)
							&& cur_dispbuf) {
						pr_info(
						"C.w:%d c.h:%d-v.w:%dv.h:%d\n",
						cur_dispbuf->width,
						cur_dispbuf->height,
						vf->width,
						vf->height);
					}
				}
				video_prot_revert_vframe(&video_prot, &tmp_vf);
				if (video_prot.status) {
					static struct vpp_frame_par_s
					    prot_parms;
					static vpp_frame_par_t *next =
							next_frame_par;
				    u32 tmp_line_in_length_ =
						    next->VPP_hd_end_lines_ -
						    next->VPP_hd_start_lines_
						    + 1;
					u32 tmp_pic_in_height_ =
						    next->VPP_vd_end_lines_
						    -
						    next->VPP_vd_start_lines_
						    + 1;
					prot_get_parameter(wide_setting,
							   &tmp_vf, &prot_parms,
							   vinfo);
					video_prot_axis(&video_prot,
					prot_parms.VPP_hd_start_lines_,
					prot_parms.VPP_hd_end_lines_,
					prot_parms.VPP_vd_start_lines_,
					prot_parms.VPP_vd_end_lines_);
					vpp_set_filters(process_3d_type,
					wide_setting, &tmp_vf,
						next_frame_par, vinfo);

					if (tmp_line_in_length_ <
					    tmp_vf.width) {
						next->VPP_line_in_length_
						=
						tmp_line_in_length_ /
						(next->hscale_skip_count
						+ 1);
						next->VPP_hd_start_lines_;
						next->VPP_hf_ini_phase_ =
						0;
						next->VPP_hd_end_lines_
						=
						tmp_line_in_length_
						- 1;
					}
					if (tmp_pic_in_height_ <
					    tmp_vf.height) {
						next->VPP_pic_in_height_
						=
						tmp_pic_in_height_ /
						(next->vscale_skip_count
						+ 1);
						next->VPP_vd_start_lines_;
						next->VPP_hf_ini_phase_ =
						0;
						next->VPP_vd_end_lines_
						=
						tmp_pic_in_height_ -
						1;
					}
				} else {
					vpp_set_filters(process_3d_type,
						wide_setting, vf,
						next_frame_par, vinfo);
				}

			} else {
				video_prot.angle_changed = 0;
				vpp_set_filters(process_3d_type, wide_setting,
						vf, next_frame_par, vinfo);
			}
		} else
#else
		{
			vpp_set_filters(process_3d_type, wide_setting, vf,
					next_frame_par, vinfo);
		}
#endif

		/* apply new vpp settings */
		frame_par_ready_to_set = 1;

		/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
		if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
		    && !is_meson_mtvd_cpu()) {
			if ((vf->width > 1920) && (vf->height > 1088)) {
				if (vpu_clk_level == 0) {
					vpu_clk_level = 1;

					spin_lock_irqsave(&lock, flags);
					vpu_delay_work_flag |=
					    VPU_DELAYWORK_VPU_CLK;
					spin_unlock_irqrestore(&lock, flags);
				}
			} else {
				if (vpu_clk_level == 1) {
					vpu_clk_level = 0;

					spin_lock_irqsave(&lock, flags);
					vpu_delay_work_flag |=
					    VPU_DELAYWORK_VPU_CLK;
					spin_unlock_irqrestore(&lock, flags);
				}
			}
		}
		/* #endif */

	}

	if (((vf->type & VIDTYPE_NO_VIDEO_ENABLE) == 0) &&
	    ((!property_changed_true) || (vf != cur_dispbuf))) {
		if (disable_video == VIDEO_DISABLE_FORNEXT) {
			EnableVideoLayer();
			disable_video = VIDEO_DISABLE_NONE;
		}
		if (first_picture && (disable_video != VIDEO_DISABLE_NORMAL)) {
			EnableVideoLayer();

			if (vf->type & VIDTYPE_MVC)
				EnableVideoLayer2();
		}
	}

	cur_dispbuf = vf;
	if (keep_video_on && cur_dispbuf != &vf_local) {
		pr_info("toggle new frame after keep.\n");
		keep_video_on = 0;
	}
	if (first_picture) {
		frame_par_ready_to_set = 1;

#ifdef VIDEO_PTS_CHASE
		av_sync_flag = 0;
#endif
	}
}

static void viu_set_dcu(struct vpp_frame_par_s *frame_par, struct vframe_s *vf)
{
	u32 r;
	u32 vphase, vini_phase;
	u32 pat, loop;
	static const u32 vpat[] = { 0, 0x8, 0x9, 0xa, 0xb, 0xc };
	u32 u, v;

	if (get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB) {
		if (vf->type & VIDTYPE_COMPRESS) {
			r = (3 << 24) |
			    (17 << 16) |
			    (1 << 14) | /*burst1 1*/
			    (vf->bitdepth & BITDEPTH_MASK);

			if (frame_par->hscale_skip_count)
				r |= 0x33;
			if (frame_par->vscale_skip_count)
				r |= 0xcc;
#ifdef TV_REVERSE
			if (reverse)
				r |= (1<<26)|(1<<27);
#endif
			if (vf->bitdepth & BITDEPTH_SAVING_MODE)
				r |= (1<<28); /* mem_saving_mode */
			VSYNC_WR_MPEG_REG(AFBC_MODE, r);
			VSYNC_WR_MPEG_REG(AFBC_ENABLE, 0x1700);
			VSYNC_WR_MPEG_REG(AFBC_CONV_CTRL, 0x100);
			u = (vf->bitdepth >> (BITDEPTH_U_SHIFT)) & 0x3;
			v = (vf->bitdepth >> (BITDEPTH_V_SHIFT)) & 0x3;
			VSYNC_WR_MPEG_REG(AFBC_DEC_DEF_COLOR,
				0x3FF00000 | /*Y,bit20+*/
				0x80 << (u + 10) |
				0x80 << v);
			/* chroma formatter */
			VSYNC_WR_MPEG_REG(AFBC_VD_CFMT_CTRL,
				HFORMATTER_RRT_PIXEL0 |
				HFORMATTER_YC_RATIO_2_1 |
				HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN |
				/*(0xa << VFORMATTER_INIPHASE_BIT) |*/
				(0x8 << VFORMATTER_PHASE_BIT) |
				VFORMATTER_EN);
			VSYNC_WR_MPEG_REG_BITS(VIU_MISC_CTRL0 +
					cur_dev->viu_off, 1, 20, 1);
			return;

		} else {
			VSYNC_WR_MPEG_REG_BITS(VIU_MISC_CTRL0 +
					cur_dev->viu_off, 0, 20, 1);
			VSYNC_WR_MPEG_REG(AFBC_ENABLE, 0);
		}
	}

	r = (3 << VDIF_URGENT_BIT) |
	    (17 << VDIF_HOLD_LINES_BIT) |
	    VDIF_FORMAT_SPLIT |
	    VDIF_CHRO_RPT_LAST | VDIF_ENABLE;
	/*  | VDIF_RESET_ON_GO_FIELD;*/
	if (debug_flag & DEBUG_FLAG_GOFIELD_MANUL)
		r |= 1<<7; /*for manul triggle gofiled.*/

	if ((vf->type & VIDTYPE_VIU_SINGLE_PLANE) == 0)
		r |= VDIF_SEPARATE_EN;
	else {
		if (vf->type & VIDTYPE_VIU_422)
			r |= VDIF_FORMAT_422;
		else {
			r |= VDIF_FORMAT_RGB888_YUV444 |
			    VDIF_DEMUX_MODE_RGB_444;
		}
	}
#if HAS_VPU_PROT
	if (has_vpu_prot()) {
		if (video_prot.status && use_prot) {
			r |= VDIF_DEMUX_MODE | VDIF_LAST_LINE | 3 <<
			    VDIF_BURSTSIZE_Y_BIT | 1 << VDIF_BURSTSIZE_CB_BIT |
			    1 << VDIF_BURSTSIZE_CR_BIT;
			r &= 0xffffffbf;
		}
	}
#endif
	if (frame_par->hscale_skip_count)
		r |= VDIF_CHROMA_HZ_AVG | VDIF_LUMA_HZ_AVG;

	VSYNC_WR_MPEG_REG(VD1_IF0_GEN_REG + cur_dev->viu_off, r);
	VSYNC_WR_MPEG_REG(VD2_IF0_GEN_REG, r);

	/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */
	if (get_cpu_type() >= MESON_CPU_MAJOR_ID_M6) {
		if (vf->type & VIDTYPE_VIU_NV21) {
			VSYNC_WR_MPEG_REG_BITS(VD1_IF0_GEN_REG2 +
				cur_dev->viu_off, 1, 0, 1);
		} else {
			VSYNC_WR_MPEG_REG_BITS(VD1_IF0_GEN_REG2 +
				cur_dev->viu_off, 0, 0, 1);
		}
#if HAS_VPU_PROT
		if (has_vpu_prot()) {
			if (use_prot) {
				if (vf->video_angle == 2) {
					VSYNC_WR_MPEG_REG_BITS(VD1_IF0_GEN_REG2
						+
					cur_dev->viu_off,
						0xf, 2, 4);
				} else {
					VSYNC_WR_MPEG_REG_BITS(VD1_IF0_GEN_REG2
						+
					cur_dev->viu_off,
					0, 2, 4);
				}
			}
		}
#else
#ifdef TV_REVERSE
		if (reverse) {
			VSYNC_WR_MPEG_REG_BITS((VD1_IF0_GEN_REG2 +
				cur_dev->viu_off), 0xf, 2, 4);
		} else {
			VSYNC_WR_MPEG_REG_BITS((VD1_IF0_GEN_REG2 +
				cur_dev->viu_off), 0, 2, 4);
		}
#endif
#endif
	}
	/* #endif */

	/* chroma formatter */
	if (vf->type & VIDTYPE_VIU_444) {
		VSYNC_WR_MPEG_REG(VIU_VD1_FMT_CTRL + cur_dev->viu_off,
				  HFORMATTER_YC_RATIO_1_1);
		VSYNC_WR_MPEG_REG(VIU_VD2_FMT_CTRL + cur_dev->viu_off,
				  HFORMATTER_YC_RATIO_1_1);
	} else if (vf->type & VIDTYPE_VIU_FIELD) {
		vini_phase = 0xc << VFORMATTER_INIPHASE_BIT;
		vphase =
		    ((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08) <<
		    VFORMATTER_PHASE_BIT;

		VSYNC_WR_MPEG_REG(VIU_VD1_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 | HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN |
				vini_phase | vphase |
				VFORMATTER_EN);

		VSYNC_WR_MPEG_REG(VIU_VD2_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 | HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN | vini_phase | vphase |
				VFORMATTER_EN);
	} else if (vf->type & VIDTYPE_MVC) {
		VSYNC_WR_MPEG_REG(VIU_VD1_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 |
				HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN |
				(0xe << VFORMATTER_INIPHASE_BIT) |
				(((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08)
				<< VFORMATTER_PHASE_BIT) | VFORMATTER_EN);
		VSYNC_WR_MPEG_REG(VIU_VD2_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 | HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN | (0xa <<
				VFORMATTER_INIPHASE_BIT) |
				(((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08)
				<< VFORMATTER_PHASE_BIT) | VFORMATTER_EN);
	} else if ((vf->type & VIDTYPE_INTERLACE)
		   &&
		   (((vf->type & VIDTYPE_TYPEMASK) == VIDTYPE_INTERLACE_TOP))) {
		VSYNC_WR_MPEG_REG(VIU_VD1_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 | HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN | (0xe <<
				VFORMATTER_INIPHASE_BIT) |
				(((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08)
				<< VFORMATTER_PHASE_BIT) | VFORMATTER_EN);

		VSYNC_WR_MPEG_REG(VIU_VD2_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 |
				HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN |
				(0xe << VFORMATTER_INIPHASE_BIT) |
				(((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08)
				<< VFORMATTER_PHASE_BIT) | VFORMATTER_EN);
	} else {
		VSYNC_WR_MPEG_REG(VIU_VD1_FMT_CTRL + cur_dev->viu_off,
				  HFORMATTER_YC_RATIO_2_1 |
				  HFORMATTER_EN |
				  VFORMATTER_RPTLINE0_EN |
				  (0xa << VFORMATTER_INIPHASE_BIT) |
				  (((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08)
				   << VFORMATTER_PHASE_BIT) | VFORMATTER_EN);

		VSYNC_WR_MPEG_REG(VIU_VD2_FMT_CTRL + cur_dev->viu_off,
				HFORMATTER_YC_RATIO_2_1 |
				HFORMATTER_EN |
				VFORMATTER_RPTLINE0_EN |
				(0xa << VFORMATTER_INIPHASE_BIT) |
				(((vf->type & VIDTYPE_VIU_422) ? 0x10 : 0x08)
				<< VFORMATTER_PHASE_BIT) | VFORMATTER_EN);
	}
#if HAS_VPU_PROT
	if (has_vpu_prot()) {
		if (video_prot.status && use_prot) {
			VSYNC_WR_MPEG_REG_BITS(VIU_VD1_FMT_CTRL +
				cur_dev->viu_off, 0,
				VFORMATTER_INIPHASE_BIT, 4);
			VSYNC_WR_MPEG_REG_BITS(VIU_VD1_FMT_CTRL +
				cur_dev->viu_off, 0, 16, 1);
			VSYNC_WR_MPEG_REG_BITS(VIU_VD1_FMT_CTRL +
				cur_dev->viu_off, 1, 17, 1);
		}
	}
#endif
	/* LOOP/SKIP pattern */
	pat = vpat[frame_par->vscale_skip_count];

	if (vf->type & VIDTYPE_VIU_FIELD) {
		loop = 0;

		if (vf->type & VIDTYPE_INTERLACE)
			pat = vpat[frame_par->vscale_skip_count >> 1];
	} else if (vf->type & VIDTYPE_MVC) {
		loop = 0x11;
		pat = 0x80;
	} else if ((vf->type & VIDTYPE_TYPEMASK) == VIDTYPE_INTERLACE_TOP) {
		loop = 0x11;
		pat <<= 4;
	} else
		loop = 0;

	VSYNC_WR_MPEG_REG(VD1_IF0_RPT_LOOP + cur_dev->viu_off,
			(loop << VDIF_CHROMA_LOOP1_BIT) |
			(loop << VDIF_LUMA_LOOP1_BIT) |
			(loop << VDIF_CHROMA_LOOP0_BIT) |
			(loop << VDIF_LUMA_LOOP0_BIT));

	VSYNC_WR_MPEG_REG(VD2_IF0_RPT_LOOP,
			(loop << VDIF_CHROMA_LOOP1_BIT) |
			(loop << VDIF_LUMA_LOOP1_BIT) |
			(loop << VDIF_CHROMA_LOOP0_BIT) |
			(loop << VDIF_LUMA_LOOP0_BIT));

	VSYNC_WR_MPEG_REG(VD1_IF0_LUMA0_RPT_PAT + cur_dev->viu_off, pat);
	VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA0_RPT_PAT + cur_dev->viu_off, pat);
	VSYNC_WR_MPEG_REG(VD1_IF0_LUMA1_RPT_PAT + cur_dev->viu_off, pat);
	VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA1_RPT_PAT + cur_dev->viu_off, pat);

	if (vf->type & VIDTYPE_MVC)
		pat = 0x88;

	VSYNC_WR_MPEG_REG(VD2_IF0_LUMA0_RPT_PAT, pat);
	VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA0_RPT_PAT, pat);
	VSYNC_WR_MPEG_REG(VD2_IF0_LUMA1_RPT_PAT, pat);
	VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA1_RPT_PAT, pat);

#ifndef TV_3D_FUNCTION_OPEN
	/* picture 0/1 control */
	if (((vf->type & VIDTYPE_INTERLACE) == 0) &&
	    ((vf->type & VIDTYPE_VIU_FIELD) == 0) &&
	    ((vf->type & VIDTYPE_MVC) == 0)) {
		/* progressive frame in two pictures */
		VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
		cur_dev->viu_off, (2 << 26) |	/* two pic mode */
		(2 << 24) |	/* use own last line */
		(2 << 8) |	/* toggle pic 0 and 1, use pic0 first */
		(0x01));	/* loop pattern */
		VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL + cur_dev->viu_off,
		(2 << 26) |	/* two pic mode */
		(2 << 24) |	/* use own last line */
		(2 << 8) |	/* toggle pic 0 and 1, use pic0 first */
		(0x01));	/* loop pattern */
	} else {
		VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL + cur_dev->viu_off, 0);
		VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL + cur_dev->viu_off, 0);
		VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL, 0);
		VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL, 0);
	}
#else
	/* picture 0/1 control */
	if ((((vf->type & VIDTYPE_INTERLACE) == 0) &&
	     ((vf->type & VIDTYPE_VIU_FIELD) == 0) &&
	     ((vf->type & VIDTYPE_MVC) == 0)) ||
	    (frame_par->vpp_2pic_mode & 0x3)) {
		/* progressive frame in two pictures */
		if (frame_par->vpp_2pic_mode & VPP_PIC1_FIRST) {
			VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
			cur_dev->viu_off, (2 << 26) |	/* two pic mode */
			(2 << 24) |	/* use own last line */
			(1 << 8) |	/* toggle pic 0 and 1, use pic1 first*/
			(0x01));	/* loop pattern */
			VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
			cur_dev->viu_off, (2 << 26) |	/* two pic mode */
			(2 << 24) |	/* use own last line */
			(1 << 8) |	/* toggle pic 0 and 1,use pic1 first */
			(0x01));	/* loop pattern */
		} else {
			VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
			cur_dev->viu_off, (2 << 26) |	/* two pic mode */
			(2 << 24) |	/* use own last line */
			(2 << 8) |	/* toggle pic 0 and 1, use pic0 first */
			(0x01));	/* loop pattern */
			VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
			cur_dev->viu_off, (2 << 26) |	/* two pic mode */
			(2 << 24) |/* use own last line */
			(2 << 8)  |/* toggle pic 0 and 1, use pic0 first */
			(0x01));/* loop pattern */

		}
	} else {
		if (frame_par->vpp_2pic_mode & VPP_SELECT_PIC1) {
			VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL + cur_dev->viu_off,
					  0x4000000);
			VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
					  cur_dev->viu_off, 0x4000000);
			VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL + cur_dev->viu_off,
					  0x4000000);
			VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL +
					  cur_dev->viu_off, 0x4000000);
		} else {
			VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL + cur_dev->viu_off,
					  0);
			VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
					  cur_dev->viu_off, 0);
			VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL, 0);
			VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL, 0);
		}
	}
#endif


}

#if 1				/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */

static int detect_vout_type(void)
{
	int vout_type = VOUT_TYPE_PROG;

	if ((vinfo) && (vinfo->field_height != vinfo->height)) {
		switch (vinfo->mode) {
		case VMODE_480I:
		case VMODE_480CVBS:
		case VMODE_576I:
		case VMODE_576CVBS:
			vout_type = (READ_VCBUS_REG(ENCI_INFO_READ) &
				(1 << 29)) ?
				VOUT_TYPE_BOT_FIELD : VOUT_TYPE_TOP_FIELD;
			break;

		case VMODE_1080I:
		case VMODE_1080I_50HZ:
			/* vout_type = (((READ_VCBUS_REG(ENCI_INFO_READ)
			>> 16) & 0x1fff) < 562) ? */
			vout_type = (((READ_VCBUS_REG(ENCP_INFO_READ) >> 16) &
				0x1fff) < 562) ?
				VOUT_TYPE_TOP_FIELD : VOUT_TYPE_BOT_FIELD;
			break;

		default:
			break;
		}
#ifdef CONFIG_VSYNC_RDMA
		if (is_vsync_rdma_enable()) {
			if (vout_type == VOUT_TYPE_TOP_FIELD)
				vout_type = VOUT_TYPE_BOT_FIELD;
			else if (vout_type == VOUT_TYPE_BOT_FIELD)
				vout_type = VOUT_TYPE_TOP_FIELD;
		}
#endif
	}

	return vout_type;
}

#else
static int detect_vout_type(void)
{
#if defined(CONFIG_AM_TCON_OUTPUT)
	return VOUT_TYPE_PROG;
#else
	int vout_type;
	int encp_enable = READ_VCBUS_REG(ENCP_VIDEO_EN) & 1;

	if (encp_enable) {
		if (READ_VCBUS_REG(ENCP_VIDEO_MODE) & (1 << 12)) {
			/* 1080I */
			if (READ_VCBUS_REG(VENC_ENCP_LINE) < 562)
				vout_type = VOUT_TYPE_TOP_FIELD;

			else
				vout_type = VOUT_TYPE_BOT_FIELD;

		} else
			vout_type = VOUT_TYPE_PROG;

	} else {
		vout_type = (READ_VCBUS_REG(VENC_STATA) & 1) ?
		    VOUT_TYPE_BOT_FIELD : VOUT_TYPE_TOP_FIELD;
	}

	return vout_type;
#endif
}
#endif

#ifdef INTERLACE_FIELD_MATCH_PROCESS
static inline bool interlace_field_type_need_match(int vout_type,
						   struct vframe_s *vf)
{
	if (DUR2PTS(vf->duration) != vsync_pts_inc)
		return false;

	if ((vout_type == VOUT_TYPE_TOP_FIELD) &&
	    ((vf->type & VIDTYPE_TYPEMASK) == VIDTYPE_INTERLACE_BOTTOM))
		return true;
	else if ((vout_type == VOUT_TYPE_BOT_FIELD) &&
		 ((vf->type & VIDTYPE_TYPEMASK) == VIDTYPE_INTERLACE_TOP))
		return true;

	return false;
}
#endif

static int calc_hold_line(void)
{
	if ((READ_VCBUS_REG(ENCI_VIDEO_EN) & 1) == 0)
		return READ_VCBUS_REG(ENCP_VIDEO_VAVON_BLINE) >> 1;
	else
		return READ_VCBUS_REG(ENCP_VFIFO2VD_LINE_TOP_START) >> 1;
}

/* add a new function to check if current display frame has been
displayed for its duration */
static inline bool duration_expire(struct vframe_s *cur_vf,
				   struct vframe_s *next_vf, u32 dur)
{
	u32 pts;
	s32 dur_disp;
	static s32 rpt_tab_idx;
	static const u32 rpt_tab[4] = { 0x100, 0x100, 0x300, 0x300 };

	if ((cur_vf == NULL) || (cur_dispbuf == &vf_local))
		return true;

	pts = next_vf->pts;
	if (pts == 0)
		dur_disp = DUR2PTS(cur_vf->duration);
	else
		dur_disp = pts - timestamp_vpts_get();

	if ((dur << 8) >= (dur_disp * rpt_tab[rpt_tab_idx & 3])) {
		rpt_tab_idx = (rpt_tab_idx + 1) & 3;
		return true;
	} else
		return false;
}

#define VPTS_RESET_THRO

static inline bool vpts_expire(struct vframe_s *cur_vf,
			       struct vframe_s *next_vf)
{
	u32 pts = next_vf->pts;
#ifdef VIDEO_PTS_CHASE
	u32 vid_pts, scr_pts;
#endif
	u32 systime;
	u32 adjust_pts, org_vpts;

	if (debug_flag & DEBUG_FLAG_TOGGLE_FRAME_PER_VSYNC)
		return true;
	if (/*(cur_vf == NULL) || (cur_dispbuf == &vf_local) ||*/ debugflags &
	    DEBUG_FLAG_FFPLAY)
		return true;

	if (FREERUN_NODUR == freerun_mode || hdmi_in_onvideo)
		return true;

	if ((trickmode_i == 1) || ((trickmode_fffb == 1))) {
		if (((0 == atomic_read(&trickmode_framedone))
		     || (trickmode_i == 1)) && (!to_notify_trick_wait)
		    && (trickmode_duration_count <= 0)) {
#if 0
			if (cur_vf)
				pts = timestamp_vpts_get() +
				trickmode_duration;
			else
				return true;
#else
			return true;
#endif
		} else
			return false;
	}

	if (next_vf->duration == 0)

		return true;

	systime = timestamp_pcrscr_get();

	if (((pts == 0) && (cur_dispbuf != &vf_local))
	    || (FREERUN_DUR == freerun_mode)) {
		pts =
		    timestamp_vpts_get() +
		    (cur_vf ? DUR2PTS(cur_vf->duration) : 0);
	}
	/* check video PTS discontinuity */
	else if (timestamp_pcrscr_enable_state() > 0 &&
		 (enable_video_discontinue_report) &&
		 (abs(systime - pts) > tsync_vpts_discontinuity_margin()) &&
		 ((next_vf->flag & VFRAME_FLAG_NO_DISCONTINUE) == 0)) {
		pts =
		    timestamp_vpts_get() +
		    (cur_vf ? DUR2PTS(cur_vf->duration) : 0);
		/* pr_info("system=0x%x vpts=0x%x\n", systime,
		timestamp_vpts_get()); */
		if ((int)(systime - pts) >= 0) {
			if (next_vf->pts != 0)
				tsync_avevent_locked(VIDEO_TSTAMP_DISCONTINUITY,
						     next_vf->pts);
			else if (next_vf->pts == 0 &&
				(tsync_get_mode() != TSYNC_MODE_PCRMASTER))
				tsync_avevent_locked(VIDEO_TSTAMP_DISCONTINUITY,
						     pts);

			pr_info("discontinue, systime=0x%x vpts=0x%x next_vf->pts = 0x%x\n",
				systime,
				pts,
				next_vf->pts);

			/* pts==0 is a keep frame maybe. */
			if (systime > next_vf->pts || next_vf->pts == 0)
				return true;
			if (omx_secret_mode == true)
				return true;

			return false;
		} else if (omx_secret_mode == true)
			return true;
	}
#if 1
	if (vsync_pts_inc_upint && (!freerun_mode)) {
		struct vframe_states frame_states;
		u32 delayed_ms, t1, t2;
		delayed_ms =
		    calculation_stream_delayed_ms(PTS_TYPE_VIDEO, &t1, &t2);
		if (vf_get_states(&frame_states) == 0) {
			u32 pcr = timestamp_pcrscr_get();
			u32 vpts = timestamp_vpts_get();
			u32 diff = pcr - vpts;
			if (delayed_ms > 200) {
				vsync_freerun++;
				if (pcr < next_vf->pts
				    || pcr < vpts + next_vf->duration) {
					if (next_vf->pts > 0) {
						timestamp_pcrscr_set
						    (next_vf->pts);
					} else {
						timestamp_pcrscr_set(vpts +
							next_vf->duration);
					}
				}
				return true;
			} else if ((frame_states.buf_avail_num >= 3)
				   && diff < vsync_pts_inc << 2) {
				vsync_pts_inc_adj =
				    vsync_pts_inc + (vsync_pts_inc >> 2);
				vsync_pts_125++;
			} else if ((frame_states.buf_avail_num >= 2
				    && diff < vsync_pts_inc << 1)) {
				vsync_pts_inc_adj =
				    vsync_pts_inc + (vsync_pts_inc >> 3);
				vsync_pts_112++;
			} else if (frame_states.buf_avail_num >= 1
				   && diff < vsync_pts_inc - 20) {
				vsync_pts_inc_adj = vsync_pts_inc + 10;
				vsync_pts_101++;
			} else {
				vsync_pts_inc_adj = 0;
				vsync_pts_100++;
			}
		}
	}
#endif

#ifdef VIDEO_PTS_CHASE
	vid_pts = timestamp_vpts_get();
	scr_pts = timestamp_pcrscr_get();
	vid_pts += vsync_pts_inc;

	if (av_sync_flag) {
		if (vpts_chase) {
			if ((abs(vid_pts - scr_pts) < 6000)
			    || (abs(vid_pts - scr_pts) > 90000)) {
				vpts_chase = 0;
				pr_info("leave vpts chase mode, diff:%d\n",
				       vid_pts - scr_pts);
			}
		} else if ((abs(vid_pts - scr_pts) > 9000)
			   && (abs(vid_pts - scr_pts) < 90000)) {
			vpts_chase = 1;
			if (vid_pts < scr_pts)
				vpts_chase_pts_diff = 50;
			else
				vpts_chase_pts_diff = -50;
			vpts_chase_counter =
			    ((int)(scr_pts - vid_pts)) / vpts_chase_pts_diff;
			pr_info("enter vpts chase mode, diff:%d\n",
			       vid_pts - scr_pts);
		} else if (abs(vid_pts - scr_pts) >= 90000) {
			pr_info("video pts discontinue, diff:%d\n",
			       vid_pts - scr_pts);
		}
	} else
		vpts_chase = 0;

	if (vpts_chase) {
		u32 curr_pts =
		    scr_pts - vpts_chase_pts_diff * vpts_chase_counter;

	/* pr_info("vchase pts %d, %d, %d, %d, %d\n",
	curr_pts, scr_pts, curr_pts-scr_pts, vid_pts, vpts_chase_counter); */
		return ((int)(curr_pts - pts)) >= 0;
	} else {
		int aud_start = (timestamp_apts_get() != -1);

		if (!av_sync_flag && aud_start && (abs(scr_pts - pts) < 9000)
		    && ((int)(scr_pts - pts) < 0)) {
			av_sync_flag = 1;
			pr_info("av sync ok\n");
		}
		return ((int)(scr_pts - pts)) >= 0;
	}
#else
	if (smooth_sync_enable) {
		org_vpts = timestamp_vpts_get();
		if ((abs(org_vpts + vsync_pts_inc - systime) <
			M_PTS_SMOOTH_MAX)
		    && (abs(org_vpts + vsync_pts_inc - systime) >
			M_PTS_SMOOTH_MIN)) {

			if (!video_frame_repeat_count) {
				vpts_ref = org_vpts;
				video_frame_repeat_count++;
			}

			if ((int)(org_vpts + vsync_pts_inc - systime) > 0) {
				adjust_pts =
				    vpts_ref + (vsync_pts_inc -
						M_PTS_SMOOTH_ADJUST) *
				    video_frame_repeat_count;
			} else {
				adjust_pts =
				    vpts_ref + (vsync_pts_inc +
						M_PTS_SMOOTH_ADJUST) *
				    video_frame_repeat_count;
			}

			return (int)(adjust_pts - pts) >= 0;
		}

		if (video_frame_repeat_count) {
			vpts_ref = 0;
			video_frame_repeat_count = 0;
		}
	}

	return (int)(timestamp_pcrscr_get() - pts) >= 0;
#endif
}

static void vsync_notify(void)
{
	if (video_notify_flag & VIDEO_NOTIFY_TRICK_WAIT) {
		wake_up_interruptible(&amvideo_trick_wait);
		video_notify_flag &= ~VIDEO_NOTIFY_TRICK_WAIT;
	}
	if (video_notify_flag & VIDEO_NOTIFY_FRAME_WAIT) {
		video_notify_flag &= ~VIDEO_NOTIFY_FRAME_WAIT;
		vf_notify_provider(RECEIVER_NAME,
				   VFRAME_EVENT_RECEIVER_FRAME_WAIT, NULL);
	}
#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
	if (video_notify_flag & VIDEO_NOTIFY_POS_CHANGED) {
		video_notify_flag &= ~VIDEO_NOTIFY_POS_CHANGED;
		vf_notify_provider(RECEIVER_NAME,
				   VFRAME_EVENT_RECEIVER_POS_CHANGED, NULL);
	}
#endif
	if (video_notify_flag &
	    (VIDEO_NOTIFY_PROVIDER_GET | VIDEO_NOTIFY_PROVIDER_PUT)) {
		int event = 0;

		if (video_notify_flag & VIDEO_NOTIFY_PROVIDER_GET)
			event |= VFRAME_EVENT_RECEIVER_GET;
		if (video_notify_flag & VIDEO_NOTIFY_PROVIDER_PUT)
			event |= VFRAME_EVENT_RECEIVER_PUT;

		vf_notify_provider(RECEIVER_NAME, event, NULL);

		video_notify_flag &=
		    ~(VIDEO_NOTIFY_PROVIDER_GET | VIDEO_NOTIFY_PROVIDER_PUT);
	}
#ifdef CONFIG_CLK81_DFS
	check_and_set_clk81();
#endif

#ifdef CONFIG_GAMMA_PROC
	gamma_adjust();
#endif

}

#ifdef FIQ_VSYNC
static irqreturn_t vsync_bridge_isr(int irq, void *dev_id)
{
	vsync_notify();

	return IRQ_HANDLED;
}
#endif

int get_vsync_count(unsigned char reset)
{
	if (reset)
		vsync_count = 0;
	return vsync_count;
}
EXPORT_SYMBOL(get_vsync_count);

int get_vsync_pts_inc_mode(void)
{
	return vsync_pts_inc_upint;
}
EXPORT_SYMBOL(get_vsync_pts_inc_mode);

void set_vsync_pts_inc_mode(int inc)
{
	vsync_pts_inc_upint = inc;
}
EXPORT_SYMBOL(set_vsync_pts_inc_mode);

#ifdef CONFIG_VSYNC_RDMA
void vsync_rdma_process(void)
{
	vsync_rdma_config();
}
#endif

/* #ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2 */
static enum vmode_e old_vmode = VMODE_MAX;
/* #endif */
static enum vmode_e new_vmode = VMODE_MAX;

static inline bool video_vf_dirty_put(struct vframe_s *vf)
{
	if (!vf->frame_dirty)
		return false;
	if (cur_dispbuf != vf) {
		if (vf->source_type != VFRAME_SOURCE_TYPE_OSD) {
			if (vf->pts != 0) {
				amlog_mask(LOG_MASK_TIMESTAMP,
				"vpts to vf->pts:0x%x,scr:0x%x,abs_scr: 0x%x\n",
				vf->pts, timestamp_pcrscr_get(),
				READ_MPEG_REG(SCR_HIU));
				timestamp_vpts_set(vf->pts);
			} else if (cur_dispbuf) {
				amlog_mask(LOG_MASK_TIMESTAMP,
				     "vpts inc:0x%x,scr: 0x%x, abs_scr: 0x%x\n",
				     timestamp_vpts_get() +
				     DUR2PTS(cur_dispbuf->duration),
				     timestamp_pcrscr_get(),
				     READ_MPEG_REG(SCR_HIU));
				timestamp_vpts_inc(
						DUR2PTS(cur_dispbuf->duration));

				vpts_remainder +=
					DUR2PTS_RM(cur_dispbuf->duration);
				if (vpts_remainder >= 0xf) {
					vpts_remainder -= 0xf;
					timestamp_vpts_inc(-1);
				}
			}
		}
	}
	video_vf_put(vf);
	return true;

}
#ifdef FIQ_VSYNC
void vsync_fisr(void)
#else
static irqreturn_t vsync_isr(int irq, void *dev_id)
#endif
{
	int hold_line;
	int enc_line;
	unsigned char frame_par_di_set = 0;
	s32 i, vout_type;
	struct vframe_s *vf;
	unsigned long flags;
	struct vdin_v4l2_ops_s *vdin_ops = NULL;
	struct vdin_arg_s arg;
	bool show_nosync = false;
	u32 vpp_misc_save, vpp_misc_set;
	int first_set = 0;
#ifdef CONFIG_AM_VIDEO_LOG
	int toggle_cnt;
#endif
	if (debug_flag & DEBUG_FLAG_VSYNC_DONONE)
		return IRQ_HANDLED;

#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	const char *dev_id_s = (const char *)dev_id;
	int dev_id_len = strlen(dev_id_s);
	if (cur_dev == &video_dev[1]) {
		if (cur_dev_idx == 0) {
			cur_dev = &video_dev[0];
			vinfo = get_current_vinfo();
			vsync_pts_inc =
			    90000 * vinfo->sync_duration_den /
			    vinfo->sync_duration_num;
			vsync_pts_inc_scale = vinfo->sync_duration_den;
			vsync_pts_inc_scale_base = vinfo->sync_duration_num;
			video_property_changed = true;
			pr_info("Change to video 0\n");
		}
	} else {
		if (cur_dev_idx != 0) {
			cur_dev = &video_dev[1];
			vinfo = get_current_vinfo2();
			vsync_pts_inc =
			    90000 * vinfo->sync_duration_den /
			    vinfo->sync_duration_num;
			vsync_pts_inc_scale = vinfo->sync_duration_den;
			vsync_pts_inc_scale_base = vinfo->sync_duration_num;
			video_property_changed = true;
			pr_info("Change to video 1\n");
		}
	}

	if ((dev_id_s[dev_id_len - 1] == '2' && cur_dev_idx == 0) ||
	    (dev_id_s[dev_id_len - 1] != '2' && cur_dev_idx != 0))
		return IRQ_HANDLED;
	/* pr_info("%s: %s\n", __func__, dev_id_s); */
#endif

	vf = video_vf_peek();
	if ((vf) && ((vf->type & VIDTYPE_NO_VIDEO_ENABLE) == 0)) {
		if ((old_vmode != new_vmode) || (debug_flag == 8)) {
			debug_flag = 1;
			video_property_changed = true;
			pr_info("detect vout mode change!!!!!!!!!!!!\n");
			old_vmode = new_vmode;
		}
	}
#ifdef CONFIG_AM_VIDEO_LOG
	toggle_cnt = 0;
#endif
	vsync_count++;
	timer_count++;

	switch (READ_VCBUS_REG(VPU_VIU_VENC_MUX_CTRL) & 0x3) {
	case 0:
		enc_line = (READ_VCBUS_REG(ENCL_INFO_READ) >> 16) & 0x1fff;
		break;
	case 1:
		enc_line = (READ_VCBUS_REG(ENCI_INFO_READ) >> 16) & 0x1fff;
		break;
	case 2:
		enc_line = (READ_VCBUS_REG(ENCP_INFO_READ) >> 16) & 0x1fff;
		break;
	case 3:
		enc_line = (READ_VCBUS_REG(ENCT_INFO_READ) >> 16) & 0x1fff;
		break;
	}
	if (enc_line > vsync_enter_line_max)
		vsync_enter_line_max = enc_line;

#ifdef CONFIG_VSYNC_RDMA
	vsync_rdma_config_pre();

	if (to_notify_trick_wait) {
		atomic_set(&trickmode_framedone, 1);
		video_notify_flag |= VIDEO_NOTIFY_TRICK_WAIT;
		to_notify_trick_wait = false;
		goto exit;
	}

	if (debug_flag & DEBUG_FLAG_PRINT_RDMA) {
		if (video_property_changed) {
			enable_rdma_log_count = 5;
			enable_rdma_log(1);
		}
		if (enable_rdma_log_count > 0)
			enable_rdma_log_count--;
	}
#endif

#if defined(CONFIG_AM_VECM)
	amvecm_on_vs(vf);
#endif
	/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
	if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8) && !is_meson_mtvd_cpu()) {
		vdin_ops = NULL;	/* /get_vdin_v4l2_ops(); */
		if (vdin_ops) {
			arg.cmd = VDIN_CMD_ISR;
			vdin_ops->tvin_vdin_func(1, &arg);
#ifdef CONFIG_AM_VIDEO2
			vdin_ops->tvin_vdin_func(0, &arg);
#endif
		}
	}
	/* #endif */
	vout_type = detect_vout_type();
	hold_line = calc_hold_line();
	if (vsync_pts_inc_upint) {
		if (vsync_pts_inc_adj) {
			/* pr_info("adj %d, org %d\n",
			vsync_pts_inc_adj, vsync_pts_inc); */
			timestamp_pcrscr_inc(vsync_pts_inc_adj);
			timestamp_apts_inc(vsync_pts_inc_adj);
		} else {
			timestamp_pcrscr_inc(vsync_pts_inc + 1);
			timestamp_apts_inc(vsync_pts_inc + 1);
		}
	} else {
		if (vsync_slow_factor == 0) {
			pr_info("invalid vsync_slow_factor, set to 1\n");
			vsync_slow_factor = 1;
		}

		if (vsync_slow_factor == 1) {
			timestamp_pcrscr_inc_scale(vsync_pts_inc_scale,
						   vsync_pts_inc_scale_base);
		} else
			timestamp_pcrscr_inc(
				vsync_pts_inc / vsync_slow_factor);
		timestamp_apts_inc(vsync_pts_inc / vsync_slow_factor);
	}
	if (omx_secret_mode == true) {
		u32 system_time = timestamp_pcrscr_get();
		int diff = system_time - omx_pts;
		if ((diff - omx_pts_interval_upper) > 0
			|| (diff - omx_pts_interval_lower) < 0) {
			timestamp_pcrscr_enable(1);
			/*pr_info("system_time=%d, omx_pts=%d, diff=%d\n",
			system_time, omx_pts, diff);*/
			timestamp_pcrscr_set(omx_pts);
		}
	} else
		omx_pts = 0;
	if (trickmode_duration_count > 0)
		trickmode_duration_count -= vsync_pts_inc;
#ifdef VIDEO_PTS_CHASE
	if (vpts_chase)
		vpts_chase_counter--;
#endif

	if (slowsync_repeat_enable)
		frame_repeat_count++;

	if (smooth_sync_enable) {
		if (video_frame_repeat_count)
			video_frame_repeat_count++;
	}

	if (atomic_read(&video_unreg_flag))
		goto exit;
	if (atomic_read(&video_pause_flag))
		goto exit;
#ifdef CONFIG_VSYNC_RDMA
	if (is_vsync_rdma_enable())
		rdma_canvas_id = next_rdma_canvas_id;
	else {
		if (rdma_enable_pre) {
			/*do not write register directly before RDMA is done */
			/*#if MESON_CPU_TYPE == MESON_CPU_TYPE_MESON6TV */
			if (is_meson_mtvd_cpu()) {
				if (debug_flag & DEBUG_FLAG_RDMA_WAIT_1) {
					while
					(
					((READ_VCBUS_REG(ENCL_INFO_READ) >> 16)
					& 0x1fff) < 50);
				}
			}
			/* #endif */
			goto exit;
		}
		rdma_canvas_id = 0;
		next_rdma_canvas_id = 1;
	}

	for (i = 0; i < dispbuf_to_put_num; i++) {
		if (dispbuf_to_put[i]) {
			video_vf_put(dispbuf_to_put[i]);
			dispbuf_to_put[i] = NULL;
		}
		dispbuf_to_put_num = 0;
	}
#endif
#if HAS_VPU_PROT
	if (has_vpu_prot()) {
		use_prot = get_use_prot();
		if (video_prot.video_started) {
			video_prot.video_started = 0;
			return IRQ_HANDLED;
		}
	}
#endif
	if (osd_prov && osd_prov->ops && osd_prov->ops->get) {
		vf = osd_prov->ops->get(osd_prov->op_arg);
		if (vf) {
			vf->source_type = VFRAME_SOURCE_TYPE_OSD;
			vsync_toggle_frame(vf);
			if (debug_flag & DEBUG_FLAG_BLACKOUT) {
				pr_info
				    ("[video4osd] toggle osd_vframe {%d,%d}\n",
				     vf->width, vf->height);
			}
			goto SET_FILTER;
		}
	}

	if ((!cur_dispbuf) || (cur_dispbuf == &vf_local)) {

		vf = video_vf_peek();

		if (vf) {
			if (hdmi_in_onvideo == 0)
				tsync_avevent_locked(VIDEO_START,
						     (vf->pts) ? vf->pts :
						     timestamp_vpts_get());

			if (show_first_frame_nosync)
				show_nosync = true;

			if (slowsync_repeat_enable)
				frame_repeat_count = 0;

		} else if ((cur_dispbuf == &vf_local)
			   && (video_property_changed)) {
			if (!(blackout | force_blackout)) {
				if ((READ_VCBUS_REG(DI_IF1_GEN_REG) &
					0x1) == 0) {
					/* setting video display
					property in unregister mode */
					u32 cur_index =
					    READ_VCBUS_REG(VD1_IF0_CANVAS0 +
							   cur_dev->viu_off);
					if (!((get_cpu_type() >=
						MESON_CPU_MAJOR_ID_GXBB) &&
						(cur_dispbuf->type &
						  VIDTYPE_COMPRESS)))
						cur_dispbuf->canvas0Addr
							= cur_index;
				}
				vsync_toggle_frame(cur_dispbuf);
			} else
				video_property_changed = false;
		} else
			goto SET_FILTER;
	}

	/* buffer switch management */
	vf = video_vf_peek();

	/* setting video display property in underflow mode */
	if ((!vf) && cur_dispbuf && (video_property_changed))
		vsync_toggle_frame(cur_dispbuf);

	if (!vf)
		underflow++;
#ifdef TV_3D_FUNCTION_OPEN
	/* toggle_3d_fa_frame  determine the out frame is L or R or blank */
	judge_3d_fa_out_mode();
#endif
	while (vf) {
		if (vpts_expire(cur_dispbuf, vf) || show_nosync) {
			amlog_mask(LOG_MASK_TIMESTAMP,
			"vpts = 0x%x, c.dur=0x%x, n.pts=0x%x, scr = 0x%x\n",
				   timestamp_vpts_get(),
				   (cur_dispbuf) ? cur_dispbuf->duration : 0,
				   vf->pts, timestamp_pcrscr_get());

			amlog_mask_if(toggle_cnt > 0, LOG_MASK_FRAMESKIP,
				      "skipped\n");

#if defined(CONFIG_AM_VECM)
			amvecm_on_vs(vf);
#endif

			vf = video_vf_get();
			if (!vf)
				break;
			if (video_vf_dirty_put(vf))
				break;
			force_blackout = 0;
#ifdef TV_3D_FUNCTION_OPEN

			if (vf) {
				if (last_mode_3d != vf->mode_3d_enable) {
					last_mode_3d = vf->mode_3d_enable;
					mode_3d_changed = 1;
				}
				video_3d_format = vf->trans_fmt;
			}
#endif

			vsync_toggle_frame(vf);
			if (trickmode_fffb == 1) {
				trickmode_vpts = vf->pts;
#ifdef CONFIG_VSYNC_RDMA
				if ((VSYNC_RD_MPEG_REG(DI_IF1_GEN_REG) & 0x1)
					== 0)
					to_notify_trick_wait = true;
				else {
					atomic_set(&trickmode_framedone, 1);
					video_notify_flag |=
					    VIDEO_NOTIFY_TRICK_WAIT;
				}
#else
				atomic_set(&trickmode_framedone, 1);
				video_notify_flag |= VIDEO_NOTIFY_TRICK_WAIT;
#endif
				break;
			}
			if (slowsync_repeat_enable)
				frame_repeat_count = 0;
			vf = video_vf_peek();
			if (!vf)
				next_peek_underflow++;

			if (debug_flag & DEBUG_FLAG_TOGGLE_FRAME_PER_VSYNC)
				break;
		} else {
			/* check if current frame's duration has expired,
			*in this example
			* it compares current frame display duration
			* with 1/1/1/1.5 frame duration
			* every 4 frames there will be one frame play
			* longer than usual.
			* you can adjust this array for any slow sync
			* control as you want.
			* The playback can be smoother than previous method.
			*/
			if (slowsync_repeat_enable) {
				if (duration_expire
				    (cur_dispbuf, vf,
				     frame_repeat_count * vsync_pts_inc)
				    && timestamp_pcrscr_enable_state()) {
					amlog_mask(LOG_MASK_SLOWSYNC,
					"slow sync toggle,repeat_count = %d\n",
					frame_repeat_count);
					amlog_mask(LOG_MASK_SLOWSYNC,
					"sys.time = 0x%x, video time = 0x%x\n",
					timestamp_pcrscr_get(),
					timestamp_vpts_get());
					vf = video_vf_get();
					if (!vf)
						break;
					vsync_toggle_frame(vf);
					frame_repeat_count = 0;

					vf = video_vf_peek();
				} else if ((cur_dispbuf) &&
					(cur_dispbuf->duration_pulldown >
					vsync_pts_inc)) {
					frame_count++;
					cur_dispbuf->duration_pulldown -=
					    PTS2DUR(vsync_pts_inc);
				}
			} else {
				if ((cur_dispbuf)
				    && (cur_dispbuf->duration_pulldown >
					vsync_pts_inc)) {
					frame_count++;
					cur_dispbuf->duration_pulldown -=
					    PTS2DUR(vsync_pts_inc);
				}
			}
			/*  setting video display property in pause mode */
			if (video_property_changed && cur_dispbuf) {
				if (blackout | force_blackout) {
					if (cur_dispbuf != &vf_local)
						vsync_toggle_frame(
								cur_dispbuf);
				} else
					vsync_toggle_frame(cur_dispbuf);
			}
#if defined(CONFIG_AM_VECM)
			amvecm_on_vs(vf);
#endif
			break;
		}

#ifdef CONFIG_AM_VIDEO_LOG
		toggle_cnt++;
#endif
	}

#ifdef INTERLACE_FIELD_MATCH_PROCESS
	if (interlace_field_type_need_match(vout_type, vf)) {
		if (field_matching_count++ == FIELD_MATCH_THRESHOLD) {
			field_matching_count = 0;
			/* adjust system time to get one more field toggle */
			/* at next vsync to match field */
			timestamp_pcrscr_inc(vsync_pts_inc);
		}
	} else
		field_matching_count = 0;
#endif

 SET_FILTER:
	/* filter setting management */
	if ((frame_par_ready_to_set) || (frame_par_force_to_set)) {
		cur_frame_par = next_frame_par;
		frame_par_di_set = 1;
	}
#ifdef TV_3D_FUNCTION_OPEN

	if (mode_3d_changed) {
		mode_3d_changed = 0;
		frame_par_force_to_set = 1;
	}
#endif
	if (cur_dispbuf) {
		struct f2v_vphase_s *vphase;
		u32 vin_type = cur_dispbuf->type & VIDTYPE_TYPEMASK;

		{
			if (frame_par_ready_to_set)
				viu_set_dcu(cur_frame_par, cur_dispbuf);
		}

#if 0
		if (get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB) {
			if (cur_dispbuf->type & VIDTYPE_COMPRESS) {
				/*SET_VCBUS_REG_MASK(VIU_MISC_CTRL0,
				    VIU_MISC_AFBC_VD1);*/
				VSYNC_WR_MPEG_REG_BITS(VIU_MISC_CTRL0 +
					cur_dev->viu_off, 1, 20, 1);
			} else {
				/*CLEAR_VCBUS_REG_MASK(VIU_MISC_CTRL0,
				    VIU_MISC_AFBC_VD1);*/
				VSYNC_WR_MPEG_REG_BITS(VIU_MISC_CTRL0 +
					cur_dev->viu_off, 0, 20, 1);
			}
		}
#endif

#ifdef TV_3D_FUNCTION_OPEN
		if ((cur_frame_par->hscale_skip_count)
		    && (cur_dispbuf->type & VIDTYPE_VIU_FIELD)) {
			VSYNC_WR_MPEG_REG_BITS(VIU_VD1_FMT_CTRL +
				cur_dev->viu_off, 0, 20, 1);
			/* HFORMATTER_EN */
			VSYNC_WR_MPEG_REG_BITS(VIU_VD2_FMT_CTRL +
				cur_dev->viu_off, 0, 20, 1);
			/* HFORMATTER_EN */
		}
		if (process_3d_type & MODE_3D_OUT_FA_MASK) {
			if (toggle_3d_fa_frame == OUT_FA_A_FRAME) {
				VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
					cur_dev->vpp_off, 1, 14, 1);
				/* VPP_VD1_PREBLEND disable */
				VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
					cur_dev->vpp_off, 1, 10, 1);
				/* VPP_VD1_POSTBLEND disable */
				VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
				VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
				VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
				VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
			} else if (OUT_FA_B_FRAME == toggle_3d_fa_frame) {
				VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
					cur_dev->vpp_off, 1, 14, 1);
				/* VPP_VD1_PREBLEND disable */
				VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
					cur_dev->vpp_off, 1, 10, 1);
				/* VPP_VD1_POSTBLEND disable */
				VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0);
				VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0);
				VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL, 0);
				VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL, 0);

			} else if (toggle_3d_fa_frame == OUT_FA_BANK_FRAME) {
				/* output a banking frame */
				VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
					cur_dev->vpp_off, 0, 14, 1);
				/* VPP_VD1_PREBLEND disable */
				VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
				cur_dev->vpp_off, 0, 10, 1);
				/* VPP_VD1_POSTBLEND disable */
			}
		}
		if ((process_3d_type & MODE_3D_OUT_TB)
		    || (process_3d_type & MODE_3D_OUT_LR)) {
			if (cur_frame_par->vpp_2pic_mode & VPP_PIC1_FIRST) {
				VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
				VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
				VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0);
				VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0);
			} else {
				VSYNC_WR_MPEG_REG(VD1_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0);
				VSYNC_WR_MPEG_REG(VD1_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0);
				VSYNC_WR_MPEG_REG(VD2_IF0_LUMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
				VSYNC_WR_MPEG_REG(VD2_IF0_CHROMA_PSEL +
						  cur_dev->viu_off, 0x4000000);
			}
			EnableVideoLayer2();
			/*
			VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
			cur_dev->vpp_off,1,15,1);//VPP_VD2_PREBLEND enable
			//VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
			cur_dev->vpp_off,1,11,1);//VPP_VD2_POSTBLEND enable
			VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
			cur_dev->vpp_off,1,6,1);//PREBLEND enable must be set!
			VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
			cur_dev->vpp_off,0x1ff,
			VPP_VD2_ALPHA_BIT,9);//vd2 alpha must set
			*/
		} else
			DisableVideoLayer2();
		/*
		else{
		VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
		cur_dev->vpp_off,0,15,1);//VPP_VD2_PREBLEND enable
		//VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
		cur_dev->vpp_off,1,11,1);//VPP_VD2_POSTBLEND enable
		VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
		cur_dev->vpp_off,0,6,1);//PREBLEND enable
		VSYNC_WR_MPEG_REG_BITS(VPP_MISC +
		cur_dev->vpp_off,0,VPP_VD2_ALPHA_BIT,9);//vd2 alpha must set
		} */
#endif
		/* vertical phase */
		vphase =
		    &cur_frame_par->VPP_vf_ini_phase_[vpp_phase_table[vin_type]
						      [vout_type]];
		VSYNC_WR_MPEG_REG(VPP_VSC_INI_PHASE + cur_dev->vpp_off,
				  ((u32) (vphase->phase) << 8));

		if (vphase->repeat_skip >= 0) {
			/* skip lines */
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL +
					       cur_dev->vpp_off,
					       skip_tab[vphase->repeat_skip],
					       VPP_PHASECTL_INIRCVNUMT_BIT,
					       VPP_PHASECTL_INIRCVNUM_WID +
					       VPP_PHASECTL_INIRPTNUM_WID);

		} else {
			/* repeat first line */
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL +
					       cur_dev->vpp_off, 4,
					       VPP_PHASECTL_INIRCVNUMT_BIT,
					       VPP_PHASECTL_INIRCVNUM_WID);
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL +
					       cur_dev->vpp_off,
					       1 - vphase->repeat_skip,
					       VPP_PHASECTL_INIRPTNUMT_BIT,
					       VPP_PHASECTL_INIRPTNUM_WID);
		}
#ifdef TV_3D_FUNCTION_OPEN

		if (force_3d_scaler == 3 && cur_frame_par->vpp_3d_scale) {
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL, 3,
					       VPP_PHASECTL_DOUBLELINE_BIT, 2);
		} else if (force_3d_scaler == 1 &&
				cur_frame_par->vpp_3d_scale) {
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL, 1,
					       VPP_PHASECTL_DOUBLELINE_BIT,
					       VPP_PHASECTL_DOUBLELINE_WID);
		} else if (force_3d_scaler == 2 &&
			cur_frame_par->vpp_3d_scale) {
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL, 2,
					       VPP_PHASECTL_DOUBLELINE_BIT, 2);
		} else {
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL, 0,
					       VPP_PHASECTL_DOUBLELINE_BIT, 2);
		}
#endif
	}

	if (((frame_par_ready_to_set) || (frame_par_force_to_set)) &&
	    (cur_frame_par)) {
		struct vppfilter_mode_s *vpp_filter =
		    &cur_frame_par->vpp_filter;

		if (cur_dispbuf) {
			u32 zoom_start_y, zoom_end_y;

			if (cur_dispbuf->type & VIDTYPE_INTERLACE) {
				if (cur_dispbuf->type & VIDTYPE_VIU_FIELD) {
					zoom_start_y =
					cur_frame_par->VPP_vd_start_lines_
					>> 1;
					zoom_end_y =
					(cur_frame_par->VPP_vd_end_lines_ + 1)
					>> 1;
				} else {
					zoom_start_y =
					cur_frame_par->VPP_vd_start_lines_;
					zoom_end_y =
					cur_frame_par->VPP_vd_end_lines_;
				}
			} else {
				if (cur_dispbuf->type & VIDTYPE_VIU_FIELD) {
					zoom_start_y =
					cur_frame_par->VPP_vd_start_lines_;
					zoom_end_y =
					cur_frame_par->VPP_vd_end_lines_;
				} else {
					zoom_start_y =
					cur_frame_par->VPP_vd_start_lines_
					>> 1;
					zoom_end_y =
					(cur_frame_par->VPP_vd_end_lines_ +
					1) >> 1;
				}
			}

			zoom_start_x_lines =
					cur_frame_par->VPP_hd_start_lines_;
			zoom_end_x_lines = cur_frame_par->VPP_hd_end_lines_;
			zoom_display_horz(cur_frame_par->hscale_skip_count);

			zoom_start_y_lines = zoom_start_y;
			zoom_end_y_lines = zoom_end_y;
			zoom_display_vert();
		}

		/* vpp filters */
		/* SET_MPEG_REG_MASK(VPP_SC_MISC + cur_dev->vpp_off, */
		/* VPP_SC_TOP_EN | VPP_SC_VERT_EN | VPP_SC_HORZ_EN); */
		VSYNC_WR_MPEG_REG(VPP_SC_MISC + cur_dev->vpp_off,
				  READ_VCBUS_REG(VPP_SC_MISC +
						 cur_dev->vpp_off) |
				  VPP_SC_TOP_EN | VPP_SC_VERT_EN |
				  VPP_SC_HORZ_EN);

		/* pps pre hsc&vsc en */
		VSYNC_WR_MPEG_REG_BITS(VPP_SC_MISC + cur_dev->vpp_off,
				       vpp_filter->vpp_pre_hsc_en,
				       VPP_SC_PREHORZ_EN_BIT, 1);
		VSYNC_WR_MPEG_REG_BITS(VPP_SC_MISC + cur_dev->vpp_off,
				       vpp_filter->vpp_pre_vsc_en,
				       VPP_SC_PREVERT_EN_BIT, 1);
		VSYNC_WR_MPEG_REG_BITS(VPP_SC_MISC + cur_dev->vpp_off,
				       vpp_filter->vpp_pre_vsc_en,
				       VPP_LINE_BUFFER_EN_BIT, 1);

#ifdef TV_3D_FUNCTION_OPEN
		if (last_mode_3d) {
			/*turn off vertical scaler when 3d display */
			/* CLEAR_MPEG_REG_MASK(VPP_SC_MISC,VPP_SC_VERT_EN); */
			VSYNC_WR_MPEG_REG(VPP_SC_MISC + cur_dev->vpp_off,
					  READ_MPEG_REG(VPP_SC_MISC +
							cur_dev->vpp_off) &
					  (~VPP_SC_VERT_EN));
		}
#endif
		/* horitontal filter settings */
		VSYNC_WR_MPEG_REG_BITS(VPP_SC_MISC + cur_dev->vpp_off,
				       vpp_filter->vpp_horz_coeff[0],
				       VPP_SC_HBANK_LENGTH_BIT,
				       VPP_SC_BANK_LENGTH_WID);

		/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
		if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
		    && !is_meson_mtvd_cpu()) {
			VSYNC_WR_MPEG_REG_BITS(VPP_VSC_PHASE_CTRL +
				cur_dev->vpp_off,
				(vpp_filter->vpp_vert_coeff[0] == 2) ? 1 : 0,
				VPP_PHASECTL_DOUBLELINE_BIT,
				VPP_PHASECTL_DOUBLELINE_WID);
		}
		/* #endif */

		if (vpp_filter->vpp_horz_coeff[1] & 0x8000) {
			VSYNC_WR_MPEG_REG(VPP_SCALE_COEF_IDX +
				cur_dev->vpp_off,
				VPP_COEF_HORZ | VPP_COEF_9BIT);
		} else {
			VSYNC_WR_MPEG_REG(VPP_SCALE_COEF_IDX +
				cur_dev->vpp_off,
				VPP_COEF_HORZ);
		}

		for (i = 0; i < (vpp_filter->vpp_horz_coeff[1] & 0xff); i++) {
			VSYNC_WR_MPEG_REG(VPP_SCALE_COEF + cur_dev->vpp_off,
					  vpp_filter->vpp_horz_coeff[i + 2]);
		}

		/* vertical filter settings */
		VSYNC_WR_MPEG_REG_BITS(VPP_SC_MISC + cur_dev->vpp_off,
				       vpp_filter->vpp_vert_coeff[0],
				       VPP_SC_VBANK_LENGTH_BIT,
				       VPP_SC_BANK_LENGTH_WID);

		VSYNC_WR_MPEG_REG(VPP_SCALE_COEF_IDX + cur_dev->vpp_off,
				  VPP_COEF_VERT);
		for (i = 0; i < vpp_filter->vpp_vert_coeff[1]; i++) {
			VSYNC_WR_MPEG_REG(VPP_SCALE_COEF + cur_dev->vpp_off,
					  vpp_filter->vpp_vert_coeff[i + 2]);
		}
#if (!HAS_VPU_PROT)
		if (is_meson_gxbb_cpu()) {
			cur_frame_par->VPP_pic_in_height_ = zoom_end_y_lines -
				zoom_start_y_lines + 1;
			if (cur_dispbuf->type & VIDTYPE_MVC)
				cur_frame_par->VPP_pic_in_height_ *= 2;
			cur_frame_par->VPP_line_in_length_ = zoom_end_x_lines -
				zoom_start_x_lines + 1;
		}
#endif
		VSYNC_WR_MPEG_REG(VPP_PIC_IN_HEIGHT + cur_dev->vpp_off,
				  cur_frame_par->VPP_pic_in_height_);

		VSYNC_WR_MPEG_REG_BITS(VPP_HSC_PHASE_CTRL + cur_dev->vpp_off,
				       cur_frame_par->VPP_hf_ini_phase_,
				       VPP_HSC_TOP_INI_PHASE_BIT,
				       VPP_HSC_TOP_INI_PHASE_WID);
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_H_START_END +
				  cur_dev->vpp_off,
				  ((cur_frame_par->VPP_post_blend_vd_h_start_ &
				    VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT) |
				  ((cur_frame_par->VPP_post_blend_vd_h_end_ &
				    VPP_VD_SIZE_MASK)
				   << VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_VD1_V_START_END +
				  cur_dev->vpp_off,
				  ((cur_frame_par->VPP_post_blend_vd_v_start_ &
				    VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT) |
				  ((cur_frame_par->VPP_post_blend_vd_v_end_ &
				    VPP_VD_SIZE_MASK)
				   << VPP_VD1_END_BIT));
		VSYNC_WR_MPEG_REG(VPP_POSTBLEND_H_SIZE + cur_dev->vpp_off,
				  cur_frame_par->VPP_post_blend_h_size_);

		if ((cur_frame_par->VPP_post_blend_vd_v_end_ -
		     cur_frame_par->VPP_post_blend_vd_v_start_ + 1) > 1080) {
			VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_V_START_END +
			cur_dev->vpp_off,
			((cur_frame_par->VPP_post_blend_vd_v_start_ &
			VPP_VD_SIZE_MASK) << VPP_VD1_START_BIT) |
			((cur_frame_par->VPP_post_blend_vd_v_end_ &
			VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
		} else {
			VSYNC_WR_MPEG_REG(VPP_PREBLEND_VD1_V_START_END +
				cur_dev->vpp_off,
				((0 & VPP_VD_SIZE_MASK) <<
				VPP_VD1_START_BIT) | ((1079 &
				VPP_VD_SIZE_MASK) << VPP_VD1_END_BIT));
		}

		vpp_settings_h(cur_frame_par);
		vpp_settings_v(cur_frame_par);
		frame_par_ready_to_set = 0;
		frame_par_force_to_set = 0;
		first_set = 1;
	}
	/* VPP one time settings */
	wait_sync = 0;

	if (cur_dispbuf && cur_dispbuf->process_fun) {
		/* for new deinterlace driver */
#ifdef CONFIG_VSYNC_RDMA
		if (debug_flag & DEBUG_FLAG_PRINT_RDMA) {
			if (enable_rdma_log_count > 0)
				pr_info("call process_fun\n");
		}
#endif
		cur_dispbuf->process_fun(cur_dispbuf->private_data,
					 zoom_start_x_lines |
					 (cur_frame_par->vscale_skip_count <<
					  24) | (frame_par_di_set << 16),
					 zoom_end_x_lines, zoom_start_y_lines,
					 zoom_end_y_lines, cur_dispbuf);
	}

 exit:
	vpp_misc_save = READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off);
	vpp_misc_set = vpp_misc_save;
	if ((video_enabled == 1) && ((vpp_misc_save & VPP_VD1_POSTBLEND) == 0)
	&& (video_onoff_state == VIDEO_ENABLE_STATE_IDLE)) {
		SET_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off,
				VPP_VD1_PREBLEND | VPP_VD1_POSTBLEND
				   | VPP_POSTBLEND_EN);
		pr_info("VPP_VD1_POSTBLEND register rdma write fail!");
	}
	if ((video_enabled == 1) && cur_frame_par
	&& (cur_dispbuf != &vf_local) && (first_set == 0)
	&& (video_onoff_state == VIDEO_ENABLE_STATE_IDLE)) {
		struct vppfilter_mode_s *vpp_filter =
		    &cur_frame_par->vpp_filter;
		u32 h_phase_step , v_phase_step;
		h_phase_step = READ_VCBUS_REG(
		VPP_HSC_START_PHASE_STEP + cur_dev->vpp_off);
		v_phase_step = READ_VCBUS_REG(
		VPP_VSC_START_PHASE_STEP + cur_dev->vpp_off);
		if ((vpp_filter->vpp_hsc_start_phase_step != h_phase_step) ||
		(vpp_filter->vpp_vsc_start_phase_step != v_phase_step)) {
			video_property_changed = true;
			pr_info("frame info register rdma write fail!\n");
		}
	}
	if (likely(video_onoff_state != VIDEO_ENABLE_STATE_IDLE)) {
		/* state change for video layer enable/disable */

		spin_lock_irqsave(&video_onoff_lock, flags);

		if (video_onoff_state == VIDEO_ENABLE_STATE_ON_REQ) {
			/* the video layer is enabled one vsync later,assumming
			 * all registers are ready from RDMA.
			 */
			video_onoff_state = VIDEO_ENABLE_STATE_ON_PENDING;
		} else if (video_onoff_state ==
				VIDEO_ENABLE_STATE_ON_PENDING) {
#if 0
			SET_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off,
					VPP_VD1_PREBLEND | VPP_VD1_POSTBLEND
					   | VPP_POSTBLEND_EN);
#else
			vpp_misc_set |= VPP_VD1_PREBLEND | VPP_VD1_POSTBLEND |
				VPP_POSTBLEND_EN;
#endif
			video_onoff_state = VIDEO_ENABLE_STATE_IDLE;

			if (debug_flag & DEBUG_FLAG_BLACKOUT)
				pr_info("VsyncEnableVideoLayer\n");
			vpu_delay_work_flag |=
				VPU_VIDEO_LAYER1_CHANGED;
		} else if (video_onoff_state == VIDEO_ENABLE_STATE_OFF_REQ) {
			/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
			if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
			    && !is_meson_mtvd_cpu()) {
#if 0
				CLEAR_VCBUS_REG_MASK(VPP_MISC +
						     cur_dev->vpp_off,
						     VPP_VD1_PREBLEND |
						     VPP_VD2_PREBLEND |
						     VPP_VD2_POSTBLEND |
						     VPP_VD1_POSTBLEND);
#else
				vpp_misc_set &=	~(VPP_VD1_PREBLEND |
						  VPP_VD2_PREBLEND |
						  VPP_VD2_POSTBLEND |
						  VPP_VD1_POSTBLEND);
#endif
			} else {
				/* #else */
#if 0
				CLEAR_VCBUS_REG_MASK(VPP_MISC +
						     cur_dev->vpp_off,
						     VPP_VD1_PREBLEND |
						     VPP_VD2_PREBLEND |
						     VPP_VD2_POSTBLEND);
#else
				vpp_misc_set &= ~(VPP_VD1_PREBLEND |
						  VPP_VD2_PREBLEND |
						  VPP_VD2_POSTBLEND);
#endif
			}
			/* #endif */
			video_onoff_state = VIDEO_ENABLE_STATE_IDLE;
			vpu_delay_work_flag |=
				VPU_VIDEO_LAYER1_CHANGED;
			if (debug_flag & DEBUG_FLAG_BLACKOUT)
				pr_info("VsyncDisableVideoLayer\n");
		}

		spin_unlock_irqrestore(&video_onoff_lock, flags);
	}

	if (likely(video2_onoff_state != VIDEO_ENABLE_STATE_IDLE)) {
		/* state change for video layer2 enable/disable */

		spin_lock_irqsave(&video2_onoff_lock, flags);

		if (video2_onoff_state == VIDEO_ENABLE_STATE_ON_REQ) {
			/* the video layer 2
			is enabled one vsync later, assumming
			* all registers are ready from RDMA.
			*/
			video2_onoff_state = VIDEO_ENABLE_STATE_ON_PENDING;
		} else if (video2_onoff_state ==
				VIDEO_ENABLE_STATE_ON_PENDING) {
#if 0
			SET_VCBUS_REG_MASK(VPP_MISC + cur_dev->vpp_off,
					   VPP_PREBLEND_EN | VPP_VD2_PREBLEND |
					   (0x1ff << VPP_VD2_ALPHA_BIT));
#else
			vpp_misc_set |= VPP_PREBLEND_EN | VPP_VD2_PREBLEND |
					(0x1ff << VPP_VD2_ALPHA_BIT);
#endif
			video2_onoff_state = VIDEO_ENABLE_STATE_IDLE;

			if (debug_flag & DEBUG_FLAG_BLACKOUT)
				pr_info("VsyncEnableVideoLayer2\n");
		} else if (video2_onoff_state == VIDEO_ENABLE_STATE_OFF_REQ) {
			/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
			if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
			    && !is_meson_mtvd_cpu()) {
#if 0
				CLEAR_VCBUS_REG_MASK(VPP_MISC +
						     cur_dev->vpp_off,
						     VPP_VD2_PREBLEND |
						     VPP_VD2_POSTBLEND);
#else
				vpp_misc_set &= ~(VPP_VD2_PREBLEND |
						  VPP_VD2_POSTBLEND);
#endif
			} else {
				/* #else */
#if 0
				CLEAR_VCBUS_REG_MASK(VPP_MISC +
						     cur_dev->vpp_off,
						     VPP_VD2_PREBLEND |
						     VPP_VD2_POSTBLEND);
#else
				vpp_misc_set &= ~(VPP_VD2_PREBLEND |
						  VPP_VD2_POSTBLEND);
#endif
			}
			/* #endif */
			video2_onoff_state = VIDEO_ENABLE_STATE_IDLE;

			if (debug_flag & DEBUG_FLAG_BLACKOUT)
				pr_info("VsyncDisableVideoLayer2\n");
		}

		spin_unlock_irqrestore(&video2_onoff_lock, flags);
	}

	if (vpp_misc_save != vpp_misc_set) {
		VSYNC_WR_MPEG_REG(VPP_MISC + cur_dev->vpp_off,
			vpp_misc_set);
	}

#ifdef CONFIG_VSYNC_RDMA
	cur_rdma_buf = cur_dispbuf;
	/* vsync_rdma_config(); */
	vsync_rdma_process();
	if (frame_par_di_set) {
		start_rdma();
		/* work around, need set one frame without RDMA??? */
	}
	if (debug_flag & DEBUG_FLAG_PRINT_RDMA) {
		if (enable_rdma_log_count == 0)
			enable_rdma_log(0);
	}
	rdma_enable_pre = is_vsync_rdma_enable();
#endif

	if (timer_count > 50) {
		timer_count = 0;
		video_notify_flag |= VIDEO_NOTIFY_FRAME_WAIT;
#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
		if ((video_scaler_mode) && (scaler_pos_changed)) {
			video_notify_flag |= VIDEO_NOTIFY_POS_CHANGED;
			scaler_pos_changed = 0;
		} else {
			scaler_pos_changed = 0;
			video_notify_flag &= ~VIDEO_NOTIFY_POS_CHANGED;
		}
#endif
	}

	switch (READ_VCBUS_REG(VPU_VIU_VENC_MUX_CTRL) & 0x3) {
	case 0:
		enc_line = (READ_VCBUS_REG(ENCL_INFO_READ) >> 16) & 0x1fff;
		break;
	case 1:
		enc_line = (READ_VCBUS_REG(ENCI_INFO_READ) >> 16) & 0x1fff;
		break;
	case 2:
		enc_line = (READ_VCBUS_REG(ENCP_INFO_READ) >> 16) & 0x1fff;
		break;
	case 3:
		enc_line = (READ_VCBUS_REG(ENCT_INFO_READ) >> 16) & 0x1fff;
		break;
	}
	if (enc_line > vsync_exit_line_max)
		vsync_exit_line_max = enc_line;

#ifdef FIQ_VSYNC
	if (video_notify_flag)
		fiq_bridge_pulse_trigger(&vsync_fiq_bridge);
#else
	if (video_notify_flag)
		vsync_notify();

	/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
	if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8) && !is_meson_mtvd_cpu()) {
		if (vpu_delay_work_flag)
			schedule_work(&vpu_delay_work);
	}
	/* #endif */

	return IRQ_HANDLED;
#endif

}

#ifdef RESERVE_CLR_FRAME
static int free_alloced_keep_buffer(void)
{
	pr_info("free_alloced_keep_buffer %p.%p.%p\n",
		(void *)keep_y_addr, (void *)keep_u_addr, (void *)keep_v_addr);
	if (keep_y_addr) {
		codec_mm_free_for_dma(MEM_NAME, keep_y_addr);
		keep_y_addr = 0;
	}

	if (keep_u_addr) {
		codec_mm_free_for_dma(MEM_NAME, keep_u_addr);
		keep_u_addr = 0;
	}

	if (keep_v_addr) {
		codec_mm_free_for_dma(MEM_NAME, keep_v_addr);
		keep_v_addr = 0;
	}
	return 0;
}

static int alloc_keep_buffer(void)
{
	int flags = CODEC_MM_FLAGS_DMA |
		CODEC_MM_FLAGS_FOR_VDECODER;
#ifndef CONFIG_GE2D_KEEP_FRAME
	/*
		if not used ge2d.
		need CPU access.
	*/
	flags = CODEC_MM_FLAGS_DMA_CPU |
	CODEC_MM_FLAGS_FOR_VDECODER;
#endif
	if (!keep_y_addr) {
		keep_y_addr = codec_mm_alloc_for_dma(
				MEM_NAME,
				PAGE_ALIGN(Y_BUFFER_SIZE)/PAGE_SIZE, 0, flags);
		if (!keep_y_addr) {
			pr_err("%s: failed to alloc y addr\n", __func__);
			goto err1;
		}
	}

	if (!keep_u_addr) {
		keep_u_addr = codec_mm_alloc_for_dma(
				MEM_NAME,
				PAGE_ALIGN(U_BUFFER_SIZE)/PAGE_SIZE, 0, flags);
		if (!keep_u_addr) {
			pr_err("%s: failed to alloc u addr\n", __func__);
			goto err1;
		}
	}

	if (!keep_v_addr) {
		keep_v_addr = codec_mm_alloc_for_dma(
				MEM_NAME,
				PAGE_ALIGN(V_BUFFER_SIZE)/PAGE_SIZE, 0, flags);
		if (!keep_v_addr) {
			pr_err("%s: failed to alloc v addr\n", __func__);
			goto err1;
		}
	}
	pr_info("alloced keep buffer yaddr=%p,u_addr=%p,v_addr=%p\n",
		(void *)keep_y_addr,
		(void *)keep_u_addr,
		(void *)keep_v_addr);
	return 0;

 err1:
	free_alloced_keep_buffer();
	return -ENOMEM;
}

void try_free_keep_video(void)
{
	if (keep_video_on) {
		pr_info("disbled keep video before free keep buffer.\n");
		keep_video_on = 0;
		cur_dispbuf = NULL;
		if (!disable_video) {
			/*if not disable video,changed to 2 for */
			pr_info("disbled video for next before free keep buffer!\n");
			_video_set_disable(VIDEO_DISABLE_FORNEXT);
		} else if (video_enabled) {
			safe_disble_videolayer();
		}
	}
	free_alloced_keep_buffer();
	return;
}
#endif

void get_video_keep_buffer(ulong *addr, ulong *phys_addr)
{
#if 1
	if (addr) {
		addr[0] = (ulong) 0;
		addr[1] = (ulong) 0;
		addr[2] = (ulong) 0;
	}

	if (phys_addr) {
		if (!keep_y_addr || !keep_u_addr || !keep_v_addr)
			alloc_keep_buffer();
		phys_addr[0] = keep_y_addr;
		phys_addr[1] = keep_u_addr;
		phys_addr[2] = keep_v_addr;
	}
#endif
	if (debug_flag & DEBUG_FLAG_BLACKOUT) {
		pr_info("%s: y=%lx u=%lx v=%lx\n", __func__, phys_addr[0],
		       phys_addr[1], phys_addr[2]);
	}
}

/*********************************************************
 * FIQ Routines
 *********************************************************/

static void vsync_fiq_up(void)
{
#ifdef FIQ_VSYNC
	request_fiq(INT_VIU_VSYNC, &vsync_fisr);
#else
	int r;
	/*TODO irq     */
	r = vdec_request_irq(VSYNC_IRQ, &vsync_isr,
			"vsync", (void *)video_dev_id);

#ifdef CONFIG_MESON_TRUSTZONE
	if (num_online_cpus() > 1)
		irq_set_affinity(INT_VIU_VSYNC, cpumask_of(1));
#endif
#endif
}

static void vsync_fiq_down(void)
{
#ifdef FIQ_VSYNC
	free_fiq(INT_VIU_VSYNC, &vsync_fisr);
#else
	/*TODO irq */
	vdec_free_irq(VSYNC_IRQ, (void *)video_dev_id);
#endif
}

#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
static void vsync2_fiq_up(void)
{
	int r;
	r = request_irq(INT_VIU2_VSYNC, &vsync_isr,
			IRQF_SHARED, "vsync", (void *)video_dev_id2);
}

static void vsync2_fiq_down(void)
{
	free_irq(INT_VIU2_VSYNC, (void *)video_dev_id2);
}

#endif

int get_curren_frame_para(int *top, int *left, int *bottom, int *right)
{
	if (!cur_frame_par)
		return -1;
	*top = cur_frame_par->VPP_vd_start_lines_;
	*left = cur_frame_par->VPP_hd_start_lines_;
	*bottom = cur_frame_par->VPP_vd_end_lines_;
	*right = cur_frame_par->VPP_hd_end_lines_;
	return 0;
}

int get_current_vscale_skip_count(struct vframe_s *vf)
{
	static struct vpp_frame_par_s frame_par;

	vpp_set_filters(process_3d_type, wide_setting, vf, &frame_par, vinfo);

	return frame_par.vscale_skip_count;
}

int query_video_status(int type, int *value)
{
	if (value == NULL)
		return -1;
	switch (type) {
	case 0:
		*value = trickmode_fffb;
		break;
	default:
		break;
	}
	return 0;
}

static void video_vf_unreg_provider(void)
{
	ulong flags;

	new_frame_count = 0;

	atomic_set(&video_unreg_flag, 1);
	spin_lock_irqsave(&lock, flags);

#ifdef CONFIG_VSYNC_RDMA
	dispbuf_to_put_num = DISPBUF_TO_PUT_MAX;
	while (dispbuf_to_put_num > 0) {
		dispbuf_to_put_num--;
		dispbuf_to_put[dispbuf_to_put_num] = NULL;
	}
	cur_rdma_buf = NULL;
#endif
	if (cur_dispbuf) {
		vf_local = *cur_dispbuf;
		cur_dispbuf = &vf_local;
		cur_dispbuf->video_angle = 0;
	}

	if (trickmode_fffb) {
		atomic_set(&trickmode_framedone, 0);
		to_notify_trick_wait = false;
	}

	if (blackout | force_blackout) {
		safe_disble_videolayer();
		try_free_keep_video();
	}

	vsync_pts_100 = 0;
	vsync_pts_112 = 0;
	vsync_pts_125 = 0;
	vsync_freerun = 0;
	video_prot.video_started = 0;
	spin_unlock_irqrestore(&lock, flags);

#ifdef CONFIG_GE2D_KEEP_FRAME
	if (cur_dispbuf) {
		/* TODO: mod gate */
		/* switch_mod_gate_by_name("ge2d", 1); */
		vf_keep_current();
		/* TODO: mod gate */
		/* switch_mod_gate_by_name("ge2d", 0); */
	}
	if (hdmi_in_onvideo == 0)
		tsync_avevent(VIDEO_STOP, 0);
#else
	/* if (!trickmode_fffb) */
	if (cur_dispbuf)
		vf_keep_current();
	if (hdmi_in_onvideo == 0)
		tsync_avevent(VIDEO_STOP, 0);
#endif
	atomic_set(&video_unreg_flag, 0);
	enable_video_discontinue_report = 1;
}

static void video_vf_light_unreg_provider(void)
{
	ulong flags;

	spin_lock_irqsave(&lock, flags);
#ifdef CONFIG_VSYNC_RDMA
	dispbuf_to_put_num = DISPBUF_TO_PUT_MAX;
	while (dispbuf_to_put_num > 0) {
		dispbuf_to_put_num--;
		dispbuf_to_put[dispbuf_to_put_num] = NULL;
	}
	cur_rdma_buf = NULL;
#endif

	if (cur_dispbuf) {
		vf_local = *cur_dispbuf;
		cur_dispbuf = &vf_local;
	}
#if HAS_VPU_PROT
	if (has_vpu_prot()) {
		if (get_vpu_mem_pd_vmod(VPU_VIU_VD1) == VPU_MEM_POWER_DOWN ||
		    get_vpu_mem_pd_vmod(VPU_PIC_ROT2) == VPU_MEM_POWER_DOWN ||
		    READ_VCBUS_REG(VPU_PROT3_CLK_GATE) == 0) {
			{
				PROT_MEM_POWER_ON();
				/* VD1_MEM_POWER_ON(); */
				video_prot_gate_on();
				video_prot.video_started = 1;
				video_prot.angle_changed = 1;
			}
		}
	}
#endif
	spin_unlock_irqrestore(&lock, flags);
}

static int video_receiver_event_fun(int type, void *data, void *private_data)
{
#ifdef CONFIG_AM_VIDEO2
	char *provider_name;
#endif
	if (type == VFRAME_EVENT_PROVIDER_UNREG) {
		video_vf_unreg_provider();
#ifdef CONFIG_AM_VIDEO2
		set_clone_frame_rate(android_clone_rate, 200);
#endif
	} else if (type == VFRAME_EVENT_PROVIDER_RESET) {
		video_vf_light_unreg_provider();
	} else if (type == VFRAME_EVENT_PROVIDER_LIGHT_UNREG)
		video_vf_light_unreg_provider();
	else if (type == VFRAME_EVENT_PROVIDER_REG) {
		enable_video_discontinue_report = 1;
#ifdef CONFIG_AM_VIDEO2
		provider_name = (char *)data;
		if (strncmp(provider_name, "decoder", 7) == 0
		    || strncmp(provider_name, "ppmgr", 5) == 0
		    || strncmp(provider_name, "deinterlace", 11) == 0
		    || strncmp(provider_name, "d2d3", 11) == 0) {
			set_clone_frame_rate(noneseamless_play_clone_rate, 0);
			set_clone_frame_rate(video_play_clone_rate, 100);
		}
#endif
#ifdef TV_3D_FUNCTION_OPEN

		if ((process_3d_type & MODE_3D_FA) && !cur_dispbuf->trans_fmt)
			/*notify di 3d mode is frame
			alternative mode,passing two buffer in one frame */
			vf_notify_receiver_by_name("deinterlace",
				VFRAME_EVENT_PROVIDER_SET_3D_VFRAME_INTERLEAVE,
				(void *)1);
#endif

		video_vf_light_unreg_provider();
	} else if (type == VFRAME_EVENT_PROVIDER_FORCE_BLACKOUT) {
		force_blackout = 1;
		if (debug_flag & DEBUG_FLAG_BLACKOUT) {
			pr_info("%s VFRAME_EVENT_PROVIDER_FORCE_BLACKOUT\n",
			       __func__);
		}
	} else if (type == VFRAME_EVENT_PROVIDER_FR_HINT) {
#ifdef CONFIG_AM_VOUT
		if (data != NULL)
			set_vframe_rate_hint((unsigned long)(data));
#endif
	} else if (type == VFRAME_EVENT_PROVIDER_FR_END_HINT) {
#ifdef CONFIG_AM_VOUT
		set_vframe_rate_end_hint();
#endif
	}
	return 0;
}

static int video4osd_receiver_event_fun(int type, void *data,
					void *private_data)
{
	if (type == VFRAME_EVENT_PROVIDER_UNREG) {
		osd_prov = NULL;
		if (debug_flag & DEBUG_FLAG_BLACKOUT)
			pr_info("[video4osd] clear osd_prov\n");
	} else if (type == VFRAME_EVENT_PROVIDER_REG) {
		osd_prov = vf_get_provider(RECEIVER4OSD_NAME);

		if (debug_flag & DEBUG_FLAG_BLACKOUT)
			pr_info("[video4osd] set osd_prov\n");
	}
	return 0;
}

unsigned int get_post_canvas(void)
{
	return post_canvas;
}

static int canvas_dup(ulong dst, ulong src_paddr, ulong size)
{
	void *src_addr = codec_mm_phys_to_virt(src_paddr);
	void *dst_addr = codec_mm_phys_to_virt(dst);

	if (src_paddr && dst && src_addr && dst_addr) {
		dma_addr_t dma_addr = 0;
		memcpy(dst_addr, src_addr, size);
		dma_addr = dma_map_single(
					amports_get_dma_device(), dst_addr,
					size, DMA_TO_DEVICE);
		dma_unmap_single(amports_get_dma_device(), dma_addr,
					FETCHBUF_SIZE, DMA_TO_DEVICE);
		return 1;
	}

	return 0;
}
unsigned int vf_keep_current(void)
{
	u32 cur_index;
	u32 y_index, u_index, v_index;
	struct canvas_s cs0, cs1, cs2, cd;

	if (!cur_dispbuf) {
		pr_info("keep exit without cur_dispbuf\n");
		return 0;
	}

	if (cur_dispbuf->source_type == VFRAME_SOURCE_TYPE_OSD) {
		pr_info("keep exit is osd\n");
		return 0;
	}
	if (READ_VCBUS_REG(DI_IF1_GEN_REG) & 0x1) {
		pr_info("keep exit is di\n");
		return 0;
	}
	if (debug_flag & DEBUG_FLAG_TOGGLE_SKIP_KEEP_CURRENT) {
		pr_info("keep exit is skip current\n");
		return 0;
	}

#ifdef CONFIG_AM_VIDEOCAPTURE
	ext_frame_capture_poll(1);	/*pull  if have capture end frame */
#endif
	if (blackout | force_blackout) {
		pr_info("keep exit is skip current\n");
		return 0;
	}

	if (0 == (READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off) &
		VPP_VD1_POSTBLEND)) {
		pr_info("keep exit is skip VPP_VD1_POSTBLEND\n");
		return 0;
	}

	if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB) &&
		(cur_dispbuf->type & VIDTYPE_COMPRESS)) {
		/* todo: duplicate compressed video frame */
		pr_info("keep exit is skip VIDTYPE_COMPRESS\n");
		return -1;
	}
	cur_index = READ_VCBUS_REG(VD1_IF0_CANVAS0 + cur_dev->viu_off);
	y_index = cur_index & 0xff;
	u_index = (cur_index >> 8) & 0xff;
	v_index = (cur_index >> 16) & 0xff;
	canvas_read(y_index, &cd);

	if ((cd.width * cd.height) <= 2048 * 1088 &&
			!keep_y_addr) {
		alloc_keep_buffer();
	}
	if (!keep_y_addr
	    || (cur_dispbuf->type & VIDTYPE_VIU_422) == VIDTYPE_VIU_422) {
		/* no support VIDTYPE_VIU_422... */
		return -1;
	}


	if (debug_flag & DEBUG_FLAG_BLACKOUT) {
		pr_info("%s keep_y_addr=%p %x\n", __func__, (void *)keep_y_addr,
		       canvas_get_addr(y_index));
	}

	if ((cur_dispbuf->type & VIDTYPE_VIU_422) == VIDTYPE_VIU_422) {
		return -1;
		/* no VIDTYPE_VIU_422 type frame need keep,avoid memcpy crash*/
		if ((Y_BUFFER_SIZE < (cd.width * cd.height))) {
			pr_info("[%s::%d]data > buf size: %x,%x,%x, %x,%x\n",
				__func__, __LINE__, Y_BUFFER_SIZE,
				U_BUFFER_SIZE, V_BUFFER_SIZE,
				cd.width, cd.height);
			return -1;
		}
		if (keep_phy_addr(keep_y_addr) != canvas_get_addr(y_index) &&
				canvas_dup(keep_phy_addr(keep_y_addr),
				canvas_get_addr(y_index),
				(cd.width) * (cd.height))) {
#ifdef CONFIG_VSYNC_RDMA
			canvas_update_addr(disp_canvas_index[0][0],
					   keep_phy_addr(keep_y_addr));
			canvas_update_addr(disp_canvas_index[1][0],
					   keep_phy_addr(keep_y_addr));
#else
			canvas_update_addr(y_index,
				keep_phy_addr(keep_y_addr));
#endif
			if (debug_flag & DEBUG_FLAG_BLACKOUT)
				pr_info("%s: VIDTYPE_VIU_422\n", __func__);
		}
	} else if ((cur_dispbuf->type & VIDTYPE_VIU_444) == VIDTYPE_VIU_444) {
		if ((Y_BUFFER_SIZE < (cd.width * cd.height))) {
			pr_info
			    ("[%s::%d] error:data>buf size: %x,%x,%x, %x,%x\n",
			     __func__, __LINE__, Y_BUFFER_SIZE,
			     U_BUFFER_SIZE, V_BUFFER_SIZE,
				cd.width, cd.height);
			return -1;
		}
#ifdef CONFIG_GE2D_KEEP_FRAME
		ge2d_keeplastframe_block(cur_index, GE2D_FORMAT_M24_YUV444);
#else
		if (keep_phy_addr(keep_y_addr) != canvas_get_addr(y_index) &&
				canvas_dup(keep_phy_addr(keep_y_addr),
				canvas_get_addr(y_index),
				(cd.width) * (cd.height))) {
#ifdef CONFIG_VSYNC_RDMA
			canvas_update_addr(disp_canvas_index[0][0],
					   keep_phy_addr(keep_y_addr));
			canvas_update_addr(disp_canvas_index[1][0],
					   keep_phy_addr(keep_y_addr));
#else
			canvas_update_addr(y_index,
					keep_phy_addr(keep_y_addr));
#endif
		}
#endif
		if (debug_flag & DEBUG_FLAG_BLACKOUT)
			pr_info("%s: VIDTYPE_VIU_444\n", __func__);
	} else if ((cur_dispbuf->type & VIDTYPE_VIU_NV21) == VIDTYPE_VIU_NV21) {
		canvas_read(y_index, &cs0);
		canvas_read(u_index, &cs1);
		if ((Y_BUFFER_SIZE < (cs0.width * cs0.height))
		    || (U_BUFFER_SIZE < (cs1.width * cs1.height))) {
			pr_info("## [%s::%d] error: yuv data size larger",
				__func__, __LINE__);
			return -1;
		}
#ifdef CONFIG_GE2D_KEEP_FRAME
		ge2d_keeplastframe_block(cur_index, GE2D_FORMAT_M24_NV21);
#else
		if (keep_phy_addr(keep_y_addr) != canvas_get_addr(y_index) &&
		    canvas_dup(keep_phy_addr(keep_y_addr),
					canvas_get_addr(y_index),
					(cs0.width * cs0.height))
		    && canvas_dup(keep_phy_addr(keep_u_addr),
					canvas_get_addr(u_index),
					(cs1.width * cs1.height))) {
#ifdef CONFIG_VSYNC_RDMA
			canvas_update_addr(disp_canvas_index[0][0],
					   keep_phy_addr(keep_y_addr));
			canvas_update_addr(disp_canvas_index[1][0],
					   keep_phy_addr(keep_y_addr));
			canvas_update_addr(disp_canvas_index[0][1],
					   keep_phy_addr(keep_u_addr));
			canvas_update_addr(disp_canvas_index[1][1],
					   keep_phy_addr(keep_u_addr));
#else
			canvas_update_addr(y_index,
				keep_phy_addr(keep_y_addr));
			canvas_update_addr(u_index,
				keep_phy_addr(keep_u_addr));
#endif
		}
#endif
		if (debug_flag & DEBUG_FLAG_BLACKOUT)
			pr_info("%s: VIDTYPE_VIU_NV21\n", __func__);
	} else {
		canvas_read(y_index, &cs0);
		canvas_read(u_index, &cs1);
		canvas_read(v_index, &cs2);

		if ((Y_BUFFER_SIZE < (cs0.width * cs0.height))
		    || (U_BUFFER_SIZE < (cs1.width * cs1.height))
		    || (V_BUFFER_SIZE < (cs2.width * cs2.height))) {
			pr_info("## [%s::%d] error: yuv data size larger than buf size: %x,%x,%x, %x,%x, %x,%x, %x,%x,\n",
			__func__, __LINE__, Y_BUFFER_SIZE,
			U_BUFFER_SIZE, V_BUFFER_SIZE, cs0.width,
			cs0.height, cs1.width, cs1.height, cs2.width,
			cs2.height);
			return -1;
		}
#ifdef CONFIG_GE2D_KEEP_FRAME
		ge2d_keeplastframe_block(cur_index, GE2D_FORMAT_M24_YUV420);
#else
		if (keep_phy_addr(keep_y_addr) != canvas_get_addr(y_index) &&
			/*must not the same address */
		    canvas_dup(keep_phy_addr(keep_y_addr),
					canvas_get_addr(y_index),
					(cs0.width * cs0.height))
		    && canvas_dup(keep_phy_addr(keep_u_addr),
					canvas_get_addr(u_index),
					(cs1.width * cs1.height))
			&& canvas_dup(keep_phy_addr(keep_v_addr),
					canvas_get_addr(v_index),
					(cs2.width * cs2.height))) {
#ifdef CONFIG_VSYNC_RDMA
			canvas_update_addr(disp_canvas_index[0][0],
					   keep_phy_addr(keep_y_addr));
			canvas_update_addr(disp_canvas_index[1][0],
					   keep_phy_addr(keep_y_addr));
			canvas_update_addr(disp_canvas_index[0][1],
					   keep_phy_addr(keep_u_addr));
			canvas_update_addr(disp_canvas_index[1][1],
					   keep_phy_addr(keep_u_addr));
			canvas_update_addr(disp_canvas_index[0][2],
					   keep_phy_addr(keep_v_addr));
			canvas_update_addr(disp_canvas_index[1][2],
					   keep_phy_addr(keep_v_addr));
#else
			canvas_update_addr(y_index,
				keep_phy_addr(keep_y_addr));
			canvas_update_addr(u_index,
				keep_phy_addr(keep_u_addr));
			canvas_update_addr(v_index,
				keep_phy_addr(keep_v_addr));
#endif
		}

		if (debug_flag & DEBUG_FLAG_BLACKOUT)
			pr_info("%s: VIDTYPE_VIU_420\n", __func__);
#endif
	}
	keep_video_on = 1;
	pr_info("%s: keep video on with keep\n", __func__);
	return 0;

}

u32 get_blackout_policy(void)
{
	return blackout;
}
EXPORT_SYMBOL(get_blackout_policy);

u32 set_blackout_policy(int policy)
{
	blackout = policy;
	return 0;
}
EXPORT_SYMBOL(set_blackout_policy);

u8 is_vpp_postblend(void)
{
	if (READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off) & VPP_VD1_POSTBLEND)
		return 1;
	return 0;
}
EXPORT_SYMBOL(is_vpp_postblend);

void pause_video(unsigned char pause_flag)
{
	atomic_set(&video_pause_flag, pause_flag ? 1 : 0);
}
EXPORT_SYMBOL(pause_video);
/*********************************************************
 * Utilities
 *********************************************************/
int _video_set_disable(u32 val)
{
	if ((val < VIDEO_DISABLE_NONE) || (val > VIDEO_DISABLE_FORNEXT))
		return -EINVAL;

	disable_video = val;

	if (disable_video != VIDEO_DISABLE_NONE) {
		safe_disble_videolayer();

		if ((disable_video == VIDEO_DISABLE_FORNEXT) && cur_dispbuf
		    && (cur_dispbuf != &vf_local))
			video_property_changed = true;

	} else {
		if (cur_dispbuf && (cur_dispbuf != &vf_local))
			EnableVideoLayer();
	}

	return 0;
}

static void _set_video_crop(int *p)
{
	vpp_set_video_source_crop(p[0], p[1], p[2], p[3]);

	video_property_changed = true;
}

static void _set_video_window(int *p)
{
	int w, h;
	int *parsed = p;
#ifdef TV_REVERSE
	int temp, temp1;
	const struct vinfo_s *info = get_current_vinfo();

	/* pr_info(KERN_DEBUG "%s: %u
	get vinfo(%d,%d).\n", __func__, __LINE__,
	info->width, info->height); */
	if (reverse) {
		temp = parsed[0];
		temp1 = parsed[1];
		parsed[0] = info->width - parsed[2] - 1;
		parsed[1] = info->height - parsed[3] - 1;
		parsed[2] = info->width - temp - 1;
		parsed[3] = info->height - temp1 - 1;
	}
#endif
	if (parsed[0] < 0 && parsed[2] < 2) {
		parsed[2] = 2;
		parsed[0] = 0;
	}
	if (parsed[1] < 0 && parsed[3] < 2) {
		parsed[3] = 2;
		parsed[1] = 0;
	}
	w = parsed[2] - parsed[0] + 1;
	h = parsed[3] - parsed[1] + 1;

#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
	if (video_scaler_mode) {
		if ((w == 1) && (h == 1)) {
			w = 0;
			h = 0;
		}
		if ((content_left != parsed[0]) || (content_top != parsed[1])
		    || (content_w != w) || (content_h != h))
			scaler_pos_changed = 1;
		content_left = parsed[0];
		content_top = parsed[1];
		content_w = w;
		content_h = h;
		/* video_notify_flag =
		video_notify_flag|VIDEO_NOTIFY_POS_CHANGED; */
	} else
#endif
	{
		if ((w == 1) && (h == 1)) {
			w = h = 0;
			vpp_set_video_layer_position(parsed[0], parsed[1], 0,
						     0);
		} else if ((w > 0) && (h > 0)) {
			vpp_set_video_layer_position(parsed[0], parsed[1], w,
						     h);
		}
	}
	video_property_changed = true;
}

/*********************************************************
 * /dev/amvideo APIs
 *********************************************************/
static int amvideo_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int amvideo_poll_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int amvideo_release(struct inode *inode, struct file *file)
{
	if (blackout | force_blackout) {
		/*	DisableVideoLayer();
		don't need it ,it have problem on  pure music playing */
	}
	return 0;
}

static int amvideo_poll_release(struct inode *inode, struct file *file)
{
	if (blackout | force_blackout) {
		/*	DisableVideoLayer();
		don't need it ,it have problem on  pure music playing */
	}
	return 0;
}

static long amvideo_ioctl(struct file *file, unsigned int cmd, ulong arg)
{
	long ret = 0;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case AMSTREAM_IOC_SET_OMX_VPTS:{
			u32 pts;
			get_user(pts, (u32 __user *)argp);
			omx_pts = pts;
		}
		break;

	case AMSTREAM_IOC_GET_OMX_VPTS:
		put_user(omx_pts, (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_TRICKMODE:
		if (arg == TRICKMODE_I)
			trickmode_i = 1;
		else if (arg == TRICKMODE_FFFB)
			trickmode_fffb = 1;
		else {
			trickmode_i = 0;
			trickmode_fffb = 0;
		}
		to_notify_trick_wait = false;
		atomic_set(&trickmode_framedone, 0);
		tsync_trick_mode(trickmode_fffb);
		break;

	case AMSTREAM_IOC_TRICK_STAT:
		put_user(atomic_read(&trickmode_framedone),
			 (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_GET_TRICK_VPTS:
		put_user(trickmode_vpts, (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_VPAUSE:
		tsync_avevent(VIDEO_PAUSE, arg);
		break;

	case AMSTREAM_IOC_AVTHRESH:
		tsync_set_avthresh(arg);
		break;

	case AMSTREAM_IOC_SYNCTHRESH:
		tsync_set_syncthresh(arg);
		break;

	case AMSTREAM_IOC_SYNCENABLE:
		tsync_set_enable(arg);
		break;

	case AMSTREAM_IOC_SET_SYNC_ADISCON:
		tsync_set_sync_adiscont(arg);
		break;

	case AMSTREAM_IOC_SET_SYNC_VDISCON:
		tsync_set_sync_vdiscont(arg);
		break;

	case AMSTREAM_IOC_GET_SYNC_ADISCON:
		put_user(tsync_get_sync_adiscont(), (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_GET_SYNC_VDISCON:
		put_user(tsync_get_sync_vdiscont(), (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_GET_SYNC_ADISCON_DIFF:
		put_user(tsync_get_sync_adiscont_diff(), (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_GET_SYNC_VDISCON_DIFF:
		put_user(tsync_get_sync_vdiscont_diff(), (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_SET_SYNC_ADISCON_DIFF:
		tsync_set_sync_adiscont_diff(arg);
		break;

	case AMSTREAM_IOC_SET_SYNC_VDISCON_DIFF:
		tsync_set_sync_vdiscont_diff(arg);
		break;

	case AMSTREAM_IOC_VF_STATUS:{
			struct vframe_states vfsta;
			struct vframe_states states;
			vf_get_states(&vfsta);
			states.vf_pool_size = vfsta.vf_pool_size;
			states.buf_avail_num = vfsta.buf_avail_num;
			states.buf_free_num = vfsta.buf_free_num;
			states.buf_recycle_num = vfsta.buf_recycle_num;
			if (copy_to_user(argp, &states, sizeof(states)))
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_GET_VIDEO_DISABLE:
		put_user(disable_video, (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_SET_VIDEO_DISABLE:
		ret = _video_set_disable(arg);
		break;

	case AMSTREAM_IOC_GET_VIDEO_DISCONTINUE_REPORT:
		put_user(enable_video_discontinue_report, (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_SET_VIDEO_DISCONTINUE_REPORT:
		enable_video_discontinue_report = (arg == 0) ? 0 : 1;
		break;

	case AMSTREAM_IOC_GET_VIDEO_AXIS:{
			int axis[4];
#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
			if (video_scaler_mode) {
				axis[0] = content_left;
				axis[1] = content_top;
				axis[2] = content_w;
				axis[3] = content_h;
			} else
#endif
			{
				vpp_get_video_layer_position(
					&axis[0], &axis[1],
					&axis[2],
					&axis[3]);
			}

			axis[2] = axis[0] + axis[2] - 1;
			axis[3] = axis[1] + axis[3] - 1;

			if (copy_to_user(argp, &axis[0], sizeof(axis)) != 0)
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_SET_VIDEO_AXIS:{
			int axis[4];
			if (copy_from_user(axis, argp, sizeof(axis)) == 0)
				_set_video_window(axis);
			else
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_GET_VIDEO_CROP:{
			int crop[4];
			{
				vpp_get_video_source_crop(&crop[0], &crop[1],
							  &crop[2], &crop[3]);
			}

			if (copy_to_user(argp, &crop[0], sizeof(crop)) != 0)
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_SET_VIDEO_CROP:{
			int crop[4];
			if (copy_from_user(crop, argp, sizeof(crop)) == 0)
				_set_video_crop(crop);
			else
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_GET_SCREEN_MODE:
		if (copy_to_user(argp, &wide_setting, sizeof(u32)) != 0)
			ret = -EFAULT;
		break;

	case AMSTREAM_IOC_SET_SCREEN_MODE:{
			u32 mode;
			if (copy_from_user(&mode, argp, sizeof(u32)) == 0) {
				if (mode >= VIDEO_WIDEOPTION_MAX)
					ret = -EINVAL;
				else if (mode != wide_setting) {
					wide_setting = mode;
					video_property_changed = true;
				}
			} else
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_GET_BLACKOUT_POLICY:
		if (copy_to_user(argp, &blackout, sizeof(u32)) != 0)
			ret = -EFAULT;
		break;

	case AMSTREAM_IOC_SET_BLACKOUT_POLICY:{
			u32 mode;
			if (copy_from_user(&mode, argp, sizeof(u32)) == 0) {
				if (mode > 2)
					ret = -EINVAL;
				else
					blackout = mode;
			} else
				ret = -EFAULT;
		}
		break;

	case AMSTREAM_IOC_CLEAR_VBUF:{
			unsigned long flags;
			spin_lock_irqsave(&lock, flags);
			cur_dispbuf = NULL;
			spin_unlock_irqrestore(&lock, flags);
		}
		break;

	case AMSTREAM_IOC_CLEAR_VIDEO:
		if (blackout)
			safe_disble_videolayer();
		break;

	case AMSTREAM_IOC_SET_FREERUN_MODE:
		if (arg > FREERUN_DUR)
			ret = -EFAULT;
		else
			freerun_mode = arg;
		break;

	case AMSTREAM_IOC_GET_FREERUN_MODE:
		put_user(freerun_mode, (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_DISABLE_SLOW_SYNC:
		if (arg)
			disable_slow_sync = 1;
		else
			disable_slow_sync = 0;
		break;
	/****************************************************************
	3d process ioctl
	*****************************************************************/
	case AMSTREAM_IOC_SET_3D_TYPE:
		{
#ifdef TV_3D_FUNCTION_OPEN
			unsigned int set_3d =
				VFRAME_EVENT_PROVIDER_SET_3D_VFRAME_INTERLEAVE,
			unsigned int type = (unsigned int)arg;
			if (type != process_3d_type) {
				process_3d_type = type;
				if (mvc_flag)
					process_3d_type |= MODE_3D_MVC;
				video_property_changed = true;
				if ((process_3d_type & MODE_3D_FA)
						&& !cur_dispbuf->trans_fmt)
					/*notify di 3d mode is frame
					  alternative mode,passing two
					  buffer in one frame */
					vf_notify_receiver_by_name(
							"deinterlace",
							set_3d,
							(void *)1);
				else
					vf_notify_receiver_by_name(
							"deinterlace",
							set_3d,
							(void *)0);
			}
#endif
			break;
		}
	case AMSTREAM_IOC_GET_3D_TYPE:
#ifdef TV_3D_FUNCTION_OPEN
		put_user(process_3d_type, (u32 __user *)argp);

#endif
		break;
	case AMSTREAM_IOC_SET_VSYNC_UPINT:
		vsync_pts_inc_upint = arg;
		break;

	case AMSTREAM_IOC_GET_VSYNC_SLOW_FACTOR:
		put_user(vsync_slow_factor, (u32 __user *)argp);
		break;

	case AMSTREAM_IOC_SET_VSYNC_SLOW_FACTOR:
		vsync_slow_factor = arg;
		break;

	default:
		return -EINVAL;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long amvideo_compat_ioctl(struct file *file, unsigned int cmd, ulong arg)
{
	long ret = 0;

	switch (cmd) {
	case AMSTREAM_IOC_SET_OMX_VPTS:
	case AMSTREAM_IOC_GET_OMX_VPTS:
	case AMSTREAM_IOC_TRICK_STAT:
	case AMSTREAM_IOC_GET_TRICK_VPTS:
	case AMSTREAM_IOC_GET_SYNC_ADISCON:
	case AMSTREAM_IOC_GET_SYNC_VDISCON:
	case AMSTREAM_IOC_GET_SYNC_ADISCON_DIFF:
	case AMSTREAM_IOC_GET_SYNC_VDISCON_DIFF:
	case AMSTREAM_IOC_VF_STATUS:
	case AMSTREAM_IOC_GET_VIDEO_DISABLE:
	case AMSTREAM_IOC_GET_VIDEO_DISCONTINUE_REPORT:
	case AMSTREAM_IOC_GET_VIDEO_AXIS:
	case AMSTREAM_IOC_SET_VIDEO_AXIS:
	case AMSTREAM_IOC_GET_VIDEO_CROP:
	case AMSTREAM_IOC_SET_VIDEO_CROP:
	case AMSTREAM_IOC_GET_SCREEN_MODE:
	case AMSTREAM_IOC_SET_SCREEN_MODE:
	case AMSTREAM_IOC_GET_BLACKOUT_POLICY:
	case AMSTREAM_IOC_SET_BLACKOUT_POLICY:
	case AMSTREAM_IOC_GET_FREERUN_MODE:
	case AMSTREAM_IOC_GET_3D_TYPE:
	case AMSTREAM_IOC_GET_VSYNC_SLOW_FACTOR:
		arg = (unsigned long) compat_ptr(arg);
	case AMSTREAM_IOC_TRICKMODE:
	case AMSTREAM_IOC_VPAUSE:
	case AMSTREAM_IOC_AVTHRESH:
	case AMSTREAM_IOC_SYNCTHRESH:
	case AMSTREAM_IOC_SYNCENABLE:
	case AMSTREAM_IOC_SET_SYNC_ADISCON:
	case AMSTREAM_IOC_SET_SYNC_VDISCON:
	case AMSTREAM_IOC_SET_SYNC_ADISCON_DIFF:
	case AMSTREAM_IOC_SET_SYNC_VDISCON_DIFF:
	case AMSTREAM_IOC_SET_VIDEO_DISABLE:
	case AMSTREAM_IOC_SET_VIDEO_DISCONTINUE_REPORT:
	case AMSTREAM_IOC_CLEAR_VBUF:
	case AMSTREAM_IOC_CLEAR_VIDEO:
	case AMSTREAM_IOC_SET_FREERUN_MODE:
	case AMSTREAM_IOC_DISABLE_SLOW_SYNC:
	case AMSTREAM_IOC_SET_3D_TYPE:
	case AMSTREAM_IOC_SET_VSYNC_UPINT:
	case AMSTREAM_IOC_SET_VSYNC_SLOW_FACTOR:
		return amvideo_ioctl(file, cmd, arg);
	default:
		return -EINVAL;
	}

	return ret;
}
#endif

static unsigned int amvideo_poll(struct file *file, poll_table *wait_table)
{
	poll_wait(file, &amvideo_trick_wait, wait_table);

	if (atomic_read(&trickmode_framedone)) {
		atomic_set(&trickmode_framedone, 0);
		return POLLOUT | POLLWRNORM;
	}

	return 0;
}

static unsigned int amvideo_poll_poll(struct file *file, poll_table *wait_table)
{
	poll_wait(file, &amvideo_sizechange_wait, wait_table);

	if (atomic_read(&video_sizechange)) {
		atomic_set(&video_sizechange, 0);
		return POLLIN | POLLWRNORM;
	}

	return 0;
}

static const struct file_operations amvideo_fops = {
	.owner = THIS_MODULE,
	.open = amvideo_open,
	.release = amvideo_release,
	.unlocked_ioctl = amvideo_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = amvideo_compat_ioctl,
#endif
	.poll = amvideo_poll,
};

static const struct file_operations amvideo_poll_fops = {
	.owner = THIS_MODULE,
	.open = amvideo_poll_open,
	.release = amvideo_poll_release,
	.poll = amvideo_poll_poll,
};

/*********************************************************
 * SYSFS property functions
 *********************************************************/
#define MAX_NUMBER_PARA 10
#define AMVIDEO_CLASS_NAME "video"
#define AMVIDEO_POLL_CLASS_NAME "video_poll"

static int parse_para(const char *para, int para_num, int *result)
{
	char *token = NULL;
	char *params, *params_base;
	int *out = result;
	int len = 0, count = 0;
	int res = 0;
	int ret = 0;

	if (!para)
		return 0;

	params = kstrdup(para, GFP_KERNEL);
	params_base = params;
	token = params;
	len = strlen(token);
	do {
		token = strsep(&params, " ");
		while (token && (isspace(*token)
				|| !isgraph(*token)) && len) {
			token++;
			len--;
		}
		if (len == 0)
			break;
		ret = kstrtoint(token, 0, &res);
		if (ret < 0)
			break;
		len = strlen(token);
		*out++ = res;
		count++;
	} while ((token) && (count < para_num) && (len > 0));

	kfree(params_base);
	return count;
}

static void set_video_crop(const char *para)
{
	int parsed[4];

	if (likely(parse_para(para, 4, parsed) == 4))
		_set_video_crop(parsed);
	amlog_mask(LOG_MASK_SYSFS,
		   "video crop=>x0:%d,y0:%d,x1:%d,y1:%d\n ",
		   parsed[0], parsed[1], parsed[2], parsed[3]);
}

static void set_video_speed_check(const char *para)
{
	int parsed[2];

	if (likely(parse_para(para, 2, parsed) == 2))
		vpp_set_video_speed_check(parsed[0], parsed[1]);
	amlog_mask(LOG_MASK_SYSFS,
		   "video speed_check=>h:%d,w:%d\n ", parsed[0], parsed[1]);
}

static void set_video_window(const char *para)
{
	int parsed[4];

	if (likely(parse_para(para, 4, parsed) == 4))
		_set_video_window(parsed);
	amlog_mask(LOG_MASK_SYSFS,
		   "video=>x0:%d,y0:%d,x1:%d,y1:%d\n ",
		   parsed[0], parsed[1], parsed[2], parsed[3]);
}

static ssize_t video_3d_scale_store(struct class *cla,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
#ifdef TV_3D_FUNCTION_OPEN
	u32 enable;
	size_t r;
	r = sscanf(buf, "%u\n", &enable);
	if (r != 1)
		return -EINVAL;
	vpp_set_3d_scale(enable);
	video_property_changed = true;
	amlog_mask(LOG_MASK_SYSFS, "%s:%s 3d scale.\n", __func__,
		   enable ? "enable" : "disable");
#endif
	return count;
}

static ssize_t video_crop_show(struct class *cla, struct class_attribute *attr,
			       char *buf)
{
	u32 t, l, b, r;

	vpp_get_video_source_crop(&t, &l, &b, &r);
	return snprintf(buf, 40, "%d %d %d %d\n", t, l, b, r);
}

static ssize_t video_crop_store(struct class *cla,
		struct class_attribute *attr,
		const char *buf, size_t count)
{
	mutex_lock(&video_module_mutex);

	set_video_crop(buf);

	mutex_unlock(&video_module_mutex);

	return strnlen(buf, count);
}

static ssize_t video_state_show(struct class *cla,
		struct class_attribute *attr,
		char *buf)
{
	ssize_t len = 0;
	struct vppfilter_mode_s *vpp_filter = NULL;
	if (!cur_frame_par)
		return len;
	vpp_filter = &cur_frame_par->vpp_filter;
	len += sprintf(buf + len,
		"zoom_start_x_lines:%u.zoom_end_x_lines:%u.\n",
		zoom_start_x_lines, zoom_end_x_lines);
	len += sprintf(buf + len,
		"zoom_start_y_lines:%u.zoom_end_y_lines:%u.\n",
		    zoom_start_y_lines, zoom_end_y_lines);
	len += sprintf(buf + len, "frame parameters: pic_in_height %u.\n",
		    cur_frame_par->VPP_pic_in_height_);
	len += sprintf(buf + len,
		"frame parameters: VPP_line_in_length_ %u.\n",
		cur_frame_par->VPP_line_in_length_);
	len += sprintf(buf + len, "vscale_skip_count %u.\n",
		    cur_frame_par->vscale_skip_count);
	len += sprintf(buf + len, "hscale_skip_count %u.\n",
		    cur_frame_par->hscale_skip_count);
#ifdef TV_3D_FUNCTION_OPEN
	len += sprintf(buf + len, "vpp_2pic_mode %u.\n",
		    cur_frame_par->vpp_2pic_mode);
	len += sprintf(buf + len, "vpp_3d_scale %u.\n",
		    cur_frame_par->vpp_3d_scale);
	len += sprintf(buf + len,
		"vpp_3d_mode %u.\n", cur_frame_par->vpp_3d_mode);
#endif
	len +=
	    sprintf(buf + len, "hscale phase step 0x%x.\n",
		    vpp_filter->vpp_hsc_start_phase_step);
	len +=
	    sprintf(buf + len, "vscale phase step 0x%x.\n",
		    vpp_filter->vpp_vsc_start_phase_step);
	len +=
	    sprintf(buf + len, "pps pre hsc enable %d.\n",
		    vpp_filter->vpp_pre_hsc_en);
	len +=
	    sprintf(buf + len, "pps pre vsc enable %d.\n",
		    vpp_filter->vpp_pre_vsc_en);
	return len;
}

static ssize_t video_axis_show(struct class *cla,
		struct class_attribute *attr,
		char *buf)
{
	int x, y, w, h;
#ifdef CONFIG_POST_PROCESS_MANAGER_PPSCALER
	if (video_scaler_mode) {
		x = content_left;
		y = content_top;
		w = content_w;
		h = content_h;
	} else
#endif
	{
		vpp_get_video_layer_position(&x, &y, &w, &h);
	}
	return snprintf(buf, 40, "%d %d %d %d\n", x, y, x + w - 1, y + h - 1);
}

static ssize_t video_axis_store(struct class *cla,
		struct class_attribute *attr,
			const char *buf, size_t count)
{
	mutex_lock(&video_module_mutex);

	set_video_window(buf);

	mutex_unlock(&video_module_mutex);

	return strnlen(buf, count);
}

static ssize_t video_global_offset_show(struct class *cla,
			struct class_attribute *attr, char *buf)
{
	int x, y;
	vpp_get_global_offset(&x, &y);

	return snprintf(buf, 40, "%d %d\n", x, y);
}

static ssize_t video_global_offset_store(struct class *cla,
					 struct class_attribute *attr,
					 const char *buf, size_t count)
{
	int parsed[2];

	mutex_lock(&video_module_mutex);

	if (likely(parse_para(buf, 2, parsed) == 2)) {
		vpp_set_global_offset(parsed[0], parsed[1]);
		video_property_changed = true;

		amlog_mask(LOG_MASK_SYSFS,
			   "video_offset=>x0:%d,y0:%d\n ",
			   parsed[0], parsed[1]);
	}

	mutex_unlock(&video_module_mutex);

	return count;
}

static ssize_t video_zoom_show(struct class *cla,
			struct class_attribute *attr,
			char *buf)
{
	u32 r = vpp_get_zoom_ratio();

	return snprintf(buf, 40, "%d\n", r);
}

static ssize_t video_zoom_store(struct class *cla,
			struct class_attribute *attr,
			const char *buf, size_t count)
{
	unsigned long r;
	int ret = 0;

	ret = kstrtoul(buf, 0, (unsigned long *)&r);
	if (ret < 0)
		return -EINVAL;

	if ((r <= MAX_ZOOM_RATIO) && (r != vpp_get_zoom_ratio())) {
		vpp_set_zoom_ratio(r);
		video_property_changed = true;
	}

	return count;
}

static ssize_t video_screen_mode_show(struct class *cla,
				      struct class_attribute *attr, char *buf)
{
	const char *wide_str[] = {
		"normal", "full stretch", "4-3", "16-9", "non-linear",
		"normal-noscaleup",
		"4-3 ignore", "4-3 letter box", "4-3 pan scan", "4-3 combined",
		"16-9 ignore", "16-9 letter box", "16-9 pan scan",
		"16-9 combined"
	};

	if (wide_setting < ARRAY_SIZE(wide_str)) {
		return sprintf(buf, "%d:%s\n", wide_setting,
			       wide_str[wide_setting]);
	} else
		return 0;
}

static ssize_t video_screen_mode_store(struct class *cla,
				       struct class_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned long mode;
	int ret = 0;

	ret = kstrtoul(buf, 0, (unsigned long *)&mode);
	if (ret < 0)
		return -EINVAL;

	if ((mode < VIDEO_WIDEOPTION_MAX) && (mode != wide_setting)) {
		wide_setting = mode;
		video_property_changed = true;
	}

	return count;
}

static ssize_t video_blackout_policy_show(struct class *cla,
					  struct class_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%d\n", blackout);
}

static ssize_t video_blackout_policy_store(struct class *cla,
					   struct class_attribute *attr,
					   const char *buf, size_t count)
{
	size_t r;

	r = sscanf(buf, "%d", &blackout);

	if (debug_flag & DEBUG_FLAG_BLACKOUT)
		pr_info("%s(%d)\n", __func__, blackout);
	if (r != 1)
		return -EINVAL;

	return count;
}

static ssize_t video_brightness_show(struct class *cla,
				     struct class_attribute *attr, char *buf)
{
	s32 val = (READ_VCBUS_REG(VPP_VADJ1_Y + cur_dev->vpp_off) >> 8) &
			0x1ff;

	val = (val << 23) >> 23;

	return sprintf(buf, "%d\n", val);
}

static ssize_t video_brightness_store(struct class *cla,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	size_t r;
	int val;

	r = sscanf(buf, "%d", &val);
	if ((r != 1) || (val < -255) || (val > 255))
		return -EINVAL;

	WRITE_VCBUS_REG_BITS(VPP_VADJ1_Y + cur_dev->vpp_off, val, 8, 9);
	WRITE_VCBUS_REG(VPP_VADJ_CTRL + cur_dev->vpp_off, VPP_VADJ1_EN);

	return count;
}

static ssize_t video_contrast_show(struct class *cla,
				   struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n",
		       (int)(READ_VCBUS_REG(VPP_VADJ1_Y + cur_dev->vpp_off) &
			     0xff) - 0x80);
}

static ssize_t video_contrast_store(struct class *cla,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	size_t r;
	int val;

	r = sscanf(buf, "%d", &val);
	if ((r != 1) || (val < -127) || (val > 127))
		return -EINVAL;

	val += 0x80;

	WRITE_VCBUS_REG_BITS(VPP_VADJ1_Y + cur_dev->vpp_off, val, 0, 8);
	WRITE_VCBUS_REG(VPP_VADJ_CTRL + cur_dev->vpp_off, VPP_VADJ1_EN);

	return count;
}

static ssize_t vpp_brightness_show(struct class *cla,
				   struct class_attribute *attr, char *buf)
{
	s32 val = (READ_VCBUS_REG(VPP_VADJ2_Y +
			cur_dev->vpp_off) >> 8) & 0x1ff;

	val = (val << 23) >> 23;

	return sprintf(buf, "%d\n", val);
}

static ssize_t vpp_brightness_store(struct class *cla,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	size_t r;
	int val;

	r = sscanf(buf, "%d", &val);
	if ((r != 1) || (val < -255) || (val > 255))
		return -EINVAL;

	WRITE_VCBUS_REG_BITS(VPP_VADJ2_Y + cur_dev->vpp_off, val, 8, 9);
	WRITE_VCBUS_REG(VPP_VADJ_CTRL + cur_dev->vpp_off, VPP_VADJ2_EN);

	return count;
}

static ssize_t vpp_contrast_show(struct class *cla,
				 struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n",
		       (int)(READ_VCBUS_REG(VPP_VADJ2_Y + cur_dev->vpp_off) &
			     0xff) - 0x80);
}

static ssize_t vpp_contrast_store(struct class *cla,
			struct class_attribute *attr, const char *buf,
			size_t count)
{
	size_t r;
	int val;

	r = sscanf(buf, "%d", &val);
	if ((r != 1) || (val < -127) || (val > 127))
		return -EINVAL;

	val += 0x80;

	WRITE_VCBUS_REG_BITS(VPP_VADJ2_Y + cur_dev->vpp_off, val, 0, 8);
	WRITE_VCBUS_REG(VPP_VADJ_CTRL + cur_dev->vpp_off, VPP_VADJ2_EN);

	return count;
}

static ssize_t video_saturation_show(struct class *cla,
				     struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n",
		READ_VCBUS_REG(VPP_VADJ1_Y + cur_dev->vpp_off) & 0xff);
}

static ssize_t video_saturation_store(struct class *cla,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	size_t r;
	int val;

	r = sscanf(buf, "%d", &val);
	if ((r != 1) || (val < -127) || (val > 127))
		return -EINVAL;

	WRITE_VCBUS_REG_BITS(VPP_VADJ1_Y + cur_dev->vpp_off, val, 0, 8);
	WRITE_VCBUS_REG(VPP_VADJ_CTRL + cur_dev->vpp_off, VPP_VADJ1_EN);

	return count;
}

static ssize_t vpp_saturation_hue_show(struct class *cla,
				       struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", READ_VCBUS_REG(VPP_VADJ2_MA_MB));
}

static ssize_t vpp_saturation_hue_store(struct class *cla,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	size_t r;
	s32 mab = 0;
	s16 mc = 0, md = 0;

	r = sscanf(buf, "0x%x", &mab);
	if ((r != 1) || (mab & 0xfc00fc00))
		return -EINVAL;

	WRITE_VCBUS_REG(VPP_VADJ2_MA_MB, mab);
	mc = (s16) ((mab << 22) >> 22);	/* mc = -mb */
	mc = 0 - mc;
	if (mc > 511)
		mc = 511;
	if (mc < -512)
		mc = -512;
	md = (s16) ((mab << 6) >> 22);	/* md =  ma; */
	mab = ((mc & 0x3ff) << 16) | (md & 0x3ff);
	WRITE_VCBUS_REG(VPP_VADJ2_MC_MD, mab);
	/* WRITE_MPEG_REG(VPP_VADJ_CTRL, 1); */
	WRITE_VCBUS_REG_BITS(VPP_VADJ_CTRL + cur_dev->vpp_off, 1, 2, 1);
#ifdef PQ_DEBUG_EN
	pr_info("\n[amvideo..] set vpp_saturation OK!!!\n");
#endif
	return count;
}

/* [   24] 1/enable, 0/disable */
/* [23:16] Y */
/* [15: 8] Cb */
/* [ 7: 0] Cr */
static ssize_t video_test_screen_show(struct class *cla,
				      struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", test_screen);
}

static ssize_t video_test_screen_store(struct class *cla,
				       struct class_attribute *attr,
				       const char *buf, size_t count)
{
	size_t r;
	unsigned data = 0x0;
	r = sscanf(buf, "0x%x", &test_screen);
	if (r != 1)
		return -EINVAL;

	/* vdin0 pre post blend enable or disabled */
	data = READ_VCBUS_REG(VPP_MISC);
	if (test_screen & 0x01000000)
		data |= VPP_VD1_PREBLEND;
	else
		data &= (~VPP_VD1_PREBLEND);

	if (test_screen & 0x02000000)
		data |= VPP_VD1_POSTBLEND;
	else
		data &= (~VPP_VD1_POSTBLEND);
	/*
	   if (test_screen & 0x04000000)
	   data |= VPP_VD2_PREBLEND;
	   else
	   data &= (~VPP_VD2_PREBLEND);

	   if (test_screen & 0x08000000)
	   data |= VPP_VD2_POSTBLEND;
	   else
	   data &= (~VPP_VD2_POSTBLEND);
	 */
	/* show test screen */
	WRITE_VCBUS_REG(VPP_DUMMY_DATA1, test_screen & 0x00ffffff);

	WRITE_VCBUS_REG(VPP_MISC, data);

	if (debug_flag & DEBUG_FLAG_BLACKOUT) {
		pr_info("%s write(VPP_MISC,%x) write(VPP_DUMMY_DATA1, %x)\n",
		       __func__, data, test_screen & 0x00ffffff);
	}
	return count;
}

static ssize_t video_nonlinear_factor_show(struct class *cla,
					   struct class_attribute *attr,
					   char *buf)
{
	return sprintf(buf, "%d\n", vpp_get_nonlinear_factor());
}

static ssize_t video_nonlinear_factor_store(struct class *cla,
					    struct class_attribute *attr,
					    const char *buf, size_t count)
{
	size_t r;
	u32 factor;

	r = sscanf(buf, "%d", &factor);
	if (r != 1)
		return -EINVAL;

	if (vpp_set_nonlinear_factor(factor) == 0)
		video_property_changed = true;

	return count;
}

static ssize_t video_disable_show(struct class *cla,
				  struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", disable_video);
}

static ssize_t video_disable_store(struct class *cla,
				   struct class_attribute *attr,
				   const char *buf, size_t count)
{
	size_t r;
	int val;
	if (debug_flag & DEBUG_FLAG_BLACKOUT)
		pr_info("%s(%s)\n", __func__, buf);
	r = sscanf(buf, "%d", &val);
	if (r != 1)
		return -EINVAL;

	if (_video_set_disable(val) < 0)
		return -EINVAL;

	return count;
}

static ssize_t video_freerun_mode_show(struct class *cla,
				       struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", freerun_mode);
}

static ssize_t video_freerun_mode_store(struct class *cla,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	size_t r;

	r = sscanf(buf, "%d", &freerun_mode);

	if (debug_flag)
		pr_info("%s(%d)\n", __func__, freerun_mode);
	if (r != 1)
		return -EINVAL;

	return count;
}

static ssize_t video_speed_check_show(struct class *cla,
				      struct class_attribute *attr, char *buf)
{
	u32 h, w;

	vpp_get_video_speed_check(&h, &w);

	return snprintf(buf, 40, "%d %d\n", h, w);
}

static ssize_t video_speed_check_store(struct class *cla,
				       struct class_attribute *attr,
				       const char *buf, size_t count)
{

	set_video_speed_check(buf);

	return strnlen(buf, count);
}

static ssize_t threedim_mode_store(struct class *cla,
				   struct class_attribute *attr,
				   const char *buf, size_t len)
{
#ifdef TV_3D_FUNCTION_OPEN

	u32 type;
	size_t r;
	r = sscanf(buf, "%x\n", &type);
	if (r != 1)
		return -EINVAL;
	if (type != process_3d_type) {
		process_3d_type = type;
		if (mvc_flag)
			process_3d_type |= MODE_3D_MVC;
		video_property_changed = true;
		if ((process_3d_type & MODE_3D_FA) && !cur_dispbuf->trans_fmt)
			/*notify di 3d mode is frame alternative mode,
			passing two buffer in one frame */
			vf_notify_receiver_by_name("deinterlace",
			VFRAME_EVENT_PROVIDER_SET_3D_VFRAME_INTERLEAVE,
			(void *)1);
		else
			vf_notify_receiver_by_name("deinterlace",
			VFRAME_EVENT_PROVIDER_SET_3D_VFRAME_INTERLEAVE,
			(void *)0);
	}
#endif
	return len;
}

static ssize_t threedim_mode_show(struct class *cla,
				  struct class_attribute *attr, char *buf)
{
#ifdef TV_3D_FUNCTION_OPEN
	return sprintf(buf, "process type 0x%x,trans fmt %u.\n",
		       process_3d_type, video_3d_format);
#else
	return 0;
#endif
}

static ssize_t frame_addr_show(struct class *cla, struct class_attribute *attr,
			       char *buf)
{
	struct canvas_s canvas;
	u32 addr[3];

	if (cur_dispbuf) {
		canvas_read(cur_dispbuf->canvas0Addr & 0xff, &canvas);
		addr[0] = canvas.addr;
		canvas_read((cur_dispbuf->canvas0Addr >> 8) & 0xff, &canvas);
		addr[1] = canvas.addr;
		canvas_read((cur_dispbuf->canvas0Addr >> 16) & 0xff, &canvas);
		addr[2] = canvas.addr;

		return sprintf(buf, "0x%x-0x%x-0x%x\n", addr[0], addr[1],
			       addr[2]);
	}

	return sprintf(buf, "NA\n");
}

static ssize_t frame_canvas_width_show(struct class *cla,
				       struct class_attribute *attr, char *buf)
{
	struct canvas_s canvas;
	u32 width[3];

	if (cur_dispbuf) {
		canvas_read(cur_dispbuf->canvas0Addr & 0xff, &canvas);
		width[0] = canvas.width;
		canvas_read((cur_dispbuf->canvas0Addr >> 8) & 0xff, &canvas);
		width[1] = canvas.width;
		canvas_read((cur_dispbuf->canvas0Addr >> 16) & 0xff, &canvas);
		width[2] = canvas.width;

		return sprintf(buf, "%d-%d-%d\n",
			width[0], width[1], width[2]);
	}

	return sprintf(buf, "NA\n");
}

static ssize_t frame_canvas_height_show(struct class *cla,
			struct class_attribute *attr, char *buf)
{
	struct canvas_s canvas;
	u32 height[3];

	if (cur_dispbuf) {
		canvas_read(cur_dispbuf->canvas0Addr & 0xff, &canvas);
		height[0] = canvas.height;
		canvas_read((cur_dispbuf->canvas0Addr >> 8) & 0xff, &canvas);
		height[1] = canvas.height;
		canvas_read((cur_dispbuf->canvas0Addr >> 16) & 0xff, &canvas);
		height[2] = canvas.height;

		return sprintf(buf, "%d-%d-%d\n", height[0], height[1],
			       height[2]);
	}

	return sprintf(buf, "NA\n");
}

static ssize_t frame_width_show(struct class *cla,
			struct class_attribute *attr,
			char *buf)
{
	if (cur_dispbuf)
		return sprintf(buf, "%d\n", cur_dispbuf->width);

	return sprintf(buf, "NA\n");
}

static ssize_t frame_height_show(struct class *cla,
				 struct class_attribute *attr, char *buf)
{
	if (cur_dispbuf)
		return sprintf(buf, "%d\n", cur_dispbuf->height);

	return sprintf(buf, "NA\n");
}

static ssize_t frame_format_show(struct class *cla,
				 struct class_attribute *attr, char *buf)
{
	if (cur_dispbuf) {
		if ((cur_dispbuf->type & VIDTYPE_TYPEMASK) ==
		    VIDTYPE_INTERLACE_TOP)
			return sprintf(buf, "interlace-top\n");
		else if ((cur_dispbuf->type & VIDTYPE_TYPEMASK) ==
			 VIDTYPE_INTERLACE_BOTTOM)
			return sprintf(buf, "interlace-bottom\n");
		else
			return sprintf(buf, "progressive\n");
	}

	return sprintf(buf, "NA\n");
}

static ssize_t frame_aspect_ratio_show(struct class *cla,
			struct class_attribute *attr, char *buf)
{
	if (cur_dispbuf) {
		u32 ar = (cur_dispbuf->ratio_control &
			DISP_RATIO_ASPECT_RATIO_MASK) >>
			DISP_RATIO_ASPECT_RATIO_BIT;

		if (ar)
			return sprintf(buf, "0x%x\n", ar);
		else
			return sprintf(buf, "0x%x\n",
				       (cur_dispbuf->width << 8) /
				       cur_dispbuf->height);
	}

	return sprintf(buf, "NA\n");
}

static ssize_t frame_rate_show(struct class *cla, struct class_attribute *attr,
			       char *buf)
{
	u32 cnt = frame_count - last_frame_count;
	u32 time = jiffies;
	u32 tmp = time;
	u32 rate = 0;
	u32 vsync_rate;
	ssize_t ret = 0;
	time -= last_frame_time;
	last_frame_time = tmp;
	last_frame_count = frame_count;
	if (time == 0)
		return 0;
	rate = 100 * cnt * HZ / time;
	vsync_rate = 100 * vsync_count * HZ / time;
	if (vinfo->sync_duration_den > 0) {
		ret =
		    sprintf(buf,
		"VF.fps=%d.%02d panel fps %d, dur/is: %d,v/s=%d.%02d,inc=%d\n",
				rate / 100, rate % 100,
				vinfo->sync_duration_num /
				vinfo->sync_duration_den,
				time, vsync_rate / 100, vsync_rate % 100,
				vsync_pts_inc);
	}
	if ((debugflags & DEBUG_FLAG_CALC_PTS_INC) && time > HZ * 10
	    && vsync_rate > 0) {
		if ((vsync_rate * vsync_pts_inc / 100) != 90000)
			vsync_pts_inc = 90000 * 100 / (vsync_rate);
	}
	vsync_count = 0;
	return ret;
}

static ssize_t vframe_states_show(struct class *cla,
				  struct class_attribute *attr, char *buf)
{
	int ret = 0;
	struct vframe_states states;
	unsigned long flags;
	struct vframe_s *vf;

	if (vf_get_states(&states) == 0) {
		ret += sprintf(buf + ret, "vframe_pool_size=%d\n",
			states.vf_pool_size);
		ret += sprintf(buf + ret, "vframe buf_free_num=%d\n",
			states.buf_free_num);
		ret += sprintf(buf + ret, "vframe buf_recycle_num=%d\n",
			states.buf_recycle_num);
		ret += sprintf(buf + ret, "vframe buf_avail_num=%d\n",
			states.buf_avail_num);

		spin_lock_irqsave(&lock, flags);

		vf = video_vf_peek();
		if (vf) {
			ret += sprintf(buf + ret,
				"vframe ready frame delayed =%dms\n",
				(int)(jiffies_64 -
				vf->ready_jiffies64) * 1000 /
				HZ);
			ret += sprintf(buf + ret,
				"vf index=%d\n", vf->index);
			ret += sprintf(buf + ret,
				"vf->pts=%d\n", vf->pts);
			ret += sprintf(buf + ret,
				"cur vpts=%d\n",
				timestamp_vpts_get());
			ret += sprintf(buf + ret,
				"vf canvas0Addr=%x\n", vf->canvas0Addr);
			ret += sprintf(buf + ret,
				"vf canvas1Addr=%x\n", vf->canvas1Addr);
			ret += sprintf(buf + ret,
				"vf canvas0Addr.y.addr=%x(%d)\n",
				canvas_get_addr(
				canvasY(vf->canvas0Addr)),
				canvas_get_addr(
				canvasY(vf->canvas0Addr)));
			ret += sprintf(buf + ret,
				"vf canvas0Adr.uv.adr=%x(%d)\n",
				canvas_get_addr(
				canvasUV(vf->canvas0Addr)),
				canvas_get_addr(
				canvasUV(vf->canvas0Addr)));
		}
		spin_unlock_irqrestore(&lock, flags);

	} else
		ret += sprintf(buf + ret, "vframe no states\n");

	return ret;
}

#ifdef CONFIG_AM_VOUT
static ssize_t device_resolution_show(struct class *cla,
		struct class_attribute *attr, char *buf)
{
#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	const struct vinfo_s *info;
	if (cur_dev == &video_dev[0])
		info = get_current_vinfo();
	else
		info = get_current_vinfo2();
#else
	const struct vinfo_s *info = get_current_vinfo();
#endif

	if (info != NULL)
		return sprintf(buf, "%dx%d\n", info->width, info->height);
	else
		return sprintf(buf, "0x0\n");
}
#endif

static ssize_t video_filename_show(struct class *cla,
				   struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", file_name);
}

static ssize_t video_filename_store(struct class *cla,
				    struct class_attribute *attr,
				    const char *buf, size_t count)
{
	size_t r;
	r = sscanf(buf, "%s", file_name);
	if (r != 1)
		return -EINVAL;
	return r;
}

static ssize_t video_debugflags_show(struct class *cla,
				     struct class_attribute *attr, char *buf)
{
	int len = 0;
	len += sprintf(buf + len, "value=%d\n", debugflags);
	len += sprintf(buf + len, "bit0:playing as fast!\n");
	len += sprintf(buf + len,
		"bit1:enable calc pts inc in frame rate show\n");
	return len;
}

static ssize_t video_debugflags_store(struct class *cla,
				      struct class_attribute *attr,
				      const char *buf, size_t count)
{
	size_t r;
	int value = -1, seted = 1;
	r = sscanf(buf, "%d", &value);
	if (r == 1) {
		debugflags = value;
		seted = 1;
	} else {
		r = sscanf(buf, "0x%x", &value);
		if (r == 1) {
			debugflags = value;
			seted = 1;
		}
	}

	if (seted) {
		pr_info("debugflags changed to %d(%x)\n", debugflags,
		       debugflags);
		return count;
	} else
		return -EINVAL;
}

static ssize_t trickmode_duration_show(struct class *cla,
				       struct class_attribute *attr, char *buf)
{
	return sprintf(buf, "trickmode frame duration %d\n",
		       trickmode_duration / 9000);
}

static ssize_t trickmode_duration_store(struct class *cla,
					struct class_attribute *attr,
					const char *buf, size_t count)
{
	size_t r;
	u32 s_value;

	r = sscanf(buf, "%d", &s_value);
	if (r != 1)
		return -EINVAL;
	trickmode_duration = s_value * 9000;

	return count;
}

static ssize_t video_vsync_pts_inc_upint_show(struct class *cla,
					      struct class_attribute *attr,
					      char *buf)
{
	if (vsync_pts_inc_upint)
		return sprintf(buf,
		"%d,freerun %d,1.25xInc %d,1.12xInc %d,inc+10 %d,1xInc %d\n",
		vsync_pts_inc_upint, vsync_freerun,
		vsync_pts_125, vsync_pts_112, vsync_pts_101,
		vsync_pts_100);
	else
		return sprintf(buf, "%d\n", vsync_pts_inc_upint);
}

static ssize_t video_vsync_pts_inc_upint_store(struct class *cla,
					       struct class_attribute *attr,
					       const char *buf, size_t count)
{
	size_t r;

	r = sscanf(buf, "%d", &vsync_pts_inc_upint);

	if (debug_flag)
		pr_info("%s(%d)\n", __func__, vsync_pts_inc_upint);
	if (r != 1)
		return -EINVAL;

	return count;
}

static ssize_t slowsync_repeat_enable_show(struct class *cla,
					   struct class_attribute *attr,
					   char *buf)
{
	return sprintf(buf, "slowsync repeate enable = %d\n",
		       slowsync_repeat_enable);
}

static ssize_t slowsync_repeat_enable_store(struct class *cla,
					    struct class_attribute *attr,
					    const char *buf, size_t count)
{
	size_t r;
	r = sscanf(buf, "%d", &slowsync_repeat_enable);

	if (debug_flag)
		pr_info("%s(%d)\n", __func__, slowsync_repeat_enable);

	if (r != 1)
		return -EINVAL;

	return count;
}

static ssize_t video_vsync_slow_factor_show(struct class *cla,
					    struct class_attribute *attr,
					    char *buf)
{
	return sprintf(buf, "%d\n", vsync_slow_factor);
}

static ssize_t video_vsync_slow_factor_store(struct class *cla,
					     struct class_attribute *attr,
					     const char *buf, size_t count)
{
	size_t r;

	r = sscanf(buf, "%d", &vsync_slow_factor);

	if (debug_flag)
		pr_info("%s(%d)\n", __func__, vsync_slow_factor);
	if (r != 1)
		return -EINVAL;

	return count;
}

static ssize_t fps_info_show(struct class *cla, struct class_attribute *attr,
			     char *buf)
{
	u32 cnt = frame_count - last_frame_count;
	u32 time = jiffies;
	u32 input_fps = 0;
	u32 tmp = time;

	time -= last_frame_time;
	last_frame_time = tmp;
	last_frame_count = frame_count;
	if (time != 0)
		output_fps = cnt * HZ / time;
	if (cur_dispbuf && cur_dispbuf->duration > 0) {
		input_fps = 96000 / cur_dispbuf->duration;
		if (output_fps > input_fps)
			output_fps = input_fps;
	} else
		input_fps = output_fps;
	return sprintf(buf, "input_fps:0x%x output_fps:0x%x drop_fps:0x%x\n",
		       input_fps, output_fps, input_fps - output_fps);
}

static ssize_t video_layer1_state_show(struct class *cla,
			struct class_attribute *attr, char *buf)
{
	/*return sprintf(buf, "%d\n",
				(READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off)
				& VPP_VD1_PREBLEND) ? 1 : 0);*/
	return sprintf(buf, "%d\n", video_enabled);
}

void set_video_angle(u32 s_value)
{
	if ((s_value >= 0 && s_value <= 3) && (video_angle != s_value)) {
		video_angle = s_value;
		video_prot.angle_changed = 1;
		video_prot.video_started = 1;
		pr_info("video_prot angle:%d\n", video_angle);
	}
}
EXPORT_SYMBOL(set_video_angle);

static ssize_t video_angle_show(struct class *cla, struct class_attribute *attr,
				char *buf)
{
	return snprintf(buf, 40, "%d\n", video_angle);
}

static ssize_t video_angle_store(struct class *cla,
				 struct class_attribute *attr, const char *buf,
				 size_t count)
{
	size_t r;
	u32 s_value;
	r = sscanf(buf, "%d", &s_value);
	if (r != 1)
		return -EINVAL;
	set_video_angle(s_value);
	return strnlen(buf, count);
}

static ssize_t show_first_frame_nosync_show(struct class *cla,
					    struct class_attribute *attr,
					    char *buf)
{
	return sprintf(buf, "%d\n", show_first_frame_nosync ? 1 : 0);
}

static ssize_t show_first_frame_nosync_store(struct class *cla,
					     struct class_attribute *attr,
					     const char *buf, size_t count)
{
	size_t r;
	int value;

	r = sscanf(buf, "%d", &value);

	if (r != 1)
		return -EINVAL;

	if (value == 0)
		show_first_frame_nosync = false;
	else
		show_first_frame_nosync = true;

	return count;
}

static ssize_t video_free_keep_buffer_store(struct class *cla,
				   struct class_attribute *attr,
				   const char *buf, size_t count)
{
	size_t r;
	int val;
	if (debug_flag & DEBUG_FLAG_BLACKOUT)
		pr_info("%s(%s)\n", __func__, buf);
	r = sscanf(buf, "%d", &val);
	if (r != 1)
		return -EINVAL;
	if (val == 1)
		try_free_keep_video();
	return count;
}


static struct class_attribute amvideo_class_attrs[] = {
	__ATTR(axis,
	       S_IRUGO | S_IWUSR | S_IWGRP,
	       video_axis_show,
	       video_axis_store),
	__ATTR(crop,
	       S_IRUGO | S_IWUSR,
	       video_crop_show,
	       video_crop_store),
	__ATTR(global_offset,
	       S_IRUGO | S_IWUSR,
	       video_global_offset_show,
	       video_global_offset_store),
	__ATTR(screen_mode,
	       S_IRUGO | S_IWUSR | S_IWGRP,
	       video_screen_mode_show,
	       video_screen_mode_store),
	__ATTR(blackout_policy,
	       S_IRUGO | S_IWUSR | S_IWGRP,
	       video_blackout_policy_show,
	       video_blackout_policy_store),
	__ATTR(disable_video,
	       S_IRUGO | S_IWUSR | S_IWGRP,
	       video_disable_show,
	       video_disable_store),
	__ATTR(zoom,
	       S_IRUGO | S_IWUSR | S_IWGRP,
	       video_zoom_show,
	       video_zoom_store),
	__ATTR(brightness,
	       S_IRUGO | S_IWUSR,
	       video_brightness_show,
	       video_brightness_store),
	__ATTR(contrast,
	       S_IRUGO | S_IWUSR,
	       video_contrast_show,
	       video_contrast_store),
	__ATTR(vpp_brightness,
	       S_IRUGO | S_IWUSR,
	       vpp_brightness_show,
	       vpp_brightness_store),
	__ATTR(vpp_contrast,
	       S_IRUGO | S_IWUSR,
	       vpp_contrast_show,
	       vpp_contrast_store),
	__ATTR(saturation,
	       S_IRUGO | S_IWUSR,
	       video_saturation_show,
	       video_saturation_store),
	__ATTR(vpp_saturation_hue,
	       S_IRUGO | S_IWUSR,
	       vpp_saturation_hue_show,
	       vpp_saturation_hue_store),
	__ATTR(test_screen,
	       S_IRUGO | S_IWUSR,
	       video_test_screen_show,
	       video_test_screen_store),
	__ATTR(file_name,
	       S_IRUGO | S_IWUSR,
	       video_filename_show,
	       video_filename_store),
	__ATTR(debugflags,
	       S_IRUGO | S_IWUSR,
	       video_debugflags_show,
	       video_debugflags_store),
	__ATTR(trickmode_duration,
	       S_IRUGO | S_IWUSR,
	       trickmode_duration_show,
	       trickmode_duration_store),
	__ATTR(nonlinear_factor,
	       S_IRUGO | S_IWUSR,
	       video_nonlinear_factor_show,
	       video_nonlinear_factor_store),
	__ATTR(freerun_mode,
	       S_IRUGO | S_IWUSR,
	       video_freerun_mode_show,
	       video_freerun_mode_store),
	__ATTR(video_speed_check_h_w,
	       S_IRUGO | S_IWUSR,
	       video_speed_check_show,
	       video_speed_check_store),
	__ATTR(threedim_mode,
	       S_IRUGO | S_IWUSR,
	       threedim_mode_show,
	       threedim_mode_store),
	__ATTR(vsync_pts_inc_upint,
	       S_IRUGO | S_IWUSR,
	       video_vsync_pts_inc_upint_show,
	       video_vsync_pts_inc_upint_store),
	__ATTR(vsync_slow_factor,
	       S_IRUGO | S_IWUSR,
	       video_vsync_slow_factor_show,
	       video_vsync_slow_factor_store),
	__ATTR(angle,
	       S_IRUGO | S_IWUSR,
	       video_angle_show,
	       video_angle_store),
	__ATTR(stereo_scaler,
	       S_IRUGO | S_IWUSR, NULL,
	       video_3d_scale_store),
	__ATTR(show_first_frame_nosync,
	       S_IRUGO | S_IWUSR,
	       show_first_frame_nosync_show,
	       show_first_frame_nosync_store),
	__ATTR(slowsync_repeat_enable,
	       S_IRUGO | S_IWUSR,
	       slowsync_repeat_enable_show,
	       slowsync_repeat_enable_store),
	__ATTR(free_keep_buffer,
	       S_IRUGO | S_IWUSR | S_IWGRP, NULL,
	       video_free_keep_buffer_store),
#ifdef CONFIG_AM_VOUT
	__ATTR_RO(device_resolution),
#endif
	__ATTR_RO(frame_addr),
	__ATTR_RO(frame_canvas_width),
	__ATTR_RO(frame_canvas_height),
	__ATTR_RO(frame_width),
	__ATTR_RO(frame_height),
	__ATTR_RO(frame_format),
	__ATTR_RO(frame_aspect_ratio),
	__ATTR_RO(frame_rate),
	__ATTR_RO(vframe_states),
	__ATTR_RO(video_state),
	__ATTR_RO(fps_info),
	__ATTR_RO(video_layer1_state),
	__ATTR_NULL
};

static struct class_attribute amvideo_poll_class_attrs[] = {
	__ATTR_RO(frame_width),
	__ATTR_RO(frame_height),
	__ATTR_RO(vframe_states),
	__ATTR_RO(video_state),
	__ATTR_NULL
};

#ifdef CONFIG_PM
static int amvideo_class_suspend(struct device *dev, pm_message_t state)
{
#if 0

	pm_state.event = state.event;

	if (state.event == PM_EVENT_SUSPEND) {
		pm_state.vpp_misc =
			READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off);

		/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
		if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
		    && !is_meson_mtvd_cpu()) {
#if HAS_VPU_PROT

			pm_state.mem_pd_vd1 = get_vpu_mem_pd_vmod(VPU_VIU_VD1);
			pm_state.mem_pd_vd2 = get_vpu_mem_pd_vmod(VPU_VIU_VD2);
			pm_state.mem_pd_di_post =
			    get_vpu_mem_pd_vmod(VPU_DI_POST);

			if (has_vpu_prot()) {
				pm_state.mem_pd_prot2 =
				    get_vpu_mem_pd_vmod(VPU_PIC_ROT2);
				pm_state.mem_pd_prot3 =
				    get_vpu_mem_pd_vmod(VPU_PIC_ROT3);
			}
#endif
		}
		/* #endif */
		DisableVideoLayer_NoDelay();

		msleep(50);
		/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
		if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
		    && !is_meson_mtvd_cpu()) {
#if HAS_VPU_PROT

			switch_vpu_mem_pd_vmod(VPU_VIU_VD1,
					VPU_MEM_POWER_DOWN);
			switch_vpu_mem_pd_vmod(VPU_VIU_VD2,
					VPU_MEM_POWER_DOWN);
			switch_vpu_mem_pd_vmod(VPU_DI_POST,
					VPU_MEM_POWER_DOWN);

			if (has_vpu_prot()) {
				switch_vpu_mem_pd_vmod(VPU_PIC_ROT2,
						       VPU_MEM_POWER_DOWN);
				switch_vpu_mem_pd_vmod(VPU_PIC_ROT3,
						       VPU_MEM_POWER_DOWN);
			}
#endif

			vpu_delay_work_flag = 0;
		}
		/* #endif */

	}
#endif
	return 0;
}

static int amvideo_class_resume(struct device *dev)
{
#if 0
#define VPP_MISC_VIDEO_BITS_MASK \
	((VPP_VD2_ALPHA_MASK << VPP_VD2_ALPHA_BIT) | \
	VPP_VD2_PREBLEND | VPP_VD1_PREBLEND |\
	VPP_VD2_POSTBLEND | VPP_VD1_POSTBLEND | VPP_POSTBLEND_EN)\

	if (pm_state.event == PM_EVENT_SUSPEND) {
		/* #if MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
		if ((get_cpu_type() >= MESON_CPU_MAJOR_ID_M8)
		    && !is_meson_mtvd_cpu()) {
			switch_vpu_mem_pd_vmod(VPU_VIU_VD1,
					       pm_state.mem_pd_vd1);
			switch_vpu_mem_pd_vmod(VPU_VIU_VD2,
					       pm_state.mem_pd_vd2);
			switch_vpu_mem_pd_vmod(VPU_DI_POST,
					       pm_state.mem_pd_di_post);
#if HAS_VPU_PROT
			if (has_vpu_prot()) {
				switch_vpu_mem_pd_vmod(VPU_PIC_ROT2,
						       pm_state.mem_pd_prot2);
				switch_vpu_mem_pd_vmod(VPU_PIC_ROT3,
						       pm_state.mem_pd_prot3);
			}
#endif
		}
		/* #endif */
		WRITE_VCBUS_REG(VPP_MISC + cur_dev->vpp_off,
			(READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off) &
			(~VPP_MISC_VIDEO_BITS_MASK)) |
			(pm_state.vpp_misc & VPP_MISC_VIDEO_BITS_MASK));
		WRITE_VCBUS_REG(VPP_MISC +
			cur_dev->vpp_off, pm_state.vpp_misc);

		pm_state.event = -1;
		if (debug_flag & DEBUG_FLAG_BLACKOUT) {
			pr_info("%s write(VPP_MISC,%x)\n", __func__,
			       pm_state.vpp_misc);
		}
	}
#ifdef CONFIG_SCREEN_ON_EARLY
	if (power_key_pressed) {
		vout_pll_resume_early();
		osd_resume_early();
		resume_vout_early();
		power_key_pressed = 0;
	}
#endif
#endif
	return 0;
}
#endif

static struct class amvideo_class = {
	.name = AMVIDEO_CLASS_NAME,
	.class_attrs = amvideo_class_attrs,
#ifdef CONFIG_PM
	.suspend = amvideo_class_suspend,
	.resume = amvideo_class_resume,
#endif
};

static struct class amvideo_poll_class = {
	.name = AMVIDEO_POLL_CLASS_NAME,
	.class_attrs = amvideo_poll_class_attrs,
};

#ifdef TV_REVERSE
static int __init vpp_axis_reverse(char *str)
{
	unsigned char *ptr = str;
	pr_info("%s: bootargs is %s\n", __func__, str);
	if (strstr(ptr, "1"))
		reverse = true;
	else
		reverse = false;

	return 0;
}

__setup("video_reverse=", vpp_axis_reverse);
#endif

struct vframe_s *get_cur_dispbuf(void)
{
	return cur_dispbuf;
}

static struct device *amvideo_dev;
static struct device *amvideo_poll_dev;


#ifdef CONFIG_AM_VOUT
int vout_notify_callback(struct notifier_block *block, unsigned long cmd,
			 void *para)
{
	const struct vinfo_s *info;
	ulong flags;

#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	if (cur_dev != &video_dev[0])
		return 0;
#endif
	switch (cmd) {
	case VOUT_EVENT_MODE_CHANGE:
		info = get_current_vinfo();
		spin_lock_irqsave(&lock, flags);
		vinfo = info;
		/* pre-calculate vsync_pts_inc in 90k unit */
		vsync_pts_inc = 90000 * vinfo->sync_duration_den /
				vinfo->sync_duration_num;
		vsync_pts_inc_scale = vinfo->sync_duration_den;
		vsync_pts_inc_scale_base = vinfo->sync_duration_num;
		spin_unlock_irqrestore(&lock, flags);
		new_vmode = vinfo->mode;
		break;
	case VOUT_EVENT_OSD_PREBLEND_ENABLE:
		vpp_set_osd_layer_preblend(para);
		break;
	case VOUT_EVENT_OSD_DISP_AXIS:
		vpp_set_osd_layer_position(para);
		break;
	}
	return 0;
}

#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
int vout2_notify_callback(struct notifier_block *block, unsigned long cmd,
			  void *para)
{
	const struct vinfo_s *info;
	ulong flags;

	if (cur_dev != &video_dev[1])
		return 0;

	switch (cmd) {
	case VOUT_EVENT_MODE_CHANGE:
		info = get_current_vinfo2();
		spin_lock_irqsave(&lock, flags);
		vinfo = info;
		/* pre-calculate vsync_pts_inc in 90k unit */
		vsync_pts_inc = 90000 * vinfo->sync_duration_den /
				vinfo->sync_duration_num;
		vsync_pts_inc_scale = vinfo->sync_duration_den;
		vsync_pts_inc_scale_base = vinfo->sync_duration_num;
		spin_unlock_irqrestore(&lock, flags);
		break;
	case VOUT_EVENT_OSD_PREBLEND_ENABLE:
		vpp_set_osd_layer_preblend(para);
		break;
	case VOUT_EVENT_OSD_DISP_AXIS:
		vpp_set_osd_layer_position(para);
		break;
	}
	return 0;
}
#endif


static struct notifier_block vout_notifier = {
	.notifier_call = vout_notify_callback,
};

#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
static struct notifier_block vout2_notifier = {
	.notifier_call = vout2_notify_callback,
};
#endif


static void vout_hook(void)
{
	vout_register_client(&vout_notifier);

#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	vout2_register_client(&vout2_notifier);
#endif

	vinfo = get_current_vinfo();

	if (!vinfo) {
		set_current_vmode(VMODE_720P);

		vinfo = get_current_vinfo();
	}

	if (vinfo) {
		vsync_pts_inc = 90000 * vinfo->sync_duration_den /
			vinfo->sync_duration_num;
		vsync_pts_inc_scale = vinfo->sync_duration_den;
		vsync_pts_inc_scale_base = vinfo->sync_duration_num;
		old_vmode = new_vmode = vinfo->mode;
	}
#ifdef CONFIG_AM_VIDEO_LOG
	if (vinfo) {
		amlog_mask(LOG_MASK_VINFO, "vinfo = %p\n", vinfo);
		amlog_mask(LOG_MASK_VINFO, "display platform %s:\n",
			   vinfo->name);
		amlog_mask(LOG_MASK_VINFO, "\tresolution %d x %d\n",
			   vinfo->width, vinfo->height);
		amlog_mask(LOG_MASK_VINFO, "\taspect ratio %d : %d\n",
			   vinfo->aspect_ratio_num, vinfo->aspect_ratio_den);
		amlog_mask(LOG_MASK_VINFO, "\tsync duration %d : %d\n",
			   vinfo->sync_duration_num, vinfo->sync_duration_den);
	}
#endif
}
#endif

#if 1		/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */

static void do_vpu_delay_work(struct work_struct *work)
{
	unsigned long flags;
	unsigned r;

	if (vpu_delay_work_flag & VPU_VIDEO_LAYER1_CHANGED) {
		vpu_delay_work_flag &= ~VPU_VIDEO_LAYER1_CHANGED;
		switch_set_state(&video1_state_sdev, !!video_enabled);
	}
	spin_lock_irqsave(&delay_work_lock, flags);

	if (vpu_delay_work_flag & VPU_DELAYWORK_VPU_CLK) {
		vpu_delay_work_flag &= ~VPU_DELAYWORK_VPU_CLK;

		spin_unlock_irqrestore(&delay_work_lock, flags);

		if (vpu_clk_level > 0)
			request_vpu_clk_vmod(360000000, VPU_VIU_VD1);
		else
			release_vpu_clk_vmod(VPU_VIU_VD1);

		spin_lock_irqsave(&delay_work_lock, flags);
	}

	r = READ_VCBUS_REG(VPP_MISC + cur_dev->vpp_off);

	if (vpu_mem_power_off_count > 0) {
		vpu_mem_power_off_count--;

		if (vpu_mem_power_off_count == 0) {
			if ((vpu_delay_work_flag &
			     VPU_DELAYWORK_MEM_POWER_OFF_VD1)
			    && ((r & VPP_VD1_PREBLEND) == 0)) {
				vpu_delay_work_flag &=
				    ~VPU_DELAYWORK_MEM_POWER_OFF_VD1;

				switch_vpu_mem_pd_vmod(VPU_VIU_VD1,
						       VPU_MEM_POWER_DOWN);
				switch_vpu_mem_pd_vmod(VPU_AFBC_DEC,
						       VPU_MEM_POWER_DOWN);
				switch_vpu_mem_pd_vmod(VPU_DI_POST,
						       VPU_MEM_POWER_DOWN);
			}

			if ((vpu_delay_work_flag &
			     VPU_DELAYWORK_MEM_POWER_OFF_VD2)
			    && ((r & VPP_VD2_PREBLEND) == 0)) {
				vpu_delay_work_flag &=
				    ~VPU_DELAYWORK_MEM_POWER_OFF_VD2;

				switch_vpu_mem_pd_vmod(VPU_VIU_VD2,
						       VPU_MEM_POWER_DOWN);
			}

			if ((vpu_delay_work_flag &
			     VPU_DELAYWORK_MEM_POWER_OFF_PROT)
			    && ((r & VPP_VD1_PREBLEND) == 0)) {
				vpu_delay_work_flag &=
				    ~VPU_DELAYWORK_MEM_POWER_OFF_PROT;

#if HAS_VPU_PROT
				if (has_vpu_prot()) {
					switch_vpu_mem_pd_vmod(VPU_PIC_ROT2,
						VPU_MEM_POWER_DOWN);
					switch_vpu_mem_pd_vmod(VPU_PIC_ROT3,
						VPU_MEM_POWER_DOWN);
				}
#endif
			}
		}
	}

	spin_unlock_irqrestore(&delay_work_lock, flags);
}
#endif

/*********************************************************/
static int __init video_early_init(void)
{
	/* todo: move this to clock tree, enable VPU clock */
	/* WRITE_CBUS_REG(HHI_VPU_CLK_CNTL,
	(1<<9) | (1<<8) | (3)); // fclk_div3/4 = ~200M */
	/* WRITE_CBUS_REG(HHI_VPU_CLK_CNTL,
	(3<<9) | (1<<8) | (0)); // fclk_div7/1 = 364M
	//moved to vpu.c, default config by dts */

	if (get_logo_vmode() >= VMODE_MAX) {
#if 1				/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */
		WRITE_VCBUS_REG_BITS(VPP_OFIFO_SIZE, 0x77f,
			VPP_OFIFO_SIZE_BIT, VPP_OFIFO_SIZE_WID);
#if 0			/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESONG9TV */
		WRITE_VCBUS_REG_BITS(VPP_OFIFO_SIZE, 0x800, VPP_OFIFO_SIZE_BIT,
				     VPP_OFIFO_SIZE_WID);
#endif
#endif			/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON6 */
	}
#if 1			/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
	WRITE_VCBUS_REG(VPP_PREBLEND_VD1_H_START_END, 4096);
	WRITE_VCBUS_REG(VPP_BLEND_VD2_H_START_END, 4096);
#endif
	 /*fix S905 av out flicker black dot*/
	SET_VCBUS_REG_MASK(VPP_MISC, VPP_OUT_SATURATE);

	if (get_logo_vmode() >= VMODE_MAX) {
		CLEAR_VCBUS_REG_MASK(VPP_VSC_PHASE_CTRL,
				     VPP_PHASECTL_TYPE_INTERLACE);
#ifndef CONFIG_FB_AML_TCON
		SET_VCBUS_REG_MASK(VPP_MISC, VPP_OUT_SATURATE);
#endif
		WRITE_VCBUS_REG(VPP_HOLD_LINES + cur_dev->vpp_off, 0x08080808);
	}
#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	if (get_logo_vmode() >= VMODE_MAX) {
		CLEAR_VCBUS_REG_MASK(VPP2_VSC_PHASE_CTRL,
				     VPP_PHASECTL_TYPE_INTERLACE);
#ifndef CONFIG_FB_AML_TCON
		SET_VCBUS_REG_MASK(VPP2_MISC, VPP_OUT_SATURATE);
#endif
		WRITE_VCBUS_REG(VPP2_HOLD_LINES, 0x08080808);
	}
#if 1				/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
	WRITE_VCBUS_REG_BITS(VPP2_OFIFO_SIZE, 0x800,
			     VPP_OFIFO_SIZE_BIT, VPP_OFIFO_SIZE_WID);
#else
	WRITE_VCBUS_REG_BITS(VPP2_OFIFO_SIZE, 0x780,
			     VPP_OFIFO_SIZE_BIT, VPP_OFIFO_SIZE_WID);
#endif
	/* WRITE_VCBUS_REG_BITS(VPU_OSD3_MMC_CTRL, 1, 12, 2);
	select vdisp_mmc_arb for VIU2_OSD1 request */
	WRITE_VCBUS_REG_BITS(VPU_OSD3_MMC_CTRL, 2, 12, 2);
	/* select vdin_mmc_arb for VIU2_OSD1 request */
#endif

	/* temp: enable VPU arb mem */
	if (get_cpu_type() >= MESON_CPU_MAJOR_ID_GXBB)
		switch_vpu_mem_pd_vmod(VPU_VPU_ARB, VPU_MEM_POWER_ON);

	return 0;
}

#ifdef SUPER_SCALER_OPEN
static void super_scaler_init(void)
{
	/*load super scaler default cub setting */
	WRITE_VCBUS_REG(0x3102, 0xf84848f8);
	WRITE_VCBUS_REG(0x3103, 0xf84848f8);
	WRITE_VCBUS_REG(0x3104, 0xf84848f8);
	WRITE_VCBUS_REG(0x3105, 0xf84848f8);
	WRITE_VCBUS_REG(0x3106, 0x02330344);
	WRITE_VCBUS_REG(0x310a, 0x0080a0eb);
	WRITE_VCBUS_REG(0x310c, 0x0080a0eb);
	WRITE_VCBUS_REG(0x310d, 0x7a7a3a50);

	WRITE_VCBUS_REG(0x3112, 0x00017f00);
	WRITE_VCBUS_REG(0x3113, 0x00017f00);
	WRITE_VCBUS_REG(0x3114, 0x00017f00);
	WRITE_VCBUS_REG(0x3115, 0x00017f00);
	WRITE_VCBUS_REG(0x311a, 0xf84848f8);
	WRITE_VCBUS_REG(0x311b, 0xf84848f8);
	WRITE_VCBUS_REG(0x311c, 0xf84848f8);
	WRITE_VCBUS_REG(0x311d, 0xf84848f8);

	WRITE_VCBUS_REG(0x311e, 0x02330344);
	WRITE_VCBUS_REG(0x3122, 0x0080a0eb);
	WRITE_VCBUS_REG(0x3124, 0x0080a0eb);
	WRITE_VCBUS_REG(0x3125, 0x7a7a3a50);

	WRITE_VCBUS_REG(0x312b, 0x00017f00);
	WRITE_VCBUS_REG(0x312c, 0x00017f00);
	WRITE_VCBUS_REG(0x312d, 0x00017f00);
	WRITE_VCBUS_REG(0x312e, 0x00017f00);
}
#endif

static int __init video_init(void)
{
	int r = 0;
	/*
	   #ifdef CONFIG_ARCH_MESON1
	   ulong clk = clk_get_rate(clk_get_sys("clk_other_pll", NULL));
	   #elif !defined(CONFIG_ARCH_MESON3) && !defined(CONFIG_ARCH_MESON6)
	   ulong clk = clk_get_rate(clk_get_sys("clk_misc_pll", NULL));
	   #endif
	 */
	video_early_init();
#ifdef CONFIG_ARCH_MESON1
	no to here ulong clk =
		clk_get_rate(clk_get_sys("clk_other_pll", NULL));
#elif defined(CONFIG_ARCH_MESON2)
	not to here ulong clk =
		clk_get_rate(clk_get_sys("clk_misc_pll", NULL));
#endif
	/* #if !defined(CONFIG_ARCH_MESON3) && !defined(CONFIG_ARCH_MESON6) */
#if 0				/* MESON_CPU_TYPE <= MESON_CPU_TYPE_MESON2 */
	/* MALI clock settings */
	if ((clk <= 750000000) && (clk >= 600000000)) {
		WRITE_VCBUS_REG(HHI_MALI_CLK_CNTL,
		(2 << 9) |	/* select misc pll as clock source */
		(1 << 8) |	/* enable clock gating */
		(2 << 0));	/* Misc clk / 3 */
	} else {
		WRITE_VCBUS_REG(HHI_MALI_CLK_CNTL,
		(3 << 9) |	/* select DDR clock as clock source */
		(1 << 8) |	/* enable clock gating */
		(1 << 0));	/* DDR clk / 2 */
	}
#endif
#ifdef SUPER_SCALER_OPEN
	super_scaler_init();
#endif


	DisableVideoLayer();
	DisableVideoLayer2();

#ifndef CONFIG_AM_VIDEO2
	DisableVPP2VideoLayer();
#endif

	cur_dispbuf = NULL;

#ifdef FIQ_VSYNC
	/* enable fiq bridge */
	vsync_fiq_bridge.handle = vsync_bridge_isr;
	vsync_fiq_bridge.key = (u32) vsync_bridge_isr;
	vsync_fiq_bridge.name = "vsync_bridge_isr";

	r = register_fiq_bridge_handle(&vsync_fiq_bridge);

	if (r) {
		amlog_level(LOG_LEVEL_ERROR,
			    "video fiq bridge register error.\n");
		r = -ENOENT;
		goto err0;
	}
#endif

    /* sysfs node creation */
	r = class_register(&amvideo_poll_class);
	if (r) {
		amlog_level(LOG_LEVEL_ERROR, "create video_poll class fail.\n");
#ifdef FIQ_VSYNC
		free_irq(BRIDGE_IRQ, (void *)video_dev_id);
#else
		vdec_free_irq(VSYNC_IRQ, (void *)video_dev_id);
#endif
		goto err1;
	}

	r = class_register(&amvideo_class);
	if (r) {
		amlog_level(LOG_LEVEL_ERROR, "create video class fail.\n");
#ifdef FIQ_VSYNC
		free_irq(BRIDGE_IRQ, (void *)video_dev_id);
#else
		vdec_free_irq(VSYNC_IRQ, (void *)video_dev_id);
#endif
		goto err1;
	}

	/* create video device */
	r = register_chrdev(AMVIDEO_MAJOR, "amvideo", &amvideo_fops);
	if (r < 0) {
		amlog_level(LOG_LEVEL_ERROR,
			    "Can't register major for amvideo device\n");
		goto err2;
	}

	r = register_chrdev(0, "amvideo_poll", &amvideo_poll_fops);
	if (r < 0) {
		amlog_level(LOG_LEVEL_ERROR,
			"Can't register major for amvideo_poll device\n");
		goto err3;
	}

	amvideo_poll_major = r;

	amvideo_dev = device_create(&amvideo_class, NULL,
		MKDEV(AMVIDEO_MAJOR, 0), NULL, DEVICE_NAME);

	if (IS_ERR(amvideo_dev)) {
		amlog_level(LOG_LEVEL_ERROR, "Can't create amvideo device\n");
		goto err4;
	}

	amvideo_poll_dev = device_create(&amvideo_poll_class, NULL,
		MKDEV(amvideo_poll_major, 0), NULL, "amvideo_poll");

	if (IS_ERR(amvideo_poll_dev)) {
		amlog_level(LOG_LEVEL_ERROR,
			"Can't create amvideo_poll device\n");
		goto err5;
	}

	init_waitqueue_head(&amvideo_trick_wait);
	init_waitqueue_head(&amvideo_sizechange_wait);
#if 1				/* MESON_CPU_TYPE >= MESON_CPU_TYPE_MESON8 */
	INIT_WORK(&vpu_delay_work, do_vpu_delay_work);
#endif

#ifdef CONFIG_AM_VOUT
	vout_hook();
#endif

#ifdef CONFIG_VSYNC_RDMA
	dispbuf_to_put_num = DISPBUF_TO_PUT_MAX;
	while (dispbuf_to_put_num > 0) {
		dispbuf_to_put_num--;
		dispbuf_to_put[dispbuf_to_put_num] = NULL;
	}

	disp_canvas[0][0] =
	    (disp_canvas_index[0][2] << 16) | (disp_canvas_index[0][1] << 8) |
	    disp_canvas_index[0][0];
	disp_canvas[0][1] =
	    (disp_canvas_index[0][5] << 16) | (disp_canvas_index[0][4] << 8) |
	    disp_canvas_index[0][3];

	disp_canvas[1][0] =
	    (disp_canvas_index[1][2] << 16) | (disp_canvas_index[1][1] << 8) |
	    disp_canvas_index[1][0];
	disp_canvas[1][1] =
	    (disp_canvas_index[1][5] << 16) | (disp_canvas_index[1][4] << 8) |
	    disp_canvas_index[1][3];
#else

	disp_canvas[0] =
	    (disp_canvas_index[2] << 16) | (disp_canvas_index[1] << 8) |
	    disp_canvas_index[0];
	disp_canvas[1] =
	    (disp_canvas_index[5] << 16) | (disp_canvas_index[4] << 8) |
	    disp_canvas_index[3];
#endif
	vsync_fiq_up();
#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	vsync2_fiq_up();
#endif

	vf_receiver_init(&video_vf_recv, RECEIVER_NAME, &video_vf_receiver,
			 NULL);
	vf_reg_receiver(&video_vf_recv);

	vf_receiver_init(&video4osd_vf_recv, RECEIVER4OSD_NAME,
			 &video4osd_vf_receiver, NULL);
	vf_reg_receiver(&video4osd_vf_recv);
	switch_dev_register(&video1_state_sdev);
	switch_set_state(&video1_state_sdev, 0);
#ifdef CONFIG_GE2D_KEEP_FRAME
	/* video_frame_getmem(); */
	ge2d_videotask_init();
#endif

#ifdef CONFIG_AM_VIDEO2
	set_clone_frame_rate(android_clone_rate, 0);
#endif

	return 0;
 err5:
	device_destroy(&amvideo_class, MKDEV(AMVIDEO_MAJOR, 0));
 err4:
	unregister_chrdev(amvideo_poll_major, "amvideo_poll");
 err3:
	unregister_chrdev(AMVIDEO_MAJOR, DEVICE_NAME);

 err2:
#ifdef FIQ_VSYNC
	unregister_fiq_bridge_handle(&vsync_fiq_bridge);
#endif
	class_unregister(&amvideo_class);
 err1:
	class_unregister(&amvideo_poll_class);
#ifdef FIQ_VSYNC
 err0:
#endif
	return r;
}


static void __exit video_exit(void)
{
	vf_unreg_receiver(&video_vf_recv);

	vf_unreg_receiver(&video4osd_vf_recv);
	DisableVideoLayer();
	DisableVideoLayer2();

	vsync_fiq_down();
#ifdef CONFIG_SUPPORT_VIDEO_ON_VPP2
	vsync2_fiq_down();
#endif
	device_destroy(&amvideo_class, MKDEV(AMVIDEO_MAJOR, 0));
	device_destroy(&amvideo_poll_class, MKDEV(amvideo_poll_major, 0));

	unregister_chrdev(AMVIDEO_MAJOR, DEVICE_NAME);
	unregister_chrdev(amvideo_poll_major, "amvideo_poll");

#ifdef FIQ_VSYNC
	unregister_fiq_bridge_handle(&vsync_fiq_bridge);
#endif

	class_unregister(&amvideo_class);
	class_unregister(&amvideo_poll_class);

#ifdef CONFIG_GE2D_KEEP_FRAME
	ge2d_videotask_release();
#endif
}



MODULE_PARM_DESC(debug_flag, "\n debug_flag\n");
module_param(debug_flag, uint, 0664);

#ifdef TV_3D_FUNCTION_OPEN
MODULE_PARM_DESC(force_3d_scaler, "\n force_3d_scaler\n");
module_param(force_3d_scaler, uint, 0664);

MODULE_PARM_DESC(video_3d_format, "\n video_3d_format\n");
module_param(video_3d_format, uint, 0664);

#endif

MODULE_PARM_DESC(vsync_enter_line_max, "\n vsync_enter_line_max\n");
module_param(vsync_enter_line_max, uint, 0664);

MODULE_PARM_DESC(vsync_exit_line_max, "\n vsync_exit_line_max\n");
module_param(vsync_exit_line_max, uint, 0664);

#ifdef CONFIG_VSYNC_RDMA
MODULE_PARM_DESC(vsync_rdma_line_max, "\n vsync_rdma_line_max\n");
module_param(vsync_rdma_line_max, uint, 0664);
#endif

module_param(underflow, uint, 0664);
MODULE_PARM_DESC(underflow, "\n Underflow count\n");

module_param(next_peek_underflow, uint, 0664);
MODULE_PARM_DESC(skip, "\n Underflow count\n");

/*arch_initcall(video_early_init);
*/

module_init(video_init);
module_exit(video_exit);

MODULE_PARM_DESC(smooth_sync_enable, "\n smooth_sync_enable\n");
module_param(smooth_sync_enable, uint, 0664);

MODULE_PARM_DESC(hdmi_in_onvideo, "\n hdmi_in_onvideo\n");
module_param(hdmi_in_onvideo, uint, 0664);

#ifdef CONFIG_AM_VIDEO2
MODULE_PARM_DESC(video_play_clone_rate, "\n video_play_clone_rate\n");
module_param(video_play_clone_rate, uint, 0664);

MODULE_PARM_DESC(android_clone_rate, "\n android_clone_rate\n");
module_param(android_clone_rate, uint, 0664);

MODULE_PARM_DESC(noneseamless_play_clone_rate,
		 "\n noneseamless_play_clone_rate\n");
module_param(noneseamless_play_clone_rate, uint, 0664);

#endif

MODULE_PARM_DESC(cur_dev_idx, "\n cur_dev_idx\n");
module_param(cur_dev_idx, uint, 0664);

MODULE_PARM_DESC(new_frame_count, "\n new_frame_count\n");
module_param(new_frame_count, uint, 0664);

MODULE_PARM_DESC(omx_pts, "\n omx_pts\n");
module_param(omx_pts, uint, 0664);

MODULE_PARM_DESC(omx_pts_interval_upper, "\n omx_pts_interval\n");
module_param(omx_pts_interval_upper, int, 0664);

MODULE_PARM_DESC(omx_pts_interval_lower, "\n omx_pts_interval\n");
module_param(omx_pts_interval_lower, int, 0664);


#ifdef TV_REVERSE
module_param(reverse, bool, 0644);
MODULE_PARM_DESC(reverse, "reverse /disable reverse");
#endif
MODULE_DESCRIPTION("AMLOGIC video output driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tim Yao <timyao@amlogic.com>");
