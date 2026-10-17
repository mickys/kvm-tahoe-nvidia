# nvenc-ng: our own NVENC encoder for macOS (replacing libnvenc_h264)

Goal: a VideoToolbox H.264 (later HEVC) encoder for pre-Blackwell NVIDIA GPUs under nullmoth's driver, written from
public documentation only, that keeps the frame on the GPU end to end (like NVENC on Windows), has real rate control
and low-latency settings, and more than one session per process.

Why: with nullmoth's closed `libnvenc_h264` (plus our layout fix) a 4K Parsec frame costs ~9 ms inside the library:
~2.5 ms of CPU tiling, ~1.3 ms of copies, our 1.3 ms rearrangement, and ~3.5 ms spin-waiting for the engine
(docs/nvenc-enable-plan.md). It also ignores the bitrate, refuses odd heights and allows one session per process.

## Sources (clean room)

| What | Source | Licence |
|---|---|---|
| NVENC engine interface: methods, picture setup, RC, surfaces, status | NVIDIA open-gpu-doc `classes/video/clc9b7.h`, `nvenc_drv.h` | MIT |
| RM API: objects, channels, memory, controls | NVIDIA open-gpu-kernel-modules (RM 610.57.04) | MIT/GPL |
| Talking to RM on macOS (escape interface, channel and memory mapping) | nullmoth's NVK port (`nvk/nvk-macos.patch`, nvkmd_nvrm_*) | MIT |
| Block-linear surface layout | open-gpu-doc / Mesa `nil`, and our measured tile mapping | MIT |
| VideoToolbox plugin interface | decoder side: Apple's SPI headers in WebKit (`VTVideoDecoderSPI.h`, `CMBaseObjectSPI.h`); encoder side: black-box probing (a stub encoder that logs which slots VideoToolbox calls) | |
| H.264 headers (SPS/PPS) | ITU-T H.264 | |

Not used: nullmoth's closed codec libraries (no disassembly, no patching; only their observable behaviour).

## What the GPU offers (guest/nvenc-ng/probe, RTX 4090)

- Engines: NVENC0 `0x1b`, NVENC1 `0x1c` (two encoders), NVDEC, copy engines, GR.
- Classes: encoder `0xc9b7` (NVC9B7), channel `0xc56f` (AMPERE_CHANNEL_GPFIFO_A), usermode doorbell `0xc561`,
  copy `0xc7b5`, decoder `0xc9b0`, VA space `0x90f1`, 3D `0xc997`, compute `0xc9c0`.

## Design

```
VideoToolbox ── our VT encoder (CMBaseObject + encoder vtable) ── nvenc-ng core
                                                                     ├── RM client (rm.c): objects, controls
                                                                     ├── channel: GPFIFO on NVENC0/1, push buffers, doorbell, semaphores
                                                                     ├── surfaces: input NV12 (block-linear), reference/reconstructed pictures, bitstream, status
                                                                     ├── H.264 front end: SPS/PPS, picture setup (nvenc_drv.h), slice control, RC
                                                                     └── input path: IOSurface -> GPU block-linear surface on the GPU (copy engine or Metal), no CPU pass
```

Registration stays in the (patched) Metal plugin, which already decides per process; it would register our factory
instead of nullmoth's.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | RM client + capability probe | **done**: `guest/nvenc-ng/rm.c`, `probe` lists engines and classes |
| M1 | "Hello NVENC": VA space, GPFIFO channel on NVENC0, NVC9B7 object, push buffer with NOP + semaphore release, doorbell | the semaphore value appears in memory |
| M2 | One H.264 IDR frame: block-linear input surface, picture setup, bitstream + status buffers, our SPS/PPS | Apple's decoder decodes it, PSNR ≥ 40 dB |
| M3 | P-frames: reference/reconstructed pictures, frame numbering, RC modes (CBR/VBR, low latency), slices | a 60-frame stream decodes at the requested bitrate |
| M4 | VideoToolbox encoder plugin: probe the encoder vtable black-box, implement start/encode/complete/properties | `vttest` and ffmpeg's `h264_videotoolbox` use it |
| M5 | GPU-resident input: IOSurface -> block-linear on the GPU; event-driven completion instead of spinning | Parsec 4K encode time well under today's ~9 ms |
| M6 | Two sessions (NVENC0/1), HEVC | Sunshine-style double sessions; HEVC decodes |

Risk notes: NVENC microcode is loaded by GSP-RM, so user space only needs the class methods; the picture-setup
structures are large and must be complete (unset fields are a common source of engine faults: check the status
buffer and RM's channel error notifier after every submit). A 2026 forum report of NVENC resetting a whole Linux host
with an RTX 4090 has not reproduced here in hundreds of encodes.
