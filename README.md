# Clipper

Clipper is a native C++20 Windows instant-replay daemon (`clipper.exe`) built to avoid frame-time spikes and input latency overhead.

It captures displays via DXGI Desktop Duplication (`DuplicateOutput1`), converts `B8G8R8A8_UNORM` to `NV12` on fixed-function `ID3D11VideoProcessor` hardware, and encodes H.264 on NVENC (`13.1`) without CUDA lookahead or 3D compute passes (`vendor-documented`, verified in [`src/video_pipeline.cpp`](src/video_pipeline.cpp)). Compressed video and mixed WASAPI stereo AAC-LC audio packets live in pre-allocated RAM ring buffers (`inferred` zero active replay disk writes; `measured` 0 B active disk write during benchmark). Pressing the save hotkey writes a fast-start (`ftyp -> moov -> mdat`) MP4 atomically through a temporary file.

## Design Constraints

* Single-binary Win32 tray daemon with no visible overlay window and no swapchain hooking.
* No Electron, WebView, or `ffmpeg.exe` subprocesses.
* Zero-copy GPU color conversion via `ID3D11VideoProcessor` (`BGRA8` Full Range to BT.709 Studio `NV12`).
* NVENC low-latency single-pass H.264 (`P4`, `LOW_LATENCY`, `enableLookahead=0`, `enableAQ=0`, `enableTemporalAQ=0`, 0 B-frames, 4-slot async pool).
* Mixed desktop loopback and microphone audio encoded once via Media Foundation `CLSID_AACMFTEncoder`.

## Build Instructions

Build requirements are Visual Studio Build Tools 2022 (`MSVC`), Windows SDK (`10.0.26100.0` or compatible), CMake `>= 3.20`, and Ninja. `CMakeLists.txt` configures `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to `Embedded` (`CMP0141`) so builds succeed without `mspdbsrv.exe` IPC sockets.

```cmd
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces three binaries inside `build/`.

* `build\clipper.exe` (`204,288` bytes `measured`)
* `build\clipper_bench.exe` (`200,704` bytes `measured`)
* `build\clipper_tests.exe` (`227,328` bytes `measured`)

## Usage

Run `build\clipper.exe`. Clipper places a dark-slate square icon with a red recording dot in the Windows notification tray and loads or creates `clipper.ini` next to the executable.

* **Save Replay** stops the rolling buffer at the current timestamp and writes an MP4 to `output_dir` via hotkey `Ctrl+Alt+S` (chosen over `Alt+F10` to avoid NVIDIA overlay collisions), tray menu `Save Replay`, or tray double-click.
* **Microphone** toggles mic mixing on or off at runtime from the tray menu.
* **Open Clips Folder** opens `output_dir` in Explorer.
* **Exit** signals shutdown, drains in-flight NVENC async slots with EOS, and exits cleanly.

### Command-Line Flags

| Flag | Effect |
| --- | --- |
| `--monitor=N` | Overrides `monitor_index` |
| `--fps=N` | Overrides `fps` |
| `--bitrate=N` | Overrides `bitrate_mbps` |
| `--seconds=N` | Overrides `replay_seconds` |
| `--output-dir=PATH` | Overrides `output_dir` |
| `--hotkey=COMBO` | Overrides `hotkey_save` |
| `--no-desktop-audio` | Disables WASAPI render loopback |
| `--no-mic-audio` | Disables microphone capture |
| `--no-chime` | Disables `MessageBeep(MB_OK)` on save |
| `--fail-fast` | Exits with code `2` if initial capture startup fails |
| `--ready-event=NAME` | Signals a Win32 event handle once engine startup succeeds |

### Configuration (`clipper.ini`)

Validation enforces `bitrate_mbps * replay_seconds <= 12000` at startup (`~2.0 GB` ring cap).

