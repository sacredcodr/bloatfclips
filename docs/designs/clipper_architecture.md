# Clipper Architecture Specification (v1)

## Overview

Clipper is a single-binary native C++20 Windows instant-replay recorder (`clipper.exe`) designed to preserve game input latency and GPU render time. It captures a monitor zero-copy via DXGI Desktop Duplication, converts BGRA to NV12 via `ID3D11VideoProcessor`, encodes H.264 on NVENC fixed-function hardware without 3D/CUDA passes, stores compressed packets in pre-allocated RAM ring buffers, mixes desktop and microphone audio into one stereo AAC-LC track, and writes fast-start MP4 files on hotkey press.

## Non-Goals (v1)

- No Electron, WebView, or visible compositor overlay window during gameplay.
- No DLL injection, `Present` hooking, or multi-scene UI.
- No WGC single-window capture, HEVC/AV1 muxing, or dual separate MP4 audio tracks.
- No external `ffmpeg.exe` subprocesses or continuous disk replay caches.
- No live config hot-reload or pause-resume toggle; editing `clipper.ini` takes effect on restart.

## Data Shapes and Boundaries

### Enums and State Machine

- `EngineState`: `Stopped`, `Starting`, `Running`, `Recovering`, `Stopping`, `Faulted`.
  - Transitions: `Stopped -> Starting -> Running`, `Running -> Recovering -> Running`, any active state `-> Stopping -> Stopped`, `Starting / Recovering -> Faulted -> Stopped`.

### `AppConfig` (Parsed Once at Boundary)

Parsed at startup from `clipper.ini` and optional CLI flags, with validation concentrated at the parse boundary:

| Field | Default | Allowed Range / Values | Notes |
| --- | --- | --- | --- |
| `monitor_index` | `0` | `0..N-1` | DXGI output index |
| `fps` | `60` | `30..60` | Capture and encode target rate |
| `bitrate_mbps` | `20` | `2..80` | VBR target (`max = 1.35 * target`) |
| `replay_seconds` | `60` | `5..300` | Rolling retention window |
| `record_desktop_audio` | `true` | `true`, `false` | WASAPI loopback source |
| `record_mic_audio` | `true` | `true`, `false` | WASAPI mic source (mixed into track 1) |
| `hotkey_save` | `Ctrl+Alt+S` | Win32 modifier + VK | Avoids NVIDIA overlay `Alt+F10` collision |
| `output_dir` | `%USERPROFILE%\Videos\Clips` | Expanded directory path | Recursive creation on save |
| `save_chime` | `true` | `true`, `false` | Non-blocking `MessageBeep(MB_OK)` |

Memory budget invariant enforced at config parse time: `bitrate_mbps * replay_seconds <= 12000` (caps maximum video ring arena around 2.0 GB).

### `PacketRingBuffer` and Sizing Formulas

Video and audio streams each own an isolated `PacketRingBuffer` with pre-allocated byte slab (`arena_`) and metadata ring (`slots_`) allocated once on engine start:

- `videoArenaBytes = (replay_seconds + 3) * ceil(bitrate_mbps * 1.35 * 1'000'000 / 8)`
- `videoMaxSlots = (replay_seconds + 3) * fps + 64`
- `audioArenaBytes = (replay_seconds + 5) * (256'000 / 8)`
- `audioMaxSlots = (replay_seconds + 5) * 48 + 64`

Invariants:
- Reject packets where `size == 0 || size > arenaCapacity_` without corrupting existing slots.
- Tail wrap padding skips remaining bytes at the end of `arena_` when a packet does not fit contiguously, keeping every stored packet contiguous in `arena_`.
- Track a monotonic `generation_` incremented on recovery resets; backward IDR window slicing terminates at generation boundaries so pre-reset packets are never spliced with post-reset packets.
- `SnapshotVideoWindow` and `SnapshotAudioRange` sequentially copy selected packets into a clean snapshot payload buffer (never raw-`memcpy` across wrap-around padding).

## Thread Ownership and Lifecycle

1. **Video Capture + NVENC Thread (`COINIT_MULTITHREADED`, `THREAD_PRIORITY_ABOVE_NORMAL`):**
   - Sole owner of `ID3D11Device`, `ID3D11DeviceContext`, `ID3D11VideoDevice` / `ID3D11VideoProcessor`, `IDXGIOutputDuplication`, 4-slot registered NVENC `ID3D11Texture2D` pool, NVENC `13.1` session, and sole writer to `videoRing_`.
   - Re-arms `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` using absolute 100-ns deadlines anchored to session epoch `t0`.
   - On `DXGI_ERROR_WAIT_TIMEOUT`, re-submits the most recently captured retired frame slot to preserve constant-FPS pacing.
   - **Slot Synchronization:** Before `VideoProcessorBlt` overwrites `pool[slot]`, waits on `encodeEvent[slot]` (or `stopEvent_`), locks and appends bitstream packet to `videoRing_` (`isKeyFrame = (pictureType == NV_ENC_PIC_TYPE_IDR)`), unlocks bitstream, and unmaps `pool[slot]`.
   - **Desktop and Device Recovery:** On `E_ACCESSDENIED` / `DXGI_ERROR_ACCESS_LOST`, rebinds thread desktop via `OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP | GENERIC_READ)` (fallback `MAXIMUM_ALLOWED`), closing the prior `HDESK` only after `SetThreadDesktop` succeeds, and retries `DuplicateOutput1(BGRA8)` on backoff (`100..1000 ms`). On resolution/format change (`DXGI_OUTDUPL_DESC::ModeDesc`) or `DXGI_ERROR_DEVICE_REMOVED`, drains in-flight NVENC slots, increments generation (`AdvanceGeneration()`), and recreates D3D11/VP/NVENC resources.
