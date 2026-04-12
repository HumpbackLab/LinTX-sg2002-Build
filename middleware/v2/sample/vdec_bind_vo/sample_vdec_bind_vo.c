#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sample_comm.h"
#include "sample_vdec_lib.h"

#define PLAYER_VDEC_CHN 0
#define PLAYER_VPSS_GRP 0
#define PLAYER_VPSS_CHN 0

typedef struct fb_ctx_s {
	int fd;
	unsigned char *mem;
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	unsigned int bits_per_pixel;
	unsigned int size;
} fb_ctx_t;

typedef struct player_ctx_s {
	vdecInputCfg input_cfg;
	vdecChnCtx vdec_chn;
	fb_ctx_t fb;
	SIZE_S output_size;
	CVI_BOOL sys_inited;
	CVI_BOOL vdec_started;
	CVI_BOOL vpss_started;
} player_ctx_t;

static volatile sig_atomic_t g_stop_requested = 0;

static void player_handle_signal(int signo)
{
	if (signo == SIGINT || signo == SIGTERM || signo == SIGTSTP)
		g_stop_requested = 1;
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
	fb->mem = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->mem == MAP_FAILED) {
		SAMPLE_PRT("fb mmap failed: %s\n", strerror(errno));
		close(fb->fd);
		fb->fd = -1;
		fb->mem = NULL;
		return -1;
	}

	SAMPLE_PRT("fb0 %ux%u %ubpp stride=%u size=%u\n",
		   fb->width, fb->height, fb->bits_per_pixel, fb->stride, fb->size);
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
	CHECK_RET(CVI_VDEC_StartRecvStream(PLAYER_VDEC_CHN), "CVI_VDEC_StartRecvStream");
	ctx->vdec_started = CVI_TRUE;
	return CVI_SUCCESS;
}

static CVI_S32 init_vpss(player_ctx_t *ctx)
{
	VPSS_GRP_ATTR_S grp_attr;
	VPSS_CHN_ATTR_S chn_attr[VPSS_MAX_PHY_CHN_NUM];
	CVI_BOOL chn_enable[VPSS_MAX_PHY_CHN_NUM] = {0};

	memset(&grp_attr, 0, sizeof(grp_attr));
	memset(chn_attr, 0, sizeof(chn_attr));

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
	chn_attr[PLAYER_VPSS_CHN].enPixelFormat = PIXEL_FORMAT_RGB_888;
	chn_attr[PLAYER_VPSS_CHN].stFrameRate.s32SrcFrameRate = -1;
	chn_attr[PLAYER_VPSS_CHN].stFrameRate.s32DstFrameRate = -1;
	chn_attr[PLAYER_VPSS_CHN].u32Depth = 3;
	chn_attr[PLAYER_VPSS_CHN].bMirror = CVI_FALSE;
	chn_attr[PLAYER_VPSS_CHN].bFlip = CVI_FALSE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.bEnableBgColor = CVI_TRUE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	chn_attr[PLAYER_VPSS_CHN].stNormalize.bEnable = CVI_FALSE;

	CHECK_RET(SAMPLE_COMM_VPSS_Init(PLAYER_VPSS_GRP, chn_enable, &grp_attr, chn_attr),
		  "SAMPLE_COMM_VPSS_Init");
	CHECK_RET(SAMPLE_COMM_VPSS_Start(PLAYER_VPSS_GRP, chn_enable, &grp_attr, chn_attr),
		  "SAMPLE_COMM_VPSS_Start");
	ctx->vpss_started = CVI_TRUE;
	return CVI_SUCCESS;
}

