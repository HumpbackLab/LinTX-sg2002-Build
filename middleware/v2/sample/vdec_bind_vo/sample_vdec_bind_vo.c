#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "sample_comm.h"
#include "sample_vdec_lib.h"
#include "cvi_ive.h"
#include "cvi_vdec.h"

#define PLAYER_VDEC_CHN 0
#define PLAYER_VPSS_CHN 0
#define PLAYER_VO_CHN 0
#define PLAYER_PANEL_WIDTH 480
#define PLAYER_PANEL_HEIGHT 800
#define PLAYER_VDEC_NO_FRAME_RET ((CVI_S32)0xC0058041)

#pragma weak CVI_VDEC_SetRotation

typedef enum rotate_mode_e {
	ROTATE_MODE_CPU = 0,
	ROTATE_MODE_VDEC = 1,
	ROTATE_MODE_NONE = 2,
	ROTATE_MODE_VO = 3,
	ROTATE_MODE_VPSS = 4,
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

typedef enum display_backend_e {
	DISPLAY_BACKEND_VO = 0,
	DISPLAY_BACKEND_FB = 1,
} display_backend_t;

typedef enum vo_path_mode_e {
	VO_PATH_MODE_SENDFRAME = 0,
	VO_PATH_MODE_BIND = 1,
} vo_path_mode_t;

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
	SIZE_S panel_size;
	SIZE_S output_size;
	SIZE_S source_size;
	RECT_S video_rect;
	RECT_S content_rect;
	VPSS_GRP vpss_grp;
	SAMPLE_VO_CONFIG_S vo_config;
	rotate_mode_t rotate_mode;
	copy_mode_t copy_mode;
	output_mode_t output_mode;
	display_backend_t display_backend;
	vo_path_mode_t vo_path_mode;
	CVI_BOOL vo_cpu_pad;
	IVE_HANDLE ive_handle;
	CVI_BOOL sys_inited;
	CVI_BOOL vdec_started;
	CVI_BOOL vpss_started;
	CVI_BOOL vo_started;
	CVI_BOOL vdec_vpss_bound;
	CVI_BOOL vpss_vo_bound;
	CVI_BOOL vo_stage_ready;
	VB_BLK vo_stage_blk;
	CVI_VOID *vo_stage_mem;
	CVI_U32 vo_stage_size;
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

static CVI_S32 build_vo_padded_frame(player_ctx_t *ctx, const VIDEO_FRAME_INFO_S *src_frame,
				     VIDEO_FRAME_INFO_S **out_frame);

static CVI_VOID *player_send_stream_thread(CVI_VOID *pArgs)
{
	VDEC_THREAD_PARAM_S *param = (VDEC_THREAD_PARAM_S *)pArgs;
	CVI_BOOL b_end_of_stream = CVI_FALSE;
	CVI_BOOL b_find_start;
	CVI_BOOL b_find_end;
	CVI_U8 *buf = NULL;
	CVI_U64 pts;
	CVI_U32 start_offset;
	CVI_U32 jpeg_len;
	CVI_S32 used_bytes = 0;
	CVI_S32 read_len = 0;
	CVI_S32 ret;
	CVI_S32 i;
	CVI_S32 send_count = 0;
	CVI_S32 retry_count = 0;
	FILE *fp = NULL;
	VDEC_STREAM_S stream;

	prctl(PR_SET_NAME, "VideoSendStream", 0, 0, 0);
	SAMPLE_PRT("send thread start chn=%d file=%s timeout=%d buf=%d mode=%d type=%d\n",
		   param->s32ChnId, param->cFileName, param->s32MilliSec,
		   param->s32MinBufSize, param->s32StreamMode, param->enType);

	fp = fopen(param->cFileName, "rb");
	if (fp == NULL) {
		SAMPLE_PRT("send thread fopen failed chn=%d file=%s err=%s\n",
			   param->s32ChnId, param->cFileName, strerror(errno));
		param->bFileEnd = CVI_TRUE;
		return (CVI_VOID *)(uintptr_t)CVI_FAILURE;
	}
	SAMPLE_PRT("send thread fopen ok chn=%d file=%s\n", param->s32ChnId, param->cFileName);

	buf = malloc(param->s32MinBufSize);
	if (buf == NULL) {
		SAMPLE_PRT("send thread malloc failed chn=%d size=%d\n",
			   param->s32ChnId, param->s32MinBufSize);
		fclose(fp);
		param->bFileEnd = CVI_TRUE;
		return (CVI_VOID *)(uintptr_t)CVI_FAILURE;
	}

	pts = param->u64PtsInit;
	while (1) {
		if (param->eThreadCtrl == THREAD_CTRL_STOP) {
			SAMPLE_PRT("send thread stop requested chn=%d\n", param->s32ChnId);
			break;
		}
		if (param->eThreadCtrl == THREAD_CTRL_PAUSE) {
			usleep(100000);
			continue;
		}

		b_end_of_stream = CVI_FALSE;
		b_find_start = CVI_FALSE;
		b_find_end = CVI_FALSE;
		start_offset = 0;

		if (fseek(fp, used_bytes, SEEK_SET) != 0) {
			SAMPLE_PRT("send thread fseek failed chn=%d used=%d err=%s\n",
				   param->s32ChnId, used_bytes, strerror(errno));
			break;
		}

		read_len = fread(buf, 1, param->s32MinBufSize, fp);
		if (send_count < 3)
			SAMPLE_PRT("send thread read chn=%d used=%d len=%d\n",
				   param->s32ChnId, used_bytes, read_len);

		if (read_len == 0) {
			if (param->bCircleSend == CVI_TRUE) {
				memset(&stream, 0, sizeof(stream));
				stream.bEndOfStream = CVI_TRUE;
				ret = CVI_VDEC_SendStream(param->s32ChnId, &stream, 1000);
				SAMPLE_PRT("send thread loop eos chn=%d ret=%#x\n",
					   param->s32ChnId, ret);
				if (ret != CVI_SUCCESS && ret != CVI_ERR_VDEC_BUSY)
					break;
				used_bytes = 0;
				fseek(fp, 0, SEEK_SET);
				read_len = fread(buf, 1, param->s32MinBufSize, fp);
			} else {
				SAMPLE_PRT("send thread eof chn=%d used=%d sends=%d\n",
					   param->s32ChnId, used_bytes, send_count);
				break;
			}
		}

		if (param->s32StreamMode == VIDEO_MODE_FRAME && param->enType == PT_H264) {
			for (i = 0; i < read_len - 8; i++) {
				int tmp = buf[i + 3] & 0x1F;

				if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
				    (((tmp == 0x5 || tmp == 0x1) && ((buf[i + 4] & 0x80) == 0x80)) ||
				     (tmp == 20 && (buf[i + 7] & 0x80) == 0x80))) {
					b_find_start = CVI_TRUE;
					i += 8;
					break;
				}
			}

			for (; i < read_len - 8; i++) {
				int tmp = buf[i + 3] & 0x1F;

				if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
				    (tmp == 15 || tmp == 7 || tmp == 8 || tmp == 6 ||
				     ((tmp == 5 || tmp == 1) && ((buf[i + 4] & 0x80) == 0x80)) ||
				     (tmp == 20 && (buf[i + 7] & 0x80) == 0x80))) {
					b_find_end = CVI_TRUE;
					break;
				}
			}

			if (i > 0)
				read_len = i;
			if (!b_find_start) {
				SAMPLE_PRT("send thread no H264 start chn=%d used=%d len=%d\n",
					   param->s32ChnId, used_bytes, read_len);
			}
			if (!b_find_end)
				read_len = i + 8;
		} else if (param->s32StreamMode == VIDEO_MODE_FRAME && param->enType == PT_H265) {
			CVI_BOOL b_new_pic = CVI_FALSE;

			for (i = 0; i < read_len - 6; i++) {
				CVI_U32 tmp = (buf[i + 3] & 0x7E) >> 1;

				b_new_pic = (buf[i + 0] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
					     (tmp <= 21) && ((buf[i + 5] & 0x80) == 0x80));
				if (b_new_pic) {
					b_find_start = CVI_TRUE;
					i += 6;
					break;
				}
			}

			for (; i < read_len - 6; i++) {
				CVI_U32 tmp = (buf[i + 3] & 0x7E) >> 1;

				b_new_pic = (buf[i + 0] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
					     (tmp == 32 || tmp == 33 || tmp == 34 || tmp == 39 || tmp == 40 ||
					      ((tmp <= 21) && (buf[i + 5] & 0x80) == 0x80)));
				if (b_new_pic) {
					b_find_end = CVI_TRUE;
					break;
				}
			}

			if (i > 0)
				read_len = i;
			if (!b_find_start) {
				SAMPLE_PRT("send thread no H265 start chn=%d used=%d len=%d\n",
					   param->s32ChnId, used_bytes, read_len);
			}
			if (!b_find_end)
				read_len = i + 6;
		} else if (param->enType == PT_MJPEG || param->enType == PT_JPEG) {
			for (i = 0; i < read_len - 1; i++) {
				if (buf[i] == 0xFF && buf[i + 1] == 0xD8) {
					start_offset = i;
					b_find_start = CVI_TRUE;
					i += 2;
					break;
				}
			}

			for (; i < read_len - 3; i++) {
				if (buf[i] == 0xFF && (buf[i + 1] & 0xF0) == 0xE0) {
					jpeg_len = (buf[i + 2] << 8) + buf[i + 3];
					i += 1 + jpeg_len;
				} else {
					break;
				}
			}

			for (; i < read_len - 1; i++) {
				if (buf[i] == 0xFF && buf[i + 1] == 0xD9) {
					b_find_end = CVI_TRUE;
					break;
				}
			}
			read_len = i + 2;
			if (!b_find_start) {
				SAMPLE_PRT("send thread no JPEG start chn=%d used=%d len=%d\n",
					   param->s32ChnId, used_bytes, read_len);
			}
		} else if (read_len < param->s32MinBufSize) {
			b_end_of_stream = CVI_TRUE;
		}

		memset(&stream, 0, sizeof(stream));
		stream.u64PTS = pts;
		stream.pu8Addr = buf + start_offset;
		stream.u32Len = read_len;
		stream.bEndOfFrame = (param->s32StreamMode == VIDEO_MODE_FRAME) ? CVI_TRUE : CVI_FALSE;
		stream.bEndOfStream = b_end_of_stream;
		stream.bDisplay = 1;

		retry_count = 0;
		while (param->eThreadCtrl == THREAD_CTRL_START) {
			ret = CVI_VDEC_SendStream(param->s32ChnId, &stream, param->s32MilliSec);
			if (ret == CVI_SUCCESS) {
				if (send_count < 5 || retry_count > 0) {
					SAMPLE_PRT("send thread ok chn=%d send=%d used=%d len=%u retries=%d eos=%d\n",
						   param->s32ChnId, send_count, used_bytes, stream.u32Len,
						   retry_count, stream.bEndOfStream);
				}
				break;
			}

			retry_count++;
			if (retry_count <= 5 || (retry_count % 20) == 0) {
				SAMPLE_PRT("send thread CVI_VDEC_SendStream chn=%d ret=%#x used=%d len=%u retry=%d\n",
					   param->s32ChnId, ret, used_bytes, stream.u32Len, retry_count);
			}
			if (ret != CVI_ERR_VDEC_BUSY)
				usleep(10000);
			usleep(param->s32IntervalTime);
		}

		if (ret != CVI_SUCCESS)
			break;

		send_count++;
		used_bytes += read_len + start_offset;
		pts += param->u64PtsIncrease;
		usleep(param->s32IntervalTime);
	}

	memset(&stream, 0, sizeof(stream));
	stream.bEndOfStream = CVI_TRUE;
	ret = CVI_VDEC_SendStream(param->s32ChnId, &stream, 1000);
	SAMPLE_PRT("send thread final eos chn=%d ret=%#x sends=%d\n",
		   param->s32ChnId, ret, send_count);
	param->bFileEnd = CVI_TRUE;
	if (buf != NULL)
		free(buf);
	if (fp != NULL)
		fclose(fp);
	SAMPLE_PRT("send thread exit chn=%d fileEnd=%d sends=%d used=%d\n",
		   param->s32ChnId, param->bFileEnd, send_count, used_bytes);
	return (CVI_VOID *)(uintptr_t)CVI_SUCCESS;
}

