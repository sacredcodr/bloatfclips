# Clipper

Clipper is a native C++20 Windows instant-replay daemon and Win32 Settings app (`clipper.exe`) built to avoid frame-time spikes and input latency overhead.

It captures displays via DXGI Desktop Duplication (`DuplicateOutput1`), converts `B8G8R8A8_UNORM` to `NV12` on fixed-function `ID3D11VideoProcessor` hardware, and encodes H.264 on NVENC (`13.1`) without CUDA lookahead or 3D compute passes (`vendor-documented`, verified in [`src/video_pipeline.cpp`](src/video_pipeline.cpp)). Compressed video and mixed WASAPI stereo AAC-LC audio packets live in pre-allocated RAM ring buffers (`inferred` zero active replay disk writes; `measured` 0 B active disk write during benchmark). Closing the Settings GUI destroys its window handle and hides Clipper cleanly in the system tray so zero UI timers or GDI resources stay active during gameplay.

## Design Constraints

* Single-binary Win32 app with on-demand dark-mode Settings GUI destroyed completely when hidden to tray.
* Zero gameplay overlays, zero `Present` or swapchain hooks, zero Electron or WebView, and zero `ffmpeg.exe` subprocesses.
* Zero-copy GPU color conversion via `ID3D11VideoProcessor` (`BGRA8` Full Range to BT.709 Studio `NV12`).
* NVENC low-latency single-pass H.264 (`P4`, `LOW_LATENCY`, `enableLookahead=0`, `enableAQ=0`, `enableTemporalAQ=0`, 0 B-frames, 4-slot async pool).
* Mixed desktop loopback and microphone audio encoded once via Media Foundation `CLSID_AACMFTEncoder`.

## Build Instructions

Build requirements are Visual Studio Build Tools 2022 (`MSVC`), Windows SDK (`10.0.26100.0` or compatible), CMake `>= 3.20`, and Ninja. `CMakeLists.txt` sets `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to `Embedded` (`CMP0141`) so builds run without `mspdbsrv.exe` IPC sockets and links `clipper.exe` against native Win32 libraries (`comctl32`, `dwmapi`, `uxtheme`, `shcore`).

```cmd
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces three binaries inside `build/`.

* `build\clipper.exe` (`265,728` bytes `measured`)
* `build\clipper_bench.exe` (`203,264` bytes `measured`)
* `build\clipper_tests.exe` (`242,688` bytes `measured`)

## Usage

Launch `build\clipper.exe` from an interactive Windows desktop session. By default it starts background capture, places a dark-slate icon with a red center dot in the notification tray, opens the Settings GUI (`ClipperSettingsWindow`), and loads or writes `clipper.ini` next to the executable.

Launching `clipper.exe` again while already running focuses and re-opens the Settings GUI without spawning a duplicate capture session. Pass `--tray` or `--minimized` to start directly in the system tray without opening the window.

### Settings GUI Controls

* **Screen:** Lists attached DXGI monitors with friendly name, resolution, refresh rate, and `[Primary]` tag matching capture enumeration order (`Refresh` updates the list).
* **Frame Rate, Replay, Bitrate:** Sets target FPS (`30`, `60`), rolling seconds (`15..300`), bitrate (`5..80 Mbps`), and live estimated ring buffer RAM readout.
* **Audio and Chime Toggles:** Independently toggles Desktop Audio loopback, Microphone capture, and save chime.
* **Save Hotkey:** Click the hotkey button and press any combination (`Ctrl+Alt+S` default, single `F1..F24`/Numpad/Nav keys, or modifiers + alphanumeric/OEM keys; `Esc` cancels, **Reset** restores `Ctrl+Alt+S`).
* **Clips Folder:** Edit box paired with **Browse...** (`IFileOpenDialog` folder picker) and **Open** (opens Explorer).
* **Actions:** **Apply** saves `clipper.ini` (updating folder/chime/hotkey in-place without clearing your replay buffer, or cleanly restarting capture when display/quality/audio settings change), **Save Replay** writes the current clip, **Hide to Tray** destroys the window while keeping capture active, and **Quit** stops recording and exits.

