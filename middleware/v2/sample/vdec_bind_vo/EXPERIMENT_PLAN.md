# fb0 Playback Experiment Plan

This document is the working guide for the next round of playback optimization on the SG2002 LicheeRV Nano panel path.

It is intentionally practical. The goal is not to describe every multimedia block in the SDK, but to keep future experiments focused on the real bottleneck and make results comparable.

## Current Baseline

Working path today:

- input elementary stream: `.264` / `.265`
- `VDEC` hardware decode
- `VPSS` scale and colorspace conversion
- userspace write to `/dev/fb0`

Current framebuffer assumptions:

- panel is physically used as horizontal
- `fb0` is exposed as vertical `480x800`
- visible correct orientation requires `rotate270`
- `cvifb` 32bpp byte order is `BGRA`
- alpha must be written as `0xff`

Current implementation summary:

- default mode: `VPSS -> BGR_888 -> CPU rotate270 -> fb0`
- experimental mode: `SAMPLE_VDEC_FB_ROTATE_MODE=vdec`
- current SDK on this platform does not export `CVI_VDEC_SetRotation`, so that mode falls back to CPU rotate

## What Has Been Proven Already

- decode is functional
- framebuffer output is functional
- panel orientation mismatch is understood
- color byte order is understood
- status line and end summary are useful and should be kept

The main bottleneck is also already clear:

- not decode
- not VPSS send/get
- not RGB/BGR swap alone
- the expensive part is userspace full-frame `rotate270` plus framebuffer write

Observed baseline on a clean single-run short test:

- about `4.3 fps`
- about `98%` CPU

This means future work should prioritize removing CPU-side rotation, not micro-optimizing small parts of the existing blit loop.

## Attempt Log: 2026-04-12

Recent board-side experiments added two concrete copy-path checks.

### A. VPSS Direct `ARGB_8888` Output

Intent:

- make `VPSS` output closer to `fb0` `32bpp` layout
- reduce userspace per-pixel packing work

Result:

- `CVI_VPSS_SetChnAttr` failed during `SAMPLE_COMM_VPSS_Init`
- failure code was `0xc0068003`
- on this SDK / board combination, `VPSS -> ARGB_8888` is not currently usable in this sample

Conclusion:

- do not assume documented `ARGB_8888` support is actually usable in this playback path
- keep `BGR_888` as the working VPSS output for now

### B. TDMA-Assisted Final Copy With Staging Buffer

Intent:

- keep current working `VPSS -> BGR_888`
- do CPU rotate / pack into an intermediate `ARGB_8888` stage buffer
- use `CVI_SYS_TDMACopy2D` for the final move into `fb0`

Implementation notes:

- direct VB staging allocation failed on the board in this path
- switching the stage buffer to direct cached ION allocation made the mode runnable

Observed board result:

- mode started correctly and reported `copy=tdma`
- summary result remained about `4.42 avg_fps`
- `cpu_time=13.29s`
- `elapsed=13.56s`
- effective CPU remained about `97%` to `98%`

Conclusion:

- offloading only the final framebuffer copy to `TDMA` does not materially improve playback
- the dominant cost is still earlier CPU-side work, not the last memory move into `fb0`
- future optimization should instrument and reduce:
  - CPU rotate
  - pixel packing / alpha fill
  - any redundant framebuffer-side touching

### C. Stage Timing Instrumentation

The sample now reports cumulative timing breakdown in the end summary.

Representative `~15s` baseline run (`copy=cpu`, `output=bgr888`):

- `avg_fps=4.44`
- `cpu_time=14.60s`
- `vdec_get_ms=4.4` total, about `0.066 ms/frame`
- `vpss_send_ms=16.9` total, about `0.256 ms/frame`
- `vpss_get_ms=230.9` total, about `3.498 ms/frame`
- `blit_ms=14581.9` total, about `220.938 ms/frame`

Representative `~15s` `TDMA` run (`copy=tdma`, `output=bgr888`):

- `avg_fps=4.42`
- `cpu_time=14.63s`
- `vdec_get_ms=6.4` total, about `0.096 ms/frame`
- `vpss_send_ms=16.4` total, about `0.248 ms/frame`
- `vpss_get_ms=231.1` total, about `3.501 ms/frame`
- `blit_ms=14582.6` total, about `220.948 ms/frame`
- `tdma_ms=74.7` total, about `1.131 ms/frame`

Practical interpretation:

- the sample is overwhelmingly dominated by `blit_ms`
- `VPSS_GetChnFrame` is visible but much smaller
- `TDMA` adds only about `1.1 ms/frame`, which is tiny compared with the existing `~221 ms/frame` CPU blit cost
- this confirms the next optimization target should be the CPU-side rotate / pack path itself, not the final DMA copy