static void player_start_send_stream(VDEC_THREAD_PARAM_S *pstVdecSend,
				     pthread_t *pVdecThread)
{
	struct sched_param param;
	pthread_attr_t attr;
	int ret;

	param.sched_priority = 80;
	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

	ret = pthread_create(pVdecThread, &attr, player_send_stream_thread, (CVI_VOID *)pstVdecSend);
	pthread_attr_destroy(&attr);
	if (ret == 0)
		return;

	SAMPLE_PRT("pthread_create SCHED_RR failed for chn %d: %s, fallback to default attr\n",
		   pstVdecSend->s32ChnId, strerror(ret));
	ret = pthread_create(pVdecThread, NULL, player_send_stream_thread, (CVI_VOID *)pstVdecSend);
	if (ret != 0) {
		SAMPLE_PRT("pthread_create fallback failed for chn %d: %s\n",
			   pstVdecSend->s32ChnId, strerror(ret));
		*pVdecThread = 0;
	}
}

static const char *rotate_mode_name(rotate_mode_t mode)
{
	if (mode == ROTATE_MODE_VO)
		return "vo";
	if (mode == ROTATE_MODE_VPSS)
		return "vpss";
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

static const char *vpss_output_name(const player_ctx_t *ctx)
{
	if (ctx->display_backend == DISPLAY_BACKEND_VO)
		return "nv21";
	return output_mode_name(ctx->output_mode);
}

static const char *display_backend_name(display_backend_t backend)
{
	return (backend == DISPLAY_BACKEND_FB) ? "fb" : "vo";
}

static const char *vo_path_mode_name(vo_path_mode_t mode)
{
	return (mode == VO_PATH_MODE_BIND) ? "bind" : "sendframe";
}

static display_backend_t detect_display_backend(void)
{
	const char *mode = getenv("SAMPLE_VDEC_DISPLAY_BACKEND");

	if (mode != NULL && strcmp(mode, "fb") == 0)
		return DISPLAY_BACKEND_FB;
	if (mode != NULL && strcmp(mode, "vo") == 0)
		return DISPLAY_BACKEND_VO;
	if (getenv("SAMPLE_VDEC_FB_ROTATE_MODE") != NULL ||
	    getenv("SAMPLE_VDEC_FB_COPY_MODE") != NULL ||
	    getenv("SAMPLE_VDEC_FB_OUTPUT_MODE") != NULL)
		return DISPLAY_BACKEND_FB;

	return DISPLAY_BACKEND_VO;
}

static vo_path_mode_t detect_vo_path_mode(void)
{
	const char *mode = getenv("SAMPLE_VDEC_VO_PATH");

	if (mode != NULL && strcmp(mode, "bind") == 0)
		return VO_PATH_MODE_BIND;
	return VO_PATH_MODE_SENDFRAME;
}

static rotate_mode_t detect_rotate_mode(display_backend_t backend)
{
	const char *mode = getenv("SAMPLE_VDEC_BIND_ROTATE_MODE");

	if (mode == NULL)
		mode = getenv("SAMPLE_VDEC_FB_ROTATE_MODE");

	if (mode != NULL && strcmp(mode, "vo") == 0)
		return ROTATE_MODE_VO;
	if (mode != NULL && strcmp(mode, "vpss") == 0)
		return ROTATE_MODE_VPSS;
	if (mode != NULL && strcmp(mode, "cpu") == 0)
		return ROTATE_MODE_CPU;

	if (mode != NULL && strcmp(mode, "vdec") == 0)
		return ROTATE_MODE_VDEC;
	if (mode != NULL && strcmp(mode, "none") == 0)
		return ROTATE_MODE_NONE;

	return (backend == DISPLAY_BACKEND_FB) ? ROTATE_MODE_CPU : ROTATE_MODE_VO;
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

static ROTATION_E requested_rotation(const player_ctx_t *ctx)
{
	(void)ctx;
	return ROTATION_270;
}

static void sanitize_rotate_mode(player_ctx_t *ctx)
{
	if (ctx->display_backend == DISPLAY_BACKEND_FB) {
		if (ctx->rotate_mode == ROTATE_MODE_VO || ctx->rotate_mode == ROTATE_MODE_VPSS) {
			SAMPLE_PRT("fb backend does not use VO/VPSS rotation, fallback to cpu rotate\n");
			ctx->rotate_mode = ROTATE_MODE_CPU;
		}
		if (ctx->rotate_mode == ROTATE_MODE_VDEC && CVI_VDEC_SetRotation == NULL) {
			SAMPLE_PRT("CVI_VDEC_SetRotation not exported by current SDK, fallback to cpu rotate\n");
			ctx->rotate_mode = ROTATE_MODE_CPU;
		}
		return;
	}

	if (ctx->rotate_mode == ROTATE_MODE_CPU) {
		SAMPLE_PRT("vo backend does not use cpu rotation, fallback to vo rotation\n");
		ctx->rotate_mode = ROTATE_MODE_VO;
		return;
	}
	if (ctx->rotate_mode == ROTATE_MODE_VDEC && CVI_VDEC_SetRotation == NULL) {
		SAMPLE_PRT("CVI_VDEC_SetRotation not exported by current SDK, fallback to vo rotation\n");
		ctx->rotate_mode = ROTATE_MODE_VO;
	}
}

static void sanitize_copy_mode(player_ctx_t *ctx)
{
	if (ctx->display_backend != DISPLAY_BACKEND_FB) {
		ctx->copy_mode = COPY_MODE_CPU;
		return;
	}

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

static CVI_BOOL is_vdec_getframe_idle_ret(CVI_S32 ret)
{
	return (ret == CVI_ERR_VDEC_BUSY || ret == PLAYER_VDEC_NO_FRAME_RET);
}

static CVI_BOOL detect_env_enabled(const char *name, CVI_BOOL default_value)
{
	const char *value = getenv(name);

	if (value == NULL || value[0] == '\0')
		return default_value;
	if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "off") || !strcmp(value, "no"))
		return CVI_FALSE;
	if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "on") || !strcmp(value, "yes"))
		return CVI_TRUE;

	SAMPLE_PRT("ignore invalid %s=%s, fallback to %d\n", name, value, default_value);
	return default_value;
}

