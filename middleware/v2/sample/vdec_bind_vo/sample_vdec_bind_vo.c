#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sample_comm.h"
#include "sample_vdec_lib.h"
#include "cvi_ive.h"
#include "cvi_vdec.h"

#define PLAYER_VDEC_CHN 0
#define PLAYER_VPSS_CHN 0

#pragma weak CVI_VDEC_SetRotation

typedef enum rotate_mode_e {
	ROTATE_MODE_CPU = 0,
	ROTATE_MODE_VDEC = 1,
	ROTATE_MODE_NONE = 2,
} rotate_mode_t;

typedef enum copy_mode_e {
	COPY_MODE_CPU = 0,
	COPY_MODE_TDMA = 1,
	COPY_MODE_IVE = 2,
} copy_mode_t;

typedef enum output_mode_e {
	OUTPUT_MODE_BGR888 = 0,
	OUTPUT_MODE_ARGB8888 = 1,
} output_mode_t;

typedef struct fb_ctx_s {
	int fd;
	unsigned char *mem;
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	unsigned int bits_per_pixel;
	unsigned int size;
	unsigned long long phys_addr;
} fb_ctx_t;

typedef struct player_ctx_s {
	vdecInputCfg input_cfg;
	vdecChnCtx vdec_chn;
	fb_ctx_t fb;
	fb_ctx_t stage_fb;
	VIDEO_FRAME_INFO_S stage_frame;
	SIZE_S output_size;
	VPSS_GRP vpss_grp;
	rotate_mode_t rotate_mode;
	copy_mode_t copy_mode;
	output_mode_t output_mode;
	IVE_HANDLE ive_handle;
	CVI_BOOL sys_inited;
	CVI_BOOL vdec_started;
	CVI_BOOL vpss_started;
	CVI_BOOL stage_ready;
	CVI_U64 shown_frames;
	CVI_U64 getframe_timeouts;
	CVI_U64 vpss_send_failures;
	CVI_U64 vpss_get_failures;
	double time_vdec_get_sec;
	double time_vpss_send_sec;
	double time_vpss_get_sec;
	double time_blit_sec;
	double time_tdma_sec;
	struct timeval start_wall;
	struct timeval last_status_wall;
	struct rusage last_status_usage;
	CVI_BOOL status_initialized;
} player_ctx_t;

static volatile sig_atomic_t g_stop_requested = 0;

static const char *rotate_mode_name(rotate_mode_t mode)
{
	if (mode == ROTATE_MODE_VDEC)
		return "vdec";
	if (mode == ROTATE_MODE_NONE)
		return "none";
	return "cpu";
}

static const char *copy_mode_name(copy_mode_t mode)
{
	if (mode == COPY_MODE_TDMA)
		return "tdma";
	if (mode == COPY_MODE_IVE)
		return "ive";
	return "cpu";
}

static const char *output_mode_name(output_mode_t mode)
{
	return (mode == OUTPUT_MODE_ARGB8888) ? "argb8888" : "bgr888";
}

static rotate_mode_t detect_rotate_mode(void)
{
	const char *mode = getenv("SAMPLE_VDEC_FB_ROTATE_MODE");

	if (mode != NULL && strcmp(mode, "vdec") == 0)
		return ROTATE_MODE_VDEC;
	if (mode != NULL && strcmp(mode, "none") == 0)
		return ROTATE_MODE_NONE;

	return ROTATE_MODE_CPU;
}

static copy_mode_t detect_copy_mode(void)
{
	const char *mode = getenv("SAMPLE_VDEC_FB_COPY_MODE");

	if (mode != NULL && strcmp(mode, "tdma") == 0)
		return COPY_MODE_TDMA;
	if (mode != NULL && strcmp(mode, "ive") == 0)
		return COPY_MODE_IVE;

	return COPY_MODE_CPU;
}

static output_mode_t detect_output_mode(void)
{
	const char *mode = getenv("SAMPLE_VDEC_FB_OUTPUT_MODE");

	if (mode != NULL && strcmp(mode, "argb") == 0)
		return OUTPUT_MODE_ARGB8888;

	return OUTPUT_MODE_BGR888;
}

static void sanitize_rotate_mode(player_ctx_t *ctx)
{
	if (ctx->rotate_mode == ROTATE_MODE_VDEC && CVI_VDEC_SetRotation == NULL) {
		SAMPLE_PRT("CVI_VDEC_SetRotation not exported by current SDK, fallback to cpu rotate\n");
		ctx->rotate_mode = ROTATE_MODE_CPU;
	}
}

static void sanitize_copy_mode(player_ctx_t *ctx)
{
	if (ctx->copy_mode != COPY_MODE_TDMA)
		return;

	if (ctx->fb.bits_per_pixel != 32) {
		SAMPLE_PRT("TDMA copy currently requires 32bpp fb0, fallback to cpu copy\n");
		ctx->copy_mode = COPY_MODE_CPU;
		return;
	}
}

static void player_handle_signal(int signo)
{
	if (signo == SIGINT || signo == SIGTERM || signo == SIGTSTP)
		g_stop_requested = 1;
}

static double timeval_diff_sec(const struct timeval *newer, const struct timeval *older)
{
	return (double)(newer->tv_sec - older->tv_sec) +
	       (double)(newer->tv_usec - older->tv_usec) / 1000000.0;
}

static double rusage_cpu_sec(const struct rusage *usage)
{
	return (double)usage->ru_utime.tv_sec +
	       (double)usage->ru_utime.tv_usec / 1000000.0 +
	       (double)usage->ru_stime.tv_sec +
	       (double)usage->ru_stime.tv_usec / 1000000.0;
}

static double avg_ms_per_frame(double total_sec, CVI_U64 shown_frames)
{
	if (shown_frames == 0)
		return 0.0;
	return (total_sec * 1000.0) / (double)shown_frames;
}