### D. CPU Rotate Kernel Reordered For Linear fb Writes

Change:

- keep the same `rotate270` mapping
- stop iterating source pixels and scattering writes across framebuffer rows
- instead iterate destination rows and write each framebuffer row linearly

Observed board result on a `~15s` run:

- `avg_fps=4.95`
- `cpu_time=14.67s`
- `elapsed=14.96s`
- `vpss_get_ms=258.7` total, about `3.496 ms/frame`
- `blit_ms=14651.4` total, about `197.993 ms/frame`

Comparison against previous baseline:

- previous baseline: about `4.44 fps`, `blit ~= 220.938 ms/frame`
- reordered kernel: about `4.95 fps`, `blit ~= 197.993 ms/frame`

Interpretation:

- the write-order change alone improved fps by about `11%`
- the per-frame blit cost dropped by about `23 ms/frame`
- CPU is still effectively saturated, so this is a real improvement but not a complete solution

### E. IVE DMA Final Copy

Reference expectation:

- official IVE DMA documentation describes direct copy, interval copy and memory fill
- this confirms IVE DMA is still a copy engine, not a rotate engine

Implementation:

- add `copy=ive` runtime mode
- keep the same CPU rotate / pack into stage buffer
- replace final `TDMA` copy with `CVI_IVE_DMA(..., IVE_DMA_MODE_DIRECT_COPY, ...)`

Observed board result on a `~15s` run after the reordered CPU rotate kernel:

- `avg_fps=4.82`
- `cpu_time=14.67s`
- `elapsed=14.95s`
- `blit_ms=14267.4` total, about `198.158 ms/frame`
- final DMA copy time about `377.0 ms` total, about `5.236 ms/frame`

Comparison:

- reordered pure CPU path: about `4.95 fps`
- reordered + IVE DMA path: about `4.82 fps`

Interpretation:

- `IVE DMA` is functional in this path
- but it is slower than the already-optimized pure CPU final write path in this workload
- this further supports the conclusion that DMA-only final copy is not the main lever here

### F. Tiled CPU Rotate Attempt

Change:

- try a `16x16` tile-based rotate kernel for the `32bpp` path
- read source in tiles, then write rotated tiles into `fb0`

Observed board result on a `~15s` run:

- `avg_fps=4.86`
- `blit_ms=14724.3` total, about `201.703 ms/frame`

Comparison:

- previous linear-destination-row kernel: about `4.95 fps`, `~197.993 ms/frame`
- tiled attempt: slightly worse

Conclusion:

- the tiled variant tried here did not beat the simpler linear-destination-row implementation
- keep the linear-destination-row kernel as the current best CPU path

### G. Pre-Rotated No-Rotate Comparison Attempt

Intent:

- generate a host-side pre-rotated portrait H.264 elementary stream
- run playback with `SAMPLE_VDEC_FB_ROTATE_MODE=none`
- compare the cost of the no-rotate path against the current rotate path

What was tried:

- generated `480x800` H.264 elementary streams with `ffmpeg`
- tested both a normal encode and a conservative `Constrained Baseline` encode
- uploaded both to the board and ran with `rotate=none`, `bufwidth=480`, `bufheight=800`

Initial observed result:

- board-side playback often did not start
- repeated `CVI_VDEC_GetFrame failed with 0xc0058041`
- in many runs, `shown=0`

Re-test result on `2026-04-13`:

- this path is not a hard fail
- some board-side runs still stayed at `shown=0`
- other runs eventually started after an initial burst of `0xc0058041`
- once started, the path could reach about `2.4` to `2.5 fps`

Updated interpretation:

- the comparison is not blocked by a strict decoder rejection anymore
- the real issue is unstable startup on the generated portrait elementary stream path
- the previous "pre-rotated path does not start at all" conclusion was too strong

### H. Same-Source `rotate=cpu` vs `rotate=none` Control Test

Reason for this control:

- earlier numbers compared different media:
  - baseline runs often used `/root/test_1280x720_24fps.264`
  - pre-rotated tests used `480x800` portrait elementary streams
- that does not isolate rotation cost

Control media:

- `/root/test_800x480_24fps.264`

Board-side invocations:

```bash
SAMPLE_VDEC_FB_ROTATE_MODE=cpu \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_800x480_24fps.264 \
  --bufwidth=800 \
  --bufheight=480
```

```bash
SAMPLE_VDEC_FB_ROTATE_MODE=none \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_800x480_24fps.264 \
  --bufwidth=800 \
  --bufheight=480
```