static CVI_U32 detect_env_u32(const char *name, CVI_U32 default_value)
{
	const char *value = getenv(name);
	char *end = NULL;
	unsigned long parsed;

	if (value == NULL || value[0] == '\0')
		return default_value;

	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > UINT32_MAX) {
		SAMPLE_PRT("ignore invalid %s=%s, fallback to %u\n", name, value, default_value);
		return default_value;
	}

	return (CVI_U32)parsed;
}

static CVI_U32 align_even_down(CVI_U32 value)
{
	if (value <= 2)
		return value;
	return value & ~1U;
}

static void compute_centered_rect(const SIZE_S *src, const SIZE_S *dst, RECT_S *rect)
{
	CVI_U32 scaled_width;
	CVI_U32 scaled_height;

	memset(rect, 0, sizeof(*rect));
	if (src->u32Width == 0 || src->u32Height == 0 ||
	    dst->u32Width == 0 || dst->u32Height == 0) {
		rect->u32Width = dst->u32Width;
		rect->u32Height = dst->u32Height;
		return;
	}

	if ((CVI_U64)src->u32Width * dst->u32Height >= (CVI_U64)dst->u32Width * src->u32Height) {
		scaled_width = dst->u32Width;
		scaled_height = (CVI_U32)(((CVI_U64)dst->u32Width * src->u32Height) / src->u32Width);
	} else {
		scaled_height = dst->u32Height;
		scaled_width = (CVI_U32)(((CVI_U64)dst->u32Height * src->u32Width) / src->u32Height);
	}

	scaled_width = align_even_down(scaled_width);
	scaled_height = align_even_down(scaled_height);
	if (scaled_width == 0)
		scaled_width = (dst->u32Width > 1) ? 2 : dst->u32Width;
	if (scaled_height == 0)
		scaled_height = (dst->u32Height > 1) ? 2 : dst->u32Height;
	if (scaled_width > dst->u32Width)
		scaled_width = align_even_down(dst->u32Width);
	if (scaled_height > dst->u32Height)
		scaled_height = align_even_down(dst->u32Height);

	rect->u32Width = scaled_width;
	rect->u32Height = scaled_height;
	rect->s32X = (CVI_S32)((dst->u32Width - rect->u32Width) / 2);
	rect->s32Y = (CVI_S32)((dst->u32Height - rect->u32Height) / 2);
	rect->s32X &= ~1;
	rect->s32Y &= ~1;
}