### Tray Icon Actions

* **Left-click or Double-click** re-opens the Settings window.
* **Right-click Menu** offers **Open Settings**, **Save Replay**, **Microphone** mute toggle, **Open Clips Folder**, and **Exit**.

### Command-Line Flags

| Flag | Effect |
| --- | --- |
| `--tray`, `--minimized` | Starts background capture directly in the system tray without opening the GUI |
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
| `hotkey_save` | `Ctrl+Alt+S` | Modifiers + VK | Global save hotkey (`Numpad0..9`, `NumMul`, `NumAdd`, `NumSub`, `NumDiv`, `NumDot`, `Backtick`, `Minus`, `Equals`, `Tab`, `F1..F24`, Nav) |
| `output_dir` | `%USERPROFILE%\Videos\Clips` | Directory path | Created automatically on save |
| `save_chime` | `true` | `true`, `false` | Plays `MessageBeep(MB_OK)` after atomic rename |

## Verification and Benchmarks

Run unit, monitor enumeration, hotkey safety, and Media Foundation decode-back tests.

```powershell
.\build\clipper_tests.exe
```

Run a 3-second live desktop capture self-test outside any restricted low-integrity sandbox (desktop duplication returns `0x80070005` under low integrity).

```powershell
.\build\clipper_bench.exe --self-test-clip 3 --out .\build\test_out\live_replay.mp4
```

Run the alternating D3D11 + DXGI swapchain load benchmark spawning `clipper.exe --tray --fail-fast`. Close background GPU encoders, games, and heavy browser hardware decode tabs first for clean A/B comparisons.

```powershell
.\build\clipper_bench.exe --gpu-load-bench 3 --runs 5
```

### Measured Hardware Results

Hardware under test was Windows 11 (`10.0.26300.0`), Intel Core `i5-14500HX`, and NVIDIA GeForce RTX 4050 Laptop GPU (`Driver 621.24`, `NVENC 13.1`, dual `1920x1080` displays).

* **Unit, Monitor, Hotkey, and Codec Suite (`measured`, `clipper_tests.exe`):** All 5 suites passed with `Total failures: 0`. Synthesized 60-frame `1920x1080` H.264 + `48 kHz` AAC-LC MP4 decoded `60` video frames and `192,512` bytes of PCM (`rms=11249.7`) via `IMFSourceReader`.
* **Live 3-Second Replay Self-Test (`measured`, `--self-test-clip 3`):** Captured `137` changed + `50` static frames (`183` encoded, `0` errors), video capture plus submit median `407.0 us` (`p95 = 599.0 us`), encoded `141` AAC frames, saved `2,196,157` bytes in `4.73 ms`, and verified `dur=3.017s`, `videoFrames=181`, and `pcmBytes=577,536` via `IMFSourceReader`.
* **Headless `--tray` Swapchain 3D Benchmark (`measured`, `--gpu-load-bench 3 --runs 2`):** Verified `--tray --fail-fast --ready-event` spawn and PID-scoped shutdown (`OFF p50 = 9.383 ms`, `90.7 FPS` vs `ON p50 = 10.223 ms`, `84.7 FPS` under active laptop thermal/GPU load; `clipper.exe` used `0.309%` median CPU, `135.4 MB` median RAM, and `0 B` active disk writes).
* **GUI Window Lifecycle (`measured`):** Confirmed `ClipperSettingsWindow` launches visible with `17` child controls, destroys its HWND cleanly on `WM_CLOSE` while keeping `ClipperTrayHiddenWindow` alive, re-opens on second `clipper.exe` invocation, and exits cleanly with code `0` on tray `WM_CLOSE`.
* **Quiet-Host Offscreen 3D Shader Reference (`measured`, prior idle-host pre-GUI build, `--gpu-load-bench 3 --runs 5`):** `OFF p50 = 5.157 ms` (`178.3 FPS`) vs `ON p50 = 5.192 ms` (`175.0 FPS`), `Delta p50 = +0.035 ms` (`+0.68%`), `0.103%` CPU, `135.1 MB` RAM, and `0 B` active disk writes.