Observed result on short clean runs:

- `rotate=cpu`:
  - about `5.0 fps`
  - CPU about `99%`
  - `shown=18` at about `3.6s`
- `rotate=none`:
  - about `5.1 fps`
  - CPU about `98.5%`
  - `shown=18` at about `3.5s`

Interpretation:

- on the same source stream, `rotate=none` is not slower than `rotate=cpu`
- if anything, it is marginally faster, which matches the expectation that removing CPU-side coordinate remapping should help a little
- this means the earlier "pre-rotated no-rotate path is slower" conclusion was caused by comparing different test media, not by the `none` path itself

### I. Portrait Stream Stability Follow-Up

Control media:

- `/root/test_480x800_rot270.264`
- `/root/test_480x800_rot270_compat.264`

Observed behavior:

- startup remains inconsistent for portrait `480x800` elementary streams
- the path may show:
  - repeated `CVI_VDEC_GetFrame failed with 0xc0058041`
  - occasional `CVI_VPSS_SendFrame failed with 0xc0068003`
- some runs eventually begin showing frames
- other runs remain stuck with `shown=0`

Interpretation:

- the instability is more specific than "rotation off is bad"
- the stronger suspicion is now:
  - portrait `480x800` stream acceptance is fragile in this SDK / board path
  - and/or the resulting decoded frames are not consistently accepted by the current `VPSS` input/output configuration
- this is consistent with seeing `VPSS_SendFrame` failures mixed into the startup failure sequence

### J. Updated Bottleneck Interpretation

Current evidence does not support the claim that rotation is the only dominant cost.

What the same-source control now suggests:

- removing CPU-side rotate alone does not materially change fps
- therefore the heavy part of the current userspace path is not just rotated addressing
- the larger cost is still the full-frame CPU blit work:
  - reading the `VPSS` output frame
  - per-pixel packing / alpha fill
  - full-frame write into `fb0`

Practical implication:

- "pre-rotate the stream and set `rotate=none`" is not sufficient by itself to unlock a large fps gain
- a meaningful win likely requires avoiding most CPU per-pixel processing, not only avoiding coordinate remapping

## Optimization Goal

Target direction:

- keep decode in hardware
- keep scale/colorspace in hardware where possible
- move rotation out of userspace
- leave userspace with at most a simple linear framebuffer write

Practical success criteria:

- display remains visually correct
- orientation remains correct
- colors remain correct
- fps improves materially over current baseline
- CPU decreases materially from current baseline

For this project, a meaningful win means at least one of:

- `fps >= 8`
- CPU reduced by at least `25%`
- clear path toward `480x800 @ 24fps`

## Recommended Experiment Order

Run experiments in this order.

### 1. VPSS Rotation on NV21

Best first candidate:

- `VDEC` outputs `NV21`
- rotate in `VPSS`
- then obtain display-friendly output
- then write `fb0`

Reason:

- `VPSS` is already in the current path
- if it can rotate the frame in hardware before userspace sees it, the largest CPU cost disappears

Known caveat:

- on this SDK family, `VPSS` rotation is tied to GDC-supported formats
- practical expectation is that rotation works on `NV12` / `NV21` / `YUV400`, not arbitrary RGB output formats

So the likely valid design is:

- rotate first while frame is still `NV21`
- convert to `BGR_888` later if required

### 2. GDC Rotation on NV21

Second candidate:

- `VDEC` outputs `NV21`
- allocate rotated intermediate frame
- use `GDC` `rotate270`
- send rotated frame into `VPSS`
- let `VPSS` scale / colorspace convert for final display copy

Reason:

- `GDC` rotation is explicitly present in the SDK
- it may be easier to control than forcing the whole operation through a single `VPSS` channel setup

Tradeoff:

- more plumbing is required
- intermediate VB frame management must be correct

### 3. DMA-Assisted Final Copy

Third candidate, only after hardware rotation has been tried, and now already partially tested:

- keep hardware rotation if available
- investigate `CVI_SYS_TDMACopy` / `CVI_SYS_TDMACopy2D`
- investigate `IVE DMA`

Reason:

- DMA may help with final movement
- DMA does not solve rotation by itself
- current staging-buffer `TDMA` result did not materially improve fps or CPU
- this makes DMA a lower-priority direction unless a future path can avoid the expensive CPU-side frame preparation before the DMA step

This should not be the first optimization step.

### 4. CPU Micro-Optimization

Last resort:

- block/tile transpose instead of naive per-pixel rotate
- less branching inside blit loop
- reduce clears or redundant writes
- optional SIMD only if toolchain and payoff are both convincing

This is fallback work, not preferred work.