static void compute_display_layout(player_ctx_t *ctx, CVI_U32 src_width, CVI_U32 src_height)
{
	SIZE_S layout_src;

	ctx->source_size.u32Width = src_width;
	ctx->source_size.u32Height = src_height;
	compute_centered_rect(&ctx->source_size, &ctx->output_size, &ctx->content_rect);
	layout_src = ctx->source_size;
	if (ctx->rotate_mode == ROTATE_MODE_VO || ctx->rotate_mode == ROTATE_MODE_VPSS) {
		layout_src.u32Width = src_height;
		layout_src.u32Height = src_width;
	}
	compute_centered_rect(&layout_src, &ctx->panel_size, &ctx->video_rect);
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

	(void)status;
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

	printf("\rstatus shown=%llu fps=%.1f avg=%.1f cpu=%.1f%% elapsed=%.1fs",
	       (unsigned long long)ctx->shown_frames,
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

	(void)status;
	gettimeofday(&now, NULL);
	getrusage(RUSAGE_SELF, &usage);
	elapsed = timeval_diff_sec(&now, &ctx->start_wall);
	cpu_sec = rusage_cpu_sec(&usage);
	avg_fps = (elapsed > 0.0) ? ((double)ctx->shown_frames / elapsed) : 0.0;

	printf("\nsummary shown=%llu avg_fps=%.2f cpu_time=%.2fs elapsed=%.2fs getframe_timeouts=%llu vpss_send_fail=%llu vpss_get_fail=%llu vdec_get_ms=%.1f(%.3f/f) vpss_send_ms=%.1f(%.3f/f) vpss_get_ms=%.1f(%.3f/f) blit_ms=%.1f(%.3f/f) tdma_ms=%.1f(%.3f/f)\n",
	       (unsigned long long)ctx->shown_frames,
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
	       ctx->time_tdma_sec * 1000.0, avg_ms_per_frame(ctx->time_tdma_sec, ctx->shown_frames));
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

static CVI_S32 vo_stage_open(player_ctx_t *ctx)
{
	VB_CAL_CONFIG_S cal_cfg;
	CVI_U64 phy_addr;
	CVI_U32 chroma_offset;
	CVI_VOID *vir_addr = NULL;

	memset(&ctx->stage_frame, 0, sizeof(ctx->stage_frame));
	ctx->vo_stage_blk = VB_INVALID_HANDLE;
	ctx->vo_stage_mem = NULL;
	ctx->vo_stage_size = 0;

	COMMON_GetPicBufferConfig(ctx->output_size.u32Width, ctx->output_size.u32Height,
				  PIXEL_FORMAT_NV21, DATA_BITWIDTH_8,
				  COMPRESS_MODE_NONE, DEFAULT_ALIGN, &cal_cfg);
	ctx->vo_stage_blk = CVI_VB_GetBlock(VB_INVALID_POOLID, cal_cfg.u32VBSize);
	if (ctx->vo_stage_blk == VB_INVALID_HANDLE) {
		SAMPLE_PRT("vo stage get vb block failed size=%u\n", cal_cfg.u32VBSize);
		return CVI_FAILURE;
	}

	phy_addr = CVI_VB_Handle2PhysAddr(ctx->vo_stage_blk);
	vir_addr = CVI_SYS_MmapCache(phy_addr, cal_cfg.u32VBSize);
	if (vir_addr == NULL) {
		SAMPLE_PRT("vo stage mmap failed phy=%#lx size=%u\n", (unsigned long)phy_addr, cal_cfg.u32VBSize);
		CVI_VB_ReleaseBlock(ctx->vo_stage_blk);
		ctx->vo_stage_blk = VB_INVALID_HANDLE;
		return CVI_FAILURE;
	}

	chroma_offset = ALIGN(cal_cfg.u32MainYSize, cal_cfg.u16AddrAlign);
	ctx->vo_stage_mem = vir_addr;
	ctx->vo_stage_size = cal_cfg.u32VBSize;
	ctx->stage_frame.u32PoolId = CVI_VB_Handle2PoolId(ctx->vo_stage_blk);
	ctx->stage_frame.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
	ctx->stage_frame.stVFrame.enPixelFormat = PIXEL_FORMAT_NV21;
	ctx->stage_frame.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
	ctx->stage_frame.stVFrame.enColorGamut = COLOR_GAMUT_BT601;
	ctx->stage_frame.stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
	ctx->stage_frame.stVFrame.u32Width = ctx->output_size.u32Width;
	ctx->stage_frame.stVFrame.u32Height = ctx->output_size.u32Height;
	ctx->stage_frame.stVFrame.u32Stride[0] = cal_cfg.u32MainStride;
	ctx->stage_frame.stVFrame.u32Stride[1] = cal_cfg.u32CStride;
	ctx->stage_frame.stVFrame.u32Length[0] = cal_cfg.u32MainYSize;
	ctx->stage_frame.stVFrame.u32Length[1] = cal_cfg.u32MainCSize;
	ctx->stage_frame.stVFrame.u64PhyAddr[0] = phy_addr;
	ctx->stage_frame.stVFrame.u64PhyAddr[1] = phy_addr + chroma_offset;
	ctx->stage_frame.stVFrame.pu8VirAddr[0] = vir_addr;
	ctx->stage_frame.stVFrame.pu8VirAddr[1] = (CVI_U8 *)vir_addr + chroma_offset;
	ctx->vo_stage_ready = CVI_TRUE;
	SAMPLE_PRT("vo stage %ux%u stride=%u/%u phys=%#lx size=%u\n",
		   ctx->stage_frame.stVFrame.u32Width, ctx->stage_frame.stVFrame.u32Height,
		   ctx->stage_frame.stVFrame.u32Stride[0], ctx->stage_frame.stVFrame.u32Stride[1],
		   (unsigned long)ctx->stage_frame.stVFrame.u64PhyAddr[0], ctx->vo_stage_size);
	return CVI_SUCCESS;
}

static void vo_stage_close(player_ctx_t *ctx)
{
	if (!ctx->vo_stage_ready)
		return;

	if (ctx->vo_stage_mem != NULL)
		CVI_SYS_Munmap(ctx->vo_stage_mem, ctx->vo_stage_size);
	if (ctx->vo_stage_blk != VB_INVALID_HANDLE)
		CVI_VB_ReleaseBlock(ctx->vo_stage_blk);

	memset(&ctx->stage_frame, 0, sizeof(ctx->stage_frame));
	ctx->vo_stage_blk = VB_INVALID_HANDLE;
	ctx->vo_stage_mem = NULL;
	ctx->vo_stage_size = 0;
	ctx->vo_stage_ready = CVI_FALSE;
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
	param->s32MilliSec = (timeout_ms < 0) ? 1000 : timeout_ms;
	param->s32MinBufSize = (attr->u32Width * attr->u32Height * 3) >> 1;
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
	if (chn_cfg->u32BufWidth >= 640 && chn_cfg->u32BufHeight > 0 && chn_cfg->u32BufHeight < 128) {
		SAMPLE_PRT("suspicious decode buffer size %ux%u, force fallback to 1920x1080\n",
			   chn_cfg->u32BufWidth, chn_cfg->u32BufHeight);
		if (chn_cfg->u32BufWidth < 1920)
			chn_cfg->u32BufWidth = 1920;
		chn_cfg->u32BufHeight = 1080;
	}
	if (chn_cfg->u32MaxFrameBuffer == 0)
		chn_cfg->u32MaxFrameBuffer = 8;

	SAMPLE_PRT("validated config codec=%d input=%s buf=%ux%u maxframe=%u send_to=%d get_to=%d\n",
		   chn_cfg->enType, chn_cfg->input_path, chn_cfg->u32BufWidth, chn_cfg->u32BufHeight,
		   chn_cfg->u32MaxFrameBuffer, chn_cfg->s32sendstream_timeout,
		   chn_cfg->s32getframe_timeout);

	return CVI_SUCCESS;
}

static CVI_S32 init_system(player_ctx_t *ctx)
{
	VB_CONFIG_S vb_conf;
	vdecChnInputCfg *chn_cfg = &ctx->input_cfg.chnInCfg[0];
	CVI_U32 decode_blk_size;
	CVI_U32 display_blk_size;
	CVI_U32 decode_blk_cnt;
	CVI_U32 display_blk_cnt;
	PIXEL_FORMAT_E display_fmt;

	memset(&vb_conf, 0, sizeof(vb_conf));
	decode_blk_size = COMMON_GetPicBufferSize(chn_cfg->u32BufWidth, chn_cfg->u32BufHeight,
						  PIXEL_FORMAT_NV21, DATA_BITWIDTH_8,
						  COMPRESS_MODE_NONE, DEFAULT_ALIGN);
	if (ctx->display_backend == DISPLAY_BACKEND_VO)
		display_fmt = PIXEL_FORMAT_NV21;
	else if (ctx->output_mode == OUTPUT_MODE_ARGB8888)
		display_fmt = PIXEL_FORMAT_ARGB_8888;
	else
		display_fmt = PIXEL_FORMAT_BGR_888;
	display_blk_size = COMMON_GetPicBufferSize(ctx->output_size.u32Width,
						   ctx->output_size.u32Height,
						   display_fmt,
						   DATA_BITWIDTH_8,
						   COMPRESS_MODE_NONE,
						   DEFAULT_ALIGN);
	decode_blk_cnt = chn_cfg->u32MaxFrameBuffer + 2;
	if (decode_blk_cnt < 8)
		decode_blk_cnt = 8;
	display_blk_cnt = 6;
	if (ctx->display_backend == DISPLAY_BACKEND_FB)
		display_blk_cnt = 4;

	vb_conf.u32MaxPoolCnt = 2;
	vb_conf.astCommPool[0].u32BlkSize = decode_blk_size;
	vb_conf.astCommPool[0].u32BlkCnt = decode_blk_cnt;
	vb_conf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	vb_conf.astCommPool[1].u32BlkSize = display_blk_size;
	vb_conf.astCommPool[1].u32BlkCnt = display_blk_cnt;
	vb_conf.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;

	SAMPLE_PRT("VB config decode_blk=%u x %u display_blk=%u x %u\n",
		   decode_blk_size, decode_blk_cnt, display_blk_size, display_blk_cnt);
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
	chn_ctx->stSampleVdecAttr.u32DisplayFrameNum = 3;

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
	chn_param.u32DisplayFrameNum = 3;
	SAMPLE_PRT("init_vdec width=%u height=%u frameBufCnt=%u displayFrameNum=%u streamBuf=%u\n",
		   chn_attr.u32PicWidth, chn_attr.u32PicHeight, chn_attr.u32FrameBufCnt,
		   chn_param.u32DisplayFrameNum, chn_attr.u32StreamBufSize);
	CHECK_RET(CVI_VDEC_SetChnParam(PLAYER_VDEC_CHN, &chn_param), "CVI_VDEC_SetChnParam");
	if (ctx->rotate_mode == ROTATE_MODE_VDEC) {
		ret = CVI_VDEC_SetRotation(PLAYER_VDEC_CHN, requested_rotation(ctx));
		if (ret != CVI_SUCCESS) {
			SAMPLE_PRT("CVI_VDEC_SetRotation failed with %#x\n", ret);
			return ret;
		}
		SAMPLE_PRT("using VDEC rotation path\n");
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
	chn_attr[PLAYER_VPSS_CHN].u32Width = (ctx->display_backend == DISPLAY_BACKEND_VO &&
					      ctx->vo_cpu_pad) ?
		ctx->content_rect.u32Width : ctx->output_size.u32Width;
	chn_attr[PLAYER_VPSS_CHN].u32Height = (ctx->display_backend == DISPLAY_BACKEND_VO &&
					       ctx->vo_cpu_pad) ?
		ctx->content_rect.u32Height : ctx->output_size.u32Height;
	chn_attr[PLAYER_VPSS_CHN].enVideoFormat = VIDEO_FORMAT_LINEAR;
	if (ctx->display_backend == DISPLAY_BACKEND_VO)
		out_fmt = PIXEL_FORMAT_NV21;
	else
		out_fmt = (ctx->output_mode == OUTPUT_MODE_ARGB8888) ? PIXEL_FORMAT_ARGB_8888 : PIXEL_FORMAT_BGR_888;
	chn_attr[PLAYER_VPSS_CHN].enPixelFormat = out_fmt;
	chn_attr[PLAYER_VPSS_CHN].stFrameRate.s32SrcFrameRate = -1;
	chn_attr[PLAYER_VPSS_CHN].stFrameRate.s32DstFrameRate = -1;
	chn_attr[PLAYER_VPSS_CHN].u32Depth = 3;
	chn_attr[PLAYER_VPSS_CHN].bMirror = CVI_FALSE;
	chn_attr[PLAYER_VPSS_CHN].bFlip = CVI_FALSE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.enMode = ASPECT_RATIO_NONE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.bEnableBgColor = CVI_TRUE;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.stVideoRect.s32X = 0;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.stVideoRect.s32Y = 0;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.stVideoRect.u32Width = chn_attr[PLAYER_VPSS_CHN].u32Width;
	chn_attr[PLAYER_VPSS_CHN].stAspectRatio.stVideoRect.u32Height = chn_attr[PLAYER_VPSS_CHN].u32Height;
	chn_attr[PLAYER_VPSS_CHN].stNormalize.bEnable = CVI_FALSE;

	ret = SAMPLE_COMM_VPSS_Init(ctx->vpss_grp, chn_enable, &grp_attr, chn_attr);
	if (ret != CVI_SUCCESS && ctx->display_backend == DISPLAY_BACKEND_FB &&
	    ctx->output_mode == OUTPUT_MODE_ARGB8888) {
		SAMPLE_PRT("VPSS argb8888 output failed with %#x, fallback to bgr888\n", ret);
		ctx->output_mode = OUTPUT_MODE_BGR888;
		chn_attr[PLAYER_VPSS_CHN].enPixelFormat = PIXEL_FORMAT_BGR_888;
		ret = SAMPLE_COMM_VPSS_Init(ctx->vpss_grp, chn_enable, &grp_attr, chn_attr);
	}
	CHECK_RET(ret, "SAMPLE_COMM_VPSS_Init");
	CHECK_RET(SAMPLE_COMM_VPSS_Start(ctx->vpss_grp, chn_enable, &grp_attr, chn_attr),
		  "SAMPLE_COMM_VPSS_Start");
	if (ctx->rotate_mode == ROTATE_MODE_VPSS) {
		CHECK_RET(CVI_VPSS_SetChnRotation(ctx->vpss_grp, PLAYER_VPSS_CHN, requested_rotation(ctx)),
			  "CVI_VPSS_SetChnRotation");
	}
	SAMPLE_PRT("using VPSS grp %d output=%s\n", ctx->vpss_grp, vpss_output_name(ctx));
	ctx->vpss_started = CVI_TRUE;
	return CVI_SUCCESS;
}

static CVI_S32 init_vo(player_ctx_t *ctx)
{
	CVI_S32 ret;

	memset(&ctx->vo_config, 0, sizeof(ctx->vo_config));
	ret = SAMPLE_COMM_VO_GetDefConfig(&ctx->vo_config);
	if (ret != CVI_SUCCESS)
		return ret;

	ctx->vo_config.VoDev = 0;
	ctx->vo_config.stVoPubAttr.enIntfType = VO_INTF_MIPI;
	ctx->vo_config.stVoPubAttr.enIntfSync = VO_OUTPUT_480x800_60;
	ctx->vo_config.stDispRect.s32X = 0;
	ctx->vo_config.stDispRect.s32Y = 0;
	ctx->vo_config.stDispRect.u32Width = ctx->panel_size.u32Width;
	ctx->vo_config.stDispRect.u32Height = ctx->panel_size.u32Height;
	ctx->vo_config.stImageSize = ctx->panel_size;
	ctx->vo_config.enPixFormat = PIXEL_FORMAT_NV21;
	ctx->vo_config.enVoMode = VO_MODE_1MUX;
	ctx->vo_config.u32DisBufLen = 3;

	CHECK_RET(SAMPLE_COMM_VO_StartVO(&ctx->vo_config), "SAMPLE_COMM_VO_StartVO");
	ctx->vo_started = CVI_TRUE;
	return CVI_SUCCESS;
}

static CVI_S32 apply_vo_layout(player_ctx_t *ctx)
{
	VO_CHN_ATTR_S chn_attr;
	CVI_S32 ret;

	memset(&chn_attr, 0, sizeof(chn_attr));
	chn_attr.u32Priority = 0;
	if (ctx->vo_cpu_pad) {
		chn_attr.stRect.s32X = 0;
		chn_attr.stRect.s32Y = 0;
		chn_attr.stRect.u32Width = ctx->panel_size.u32Width;
		chn_attr.stRect.u32Height = ctx->panel_size.u32Height;
	} else {
		chn_attr.stRect = ctx->video_rect;
	}

	CHECK_RET(CVI_VO_DisableChn(ctx->vo_config.VoDev, PLAYER_VO_CHN), "CVI_VO_DisableChn");
	ret = CVI_VO_SetChnAttr(ctx->vo_config.VoDev, PLAYER_VO_CHN, &chn_attr);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VO_SetChnAttr layout failed with %#x\n", ret);
		return ret;
	}
	CHECK_RET(CVI_VO_EnableChn(ctx->vo_config.VoDev, PLAYER_VO_CHN), "CVI_VO_EnableChn");
	if (ctx->rotate_mode == ROTATE_MODE_VO) {
		CHECK_RET(CVI_VO_SetChnRotation(ctx->vo_config.VoDev, PLAYER_VO_CHN, requested_rotation(ctx)),
			  "CVI_VO_SetChnRotation");
	}
	SAMPLE_PRT("vo rect=%d,%d %ux%u cpu_pad=%d\n",
		   chn_attr.stRect.s32X, chn_attr.stRect.s32Y,
		   chn_attr.stRect.u32Width, chn_attr.stRect.u32Height,
		   ctx->vo_cpu_pad);
	return CVI_SUCCESS;
}

static CVI_S32 bind_pipeline(player_ctx_t *ctx)
{
	CHECK_RET(SAMPLE_COMM_VPSS_Bind_VO(ctx->vpss_grp, PLAYER_VPSS_CHN,
					   ctx->vo_config.VoDev, PLAYER_VO_CHN),
		  "SAMPLE_COMM_VPSS_Bind_VO");
	ctx->vpss_vo_bound = CVI_TRUE;
	CHECK_RET(SAMPLE_COMM_VDEC_Bind_VPSS(PLAYER_VDEC_CHN, ctx->vpss_grp),
		  "SAMPLE_COMM_VDEC_Bind_VPSS");
	ctx->vdec_vpss_bound = CVI_TRUE;
	return CVI_SUCCESS;
}

static CVI_S32 playback_loop_vo_bind(player_ctx_t *ctx)
{
	VDEC_THREAD_PARAM_S *send_param = &ctx->vdec_chn.stVdecThreadParamSend;
	VDEC_CHN_STATUS_S status;
	CVI_S32 ret;
	gettimeofday(&ctx->start_wall, NULL);
	ctx->last_status_wall = ctx->start_wall;
	getrusage(RUSAGE_SELF, &ctx->last_status_usage);
	ctx->status_initialized = CVI_TRUE;

	while (!g_stop_requested) {
		ret = CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status);
		if (ret != CVI_SUCCESS) {
			SAMPLE_PRT("CVI_VDEC_QueryStatus failed with %#x\n", ret);
			return ret;
		}

		ctx->shown_frames = status.u32DecodeStreamFrames;
		{
			struct timeval now;

			gettimeofday(&now, NULL);
			if (timeval_diff_sec(&now, &ctx->last_status_wall) >= 0.5)
				print_status_line(ctx, &status);
		}

		if (send_param->bFileEnd &&
		    status.u32LeftStreamBytes == 0 &&
		    status.u32LeftStreamFrames == 0 &&
		    status.u32LeftPics == 0) {
			print_summary(ctx, &status);
			return CVI_SUCCESS;
		}

		usleep(100000);
	}

	ret = CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status);
	if (ret == CVI_SUCCESS) {
		ctx->shown_frames = status.u32DecodeStreamFrames;
		print_summary(ctx, &status);
	}
	return ret;
}