static void print_status_line(player_ctx_t *ctx, const VDEC_CHN_STATUS_S *status)
{
	struct timeval now;
	struct rusage usage;
	double elapsed;
	double interval_wall;
	double interval_cpu;
	double cpu_percent;
	double avg_fps;
	double inst_fps;
	static CVI_U64 last_shown_frames;

	gettimeofday(&now, NULL);
	getrusage(RUSAGE_SELF, &usage);

	if (!ctx->status_initialized) {
		ctx->start_wall = now;
		ctx->last_status_wall = now;
		ctx->last_status_usage = usage;
		ctx->status_initialized = CVI_TRUE;
		last_shown_frames = ctx->shown_frames;
	}

	elapsed = timeval_diff_sec(&now, &ctx->start_wall);
	interval_wall = timeval_diff_sec(&now, &ctx->last_status_wall);
	interval_cpu = rusage_cpu_sec(&usage) - rusage_cpu_sec(&ctx->last_status_usage);
	cpu_percent = (interval_wall > 0.0) ? (interval_cpu / interval_wall) * 100.0 : 0.0;
	avg_fps = (elapsed > 0.0) ? ((double)ctx->shown_frames / elapsed) : 0.0;
	inst_fps = (interval_wall > 0.0) ?
		((double)(ctx->shown_frames - last_shown_frames) / interval_wall) : 0.0;

	printf("\rstatus shown=%llu recv=%u dec=%u leftBytes=%u leftFrames=%u leftPics=%u fps=%.1f avg=%.1f cpu=%.1f%% elapsed=%.1fs",
	       (unsigned long long)ctx->shown_frames,
	       status->u32RecvStreamFrames,
	       status->u32DecodeStreamFrames,
	       status->u32LeftStreamBytes,
	       status->u32LeftStreamFrames,
	       status->u32LeftPics,
	       inst_fps,
	       avg_fps,
	       cpu_percent,
	       elapsed);
	fflush(stdout);

	ctx->last_status_wall = now;
	ctx->last_status_usage = usage;
	last_shown_frames = ctx->shown_frames;
}

static void print_summary(const player_ctx_t *ctx, const VDEC_CHN_STATUS_S *status)
{
	struct timeval now;
	struct rusage usage;
	double elapsed;
	double cpu_sec;
	double avg_fps;

	gettimeofday(&now, NULL);
	getrusage(RUSAGE_SELF, &usage);
	elapsed = timeval_diff_sec(&now, &ctx->start_wall);
	cpu_sec = rusage_cpu_sec(&usage);
	avg_fps = (elapsed > 0.0) ? ((double)ctx->shown_frames / elapsed) : 0.0;

	printf("\nsummary shown=%llu recv=%u dec=%u avg_fps=%.2f cpu_time=%.2fs elapsed=%.2fs getframe_timeouts=%llu vpss_send_fail=%llu vpss_get_fail=%llu vdec_get_ms=%.1f(%.3f/f) vpss_send_ms=%.1f(%.3f/f) vpss_get_ms=%.1f(%.3f/f) blit_ms=%.1f(%.3f/f) tdma_ms=%.1f(%.3f/f) leftBytes=%u leftFrames=%u leftPics=%u\n",
	       (unsigned long long)ctx->shown_frames,
	       status->u32RecvStreamFrames,
	       status->u32DecodeStreamFrames,
	       avg_fps,
	       cpu_sec,
	       elapsed,
	       (unsigned long long)ctx->getframe_timeouts,
	       (unsigned long long)ctx->vpss_send_failures,
	       (unsigned long long)ctx->vpss_get_failures,
	       ctx->time_vdec_get_sec * 1000.0, avg_ms_per_frame(ctx->time_vdec_get_sec, ctx->shown_frames),
	       ctx->time_vpss_send_sec * 1000.0, avg_ms_per_frame(ctx->time_vpss_send_sec, ctx->shown_frames),
	       ctx->time_vpss_get_sec * 1000.0, avg_ms_per_frame(ctx->time_vpss_get_sec, ctx->shown_frames),
	       ctx->time_blit_sec * 1000.0, avg_ms_per_frame(ctx->time_blit_sec, ctx->shown_frames),
	       ctx->time_tdma_sec * 1000.0, avg_ms_per_frame(ctx->time_tdma_sec, ctx->shown_frames),
	       status->u32LeftStreamBytes,
	       status->u32LeftStreamFrames,
	       status->u32LeftPics);
}

static int detect_fb0_size(SIZE_S *size)
{
	FILE *fp;
	unsigned int width = 0;
	unsigned int height = 0;
	char buf[64];

	fp = fopen("/sys/class/graphics/fb0/modes", "r");
	if (fp != NULL) {
		if (fgets(buf, sizeof(buf), fp) != NULL &&
		    sscanf(buf, "U:%ux%up-%*u", &width, &height) == 2) {
			fclose(fp);
			size->u32Width = width;
			size->u32Height = height;
			return 0;
		}
		fclose(fp);
	}

	fp = fopen("/sys/class/graphics/fb0/virtual_size", "r");
	if (fp != NULL) {
		if (fgets(buf, sizeof(buf), fp) != NULL &&
		    sscanf(buf, "%u,%u", &width, &height) == 2) {
			fclose(fp);
			size->u32Width = width;
			size->u32Height = height / 2;
			return 0;
		}
		fclose(fp);
	}

	return -1;
}

static int fb_open(fb_ctx_t *fb, const char *dev)
{
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;

	memset(fb, 0, sizeof(*fb));
	fb->fd = open(dev, O_RDWR);
	if (fb->fd < 0) {
		SAMPLE_PRT("open %s failed: %s\n", dev, strerror(errno));
		return -1;
	}

	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
		SAMPLE_PRT("FBIOGET_FSCREENINFO failed: %s\n", strerror(errno));
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}
	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		SAMPLE_PRT("FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}

	fb->width = vinfo.xres;
	fb->height = vinfo.yres;
	fb->stride = finfo.line_length;
	fb->bits_per_pixel = vinfo.bits_per_pixel;
	fb->size = finfo.smem_len;
	fb->phys_addr = finfo.smem_start;
	fb->mem = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		SAMPLE_PRT("fb mmap failed: %s\n", strerror(errno));
		close(fb->fd);
		fb->fd = -1;
		fb->mem = NULL;
		return -1;
	}

	SAMPLE_PRT("fb0 %ux%u %ubpp stride=%u size=%u phys=%#llx\n",
		   fb->width, fb->height, fb->bits_per_pixel, fb->stride, fb->size,
		   fb->phys_addr);
	return 0;
}

