# Hardware video encoding (NVENC) on macOS with the nullmoth driver: research (2026-10-17)

## Why our RTX 4090 uses software encoding

- nullmoth's Metal plugin registers its NVENC encoders with VideoToolbox **only on Blackwell**
  (`plugin/NVMTLDevice.m:253` H.264, `:346` HEVC: "is not Blackwell - … encode stays on Apple's encoder").
- The encoder libraries (`libnvenc_h264.dylib`, `libnvenc_hevc.dylib`, and the `libnvdec_*` decoders) ship as
  **binaries only**. nullmoth's README lists the kexts, plugin, translator and NVK patch as the open-source parts; the
  codec libraries are not in that table, not built by the documented build scripts, and not credited to anyone else in
  NOTICE. nullmoth's own pre-Blackwell decoder fix is a one-byte binary patch (`tools/make_nvdec_t2d.py`).

## What is known about pre-Blackwell NVENC (from nullmoth's issue tracker)

- **#65** (RTX 3070 Laptop, Ampere GA104, closed 2026-10-11):
  - The encoder tiles NV12 input as Blackwell bw8 (luma) / bw16 (chroma). The classic engine (NVC7B7) expects
    t2d / block-height 2. That causes torn pictures: 17–28 dB, versus Apple's 43–51 dB.
  - The reporter replaced the tiling loops with t2d by binary patching and got H.264 to 41.9–49.4 dB.
  - HEVC on Ampere also had bitstream-level errors (NVDEC status 0xd).
  - Rate control ignores AverageBitRate (constant QP 23).
  - The decoder was fixed with mode 0 → `libnvdec_h264_t2d.dylib`. The encoders stay disabled on pre-Blackwell.
- **#105** (RTX 5070 Ti): the libraries hard-coded NVENC0 (engine type 0x1b). A 3-byte patch per library picked
  NVENC1. Fixed in 1.14.0.
- **#206** (RTX 2070 Super, Turing, open): **H.264 NVENC works on Turing** after (1) removing the Blackwell check and
  (2) the t2d input layout. The encoder opens (`encoder 0xc4b7 channel 0xc46f` on NVENC0). Sunshine then fails with
  "engine already in use in this process": only one session per process, and Sunshine opens a second before closing
  the first.
- **#93** (RTX 4090 AD102, bare metal, i9-13900K / Z790, macOS 15.8.1, driver 1.9.0, open):
  - The desktop and Metal 3 work at 4K@160 with three patches (`nvfbhz=` boot-arg, a texture live-refresh, and
    `nvsvwait`/`nvflipafter`).
  - The README BAR settings freeze the boot. It works with ResizeAppleGpuBars 0 / ResizeGpuBars -1 (NVRM resizes
    BAR1 to 8 GB).
  - **NVENC fails to open a session** (`0xe00002c2` kIOReturnBadArgument, then status 0xffff / 0x63).
  - GPU OpenCL returns unchanged buffers.
  - The copy-engine queue (class 0xc7b5) fails to initialize.
  - "No public Darwin build recipe for NVRM.kext".
- No issue asks for, or announces, publishing the codec library sources.

## Full issue/PR sweep (2026-10-17, driver 2.3.3, 254 issues/PRs)

- **No PR touches NVENC**, and the maintainer has not replied on any encoder issue (#65, #93, #105, #206). The
  2.3.3 tarball still ships the same four closed libraries (`libnvenc_h264/hevc`, `libnvdec_h264(+_t2d)/hevc`).
- **#206 (open, Turing 2070 Super):** H.264 NVENC works after (1) removing the Blackwell gate and (2) the t2d input
  layout; the encoder opens on class 0xc4b7 / channel 0xc46f, NVENC0. Then "engine already in use in this process":
  one session per process, and Sunshine opens a second before closing the first. Parsec may do the same on a
  resolution change.
- **#105 (closed, 5070 Ti):** NVENC0 hard-coded; fixed upstream in 1.14 (picks a reported NVENC engine). Measured
  there: H.264 + HEVC, 1080p + 4K, 8 sessions, all hardware, clean decode (Blackwell layout, so no tearing).
- **#93 (open, 4090 bare metal):** NVENC "never opens" = VideoToolbox -12908 (no encoder registered: the gate), not
  an engine failure. Also: `IOConnectMapMemory64 -> 0xe00002c2` after several 4K codec sessions in one process; Ada
  copy-engine class 0xc7b5 rejected (we see the same log line).
- **#175:** NVDEC is not registered in `VTDecoderXPCService` at first use (fixed by registering at load in that
  service).