static CVI_S32 playback_loop_vo_sendframe(player_ctx_t *ctx)
{
	VDEC_THREAD_PARAM_S *send_param = &ctx->vdec_chn.stVdecThreadParamSend;
	VIDEO_FRAME_INFO_S vdec_frame;
	VIDEO_FRAME_INFO_S vo_frame;
	VIDEO_FRAME_INFO_S *send_frame;
	VDEC_CHN_STATUS_S status;
	CVI_S32 ret;
	CVI_BOOL logged_first_frame = CVI_FALSE;
	unsigned int idle_loops = 0;

	memset(&vdec_frame, 0, sizeof(vdec_frame));
	memset(&vo_frame, 0, sizeof(vo_frame));
	gettimeofday(&ctx->start_wall, NULL);
	ctx->last_status_wall = ctx->start_wall;
	getrusage(RUSAGE_SELF, &ctx->last_status_usage);
	ctx->status_initialized = CVI_TRUE;

	while (!g_stop_requested) {
		ret = CVI_VDEC_GetFrame(PLAYER_VDEC_CHN, &vdec_frame, 1000);
		if (ret == CVI_SUCCESS) {
			if (!logged_first_frame) {
				SAMPLE_PRT("first vdec frame %ux%u fmt=%d stride0=%u\n",
					   vdec_frame.stVFrame.u32Width, vdec_frame.stVFrame.u32Height,
					   vdec_frame.stVFrame.enPixelFormat, vdec_frame.stVFrame.u32Stride[0]);
				logged_first_frame = CVI_TRUE;
			}
			ret = CVI_VPSS_SendFrame(ctx->vpss_grp, &vdec_frame, 1000);
			CVI_VDEC_ReleaseFrame(PLAYER_VDEC_CHN, &vdec_frame);
			memset(&vdec_frame, 0, sizeof(vdec_frame));
			if (ret != CVI_SUCCESS) {
				ctx->vpss_send_failures++;
				SAMPLE_PRT("CVI_VPSS_SendFrame failed with %#x\n", ret);
				continue;
			}

			ret = CVI_VPSS_GetChnFrame(ctx->vpss_grp, PLAYER_VPSS_CHN, &vo_frame, 1000);
			if (ret != CVI_SUCCESS) {
				ctx->vpss_get_failures++;
				SAMPLE_PRT("CVI_VPSS_GetChnFrame failed with %#x\n", ret);
				continue;
			}

			if (ctx->shown_frames == 0) {
				SAMPLE_PRT("first vpss frame %ux%u fmt=%d stride0=%u\n",
					   vo_frame.stVFrame.u32Width, vo_frame.stVFrame.u32Height,
					   vo_frame.stVFrame.enPixelFormat, vo_frame.stVFrame.u32Stride[0]);
			}

			send_frame = &vo_frame;
			if (ctx->vo_cpu_pad) {
				ret = build_vo_padded_frame(ctx, &vo_frame, &send_frame);
				if (ret != CVI_SUCCESS) {
					CVI_VPSS_ReleaseChnFrame(ctx->vpss_grp, PLAYER_VPSS_CHN, &vo_frame);
					memset(&vo_frame, 0, sizeof(vo_frame));
					continue;
				}
			}

			ret = CVI_VO_SendFrame(ctx->vo_config.VoDev, PLAYER_VO_CHN, send_frame, 1000);
			if (ret != CVI_SUCCESS) {
				SAMPLE_PRT("CVI_VO_SendFrame failed with %#x\n", ret);
			} else {
				ctx->shown_frames++;
			}
			CVI_VPSS_ReleaseChnFrame(ctx->vpss_grp, PLAYER_VPSS_CHN, &vo_frame);
			memset(&vo_frame, 0, sizeof(vo_frame));

			CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
			{
				struct timeval now;

				gettimeofday(&now, NULL);
				if (timeval_diff_sec(&now, &ctx->last_status_wall) >= 0.5)
					print_status_line(ctx, &status);
			}
			idle_loops = 0;
			continue;
		}

		if (g_stop_requested)
			break;

		if (is_vdec_getframe_idle_ret(ret))
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
		if (is_vdec_getframe_idle_ret(ret))
			usleep(5000);
	}

	CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
	print_summary(ctx, &status);
	return CVI_SUCCESS;
}