| Key | Default | Allowed Values | Notes |
| --- | --- | --- | --- |
| `monitor_index` | `0` | `0..N-1` | Index among attached NVIDIA DXGI outputs (falls back to all attached outputs) |
| `fps` | `60` | `30..60` | Capture and encode frame rate |
| `bitrate_mbps` | `20` | `2..80` | Average VBR bitrate (`max = 1.35 * average`) |
| `replay_seconds` | `60` | `5..300` | Rolling retention window |
| `record_desktop_audio` | `true` | `true`, `false` | WASAPI render loopback capture |
| `record_mic_audio` | `true` | `true`, `false` | WASAPI capture endpoint mixed into audio track 1 |
| `hotkey_save` | `Ctrl+Alt+S` | Modifiers + VK | Global save hotkey |
| `output_dir` | `%USERPROFILE%\Videos\Clips` | Directory path | Created automatically on save |
| `save_chime` | `true` | `true`, `false` | Plays `MessageBeep(MB_OK)` after atomic rename |

## Verification and Benchmarks

Run unit and Media Foundation decode-back tests.

```powershell
.\build\clipper_tests.exe
```

Run a 3-second live desktop capture self-test outside any restricted low-integrity sandbox (desktop duplication returns `0x80070005` under low integrity).

```powershell
.\build\clipper_bench.exe --self-test-clip 3 --out .\build\test_out\live_replay.mp4
```

Run the alternating 5-round D3D11 + DXGI swapchain load benchmark. Close background GPU encoders, games, and heavy browser hardware decode tabs first for clean A/B comparisons.

```powershell
.\build\clipper_bench.exe --gpu-load-bench 3 --runs 5
```

### Measured Hardware Results

Hardware under test was Windows 11 (`10.0.26300.0`), Intel Core `i5-14500HX`, and NVIDIA GeForce RTX 4050 Laptop GPU (`Driver 621.24`, `NVENC 13.1`, dual `1920x1080` displays).

* **Unit and Codec Suite (`measured`, `clipper_tests.exe`):** All 5 suites passed with `Total failures: 0`. Synthesized 60-frame `1920x1080` H.264 + `48 kHz` AAC-LC MP4 decoded `60` video frames and `192,512` bytes of PCM (`rms=11249.7`) via `IMFSourceReader`.
* **Live 3-Second Replay Self-Test (`measured`, `--self-test-clip 3`):** Captured `185` new frames (`1` static, `182` encoded, `0` errors), video capture plus submit median `425.0 us` (`p95 = 605.0 us`), encoded `141` AAC frames, saved `1,311,348` bytes in `3.27 ms`, and verified `dur=3.017s`, `videoFrames=181`, and `pcmBytes=577,536` via `IMFSourceReader`.
* **Final Swapchain 3D Load Benchmark under Host Background GPU Activity (`measured`, `--gpu-load-bench 3 --runs 5`):** During this run `nvidia-smi` reported concurrent background host load (`17–32%` GPU util, `12%` NVENC util, `2895 MiB` VRAM, `48–51 W`, `81°C`). `CLIPPER_OFF` measured `p50 = 10.629 ms` (`range 9.079..13.382`, `87.3 FPS`) and `CLIPPER_ON` measured `p50 = 11.582 ms` (`range 9.697..13.300`, `79.7 FPS`), winning on 2 of 5 alternating rounds (`Run 2` and `Run 5`). `clipper.exe` used `0.435%` median CPU, `135.2 MB` median RAM (`15 s` buffer config), and `0 B` active disk writes.
* **Prior Quiet-Host Offscreen 3D Shader Benchmark (`measured`, pre-audit build on idle GPU):** `CLIPPER_OFF` measured `p50 = 5.157 ms` (`range 5.041..5.436`, `178.3 FPS`) vs `CLIPPER_ON` `p50 = 5.192 ms` (`range 5.077..5.429`, `175.0 FPS`), `Delta p50 = +0.035 ms` (`+0.68%`), `0.103%` CPU, `135.1 MB` RAM, and `0 B` active disk writes.