static void fb_close(fb_ctx_t *fb)
{
	if (fb->mem != NULL && fb->mem != MAP_FAILED)
		munmap(fb->mem, fb->size);
	if (fb->fd >= 0)
		close(fb->fd);
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
}

static void fb_clear(fb_ctx_t *fb)
{
	unsigned int x;
	unsigned int y;

	if (fb->mem == NULL)
		return;

	if (fb->bits_per_pixel == 32) {
		for (y = 0; y < fb->height; ++y) {
			unsigned char *row = fb->mem + y * fb->stride;
			for (x = 0; x < fb->width; ++x) {
				row[x * 4 + 0] = 0x00; /* B */
				row[x * 4 + 1] = 0x00; /* G */
				row[x * 4 + 2] = 0x00; /* R */
				row[x * 4 + 3] = 0xff; /* A */
			}
		}
		return;
	}

	memset(fb->mem, 0, fb->size);
}

static CVI_S32 stage_open(player_ctx_t *ctx)
{
	CVI_S32 ret;
	CVI_VOID *vir_addr = NULL;
	CVI_U64 phy_addr = 0;
	CVI_U32 alloc_size;

	memset(&ctx->stage_fb, 0, sizeof(ctx->stage_fb));
	ctx->stage_fb.fd = -1;
	alloc_size = ctx->fb.stride * ctx->fb.height;
	ret = CVI_SYS_IonAlloc_Cached(&phy_addr, &vir_addr, "vdec_fb_stage", alloc_size);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("stage ion alloc failed with %#x\n", ret);
		return ret;
	}

	ctx->stage_fb.mem = vir_addr;
	ctx->stage_fb.width = ctx->fb.width;
	ctx->stage_fb.height = ctx->fb.height;
	ctx->stage_fb.stride = ctx->fb.stride;
	ctx->stage_fb.bits_per_pixel = 32;
	ctx->stage_fb.size = alloc_size;
	ctx->stage_fb.phys_addr = phy_addr;
	ctx->stage_ready = CVI_TRUE;
	SAMPLE_PRT("stage fb %ux%u stride=%u phys=%#llx\n",
		   ctx->stage_fb.width, ctx->stage_fb.height, ctx->stage_fb.stride,
		   ctx->stage_fb.phys_addr);
	return CVI_SUCCESS;
}

static void stage_close(player_ctx_t *ctx)
{
	if (!ctx->stage_ready)
		return;

	if (ctx->stage_fb.phys_addr != 0 && ctx->stage_fb.mem != NULL)
		CVI_SYS_IonFree(ctx->stage_fb.phys_addr, ctx->stage_fb.mem);

	memset(&ctx->stage_fb, 0, sizeof(ctx->stage_fb));
	memset(&ctx->stage_frame, 0, sizeof(ctx->stage_frame));
	ctx->stage_fb.fd = -1;
	ctx->stage_ready = CVI_FALSE;
}

static void init_send_thread_param(vdecChnCtx *chn_ctx, VDEC_THREAD_PARAM_S *param,
				   const char *path, CVI_S32 timeout_ms)
{
	SAMPLE_VDEC_ATTR *attr = &chn_ctx->stSampleVdecAttr;

	memset(param, 0, sizeof(*param));
	snprintf(param->cFileName, sizeof(param->cFileName), "%s", path);
	snprintf(param->cFilePath, sizeof(param->cFilePath), "%s", "./");
	param->enType = attr->enType;
	param->s32StreamMode = attr->enMode;
	param->s32ChnId = chn_ctx->VdecChn;
	param->s32IntervalTime = 10000;
	param->u64PtsInit = 0;
	param->u64PtsIncrease = 0;
	param->eThreadCtrl = THREAD_CTRL_START;
	param->bCircleSend = CVI_FALSE;
	param->s32MilliSec = timeout_ms;
	param->s32MinBufSize = 50 * 1024;
	param->bFileEnd = CVI_FALSE;
}

static CVI_S32 validate_config(vdecInputCfg *cfg)
{
	vdecChnInputCfg *chn_cfg = &cfg->chnInCfg[0];

	if (cfg->u32NumAllChns != 1) {
		SAMPLE_PRT("only single-channel playback is supported\n");
		return CVI_FAILURE;
	}
	if (!(chn_cfg->enType == PT_H264 || chn_cfg->enType == PT_H265)) {
		SAMPLE_PRT("only H.264/H.265 elementary streams are supported\n");
		return CVI_FAILURE;
	}
	if (strlen(chn_cfg->input_path) == 0) {
		SAMPLE_PRT("input bitstream path is required\n");
		return CVI_FAILURE;
	}
	if (access(chn_cfg->input_path, R_OK) != 0) {
		SAMPLE_PRT("cannot read %s: %s\n", chn_cfg->input_path, strerror(errno));
		return CVI_FAILURE;
	}
	if (chn_cfg->u32BufWidth == 0)
		chn_cfg->u32BufWidth = 1920;
	if (chn_cfg->u32BufHeight == 0)
		chn_cfg->u32BufHeight = 1080;
	if (chn_cfg->u32MaxFrameBuffer == 0)
		chn_cfg->u32MaxFrameBuffer = 4;

	return CVI_SUCCESS;
}