static CVI_S32 playback_loop_vo(player_ctx_t *ctx)
{
	if (ctx->vo_path_mode == VO_PATH_MODE_BIND)
		return playback_loop_vo_bind(ctx);
	return playback_loop_vo_sendframe(ctx);
}

static const unsigned char *map_frame_plane(const VIDEO_FRAME_S *vframe, int plane, CVI_BOOL *mapped)
{
	if (vframe->pu8VirAddr[plane] == NULL) {
		CVI_SYS_IonInvalidateCache(vframe->u64PhyAddr[plane], NULL, vframe->u32Length[plane]);
		*mapped = CVI_TRUE;
		return CVI_SYS_Mmap(vframe->u64PhyAddr[plane], vframe->u32Length[plane]);
	}

	CVI_SYS_IonInvalidateCache(vframe->u64PhyAddr[plane], vframe->pu8VirAddr[plane], vframe->u32Length[plane]);
	*mapped = CVI_FALSE;
	return vframe->pu8VirAddr[plane];
}

static const unsigned char *map_frame_plane0(const VIDEO_FRAME_S *vframe, CVI_BOOL *mapped)
{
	return map_frame_plane(vframe, 0, mapped);
}

static void unmap_frame_plane(const VIDEO_FRAME_S *vframe, int plane,
			      const unsigned char *src_base, CVI_BOOL mapped)
{
	if (mapped && src_base != NULL)
		CVI_SYS_Munmap((void *)src_base, vframe->u32Length[plane]);
}