2. **Audio Capture + Mix Thread (`AvSetMmThreadCharacteristicsW(L"Pro Audio", ...)`):**
   - Sole owner of WASAPI render loopback client, optional WASAPI capture mic client, `48000 Hz` stereo float mixer (`[-1.0f, 1.0f]` clamp + carried resampling phase), `1024`-sample QPC-paced FIFO, and single Media Foundation AAC-LC MFT (`48000 Hz`, `2 ch`, `192 kbps`, `MF_MT_AAC_PAYLOAD_TYPE = 0`).
   - Handles `AUDCLNT_BUFFERFLAGS_SILENT` and `AUDCLNT_E_DEVICE_INVALIDATED` reopen cleanly. Maintains a `25 ms` lead margin behind real-time and trims/zero-pads exact PCM samples only when drift exceeds `25 ms`, keeping AAC sample time locked to QPC without runaway silence injection.
3. **Clip Saver Thread:**
   - Woken by manual hotkey/menu save trigger (latching `saveRequestPtsUs`). Snapshots video and audio rings sequentially, muxes fast-start `.mp4` to a `.tmp` sibling path via `WriteMp4File` (`FILE_FLAG_SEQUENTIAL_SCAN`), atomically renames via `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`, and calls `MessageBeep(MB_OK)` when `save_chime` is enabled.
4. **Main Thread (Win32 Tray Daemon):**
   - Message-only window (`HWND_MESSAGE`) owning `Shell_NotifyIconW`, `RegisterHotKey` (reporting collision via tray balloon), and popup menu (`Save Replay`, `Mic: On/Off`, `Open Clips Folder`, `Exit`).
5. **Shutdown Sequence:**
   - Signal `stopEvent_`, join capture, audio, and saver threads to idle (`Stopping -> Stopped`), drain NVENC `NV_ENC_PIC_FLAG_EOS`, and free rings.

## Zero-3D-Core NVENC and Color Pipeline Invariants

- Hardware color conversion via `ID3D11VideoProcessor` (`DXGI_FORMAT_B8G8R8A8_UNORM` Full Range -> `DXGI_FORMAT_NV12` BT.709 Studio) feeding `NV_ENC_BUFFER_FORMAT_NV12`.
- `NV_ENC_PRESET_P4_GUID`, `NV_ENC_TUNING_INFO_LOW_LATENCY`, `NV_ENC_MULTI_PASS_DISABLED`, `enableLookahead = 0`, `enableAQ = 0`, `enableTemporalAQ = 0`, `frameIntervalP = 1`, `zeroReorderDelay = 1`, `enableEncodeAsync = 1`.
- H.264 specific: `h264Config.idrPeriod = fps`, `h264Config.repeatSPSPPS = 1`, `rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR`, `vbvBufferSize = maxBitRate / fps * 2`, BT.709 VUI color flags.

## Fast-Start H.264 + AAC ISOBMFF Muxer

- Produces `ftyp -> moov -> mdat` with **1-second time-interleaved chunks** across video and audio tracks for fast scrubbing and web playback.
- Parses H.264 Annex-B (`00 00 00 01` / `00 00 01`), extracts SPS (`7`) and PPS (`8`) into `avcC` (including the 4-byte High-profile suffix for `profile_idc` in `{44, 83, 86, 100, 110, 118, 122, 128, 134, 138, 139, 144, 244}`), strips inline NAL types `7/8/9` from all `mdat` samples, and emits 4-byte length prefixes (`avc1`) plus `colr` (`nclx` BT.709).
- Writes `mp4a`/`esds` from validated MF AAC `MF_MT_USER_DATA` (`>= 14` bytes with `wPayloadType == 0`, stripping 12-byte `HEAACWAVEINFO`; fallback `{0x11, 0x90}`).
- Computes cumulative video `stts` ticks (`llround((pts - pts0) * 90000.0 / 1e6)` delta) and fixed `1024`-sample `48000 Hz` audio `stts`. Selects `co64` and 16-byte `mdat` `largesize` before finalizing `moov` whenever file/mdat offsets exceed 4 GiB.

## Binary Layout and Verification

- `clipper_core`: static library containing `config`, `ring_buffer`, `mp4_muxer`, and pipeline modules.
- `clipper.exe`: minimal background tray daemon (no `IMFSourceReader` or test code).
- `clipper_tests.exe`: unit tests (`Config`, `PacketRingBuffer` wrap/generation/stress, ISOBMFF structural checks including `> 4 GiB` `co64` math) + real NVENC H.264 + MF AAC encode -> `WriteMp4File` -> `IMFSourceReader` decode-back verification.
- `clipper_bench.exe`: `--self-test-clip` and alternating 5-run `benchmark-checklist` CLI for live gaming latency and PresentMon Independent Flip verification.
