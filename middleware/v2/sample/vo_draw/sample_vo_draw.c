#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sample_comm.h"

static volatile sig_atomic_t keep_running = 1;

static void handle_signal(int signo)
{
	(void)signo;
	keep_running = 0;
}

static void release_frame(VIDEO_FRAME_INFO_S *frame)
{
	VB_BLK block;
	int i;

	for (i = 0; i < 3; ++i) {
		if (frame->stVFrame.pu8VirAddr[i] && frame->stVFrame.u32Length[i])
			CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[i], frame->stVFrame.u32Length[i]);
		frame->stVFrame.pu8VirAddr[i] = CVI_NULL;
	}

	block = CVI_VB_PhysAddr2Handle(frame->stVFrame.u64PhyAddr[0]);
	if (block != VB_INVALID_HANDLE)
		CVI_VB_ReleaseBlock(block);
}

static void fill_nv21_bars(VIDEO_FRAME_INFO_S *frame)
{
	static const CVI_U8 y_values[] = { 32, 64, 96, 128, 160, 192, 224, 255 };
	CVI_U8 *y_plane;
	CVI_U8 *vu_plane;
	CVI_U32 x;
	CVI_U32 y;
	CVI_U32 width = frame->stVFrame.u32Width;
	CVI_U32 height = frame->stVFrame.u32Height;
	CVI_U32 y_stride = frame->stVFrame.u32Stride[0];
	CVI_U32 vu_stride = frame->stVFrame.u32Stride[1];
	CVI_U32 bar_width = width / 8;

	y_plane = CVI_SYS_MmapCache(frame->stVFrame.u64PhyAddr[0], frame->stVFrame.u32Length[0]);
	vu_plane = CVI_SYS_MmapCache(frame->stVFrame.u64PhyAddr[1], frame->stVFrame.u32Length[1]);
	frame->stVFrame.pu8VirAddr[0] = y_plane;
	frame->stVFrame.pu8VirAddr[1] = vu_plane;

	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			CVI_U32 bar = x / (bar_width ? bar_width : 1);
			if (bar > 7)
				bar = 7;
			y_plane[y * y_stride + x] = y_values[bar];
		}
	}

	for (y = 0; y < height / 2; ++y) {
		for (x = 0; x < width; x += 2) {
			vu_plane[y * vu_stride + x] = 128;
			vu_plane[y * vu_stride + x + 1] = 128;
		}
	}

	CVI_SYS_IonInvalidateCache(frame->stVFrame.u64PhyAddr[0], y_plane, frame->stVFrame.u32Length[0]);
	CVI_SYS_IonInvalidateCache(frame->stVFrame.u64PhyAddr[1], vu_plane, frame->stVFrame.u32Length[1]);
}

static CVI_S32 start_vo_for_panel(SIZE_S size, SAMPLE_VO_CONFIG_S *config)
{
	CVI_S32 ret;

	ret = SAMPLE_COMM_VO_GetDefConfig(config);
	if (ret != CVI_SUCCESS)
		return ret;

	config->VoDev = 0;
	config->stVoPubAttr.enIntfType = VO_INTF_MIPI;
	config->stVoPubAttr.enIntfSync = VO_OUTPUT_480x800_60;
	config->stDispRect.s32X = 0;
	config->stDispRect.s32Y = 0;
	config->stDispRect.u32Width = size.u32Width;
	config->stDispRect.u32Height = size.u32Height;
	config->stImageSize = size;
	config->enPixFormat = PIXEL_FORMAT_NV21;
	config->enVoMode = VO_MODE_1MUX;
	config->u32DisBufLen = 3;

	return SAMPLE_COMM_VO_StartVO(config);
}

int main(void)
{
	CVI_S32 ret;
	SIZE_S size = { .u32Width = 480, .u32Height = 800 };
	SAMPLE_VO_CONFIG_S vo_config;
	VIDEO_FRAME_INFO_S frame;
	CVI_BOOL vo_started = CVI_FALSE;

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	ret = SAMPLE_PLAT_SYS_INIT(size);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_PLAT_SYS_INIT failed %#x\n", ret);
		return ret;
	}

	ret = start_vo_for_panel(size, &vo_config);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("start_vo_for_panel failed %#x\n", ret);
		SAMPLE_COMM_SYS_Exit();
		return ret;
	}
	vo_started = CVI_TRUE;

	ret = SAMPLE_COMM_PrepareFrame(size, PIXEL_FORMAT_NV21, &frame);
	if (ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_PrepareFrame failed %#x\n", ret);
		SAMPLE_COMM_SYS_Exit();
		return ret;
	}

	fill_nv21_bars(&frame);
	printf("Send grayscale bars to panel %ux%u, Ctrl+C to exit.\n", size.u32Width, size.u32Height);

	while (keep_running) {
		ret = CVI_VO_SendFrame(SAMPLE_VO_LAYER_VHD0, 0, &frame, 1000);
		if (ret != CVI_SUCCESS)
			SAMPLE_PRT("CVI_VO_SendFrame failed %#x\n", ret);
		usleep(33000);
	}

	if (vo_started)
		SAMPLE_COMM_VO_StopVO(&vo_config);
	release_frame(&frame);
	SAMPLE_COMM_SYS_Exit();
	return 0;
}