static void unmap_frame_plane0(const VIDEO_FRAME_S *vframe, const unsigned char *src_base, CVI_BOOL mapped)
{
	unmap_frame_plane(vframe, 0, src_base, mapped);
}

static CVI_S32 build_vo_padded_frame(player_ctx_t *ctx, const VIDEO_FRAME_INFO_S *src_frame,
				     VIDEO_FRAME_INFO_S **out_frame)
{
	const VIDEO_FRAME_S *src = &src_frame->stVFrame;
	VIDEO_FRAME_S *dst = &ctx->stage_frame.stVFrame;
	const unsigned char *src_y;
	const unsigned char *src_uv;
	unsigned char *dst_y;
	unsigned char *dst_uv;
	CVI_BOOL src_y_mapped;
	CVI_BOOL src_uv_mapped;
	CVI_U32 copy_width;
	CVI_U32 copy_height;
	CVI_U32 row;

	if (!ctx->vo_stage_ready) {
		SAMPLE_PRT("vo stage not ready\n");
		return CVI_FAILURE;
	}

	src_y = map_frame_plane(src, 0, &src_y_mapped);
	src_uv = map_frame_plane(src, 1, &src_uv_mapped);
	if (src_y == NULL || src_uv == NULL) {
		SAMPLE_PRT("map source frame failed\n");
		if (src_y != NULL)
			unmap_frame_plane(src, 0, src_y, src_y_mapped);
		if (src_uv != NULL)
			unmap_frame_plane(src, 1, src_uv, src_uv_mapped);
		return CVI_FAILURE;
	}

	dst_y = dst->pu8VirAddr[0];
	dst_uv = dst->pu8VirAddr[1];
	memset(dst_y, 0x00, dst->u32Length[0]);
	memset(dst_uv, 0x80, dst->u32Length[1]);

	copy_width = src->u32Width;
	if (copy_width > ctx->content_rect.u32Width)
		copy_width = ctx->content_rect.u32Width;
	copy_height = src->u32Height;
	if (copy_height > ctx->content_rect.u32Height)
		copy_height = ctx->content_rect.u32Height;

	for (row = 0; row < copy_height; ++row) {
		unsigned char *dst_row = dst_y + (ctx->content_rect.s32Y + row) * dst->u32Stride[0] +
				 ctx->content_rect.s32X;
		const unsigned char *src_row = src_y + row * src->u32Stride[0];
		memcpy(dst_row, src_row, copy_width);
	}

	for (row = 0; row < (copy_height / 2); ++row) {
		unsigned char *dst_row = dst_uv + ((ctx->content_rect.s32Y / 2) + row) * dst->u32Stride[1] +
				 ctx->content_rect.s32X;
		const unsigned char *src_row = src_uv + row * src->u32Stride[1];
		memcpy(dst_row, src_row, copy_width);
	}

	dst->u64PTS = src->u64PTS;
	dst->u32TimeRef = src->u32TimeRef;
	CVI_SYS_IonFlushCache(dst->u64PhyAddr[0], dst->pu8VirAddr[0], dst->u32Length[0]);
	CVI_SYS_IonFlushCache(dst->u64PhyAddr[1], dst->pu8VirAddr[1], dst->u32Length[1]);

	unmap_frame_plane(src, 0, src_y, src_y_mapped);
	unmap_frame_plane(src, 1, src_uv, src_uv_mapped);
	*out_frame = &ctx->stage_frame;
	return CVI_SUCCESS;
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

		if (g_stop_requested)
			break;

		if (is_vdec_getframe_idle_ret(ret))
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
		if (is_vdec_getframe_idle_ret(ret))
			usleep(5000);
	}

	CHECK_RET(CVI_VDEC_QueryStatus(PLAYER_VDEC_CHN, &status), "CVI_VDEC_QueryStatus");
	print_summary(ctx, &status);
	return CVI_SUCCESS;
}

