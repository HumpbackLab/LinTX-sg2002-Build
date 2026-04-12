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

That path was not reliable on this board/panel combination. Decode itself worked, but the display path through the stock VO helpers was not stable enough to use as a base.

Two external references shaped the later direction:

- Milk-V community post: `Milk-V Duo S 硬件解码H264并在LCD屏幕上显示`
- Milk-V community post: `Milk-V Duo S CVITEK MFF MP4`

The first reference reinforced that explicit frame movement and board-specific display handling were more trustworthy than stock bind mode. The second reference provided a practical fallback direction using framebuffer output when a board-specific VO path remained troublesome.

## Why This Sample Exists

This sample exists to provide a practical playback baseline on the current hardware:

- use official SDK decode input handling
- use VPSS for scaling and colorspace conversion
- display through `/dev/fb0`

The panel is physically used as a horizontal display, but the framebuffer is exposed as a vertical `480x800` target. The current implementation therefore treats decoded content as horizontal and writes it to the framebuffer with a fixed `rotate270` transform.

## Current Implementation

Current data path:

- elementary stream input (`.264` / `.265`)
- `CVI_VDEC_CreateChn`
- `CVI_VDEC_StartRecvStream`
- SDK `SAMPLE_COMM_VDEC_StartSendStream` file sender
- `CVI_VDEC_GetFrame`
- `CVI_VPSS_SendFrame`
- `CVI_VPSS_GetChnFrame`
- userspace blit to `/dev/fb0`

Display details:

- framebuffer target is read from `/dev/fb0`
- the current board exposes `fb0` as `480x800`
- default VPSS output is configured as horizontal content (`800x480`)
- default framebuffer write path applies `rotate270`
- VPSS now outputs `BGR_888`, which matches `cvifb` byte order more closely and avoids a per-pixel RGB/BGR swap in userspace
- `32bpp` framebuffer writes use `BGRA`
- alpha is forced to `0xff`

Experimental rotation mode:

- set `SAMPLE_VDEC_FB_ROTATE_MODE=vdec`
- this asks the sample to try `VDEC` `rotate270` first
- if the current SDK does not export that symbol or runtime setup rejects it, the sample falls back to CPU rotate automatically

This is enough to display standard horizontal content such as a `1280x720` test stream on the built-in panel in the expected physical viewing orientation.

## What Works

- local H.264 elementary stream playback to the built-in panel
- horizontal source material scaled to panel size
- fixed `rotate270` presentation for the current framebuffer orientation
- color output on this board's `cvifb` byte order (`BGRA`)
- lower userspace color-conversion cost than the first fb0 version because the VPSS output is already `BGR_888`

## Known Limitations

- CPU usage is high because the final display step is a userspace framebuffer blit
- this is not yet the preferred long-term zero-copy video path
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

## Next Steps

- reduce CPU cost in the framebuffer path
- add a cleaner input layer for future streamed `.264`
- evaluate whether direct MP4 demux support is worth integrating into this sample
- keep board-specific behavior documented instead of pretending the stock sample path is universally correct