- **#16, #21, #246:** 1401 writes `NVMTL_NO_NVENC=1` / `NVMTL_NO_NVENC_HEVC=1` into `nvrm610.conf` on pre-Blackwell
  machines (rule `nvenc-blackwell-only`).

## How the gate works in 2.3.3 (open source)

- `plugin/NVMTLDevice.m` `nvmtl_nvenc_register()` / `nvmtl_nvenc_hevc_register()` skip registration unless
  `nvmtl_video_is_blackwell()`.
- That reads `video-layout` from the `nvrm-gpu-profile` dictionary NVRM publishes on the GPU's IOPCIDevice
  (`kexts/NVRM/NVRM.cpp:1172`: `arch >= 0x1A0 ? "blackwell" : "classic"`; AD102 is classic).
- Decode is already right on Ada: the plugin loads `libnvdec_h264_t2d.dylib` (mode 0) for "classic".
- The plugin builds with plain `xcrun clang` (`build/build_plugin.sh`); the guest has the CLT + macOS 26 SDK.

## Documentation for writing an encoder from scratch

- NVIDIA **open-gpu-doc** `classes/video/` (MIT):
  - `nvenc_drv.h`: the engine's driver interface for H.264/HEVC. It covers picture setup, SPS/PPS, slice control,
    motion estimation and mode decision, quantization, rate control (HRD/VBV/AQ), reference handling, statistics, and
    the input/output surfaces. It says "Input pictures are either all in BL format or all in tiled 16x16 format", and
    has block height and memory-mode fields, so the input layout is described to the engine. Version magic values
    such as `0xC9B70006`.
  - Class headers: `clc9b7.h` is the **Ada NVENC class (NVC9B7)**, the `nvenc 0xc9b7` in our driver log.
    `clc7b7.h` is Ampere. Plus the NVDEC classes and `nvdec_drv.h`.
- **Mesa NVK**: Vulkan Video **decode only** (H.264, Mesa 26.3, `NVK_EXPERIMENTAL=video`), built from reverse
  engineering plus `nvdec_drv.h`. No NVK encode. ANV and RADV have encode.
- NVIDIA's Video Codec SDK (NVENCODE API) is Windows/Linux only, on top of NVIDIA's closed user-mode driver. It's not
  usable on macOS.

## Safety: NVENC can reset the whole host on an RTX 4090

NVIDIA developer forum (2026), "RTX 4090: silent whole-host reset triggered by NVENC on Linux (580-595-610)":
- An instant power-cycle-like reset with no Xid, panic, MCE or AER.
- `av1_nvenc` reproduces about 15 s into an encode. `h264_nvenc` triggers 2 in 10 runs, 11–40 s after the encode ends
  (idle / clock ramp-down). `hevc_nvenc`: none seen.
- Drivers 580.159.03, 595.71.05 and 610.43.03. The card is cool (46 °C, 92 W).

That is one setup, not proven general. But on edi, a host reset kills every VM, the dev VM included.
**Don't exercise NVENC on edi's 4090 without the user's explicit OK**, and preferably in a window where the other
VMs can go down.

## Options

1. **Upstream:** add our Ada data to #93 / #206 and ask nullmoth for a classic-layout (t2d) encoder for NVC9B7. We
   can test it.
2. **The #206 route for Ada:** remove the Blackwell check (the plugin is open source, but rebuilding it needs Xcode 16
   on a Mac; the Tahoe VM could host it) plus the t2d input-layout binary patch on `libnvenc_h264.dylib`. Ada probably
   uses the same classic layout as Ampere/Turing. Unverified, and the host-reset risk applies.
3. **Clean-room encoder:** a VideoToolbox H.264 encoder plugin built on `nvenc_drv.h` + `clc9b7.h`, using NVRM's
   channel allocation. Correct, but a large project (bitstream headers, reference management, rate control).

## Sources

- https://github.com/nullmoth/nvidia-macos-driver/issues/65
- https://github.com/nullmoth/nvidia-macos-driver/issues/105
- https://github.com/nullmoth/nvidia-macos-driver/issues/206
- https://github.com/nullmoth/nvidia-macos-driver/issues/93
- https://github.com/NVIDIA/open-gpu-doc/tree/master/classes/video (nvenc_drv.h, clc9b7.h)
- https://www.phoronix.com/news/NVK-Vulkan-Video-Mesa-26.3
- https://forums.developer.nvidia.com/t/rtx-4090-silent-whole-host-reset-triggered-by-nvenc-on-linux-580-595-610/379347
- https://www.pcgameshardware.de/Geforce-RTX-4090-Grafikkarte-279170/News/NVENC-Linux-FFmpeg-Neustart-Fehler-1550551/