## Experiment Matrix

All experiments should be organized as explicit modes so that board-side testing stays comparable.

Recommended runtime modes:

- `cpu`
  Current baseline. `VPSS -> BGR_888 -> CPU rotate270 -> fb0`
- `vpss_rot`
  Rotate in `VPSS` while frame is still `NV21`
- `gdc_rot`
  Rotate in `GDC` using an intermediate `NV21` frame
- `copy_opt`
  Optional mode for DMA or copy-path experiments after rotation work

Suggested control mechanism:

- keep using environment variable style control for quick board testing
- example: `SAMPLE_VDEC_FB_ROTATE_MODE=cpu`
- example: `SAMPLE_VDEC_FB_ROTATE_MODE=vpss`
- example: `SAMPLE_VDEC_FB_ROTATE_MODE=gdc`

If command-line switches are added later, keep environment variable compatibility for fast shell iteration.

## Standard Test Media

Use the same source whenever possible.

Recommended baseline source:

- `/root/test_1280x720_24fps.264`

Recommended invocation:

```bash
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

Do not compare numbers across different media unless the source change is intentional and documented.

## Build And Deploy Workflow

Always build in the repository environment.

Build:

```bash
cd /home/shimmer/LinTx/LicheeRV-Nano-Build
source build/cvisetup.sh
defconfig sg2002_licheervnano_sd
make -C middleware/v2/sample/vdec_bind_vo -j$(nproc)
```

Deploy:

```bash
sshpass -p root ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  root@10.85.35.1 \
  "cat > /tmp/sample_vdec_bind_vo.new && chmod 755 /tmp/sample_vdec_bind_vo.new && \
   mv /tmp/sample_vdec_bind_vo.new /root/decode_test/sample_vdec_bind_vo && \
   sha256sum /root/decode_test/sample_vdec_bind_vo" \
  < middleware/v2/sample/vdec_bind_vo/sample_vdec_bind_vo
```

## How To Test Each Mode

### Baseline CPU Mode

```bash
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

### Future VPSS Rotation Mode

```bash
SAMPLE_VDEC_FB_ROTATE_MODE=vpss \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

### Future GDC Rotation Mode

```bash
SAMPLE_VDEC_FB_ROTATE_MODE=gdc \
/root/decode_test/sample_vdec_bind_vo \
  --numChn=1 \
  --chn=0 \
  -c 264 \
  -i /root/test_1280x720_24fps.264 \
  --bufwidth=1280 \
  --bufheight=720
```

## What To Record For Every Experiment

Every run should record:

- git commit id
- selected mode
- input file
- visible result: correct / wrong orientation / wrong color / corrupted / black screen
- startup log
- status line snapshot
- end summary if available

Minimum metrics to compare:

- `shown`
- `fps`
- `avg_fps`
- `cpu`
- `cpu_time`
- `elapsed`
- `getframe_timeouts`
- `vpss_send_fail`
- `vpss_get_fail`

Recommended timing breakdown to record once instrumentation exists:

- `vdec_get_ms`
- `vpss_send_ms`
- `vpss_get_ms`
- `blit_ms`
- `tdma_ms`

If a mode fails before playback, record the first failure line exactly.

## Decision Rules

Use these rules to avoid wasting time.

### Keep A Mode

Keep a mode for further work only if:

- image is visually correct
- orientation is correct
- color is correct
- fps or CPU improves enough to matter

### Drop A Mode

Drop a mode quickly if:

- it cannot produce correct orientation without reintroducing a heavy CPU rotate
- it depends on unavailable SDK symbols
- it adds complexity but does not move fps or CPU materially
- it is less stable than the current baseline

## Known Risks

- `recv` and `dec` counters are not reliable indicators in this playback path
- `VPSS` rotation may be format-constrained in ways that require extra intermediate frames
- `GDC` integration may work but still require careful VB block lifetime handling
- some experiments may interfere with framebuffer geometry assumptions if output dimensions are chosen too early
- running multiple playback tests in parallel on the board invalidates fps and CPU conclusions

## Practical Notes

- always run only one playback test at a time on the board
- always kill leftover `sample_vdec_bind_vo` processes before measuring
- do not treat a visually incorrect but faster mode as success
- keep the status line and summary in all modes
- keep the current CPU baseline working while adding new modes

## Immediate Next Task

The next implementation task should be:

- add `vpss_rot` experiment mode
- try to keep frames in `NV21` until after rotation
- compare against current `cpu` baseline using the same `720p` input

If `vpss_rot` cannot be made correct with reasonable complexity, move to:

- `gdc_rot`

Do not start with DMA or SIMD before these two have been tried.