static void blit_rgb888_to_fb(const fb_ctx_t *fb, const VIDEO_FRAME_INFO_S *frame)
{
	const VIDEO_FRAME_S *vframe = &frame->stVFrame;
	unsigned int src_x;
	unsigned int src_y;
	unsigned int copy_width = (vframe->u32Width < fb->height) ? vframe->u32Width : fb->height;
	unsigned int copy_height = (vframe->u32Height < fb->width) ? vframe->u32Height : fb->width;
	const unsigned char *src_base;

	if (vframe->pu8VirAddr[0] == NULL) {
		CVI_SYS_IonInvalidateCache(vframe->u64PhyAddr[0], NULL, vframe->u32Length[0]);
		src_base = CVI_SYS_Mmap(vframe->u64PhyAddr[0], vframe->u32Length[0]);
		if (src_base == NULL) {
			SAMPLE_PRT("CVI_SYS_Mmap RGB frame failed\n");
			return;
		}
	} else {
		CVI_SYS_IonInvalidateCache(vframe->u64PhyAddr[0], vframe->pu8VirAddr[0], vframe->u32Length[0]);
		src_base = vframe->pu8VirAddr[0];
	}

	for (src_y = 0; src_y < copy_height; ++src_y) {
		const unsigned char *src_row = src_base + src_y * vframe->u32Stride[0];

		for (src_x = 0; src_x < copy_width; ++src_x) {
			unsigned int dst_x = src_y;
			unsigned int dst_y = copy_width - 1 - src_x;
			const unsigned char *src = src_row + src_x * 3;
			unsigned char *dst = fb->mem + dst_y * fb->stride;

			if (fb->bits_per_pixel == 32) {
				dst[dst_x * 4 + 0] = src[2];
				dst[dst_x * 4 + 1] = src[1];
				dst[dst_x * 4 + 2] = src[0];
				dst[dst_x * 4 + 3] = 0xff;
			} else if (fb->bits_per_pixel == 24) {
				dst[dst_x * 3 + 0] = src[2];
				dst[dst_x * 3 + 1] = src[1];
				dst[dst_x * 3 + 2] = src[0];
			} else if (fb->bits_per_pixel == 16) {
				unsigned short *dst16 = (unsigned short *)dst;
				unsigned char r = src[0];
				unsigned char g = src[1];
				unsigned char b = src[2];
				dst16[dst_x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
			}
		}
	}

	if (vframe->pu8VirAddr[0] == NULL)
		CVI_SYS_Munmap((void *)src_base, vframe->u32Length[0]);
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

	memset(&vdec_frame, 0, sizeof(vdec_frame));
	memset(&rgb_frame, 0, sizeof(rgb_frame));

	while (!g_stop_requested) {
		ret = CVI_VDEC_GetFrame(PLAYER_VDEC_CHN, &vdec_frame, 1000);
		if (ret == CVI_SUCCESS) {
			ret = CVI_VPSS_SendFrame(PLAYER_VPSS_GRP, &vdec_frame, 1000);
			CVI_VDEC_ReleaseFrame(PLAYER_VDEC_CHN, &vdec_frame);
			memset(&vdec_frame, 0, sizeof(vdec_frame));
			if (ret != CVI_SUCCESS) {
				SAMPLE_PRT("CVI_VPSS_SendFrame failed with %#x\n", ret);
				continue;
			}

			ret = CVI_VPSS_GetChnFrame(PLAYER_VPSS_GRP, PLAYER_VPSS_CHN, &rgb_frame, 1000);
			if (ret != CVI_SUCCESS) {
				SAMPLE_PRT("CVI_VPSS_GetChnFrame failed with %#x\n", ret);
				continue;
			}

			blit_rgb888_to_fb(&ctx->fb, &rgb_frame);
			CVI_VPSS_ReleaseChnFrame(PLAYER_VPSS_GRP, PLAYER_VPSS_CHN, &rgb_frame);
			memset(&rgb_frame, 0, sizeof(rgb_frame));
			idle_loops = 0;
			continue;
		}

		if (ret != CVI_ERR_VDEC_BUSY)
			SAMPLE_PRT("CVI_VDEC_GetFrame failed with %#x\n", ret);

		CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
		if (send_param->bFileEnd &&
		    status.u32LeftStreamBytes == 0 &&
		    status.u32LeftStreamFrames == 0 &&
		    status.u32LeftPics == 0) {
			return CVI_SUCCESS;
		}

		idle_loops++;
		if (idle_loops >= 10) {
			SAMPLE_PRT("vdec recv=%u dec=%u leftBytes=%u leftFrames=%u leftPics=%u fileEnd=%d\n",
				   status.u32RecvStreamFrames,
				   status.u32DecodeStreamFrames,
				   status.u32LeftStreamBytes,
				   status.u32LeftStreamFrames,
				   status.u32LeftPics,
				   send_param->bFileEnd);
			idle_loops = 0;
		}
	}

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
		SAMPLE_COMM_VPSS_Stop(PLAYER_VPSS_GRP, chn_enable);
		ctx->vpss_started = CVI_FALSE;
	}

	if (ctx->vdec_chn.bCreateChn) {
		CVI_VDEC_DestroyChn(PLAYER_VDEC_CHN);
		ctx->vdec_chn.bCreateChn = CVI_FALSE;
		ctx->vdec_started = CVI_FALSE;
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

	ctx.output_size.u32Width = ctx.fb.height;
	ctx.output_size.u32Height = ctx.fb.width;
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
	SAMPLE_PRT("fb playback start: %s -> %ux%u fb0 (%ubpp)\n",
		   ctx.input_cfg.chnInCfg[0].input_path,
		   ctx.output_size.u32Width, ctx.output_size.u32Height,
		   ctx.fb.bits_per_pixel);

	ret = playback_loop(&ctx);
	player_stop(&ctx);
	return ret;
}