static void player_stop(player_ctx_t *ctx)
{
	CVI_BOOL chn_enable[VPSS_MAX_PHY_CHN_NUM] = {0};

	if (ctx->vdec_vpss_bound) {
		SAMPLE_COMM_VDEC_UnBind_VPSS(PLAYER_VDEC_CHN, ctx->vpss_grp);
		ctx->vdec_vpss_bound = CVI_FALSE;
	}

	if (ctx->vpss_vo_bound) {
		SAMPLE_COMM_VPSS_UnBind_VO(ctx->vpss_grp, PLAYER_VPSS_CHN,
					    ctx->vo_config.VoDev, PLAYER_VO_CHN);
		ctx->vpss_vo_bound = CVI_FALSE;
	}

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

	if (ctx->vo_started) {
		SAMPLE_COMM_VO_StopVO(&ctx->vo_config);
		ctx->vo_started = CVI_FALSE;
	}

	if (ctx->vdec_chn.bCreateChn) {
		CVI_VDEC_DestroyChn(PLAYER_VDEC_CHN);
		ctx->vdec_chn.bCreateChn = CVI_FALSE;
		ctx->vdec_started = CVI_FALSE;
	}

	vo_stage_close(ctx);
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
	ctx.display_backend = detect_display_backend();
	ctx.vo_path_mode = detect_vo_path_mode();
	ctx.rotate_mode = detect_rotate_mode(ctx.display_backend);
	ctx.copy_mode = detect_copy_mode();
	ctx.output_mode = detect_output_mode();
	ctx.vo_cpu_pad = detect_env_enabled("SAMPLE_VDEC_VO_CPU_PAD", CVI_TRUE);
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
	if (ctx.display_backend != DISPLAY_BACKEND_VO || ctx.vo_path_mode != VO_PATH_MODE_SENDFRAME) {
		if (ctx.vo_cpu_pad) {
			SAMPLE_PRT("vo cpu pad only applies to vo/sendframe, disable it for current mode\n");
			ctx.vo_cpu_pad = CVI_FALSE;
		}
	}

	ctx.panel_size.u32Width = align_even_down(
		detect_env_u32("SAMPLE_VDEC_PANEL_WIDTH", PLAYER_PANEL_WIDTH));
	ctx.panel_size.u32Height = align_even_down(
		detect_env_u32("SAMPLE_VDEC_PANEL_HEIGHT", PLAYER_PANEL_HEIGHT));

	if (ctx.display_backend == DISPLAY_BACKEND_FB) {
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
	} else {
		if (ctx.rotate_mode == ROTATE_MODE_VO || ctx.rotate_mode == ROTATE_MODE_VPSS) {
			ctx.output_size.u32Width = ctx.panel_size.u32Height;
			ctx.output_size.u32Height = ctx.panel_size.u32Width;
		} else {
			ctx.output_size = ctx.panel_size;
		}
	}
	compute_display_layout(&ctx,
			       ctx.input_cfg.chnInCfg[0].u32BufWidth,
			       ctx.input_cfg.chnInCfg[0].u32BufHeight);
	SAMPLE_PRT("display layout src=%ux%u panel=%ux%u rect=%d,%d %ux%u vpss=%ux%u\n",
		   ctx.input_cfg.chnInCfg[0].u32BufWidth,
		   ctx.input_cfg.chnInCfg[0].u32BufHeight,
		   ctx.panel_size.u32Width, ctx.panel_size.u32Height,
		   ctx.video_rect.s32X, ctx.video_rect.s32Y,
		   ctx.video_rect.u32Width, ctx.video_rect.u32Height,
		   ctx.output_size.u32Width, ctx.output_size.u32Height);
	SAMPLE_PRT("content rect=%d,%d %ux%u vo_cpu_pad=%d\n",
		   ctx.content_rect.s32X, ctx.content_rect.s32Y,
		   ctx.content_rect.u32Width, ctx.content_rect.u32Height,
		   ctx.vo_cpu_pad);

	ret = init_system(&ctx);
	if (ret != CVI_SUCCESS) {
		player_stop(&ctx);
		return ret;
	}
	if (ctx.display_backend == DISPLAY_BACKEND_VO && ctx.vo_cpu_pad) {
		ret = vo_stage_open(&ctx);
		if (ret != CVI_SUCCESS) {
			player_stop(&ctx);
			return ret;
		}
	}
	if (ctx.display_backend == DISPLAY_BACKEND_FB &&
	    (ctx.copy_mode == COPY_MODE_TDMA || ctx.copy_mode == COPY_MODE_IVE)) {
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
	if (ctx.display_backend == DISPLAY_BACKEND_VO) {
		ret = init_vo(&ctx);
		if (ret != CVI_SUCCESS) {
			player_stop(&ctx);
			return ret;
		}
		ret = apply_vo_layout(&ctx);
		if (ret != CVI_SUCCESS) {
			player_stop(&ctx);
			return ret;
		}
		if (ctx.vo_path_mode == VO_PATH_MODE_BIND) {
			ret = bind_pipeline(&ctx);
			if (ret != CVI_SUCCESS) {
				player_stop(&ctx);
				return ret;
			}
		}
	}

	init_send_thread_param(&ctx.vdec_chn, &ctx.vdec_chn.stVdecThreadParamSend,
			       ctx.input_cfg.chnInCfg[0].input_path,
			       ctx.input_cfg.chnInCfg[0].s32sendstream_timeout);
	player_start_send_stream(&ctx.vdec_chn.stVdecThreadParamSend,
				 &ctx.vdec_chn.vdecThreadSend);
	SAMPLE_PRT("send thread timeout=%dms thread=%lu\n",
		   ctx.vdec_chn.stVdecThreadParamSend.s32MilliSec,
		   (unsigned long)ctx.vdec_chn.vdecThreadSend);

	signal(SIGINT, player_handle_signal);
	signal(SIGTERM, player_handle_signal);
	signal(SIGTSTP, player_handle_signal);

	if (ctx.display_backend == DISPLAY_BACKEND_FB) {
		fb_clear(&ctx.fb);
		SAMPLE_PRT("fb playback start: %s -> %ux%u fb0 (%ubpp), rotate=%s copy=%s output=%s\n",
			   ctx.input_cfg.chnInCfg[0].input_path,
			   ctx.output_size.u32Width, ctx.output_size.u32Height,
			   ctx.fb.bits_per_pixel,
			   rotate_mode_name(ctx.rotate_mode),
			   copy_mode_name(ctx.copy_mode),
			   output_mode_name(ctx.output_mode));
		ret = playback_loop(&ctx);
	} else {
		SAMPLE_PRT("vo playback start: %s -> panel=%ux%u vpss=%ux%u rect=%d,%d %ux%u backend=%s rotate=%s path=%s\n",
			   ctx.input_cfg.chnInCfg[0].input_path,
			   ctx.panel_size.u32Width, ctx.panel_size.u32Height,
			   ctx.output_size.u32Width, ctx.output_size.u32Height,
			   ctx.video_rect.s32X, ctx.video_rect.s32Y,
			   ctx.video_rect.u32Width, ctx.video_rect.u32Height,
			   display_backend_name(ctx.display_backend),
			   rotate_mode_name(ctx.rotate_mode),
			   vo_path_mode_name(ctx.vo_path_mode));
		ret = playback_loop_vo(&ctx);
	}
	player_stop(&ctx);
	return ret;
}