static CVI_S32 init_system(player_ctx_t *ctx)
{
	VB_CONFIG_S vb_conf;
	vdecChnInputCfg *chn_cfg = &ctx->input_cfg.chnInCfg[0];
	CVI_U32 decode_blk_size;
	CVI_U32 display_blk_size;

	memset(&vb_conf, 0, sizeof(vb_conf));
	decode_blk_size = COMMON_GetPicBufferSize(chn_cfg->u32BufWidth, chn_cfg->u32BufHeight,
						  PIXEL_FORMAT_NV21, DATA_BITWIDTH_8,
						  COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	display_blk_size = COMMON_GetPicBufferSize(ctx->output_size.u32Width,
						   ctx->output_size.u32Height,
						   PIXEL_FORMAT_RGB_888,
						   DATA_BITWIDTH_8,
						   COMPRESS_MODE_NONE,
						   DEFAULT_ALIGN);

	vb_conf.u32MaxPoolCnt = 2;
	vb_conf.astCommPool[0].u32BlkSize = decode_blk_size;
	vb_conf.astCommPool[0].u32BlkCnt = 4;
	vb_conf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	vb_conf.astCommPool[1].u32BlkSize = display_blk_size;
	vb_conf.astCommPool[1].u32BlkCnt = 4;
	vb_conf.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;

	CHECK_RET(SAMPLE_COMM_SYS_Init(&vb_conf), "SAMPLE_COMM_SYS_Init");
	ctx->sys_inited = CVI_TRUE;
	return CVI_SUCCESS;
}

static CVI_S32 init_vdec(player_ctx_t *ctx)
{
	vdecChnInputCfg *chn_cfg = &ctx->input_cfg.chnInCfg[0];
	vdecChnCtx *chn_ctx = &ctx->vdec_chn;
	VDEC_CHN_ATTR_S chn_attr;
	VDEC_CHN_PARAM_S chn_param;
	PAYLOAD_TYPE_E en_type = chn_cfg->enType;
	CVI_S32 ret;

	memset(chn_ctx, 0, sizeof(*chn_ctx));
	memset(&chn_attr, 0, sizeof(chn_attr));
	memset(&chn_param, 0, sizeof(chn_param));

	chn_ctx->VdecChn = PLAYER_VDEC_CHN;
	chn_ctx->stSampleVdecAttr.enType = en_type;
	chn_ctx->stSampleVdecAttr.enMode = VIDEO_MODE_FRAME;
	chn_ctx->stSampleVdecAttr.enPixelFormat = PIXEL_FORMAT_NV21;
	chn_ctx->stSampleVdecAttr.u32Width = chn_cfg->u32BufWidth;
	chn_ctx->stSampleVdecAttr.u32Height = chn_cfg->u32BufHeight;
	chn_ctx->stSampleVdecAttr.u32FrameBufCnt = chn_cfg->u32MaxFrameBuffer;
	chn_ctx->stSampleVdecAttr.u32DisplayFrameNum = 1;

	chn_attr.enType = en_type;
	chn_attr.enMode = VIDEO_MODE_FRAME;
	chn_attr.u32PicWidth = chn_cfg->u32BufWidth;
	chn_attr.u32PicHeight = chn_cfg->u32BufHeight;
	chn_attr.u32StreamBufSize = ALIGN(chn_cfg->u32BufWidth * chn_cfg->u32BufHeight, 0x4000);
	chn_attr.u32FrameBufCnt = chn_cfg->u32MaxFrameBuffer;

	CHECK_RET(CVI_VDEC_CreateChn(PLAYER_VDEC_CHN, &chn_attr), "CVI_VDEC_CreateChn");
	chn_ctx->bCreateChn = CVI_TRUE;
	CHECK_RET(CVI_VDEC_GetChnParam(PLAYER_VDEC_CHN, &chn_param), "CVI_VDEC_GetChnParam");
	chn_param.enType = en_type;
	chn_param.enPixelFormat = PIXEL_FORMAT_NV21;
	chn_param.u32DisplayFrameNum = 1;
	CHECK_RET(CVI_VDEC_SetChnParam(PLAYER_VDEC_CHN, &chn_param), "CVI_VDEC_SetChnParam");
	if (ctx->rotate_mode == ROTATE_MODE_VDEC) {
		ret = CVI_VDEC_SetRotation(PLAYER_VDEC_CHN, ROTATION_270);
		if (ret != CVI_SUCCESS) {
			SAMPLE_PRT("CVI_VDEC_SetRotation(270) failed with %#x\n", ret);
			return ret;
		}
		SAMPLE_PRT("using VDEC rotate270 path\n");
	}
	CHECK_RET(CVI_VDEC_StartRecvStream(PLAYER_VDEC_CHN), "CVI_VDEC_StartRecvStream");
	ctx->vdec_started = CVI_TRUE;
	return CVI_SUCCESS;
}

static CVI_S32 init_vpss(player_ctx_t *ctx)
{
	VPSS_GRP_ATTR_S grp_attr;
	VPSS_CHN_ATTR_S chn_attr[VPSS_MAX_PHY_CHN_NUM];
	CVI_BOOL chn_enable[VPSS_MAX_PHY_CHN_NUM] = {0};
	VPSS_GRP vpss_grp;
	PIXEL_FORMAT_E out_fmt;
	CVI_S32 ret;

	memset(&grp_attr, 0, sizeof(grp_attr));
	memset(chn_attr, 0, sizeof(chn_attr));

	vpss_grp = CVI_VPSS_GetAvailableGrp();
	if (vpss_grp < 0) {
		SAMPLE_PRT("no available VPSS group, fallback to grp0\n");
		vpss_grp = 0;
		CVI_VPSS_DestroyGrp(vpss_grp);
	}
	ctx->vpss_grp = vpss_grp;

	grp_attr.stFrameRate.s32SrcFrameRate = -1;
	grp_attr.stFrameRate.s32DstFrameRate = -1;
	grp_attr.enPixelFormat = PIXEL_FORMAT_NV21;
	grp_attr.u32MaxW = ctx->input_cfg.chnInCfg[0].u32BufWidth;
	grp_attr.u32MaxH = ctx->input_cfg.chnInCfg[0].u32BufHeight;
	grp_attr.u8VpssDev = 0;

	chn_enable[PLAYER_VPSS_CHN] = CVI_TRUE;
	chn_attr[PLAYER_VPSS_CHN].u32Width = ctx->output_size.u32Width;
	chn_attr[PLAYER_VPSS_CHN].u32Height = ctx->output_size.u32Height;
	chn_attr[PLAYER_VPSS_CHN].enVideoFormat = VIDEO_FORMAT_LINEAR;
	out_fmt = (ctx->output_mode == OUTPUT_MODE_ARGB8888) ? PIXEL_FORMAT_ARGB_8888 : PIXEL_FORMAT_BGR_888;
	chn_attr[PLAYER_VPSS_CHN].enPixelFormat = out_fmt;
	chn_attr[PLAYER_VPSS_CHN].stFrameRate.s32SrcFrameRate = -1;
	chn_attr[PLAYER_VPSS_CHN].stFrameRate.s32DstFrameRate = -1;
	chn_attr[PLAYER_VPSS_CHN].u32Depth = 3;
	chn_attr[PLAYER_VPSS_CHN].bMirror = CVI_FALSE;
	chn_attr[PLAYER_VPSS_CHN].bFlip = CVI_FALSE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.bEnableBgColor = CVI_TRUE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	chn_attr[PLAYER_VPSS_CHN].stNormalize.bEnable = CVI_FALSE;

	ret = SAMPLE_COMM_VPSS_Init(ctx->vpss_grp, chn_enable, &grp_attr, chn_attr);
	if (ret != CVI_SUCCESS && ctx->output_mode == OUTPUT_MODE_ARGB8888) {
		SAMPLE_PRT("VPSS argb8888 output failed with %#x, fallback to bgr888\n", ret);
		ctx->output_mode = OUTPUT_MODE_BGR888;
		chn_attr[PLAYER_VPSS_CHN].enPixelFormat = PIXEL_FORMAT_BGR_888;
		ret = SAMPLE_COMM_VPSS_Init(ctx->vpss_grp, chn_enable, &grp_attr, chn_attr);
	}
	CHECK_RET(ret, "SAMPLE_COMM_VPSS_Init");
	CHECK_RET(SAMPLE_COMM_VPSS_Start(ctx->vpss_grp, chn_enable, &grp_attr, chn_attr),
		  "SAMPLE_COMM_VPSS_Start");
	SAMPLE_PRT("using VPSS grp %d output=%s\n", ctx->vpss_grp, output_mode_name(ctx->output_mode));
	ctx->vpss_started = CVI_TRUE;
	return CVI_SUCCESS;
}

static const unsigned char *map_frame_plane0(const VIDEO_FRAME_S *vframe, CVI_BOOL *mapped)
{
	if (vframe->pu8VirAddr[0] == NULL) {
		CVI_SYS_IonInvalidateCache(vframe->u64PhyAddr[0], NULL, vframe->u32Length[0]);
		*mapped = CVI_TRUE;
		return CVI_SYS_Mmap(vframe->u64PhyAddr[0], vframe->u32Length[0]);
	}

	CVI_SYS_IonInvalidateCache(vframe->u64PhyAddr[0], vframe->pu8VirAddr[0], vframe->u32Length[0]);
	*mapped = CVI_FALSE;
	return vframe->pu8VirAddr[0];
}

static void unmap_frame_plane0(const VIDEO_FRAME_S *vframe, const unsigned char *src_base, CVI_BOOL mapped)
{
	if (mapped && src_base != NULL)
		CVI_SYS_Munmap((void *)src_base, vframe->u32Length[0]);
}

static void fill_fb_black_row(const fb_ctx_t *fb, unsigned char *dst)
{
	unsigned int src_x;

	if (fb->bits_per_pixel == 32) {
		for (src_x = 0; src_x < fb->width; ++src_x) {
			dst[src_x * 4 + 0] = 0x00;
			dst[src_x * 4 + 1] = 0x00;
			dst[src_x * 4 + 2] = 0x00;
			dst[src_x * 4 + 3] = 0xff;
		}
		return;
	}
	memset(dst, 0, fb->stride);
}

static void blit_direct_to_fb(const fb_ctx_t *fb, const VIDEO_FRAME_INFO_S *frame, output_mode_t output_mode)
{
	const VIDEO_FRAME_S *vframe = &frame->stVFrame;
	unsigned int src_x;
	unsigned int src_y;
	unsigned int src_bpp = (output_mode == OUTPUT_MODE_ARGB8888) ? 4 : 3;
	unsigned int copy_width = (vframe->u32Width < fb->width) ? vframe->u32Width : fb->width;
	unsigned int copy_height = (vframe->u32Height < fb->height) ? vframe->u32Height : fb->height;
	const unsigned char *src_base;
	CVI_BOOL mapped;

	src_base = map_frame_plane0(vframe, &mapped);
	if (src_base == NULL) {
		SAMPLE_PRT("CVI_SYS_Mmap frame failed\n");
		return;
	}

	for (src_y = 0; src_y < fb->height; ++src_y) {
		unsigned char *dst = fb->mem + src_y * fb->stride;

		if (src_y >= copy_height) {
			fill_fb_black_row(fb, dst);
			continue;
		}

		if (output_mode == OUTPUT_MODE_ARGB8888 && fb->bits_per_pixel == 32) {
			const unsigned int *src32 = (const unsigned int *)(src_base + src_y * vframe->u32Stride[0]);
			unsigned int *dst32 = (unsigned int *)dst;

			for (src_x = 0; src_x < copy_width; ++src_x)
				dst32[src_x] = src32[src_x] | 0xff000000U;
			for (; src_x < fb->width; ++src_x)
				dst32[src_x] = 0xff000000U;
			continue;
		}

		{
			const unsigned char *src_row = src_base + src_y * vframe->u32Stride[0];

			for (src_x = 0; src_x < fb->width; ++src_x) {
				if (src_x < copy_width) {
					const unsigned char *src = src_row + src_x * src_bpp;

					if (fb->bits_per_pixel == 32) {
						dst[src_x * 4 + 0] = src[0];
						dst[src_x * 4 + 1] = src[1];
						dst[src_x * 4 + 2] = src[2];
						dst[src_x * 4 + 3] = 0xff;
					} else if (fb->bits_per_pixel == 24) {
						dst[src_x * 3 + 0] = src[0];
						dst[src_x * 3 + 1] = src[1];
						dst[src_x * 3 + 2] = src[2];
					} else if (fb->bits_per_pixel == 16) {
						unsigned short *dst16 = (unsigned short *)dst;
						unsigned char r = src[2];
						unsigned char g = src[1];
						unsigned char b = src[0];
						dst16[src_x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
					}
				} else if (fb->bits_per_pixel == 32) {
					dst[src_x * 4 + 0] = 0x00;
					dst[src_x * 4 + 1] = 0x00;
					dst[src_x * 4 + 2] = 0x00;
					dst[src_x * 4 + 3] = 0xff;
				} else if (fb->bits_per_pixel == 24) {
					dst[src_x * 3 + 0] = 0x00;
					dst[src_x * 3 + 1] = 0x00;
					dst[src_x * 3 + 2] = 0x00;
				} else if (fb->bits_per_pixel == 16) {
					unsigned short *dst16 = (unsigned short *)dst;
					dst16[src_x] = 0x0000;
				}
			}
		}
	}

	unmap_frame_plane0(vframe, src_base, mapped);
}

static void blit_rotate270_to_fb(const fb_ctx_t *fb, const VIDEO_FRAME_INFO_S *frame, output_mode_t output_mode)
{
	const VIDEO_FRAME_S *vframe = &frame->stVFrame;
	unsigned int dst_y;
	unsigned int dst_x;
	unsigned int src_bpp = (output_mode == OUTPUT_MODE_ARGB8888) ? 4 : 3;
	unsigned int copy_width = (vframe->u32Width < fb->height) ? vframe->u32Width : fb->height;
	unsigned int copy_height = (vframe->u32Height < fb->width) ? vframe->u32Height : fb->width;
	const unsigned char *src_base;
	CVI_BOOL mapped;

	src_base = map_frame_plane0(vframe, &mapped);
	if (src_base == NULL) {
		SAMPLE_PRT("CVI_SYS_Mmap frame failed\n");
		return;
	}

	/*
	 * Rotate by addressing source pixels through the rotated coordinate map,
	 * but write each destination row linearly to keep fb0 writes cache-friendly.
	 */
	for (dst_y = 0; dst_y < fb->height; ++dst_y) {
		unsigned char *dst_row = fb->mem + dst_y * fb->stride;

		if (dst_y >= copy_width) {
			fill_fb_black_row(fb, dst_row);
			continue;
		}

		if (fb->bits_per_pixel == 32) {
			unsigned int *dst32 = (unsigned int *)dst_row;
			unsigned int src_x = copy_width - 1 - dst_y;

			for (dst_x = 0; dst_x < copy_height; ++dst_x) {
				const unsigned char *src = src_base + dst_x * vframe->u32Stride[0] + src_x * src_bpp;

				if (output_mode == OUTPUT_MODE_ARGB8888) {
					const unsigned int *src32 = (const unsigned int *)src;
					dst32[dst_x] = (*src32) | 0xff000000U;
				} else {
					dst32[dst_x] = ((unsigned int)0xff << 24) |
						       ((unsigned int)src[2] << 16) |
						       ((unsigned int)src[1] << 8) |
						       (unsigned int)src[0];
				}
			}
			for (; dst_x < fb->width; ++dst_x)
				dst32[dst_x] = 0xff000000U;
			continue;
		}

		if (fb->bits_per_pixel == 24) {
			unsigned int src_x = copy_width - 1 - dst_y;

			for (dst_x = 0; dst_x < copy_height; ++dst_x) {
				const unsigned char *src = src_base + dst_x * vframe->u32Stride[0] + src_x * src_bpp;
				dst_row[dst_x * 3 + 0] = src[0];
				dst_row[dst_x * 3 + 1] = src[1];
				dst_row[dst_x * 3 + 2] = src[2];
			}
			memset(dst_row + copy_height * 3, 0, fb->stride - copy_height * 3);
			continue;
		}

		if (fb->bits_per_pixel == 16) {
			unsigned int src_x = copy_width - 1 - dst_y;
			unsigned short *dst16 = (unsigned short *)dst_row;

			for (dst_x = 0; dst_x < copy_height; ++dst_x) {
				const unsigned char *src = src_base + dst_x * vframe->u32Stride[0] + src_x * src_bpp;
				unsigned char r = src[2];
				unsigned char g = src[1];
				unsigned char b = src[0];
				dst16[dst_x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
			}
			for (; dst_x < fb->width; ++dst_x)
				dst16[dst_x] = 0x0000;
		}
	}

	unmap_frame_plane0(vframe, src_base, mapped);
}

static CVI_S32 tdma_copy_fb(const fb_ctx_t *src_fb, const fb_ctx_t *dst_fb)
{
	CVI_TDMA_2D_S tdma;

	if (src_fb->bits_per_pixel != 32 || dst_fb->bits_per_pixel != 32)
		return CVI_FAILURE;
	if (src_fb->phys_addr == 0 || dst_fb->phys_addr == 0)
		return CVI_FAILURE;
	if (src_fb->width < dst_fb->width || src_fb->height < dst_fb->height)
		return CVI_FAILURE;

	memset(&tdma, 0, sizeof(tdma));
	tdma.paddr_src = src_fb->phys_addr;
	tdma.paddr_dst = dst_fb->phys_addr;
	tdma.w_bytes = dst_fb->width * 4;
	tdma.h = dst_fb->height;
	tdma.stride_bytes_src = src_fb->stride;
	tdma.stride_bytes_dst = dst_fb->stride;
	return CVI_SYS_TDMACopy2D(&tdma);
}

static CVI_S32 ive_copy_fb(IVE_HANDLE handle, const fb_ctx_t *src_fb, const fb_ctx_t *dst_fb)
{
	IVE_DATA_S src;
	IVE_DST_DATA_S dst;
	IVE_DMA_CTRL_S ctrl;

	if (handle == NULL)
		return CVI_FAILURE;
	if (src_fb->bits_per_pixel != 32 || dst_fb->bits_per_pixel != 32)
		return CVI_FAILURE;
	if (src_fb->phys_addr == 0 || dst_fb->phys_addr == 0)
		return CVI_FAILURE;
	if (src_fb->width < dst_fb->width || src_fb->height < dst_fb->height)
		return CVI_FAILURE;

	memset(&src, 0, sizeof(src));
	memset(&dst, 0, sizeof(dst));
	memset(&ctrl, 0, sizeof(ctrl));
	src.u64PhyAddr = src_fb->phys_addr;
	src.u64VirAddr = (CVI_U64)(uintptr_t)src_fb->mem;
	src.u32Stride = src_fb->stride;
	src.u32Width = dst_fb->width * 4;
	src.u32Height = dst_fb->height;
	dst.u64PhyAddr = dst_fb->phys_addr;
	dst.u64VirAddr = (CVI_U64)(uintptr_t)dst_fb->mem;
	dst.u32Stride = dst_fb->stride;
	dst.u32Width = dst_fb->width * 4;
	dst.u32Height = dst_fb->height;
	ctrl.enMode = IVE_DMA_MODE_DIRECT_COPY;
	return CVI_IVE_DMA(handle, &src, &dst, &ctrl, CVI_TRUE);
}

static CVI_S32 playback_loop(player_ctx_t *ctx)
{
	vdecChnCtx *chn_ctx = &ctx->vdec_chn;
	VDEC_THREAD_PARAM_S *send_param = &chn_ctx->stVdecThreadParamSend;
	VIDEO_FRAME_INFO_S vdec_frame;
	VIDEO_FRAME_INFO_S rgb_frame;
	VDEC_CHN_STATUS_S status;
	CVI_S32 ret;
	unsigned int idle_loops = 0;
	struct timeval op_start;
	struct timeval op_end;

	memset(&vdec_frame, 0, sizeof(vdec_frame));
	memset(&rgb_frame, 0, sizeof(rgb_frame));
	gettimeofday(&ctx->start_wall, NULL);
	ctx->last_status_wall = ctx->start_wall;
	getrusage(RUSAGE_SELF, &ctx->last_status_usage);
	ctx->status_initialized = CVI_TRUE;

	while (!g_stop_requested) {
		gettimeofday(&op_start, NULL);
		ret = CVI_VDEC_GetFrame(PLAYER_VDEC_CHN, &vdec_frame, 1000);
		gettimeofday(&op_end, NULL);
		ctx->time_vdec_get_sec += timeval_diff_sec(&op_end, &op_start);
		if (ret == CVI_SUCCESS) {
			gettimeofday(&op_start, NULL);
			ret = CVI_VPSS_SendFrame(ctx->vpss_grp, &vdec_frame, 1000);
			gettimeofday(&op_end, NULL);
			ctx->time_vpss_send_sec += timeval_diff_sec(&op_end, &op_start);
			CVI_VDEC_ReleaseFrame(PLAYER_VDEC_CHN, &vdec_frame);
			memset(&vdec_frame, 0, sizeof(vdec_frame));
			if (ret != CVI_SUCCESS) {
				ctx->vpss_send_failures++;
				SAMPLE_PRT("CVI_VPSS_SendFrame failed with %#x\n", ret);
				continue;
			}

			gettimeofday(&op_start, NULL);
			ret = CVI_VPSS_GetChnFrame(ctx->vpss_grp, PLAYER_VPSS_CHN, &rgb_frame, 1000);
			gettimeofday(&op_end, NULL);
			ctx->time_vpss_get_sec += timeval_diff_sec(&op_end, &op_start);
			if (ret != CVI_SUCCESS) {
				ctx->vpss_get_failures++;
				SAMPLE_PRT("CVI_VPSS_GetChnFrame failed with %#x\n", ret);
				continue;
			}

			if (ctx->copy_mode == COPY_MODE_TDMA || ctx->copy_mode == COPY_MODE_IVE) {
				fb_ctx_t *dst_fb = &ctx->stage_fb;

				gettimeofday(&op_start, NULL);
				if (ctx->rotate_mode == ROTATE_MODE_VDEC || ctx->rotate_mode == ROTATE_MODE_NONE)
					blit_direct_to_fb(dst_fb, &rgb_frame, ctx->output_mode);
				else
					blit_rotate270_to_fb(dst_fb, &rgb_frame, ctx->output_mode);
				CVI_SYS_IonFlushCache(ctx->stage_fb.phys_addr, ctx->stage_fb.mem, ctx->stage_fb.size);
				gettimeofday(&op_end, NULL);
				ctx->time_blit_sec += timeval_diff_sec(&op_end, &op_start);
				gettimeofday(&op_start, NULL);
				if (ctx->copy_mode == COPY_MODE_TDMA)
					ret = tdma_copy_fb(&ctx->stage_fb, &ctx->fb);
				else
					ret = ive_copy_fb(ctx->ive_handle, &ctx->stage_fb, &ctx->fb);
				gettimeofday(&op_end, NULL);
				ctx->time_tdma_sec += timeval_diff_sec(&op_end, &op_start);
				if (ret != CVI_SUCCESS) {
					SAMPLE_PRT("%s copy failed with %#x, fallback to cpu blit\n",
						   (ctx->copy_mode == COPY_MODE_TDMA) ? "TDMA" : "IVE",
						   ret);
					ctx->copy_mode = COPY_MODE_CPU;
					gettimeofday(&op_start, NULL);
					if (ctx->rotate_mode == ROTATE_MODE_VDEC || ctx->rotate_mode == ROTATE_MODE_NONE)
						blit_direct_to_fb(&ctx->fb, &rgb_frame, ctx->output_mode);
					else
						blit_rotate270_to_fb(&ctx->fb, &rgb_frame, ctx->output_mode);
					gettimeofday(&op_end, NULL);
					ctx->time_blit_sec += timeval_diff_sec(&op_end, &op_start);
				}
			} else if (ctx->rotate_mode == ROTATE_MODE_VDEC || ctx->rotate_mode == ROTATE_MODE_NONE) {
				gettimeofday(&op_start, NULL);
				blit_direct_to_fb(&ctx->fb, &rgb_frame, ctx->output_mode);
				gettimeofday(&op_end, NULL);
				ctx->time_blit_sec += timeval_diff_sec(&op_end, &op_start);
			} else {
				gettimeofday(&op_start, NULL);
				blit_rotate270_to_fb(&ctx->fb, &rgb_frame, ctx->output_mode);
				gettimeofday(&op_end, NULL);
				ctx->time_blit_sec += timeval_diff_sec(&op_end, &op_start);
			}
			CVI_VPSS_ReleaseChnFrame(ctx->vpss_grp, PLAYER_VPSS_CHN, &rgb_frame);
			memset(&rgb_frame, 0, sizeof(rgb_frame));
			ctx->shown_frames++;
			idle_loops = 0;
			CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
			{
				struct timeval now;
				gettimeofday(&now, NULL);
				if (timeval_diff_sec(&now, &ctx->last_status_wall) >= 0.5)
					print_status_line(ctx, &status);
			}
			continue;
		}

		if (ret == CVI_ERR_VDEC_BUSY)
			ctx->getframe_timeouts++;
		else
			SAMPLE_PRT("CVI_VDEC_GetFrame failed with %#x\n", ret);

		CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
		if (send_param->bFileEnd &&
		    status.u32LeftStreamBytes == 0 &&
		    status.u32LeftStreamFrames == 0 &&
		    status.u32LeftPics == 0) {
			print_summary(ctx, &status);
			return CVI_SUCCESS;
		}

		idle_loops++;
		if (idle_loops >= 10) {
			print_status_line(ctx, &status);
			idle_loops = 0;
		}
	}

	CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
	print_summary(ctx, &status);
	return CVI_SUCCESS;
}

static void player_stop(player_ctx_t *ctx)
{
	CVI_BOOL chn_enable[VPSS_MAX_PHY_CHN_NUM] = {0};

	if (ctx->vdec_started) {
		SAMPLE_COMM_VDEC_StopSendStream(&ctx->vdec_chn.stVdecThreadParamSend,
						&ctx->vdec_chn.vdecThreadSend);
		CVI_VDEC_StopRecvStream(PLAYER_VDEC_CHN);
	}

	if (ctx->vpss_started) {
		chn_enable[PLAYER_VPSS_CHN] = CVI_TRUE;
		SAMPLE_COMM_VPSS_Stop(ctx->vpss_grp, chn_enable);
		ctx->vpss_started = CVI_FALSE;
	}

	if (ctx->vdec_chn.bCreateChn) {
		CVI_VDEC_DestroyChn(PLAYER_VDEC_CHN);
		ctx->vdec_chn.bCreateChn = CVI_FALSE;
		ctx->vdec_started = CVI_FALSE;
	}

	stage_close(ctx);
	if (ctx->ive_handle != NULL) {
		CVI_IVE_DestroyHandle(ctx->ive_handle);
		ctx->ive_handle = NULL;
	}
	fb_close(&ctx->fb);

	if (ctx->sys_inited) {
		SAMPLE_COMM_SYS_Exit();
		ctx->sys_inited = CVI_FALSE;
	}
}

int main(int argc, char **argv)
{
	player_ctx_t ctx;
	CVI_S32 ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.fb.fd = -1;
	ctx.vpss_grp = -1;
	ctx.stage_fb.fd = -1;
	ctx.rotate_mode = detect_rotate_mode();
	ctx.copy_mode = detect_copy_mode();
	ctx.output_mode = detect_output_mode();
	sanitize_rotate_mode(&ctx);

	ret = parseDecArgv(&ctx.input_cfg, argc, argv);
	if (ret < 0) {
		if (ret == STATUS_HELP)
			return CVI_SUCCESS;
		return CVI_FAILURE;
	}

	ret = validate_config(&ctx.input_cfg);
	if (ret != CVI_SUCCESS) {
		printVdecHelp(argv);
		return ret;
	}

	if (fb_open(&ctx.fb, "/dev/fb0") != 0)
		return CVI_FAILURE;
	sanitize_copy_mode(&ctx);

	if (ctx.rotate_mode == ROTATE_MODE_VDEC || ctx.rotate_mode == ROTATE_MODE_NONE) {
		ctx.output_size.u32Width = ctx.fb.width;
		ctx.output_size.u32Height = ctx.fb.height;
	} else {
		ctx.output_size.u32Width = ctx.fb.height;
		ctx.output_size.u32Height = ctx.fb.width;
	}
	if (ctx.output_size.u32Width == 0 || ctx.output_size.u32Height == 0) {
		if (detect_fb0_size(&ctx.output_size) != 0) {
			SAMPLE_PRT("failed to detect framebuffer size\n");
			player_stop(&ctx);
			return CVI_FAILURE;
		}
	}

	ret = init_system(&ctx);
	if (ret != CVI_SUCCESS) {
		player_stop(&ctx);
		return ret;
	}
	if (ctx.copy_mode == COPY_MODE_TDMA || ctx.copy_mode == COPY_MODE_IVE) {
		if (ctx.copy_mode == COPY_MODE_IVE) {
			ctx.ive_handle = CVI_IVE_CreateHandle();
			if (ctx.ive_handle == NULL) {
				SAMPLE_PRT("CVI_IVE_CreateHandle failed, fallback to tdma copy\n");
				ctx.copy_mode = COPY_MODE_TDMA;
			}
		}
		ret = stage_open(&ctx);
		if (ret != CVI_SUCCESS) {
			player_stop(&ctx);
			return ret;
		}
		fb_clear(&ctx.stage_fb);
	}
	ret = init_vdec(&ctx);
	if (ret != CVI_SUCCESS) {
		player_stop(&ctx);
		return ret;
	}
	ret = init_vpss(&ctx);
	if (ret != CVI_SUCCESS) {
		player_stop(&ctx);
		return ret;
	}

	init_send_thread_param(&ctx.vdec_chn, &ctx.vdec_chn.stVdecThreadParamSend,
			       ctx.input_cfg.chnInCfg[0].input_path,
			       ctx.input_cfg.chnInCfg[0].s32sendstream_timeout);
	SAMPLE_COMM_VDEC_StartSendStream(&ctx.vdec_chn.stVdecThreadParamSend,
					 &ctx.vdec_chn.vdecThreadSend);

	signal(SIGINT, player_handle_signal);
	signal(SIGTERM, player_handle_signal);
	signal(SIGTSTP, player_handle_signal);

	fb_clear(&ctx.fb);
	SAMPLE_PRT("fb playback start: %s -> %ux%u fb0 (%ubpp), rotate=%s copy=%s output=%s\n",
		   ctx.input_cfg.chnInCfg[0].input_path,
		   ctx.output_size.u32Width, ctx.output_size.u32Height,
		   ctx.fb.bits_per_pixel,
		   rotate_mode_name(ctx.rotate_mode),
		   copy_mode_name(ctx.copy_mode),
		   output_mode_name(ctx.output_mode));

	ret = playback_loop(&ctx);
	player_stop(&ctx);
	return ret;
}
