# Enabling NVENC on the RTX 4090 (Ada AD102) with nullmoth's driver 2.3.3

Status 2026-10-17. Background, sources and the issue sweep: `docs/nvenc-research.md`.

## Where things stand

| Part | State on our 4090 | Who owns the fix |
|---|---|---|
| Decoder (NVDEC H.264/HEVC) | Works: the plugin loads `libnvdec_h264_t2d.dylib` for "classic" GPUs | done |
| Encoder registration with VideoToolbox | **Skipped**: Ada is "classic", the plugin registers encoders only on "blackwell" | open source (plugin) |
| Encoder input layout | **Wrong for Ada**: frames are tiled in Blackwell's layout | closed `libnvenc_*.dylib` |
| HEVC bitstream on pre-Blackwell | **Broken beyond the layout** (NVDEC status 0xd, #65) | closed `libnvenc_hevc.dylib` |
| Sessions per process | **One** ("engine already in use in this process", #206) | closed libraries |
| Rate control | Constant QP 23, ignores bitrate (#65) | closed libraries |
| Engine choice | Fine since 1.14: uses an NVENC engine RM reports (#105) | done |

Apps see "no hardware encoder" today (VideoToolbox -12908) and use Apple's software encoder. Parsec logs
`vte_init/VTCompressionsSessionCreate = -12908`, then `encoder = Software` (4–11 fps at 4K in this VM).

## Step 1: register the encoders on Ada (open source, ~1 hour)

**Gate.** `plugin/NVMTLDevice.m`, `nvmtl_nvenc_register()` (H.264) and `nvmtl_nvenc_hevc_register()` (HEVC) return
early unless `nvmtl_video_is_blackwell()`. That reads `video-layout` from the `nvrm-gpu-profile` dictionary NVRM
publishes on the GPU's IOPCIDevice (`kexts/NVRM/NVRM.cpp:1172`: `arch >= 0x1A0 ? "blackwell" : "classic"`).

**Change.** Add an opt-in override instead of touching the decoder logic. The decoder must keep picking the `_t2d`
library, so don't change `video-layout` itself:

```objc
// nvmtl_nvenc_register(), and the same in nvmtl_nvenc_hevc_register()
if (!nvmtl_video_is_blackwell() && !getenv("NVMTL_NVENC_FORCE")) { nvlog("nvenc: ... not Blackwell ..."); return; }
```

`NVMTL_NVENC_FORCE=1` then goes in `/Library/GPUBundles/nvmtl/nvrm610.conf`. The plugin loads that file into the
process environment with `setenv` when it starts (`plugin/NVMTLObjects.m:100-118`), so `getenv` sees it in every app.

**Build** (in the guest; it has the CLT and the macOS 26 SDK):
1. Copy `plugin/` from `vendor/nvidia-macos-driver` (tag v2.3.3) and KhronosGroup `Vulkan-Headers` to the guest.
2. Build it as `build/build_plugin.sh` does: `xcrun clang -arch x86_64 -bundle -fobjc-arc -O1 -DNVMTL_RELEASE=1
   -I Vulkan-Headers/include -o NVMTLDriver NVMTLDevice.m -framework Foundation -framework Metal -framework
   IOSurface`, then `codesign -f -s -`.
3. Back up `/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver` and put the new binary in its place.
4. Log out and back in, or restart the apps plus `VTEncoderXPCService` / `VTDecoderXPCService`.

The driver's own bundle checks have to keep passing: `nullmoth-runtime-check`, and the strings list in
`build_plugin.sh`.

**Expected:** `nvenc: H.264 encoder ... VTRegisterVideoEncoderWithInfo -> 0` in the plugin log. A session opens on
the Ada encoder class (0xc9b7) and frames come out, but the picture is torn (step 2). That proves the
engine/channel/RM path works on Ada in this VM, and so do #93's other open questions (BAR mapping errors
`0xe00002c2` after several sessions).

### Step 1 result (done 2026-10-17)

- **Patch:** `guest/nvenc/0001-plugin-NVMTL_NVENC_FORCE-registers-NVENC-before-Blac.patch` (against v2.3.3). Release builds unset
  every inherited `NVMTL_*` variable when the plugin loads (`plugin/NVMTLObjects.m` `nvmtl_conf_knobs`), so the knob
  can only be set in `nvrm610.conf`. That's why it takes process names: `NVMTL_NVENC_FORCE=vttest` (comma list, or
  `1` for all). `NVMTL_NVENC_HEVC_FORCE` is separate.
- **Scripts:** `guest/nvenc/build-plugin.sh` (stock + patched, built in the guest like `build_plugin_release.sh`;
  stock matches the shipped binary's 961 exports and linked libraries), `install-plugin.sh patched|stock|restore|knob
  K=V` (the shipped plugin and conf are backed up in `/Library/NullMoth/nvenc-work/`), `relogin.sh`, `vttest.m`.
- **Installed now:** the patched plugin, with `NVMTL_NVENC_FORCE=vttest`, so only the test program sees NVENC. Every
  other app still uses Apple's encoders, so the broken picture can't leak into screen recordings or calls.
- **Measured** (`vttest`, synthetic NV12, H.264, decoded with Apple's software decoder; fps includes the test's own
  CPU frame generation):

  | encoder | size | fps | bitrate (20 Mbit/s asked) | luma PSNR avg / min |
  |---|---|---|---|---|
  | nullmoth NVENC, Ada (`com.nvmtl.videoencoder.avc.nvenc`, hardware) | 1920x1080 | 98–102 | 30 Mbit/s | 22.1 / 20.9 dB |
  | nullmoth NVENC, Ada | 3840x2160 | 29 | 118 Mbit/s | 22.7 / 22.0 dB |
  | Apple software H.264 | 1920x1080 | 7.5 | 19 Mbit/s | 54.7 / 48.3 dB |
  | Apple software H.264 | 3840x2160 | 7.8 | 26 Mbit/s | 44.8 / 37.1 dB |

- **What the picture looks like:** each 64 px checker square is in the right place, but the pixels inside small
  blocks are reordered (diagonal edges become staircases, gradients go blocky). So the block placement agrees and
  only the inside of each block differs. That fits #65 (bw8/bw16 written, t2d read) and makes option 3 below look
  practical: the permutation is local to a block.
- **Also confirmed:** no rate control (the bitrate ignores `AverageBitRate`). No NVRM errors in serial.log,
  WindowServer stable, edi unaffected.

### Parsec with NVENC (2026-10-17)

- **Enabling it:** `install-plugin.sh knob NVMTL_NVENC_FORCE=vttest,parsecd`, then restart Parsec (`killall parsecd;
  launchctl asuser 501 sudo -u <user> open -a Parsec`). `lsof -p <parsecd>` then shows `libnvenc_h264.dylib` loaded.
  The client must have **H.265 off** (only H.264 is enabled).
- **First try failed:** Parsec logged `codec = h264`, `vte_init/VTCompressionsSessionCreate = -12908`, then Software.
  The cause is its encode size: it caps the width at 3840 and scales the 4096x2160 desktop to **3840x2025**.
  nullmoth's encoder rejects an **odd height** (vttest: 3840x2025 -> -12908; 3840x2024/2026, 1921x1080 work), and
  VideoToolbox reports that as "no encoder" (-12908).
- **Fix used:** desktop mode 3840x2160@50 HiDPI (`cgs-mode.js set 1920 1080 50 2`, native `0x10400` raster), so
  Parsec encodes 3840x2160 unscaled. A general fix would be in the encoder (or a plugin shim): pad odd heights.
- **Result after the mode change (15:19):** Parsec uses NVENC (`codec = h264`, `encode 3840x2160`, no `-12908`).
  Parsec's latency figure (L) fell from ~115 ms to **~9 ms**, and FPS went from ~4 to **~30** (desktop-update bound).
  The picture shows the expected layout fault: everything is in place and in the right colours, but text and edges
  are smeared in small horizontal runs (screenshot from the user, 2026-10-17). Step 2 fixes that.
- `kVTVideoEncoderSpecification_EnableLowLatencyRateControl` gives -12902 with our encoder. Parsec didn't hit that
  (its failure was -12908), but other streaming apps might.

## Step 2: the input layout (the real blocker)

**What's wrong.** The encoder library copies the app's linear NV12 frame into a GPU surface. It always uses
Blackwell's swizzle: "bw8" for luma, "bw16" for chroma (`_sw_bw8` / `_sw_bw16`). The pre-Blackwell NVENC classes
(NVC4B7 Turing, NVC7B7 Ampere, NVC9B7 Ada) read classic block-linear, "t2d" with block height 2. Measured on Ampere
(#65): 17–28 dB, against 43–51 dB for Apple's encoder.

**Fix: select the swizzle by engine class.** NVCFB7 (Blackwell) keeps bw8/bw16; NVC4B7, NVC7B7 and NVC9B7 get t2d.
That's what nullmoth already did for the decoder (`libnvdec_h264_t2d.dylib` = untile mode 0). Ways to get it:

1. **Upstream (preferred):** nullmoth ships `libnvenc_h264_t2d.dylib` (and later an HEVC one), and the plugin picks
   it the way it picks the `_t2d` decoder. Give them our 4090 data from step 1 (log, PSNR) in #93/#206, and offer
   the card for testing.
2. **The #65/#206 route:** the reporters patched the library so `nve264_encode` uses the t2d tiler for both planes.
   With that, H.264 reached 41.9–49.4 dB on Ampere and opened and encoded on Turing. Their exact patch isn't posted
   in full. Ask supertux7 (#206) or OverlordDev (#65) for it, check it against our 2.3.3 library hashes, and re-sign
   ad hoc. H.264 only: HEVC needs more than the tiler swap (step 3).
3. **Swizzle in the plugin:** before handing a frame to the library, permute it so that the library's bw8/bw16
   write lands as t2d in memory (input = bw8⁻¹(t2d(frame))). Classic block-linear is documented (open-gpu-doc,
   Mesa `nil` image layout code). The bw8/bw16 address functions would have to be pinned down by testing, for
   example by encoding coordinate-pattern frames and decoding them. It costs one extra pass over each frame on the
   CPU or GPU.
4. **Our own encoder:** a VideoToolbox H.264 encoder on top of NVIDIA's public `nvenc_drv.h` + `clc9b7.h`
   (open-gpu-doc) and NVRM channels. It's the correct, long-term fix, but a big job: SPS/PPS/slice headers,
   reference frames, rate control.

### Step 2 result: option 3 done for H.264 (2026-10-17)

- **Measuring the mapping without lossy probes:** nullmoth's Blackwell *decoder* (`libnvdec_h264.dylib`, untile mode
  1) run on the 4090 returns a pure permutation of Apple's bit-exact decode. Our tool: `guest/nvenc/nvdec-probe.m`
  (random NV12 -> Apple H.264 -> decode with the chosen nvdec library and with Apple's). The classic
  `libnvdec_h264_t2d.dylib` matches Apple's byte for byte; the Blackwell one moves 8-byte chunks inside 64-byte x
  8-row tiles, one fixed pattern for the whole frame:
  - luma:   output chunk j (0..7) of tile row r comes from row `(r>>1) + 4*((j>>1)&1)`, column `8*(r&1) + 16*(j&1) + 32*(j>>2)`
  - chroma: row `(r&3) + 4*((j>>1)&1)`, column `8*(j&1) + 16*((r>>2)&1) + 32*(j>>2)`
  - At 1080p the chroma plane (540 rows) ends in half a tile; the decoder reads padding there (#65's green stripe).
- **Why the decoder mapping is the encoder fix:** the decoder applies P = classic⁻¹ ∘ Blackwell, and the engine
  reading the library's Blackwell tiling as classic applies P⁻¹. So a frame pre-arranged as `in[p] = img[P(p)]`
  reaches the engine intact. Checked in vttest (`VT_PRESWIZZLE=1`): 22 dB -> 51.6 dB.
- **In the plugin:** `guest/nvenc/0002-plugin-give-pre-Blackwell-NVENC-its-input-in-the-cla.patch`. After dlopening
  `libnvenc_h264.dylib` on a non-Blackwell GPU, the plugin points two entries of that image's `__got` at its own
  functions (`CVPixelBufferLockBaseAddress`, `CVPixelBufferGetBaseAddressOfPlane`, the only way the library reads
  frames). The hook returns a rearranged copy of each NV12 plane (thread-local scratch, same bytes-per-row, rows
  padded to 16, edge rows repeated). The library file is unchanged. If the redirect fails, the encoder isn't
  registered (apps keep Apple's).
- **Measured** (vttest, no app-side changes, H.264 via `com.nvmtl.videoencoder.avc.nvenc`):

  | size | luma PSNR avg / min | chroma PSNR avg (bottom 8 chroma rows) | Apple software, luma / chroma |
  |---|---|---|---|
  | 1280x720 | 51.4 / 49.9 dB | | |
  | 1920x1080 | 51.5 / 49.9 dB | 42.4 dB (38.2) | 53.5 / 53.2 dB |
  | 3840x2160 | 51.4 / 49.8 dB | 42.3 dB (43.4) | 45.5 / 47.7 dB |

  Chroma is lower than Apple's because of the fixed QP 23 (no rate control), not the layout (a wrong layout gives ~20 dB).
- **Cost:** one CPU pass over each frame (12 MB at 4K, 8-byte copies). A Metal/GPU pass would be the next
  optimisation if it shows up in Parsec's encode time.

### Optimisation: the rearrangement a band at a time (2026-10-17)

- The first version computed each 8-byte chunk's source on its own: **2.6 ms per 4K frame** (Ryzen 9 3950X), a large
  part of Parsec's ~9 ms encode. Patch `0003-plugin-rearrange-pre-Blackwell-NVENC-input-a-band-at.patch` copies whole
  8-row bands instead: with AVX2, each 64-byte output row is two 32-byte halves built from the two source rows it
  draws on (luma: every other chunk via `unpacklo/hi_epi64` + `permute4x64`; chroma: two 16-byte pairs), else from a
  64-entry offset table. Only the partial last band keeps the per-chunk path.
- `guest/nvenc/swizzle-bench.c` (best of 30, output checked against the per-chunk version):

  | per frame (luma + chroma) | per-chunk (before) | table | **AVX2 (now)** | AVX2 × 4 threads | plain memcpy |
  |---|---|---|---|---|---|
  | 1080p, macOS guest | 0.62 ms | 0.16 ms | **0.08 ms** | 0.32 ms | 0.16 ms |
  | 4K, macOS guest | 2.50 ms | 0.81 ms | **0.51 ms** | 0.64 ms | 0.97 ms |

  It now runs at about the speed of a plain copy, i.e. memory-bound; threads don't help (start-up costs more than
  the work at 1080p). The plugin's exact code was also checked byte for byte against the per-chunk version on
  Linux for both planes and odd sizes (1081 rows, 1960-byte stride, 3 rows).
- Picture unchanged (vttest: 51.6 / 51.5 / 51.4 dB luma at 1080p / 4K / 720p, chroma 42 dB).
- Testing doesn't need Apple's software decoder at run time: the encoder path never decodes. vttest uses it only as
  an independent reference (a check with nullmoth's own decoder could hide a shared layout mistake).

### Build the plugin from v2.3.1, not the v2.3.3 tag (2026-10-17)

Release 2.3.3 **ships the 2.3.1 driver** (its changelog: the 2.3.2 display changes made some Macs worse, "blank or
torn windows … WindowServer crashes on some Tahoe desktops", and were withdrawn from the package). The `v2.3.3` tag's
source still contains those plugin commits (`79f47d0` screenshots/window captures/lock screen, `cf92c5a` dummy
texture/sampler). A plugin built from the tag therefore behaves like 2.3.2: once WindowServer loaded it (first boot
after installing), the desktop glitched, visible over VNC too. A stock build from `v2.3.1` matches the shipped
binary (same 420 log strings, same exports), so `guest/nvenc/build-plugin.sh` now builds from v2.3.1
(`NULLMOTH_REV` overrides). The three plugin patches apply to v2.3.1 in order.

Applying a new plugin to WindowServer means restarting it: it ignores SIGTERM, so `kill -9` it (launchd restarts it)
or reboot. macOS auto-logs in only once per boot; after that, log in by hand (or `vm/qmp-type.py stdin` with the
password at the login window).

### Where Parsec's ~12 ms encode goes (4K, 2026-10-17)

`sample` of parsecd during a session: the VideoToolbox compression thread spends ~9 ms of CPU per frame inside the
library's `EncodeFrame` (Parsec feeds NV12 directly, no pixel conversion in that path):

| part | ms per frame | what |
|---|---|---|
| `submit_wait` | ~3.5 | the library spin-waits (`clock_gettime` loop) for NVENC: the GPU encode itself |
| `nve264_encode` own code | ~2.5 | the library's CPU tiling of the 12 MB frame into GPU memory |
| our rearrangement (`nvl_band_avx2`) | ~1.65 | cold frames; `swizzle-bench` `COLD=1`: 1.27 ms single-thread AVX2 (old per-chunk: 3.6 ms) |
| `memmove` + `bzero` in the library | ~1.3 | its own copies and zeroing |

- Splitting our step over threads (GCD 2/4/8 parts, pthreads) gains at most ~0.2 ms: it is memory-bound. Not done.
- The stream is normal (1 keyframe, then ~70 KB P-frames at 4K for desktop content), so the client's decode time is
  mostly the 4K resolution.
- A Windows host does capture and NVENC on the GPU with no CPU passes, hence ~6 ms there.
- Levers left: (1) resolution: every part above, and the client's decode, scales with pixels (1440p ≈ 0.44×,
  1080p ≈ 0.25×), but the dummy plug only offers 4K timings and NVKMS refuses scaled modes, so it takes a 1080p/1440p
  dummy plug or EDID emulator, or fixing the scaled-mode refusal in NVRMFB; (2) the library's own CPU tiling, copies
  and spin-wait: only nullmoth can move those to the GPU (encode from a GPU surface, as NVENC does on Windows).

## Step 3: HEVC

#65 found that swapping the tilers in `nve265_open` isn't enough on Ampere. The stream stays undecodable (6 dB,
NVDEC `engine status 0xd (ctb 0x0)`), so there's a second Blackwell assumption in the HEVC picture setup. This needs
nullmoth (option 1) or option 4. Until then, leave HEVC unregistered (no `NVMTL_NVENC_FORCE` for HEVC, or
`NVMTL_NO_NVENC_HEVC=1`) so apps fall back to Apple's HEVC encoder instead of producing broken streams.

## Step 4: limits that matter for streaming (Parsec, Sunshine)

- **One session per process (#206).** A client that opens a second encoder before closing the first (Sunshine;
  maybe Parsec on a resolution change) gets "engine refused" and falls back to software. The proper fix is in the
  library: a session per channel, and AD102 has two NVENC engines. Check how Parsec behaves (`~/.parsec/log.txt`,
  `encoder =` lines) before assuming it's a problem.
- **No rate control (#65):** constant QP 23 regardless of `AverageBitRate`. On a LAN that's only a bandwidth
  question. Over the internet Parsec's bitrate setting would have no effect.
- **Output resolution:** encoding the 4096x2160 desktop at full size is still a lot of work per frame, even in
  hardware. 1080p needs a 1080p dummy plug: scaled modes are refused by NVKMS (see docs/macos-vm.md, display modes).

## Test plan

1. **Snapshot first:** `post-amd` internal qcow2 snapshot + `OVMF_VARS.post-amd.fd`.
2. **Synthetic encode** (our own small VideoToolbox program, no ffmpeg in the guest):
   - H.264 with `kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder`, so it fails instead of
     falling back.
   - 1080p and 4K, 120 frames, a moving test pattern.
   - Check `EncoderID` = `com.nvmtl.videoencoder.avc.nvenc`.
   - Decode with Apple's software decoder and compute luma PSNR against the source. Target ≥ 40 dB, the same as
     Apple's encoder within a few dB.
3. **Session churn:** open, encode, close 20 times in one process, then 2 sessions back to back. Watch for
   `0xe00002c2` map errors (#93) and the one-session refusal.
4. **Parsec:** connect, and check that `encoder = Hardware` stays (no `-12908`, no fallback), plus the FPS and latency
   in `~/.parsec/log.txt`, against today's 4–11 fps.
5. **Watch during each run:** `nvidia-smi -l 1` (`/usr/local/bin/nvidia-smi`, ours) for temperature and
   utilization, the serial log for NVRM errors, and `dmesg` on edi for vfio/AER messages.

**Host risk:** a 2026 NVIDIA forum report has NVENC on an RTX 4090 power-cycling the whole Linux host (AV1 within
~15 s, H.264 sometimes after the encode ends, HEVC never). On edi that would take every VM down, the dev VM
included. Run the first encode tests at a time when that's acceptable.

## Rollback

- Plugin: put the backed-up `NVMTLDriver` back (or rerun the release `install.sh`), and remove
  `NVMTL_NVENC_FORCE` from `nvrm610.conf`.
- Patched libraries (option 2): keep the originals next to them and copy them back. Their SHA256s are in the
  release `SHA256SUMS`.
- VM: revert to the `post-amd` snapshot.

## Effort

| Work | Estimate | Result |
|---|---|---|
| Step 1 (gate + plugin rebuild) + test program | 2–3 h | encoder opens on Ada, picture torn |
| Step 2, option 2 (existing patch, H.264) | 0.5–1 day, if the patch is shared | usable H.264 hardware encode |
| Step 2, option 3 (plugin-side swizzle) | 2–4 days | usable H.264 without touching the closed library |
| Step 2, option 4 (own encoder) | weeks | H.264 (later HEVC) we fully control |
| Upstream report with our data | 1 h | nullmoth fixes both codecs properly |
