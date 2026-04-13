# sample_vdec_bind_vo

`sample_vdec_bind_vo` is a board-specific H.264/H.265 playback sample for the SG2002 LicheeRV Nano class panel setup used in this tree.

## Background

The original goal was straightforward:

- decode local H.264 on the board
- display it on the built-in panel
- later reuse the same playback path for incoming streamed `.264`

The first implementation direction followed the stock sample path:

- `VDEC -> VPSS -> VO`
- stock bind helpers
- stock sample panel validation

That path was not reliable enough in the earlier revisions of this sample, so the implementation temporarily moved to a framebuffer fallback while experiments continued.

Two external references shaped the later direction:

- Milk-V community post: `Milk-V Duo S 硬件解码H264并在LCD屏幕上显示`
- Milk-V community post: `Milk-V Duo S CVITEK MFF MP4`

The first reference reinforced that explicit frame movement and board-specific display handling were more trustworthy than stock bind mode. The second reference provided a practical fallback direction using framebuffer output when a board-specific VO path remained troublesome.

## Why This Sample Exists

This sample now provides two playback backends on the current hardware:

- `VO` backend:
  `VDEC -> VPSS -> VO`
- `fb` backend:
  `VDEC -> VPSS -> userspace blit -> /dev/fb0`

The intended fast path for the D310 panel is now the hardware `VO` backend. The older framebuffer path is still kept as a fallback and for comparison.

## Current Implementation

Default data path:

- elementary stream input (`.264` / `.265`)
- `CVI_VDEC_CreateChn`
- `CVI_VDEC_StartRecvStream`
- SDK `SAMPLE_COMM_VDEC_StartSendStream` file sender
- `VDEC -> VPSS` bind
- `VPSS -> VO` bind
- MIPI `VO_OUTPUT_480x800_60` output to the panel

Hardware display details:

- panel configuration is fixed to `480x800`
- default backend is `VO`
- default rotate mode is `vo`
- `VO` backend keeps the VPSS output in `NV21` so that hardware rotation remains available
- `vo` or `vpss` rotation modes use a fixed `rotate270` mapping that matches the panel orientation used by the earlier framebuffer path

Backend selection:

- default:
  `SAMPLE_VDEC_DISPLAY_BACKEND=vo`
- fallback:
  `SAMPLE_VDEC_DISPLAY_BACKEND=fb`

Rotation selection:

- `SAMPLE_VDEC_BIND_ROTATE_MODE=vo`
  use `CVI_VO_SetChnRotation`
- `SAMPLE_VDEC_BIND_ROTATE_MODE=vpss`
  use `CVI_VPSS_SetChnRotation`
- `SAMPLE_VDEC_BIND_ROTATE_MODE=vdec`
  try `CVI_VDEC_SetRotation` first
- `SAMPLE_VDEC_BIND_ROTATE_MODE=none`
  assume the input stream is already portrait-aligned for the panel
- `SAMPLE_VDEC_BIND_ROTATE_MODE=cpu`
  only meaningful on the framebuffer backend

Framebuffer fallback details:

- framebuffer target is read from `/dev/fb0`
- the current board exposes `fb0` as `480x800`
- default framebuffer write path applies `rotate270`
- VPSS output is `BGR_888` or `ARGB_8888` for this mode
- `32bpp` framebuffer writes use `BGRA`

## What Works

- local H.264 elementary stream playback to the built-in panel
- hardware `VDEC -> VPSS -> VO` display path for the D310 panel
- `VO` rotation mode with `NV21` output
- framebuffer fallback path for boards or SDK revisions where VO binding still needs comparison

Latest verified result on the current board:

- test stream:
  `1280x720` H.264 elementary stream
- backend:
  `SAMPLE_VDEC_DISPLAY_BACKEND=vo`
- rotate mode:
  `SAMPLE_VDEC_BIND_ROTATE_MODE=vo`
- observed status:
  `shown=617`, `leftPics=0`, `avg=44.7fps`, `cpu=23.7%`
- repeated `CVI_VDEC_GetFrame` return `0xc0058041` is now treated as a benign idle/no-frame condition rather than a fatal playback error

## Known Limitations

- input is currently elementary stream only, not direct `.mp4`
- panel timing is currently fixed to `VO_OUTPUT_480x800_60`
- the sample is still board-specific rather than a generic panel abstraction
- `VDEC` rotation availability depends on the SDK exporting `CVI_VDEC_SetRotation`
- the framebuffer backend is still CPU-heavy because the final display step is a userspace blit

VO / VPSS rotation notes:

- `VO` and `VPSS` rotation are only useful here because the path stays in `NV21`
- `VPSS` rotation is left as an experimental switch because its output alignment rules are stricter than the `VO` rotation path
- current default is `VO` rotation because it most closely matches the stock SDK samples in this tree
- in `vo/sendframe` mode, `0xc0058041` from `CVI_VDEC_GetFrame` behaves like an internal no-frame-yet return on this SDK / board combination and should be counted as idle polling, not logged as a hard failure

Framebuffer fallback notes:

- input is currently elementary stream only, not direct `.mp4`
- the playback path is tuned for the current board/panel assumption and is not a generic sample
- the implementation is functional first, not optimized

## Recommended Test Input

For current testing, use a standard horizontal source such as:

- `1280x720`, `24fps`, H.264 elementary stream

Example board-side invocation:

```bash
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

Recommended VO test:

```bash
SAMPLE_VDEC_DISPLAY_BACKEND=vo \
SAMPLE_VDEC_BIND_ROTATE_MODE=vo \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

Experimental VPSS-rotation test:

```bash
SAMPLE_VDEC_DISPLAY_BACKEND=vo \
SAMPLE_VDEC_BIND_ROTATE_MODE=vpss \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

Legacy framebuffer fallback:

```bash
SAMPLE_VDEC_DISPLAY_BACKEND=fb \
SAMPLE_VDEC_BIND_ROTATE_MODE=cpu \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

## Next Steps

- run the new `VO` backend on the target board and compare `vo` vs `vpss` rotation stability
- if needed, add a direct `VDEC -> VO` path as a separate experimental mode
- add a cleaner input layer for future streamed `.264`
- keep the framebuffer backend only as a fallback, not the primary playback path
