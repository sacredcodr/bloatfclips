# Clipper

Clipper is a native C++20 Windows instant-replay daemon and desktop app (`clipper.exe`) built to avoid frame-time spikes and input latency overhead.

It captures displays via DXGI Desktop Duplication (`DuplicateOutput1`), converts `B8G8R8A8_UNORM` to `NV12` on fixed-function `ID3D11VideoProcessor` hardware, and encodes H.264 on NVENC (`13.1`) without CUDA lookahead or 3D compute passes (`vendor-documented`, verified in [`src/video_pipeline.cpp`](src/video_pipeline.cpp)). Compressed video and mixed WASAPI stereo AAC-LC audio packets live in pre-allocated RAM ring buffers (`inferred` zero active replay disk writes; `measured` 0 B active disk write during benchmark). Closing the desktop window destroys its `HWND`, Direct2D/WIC surfaces, and UI timers and hides Clipper cleanly in the system tray so zero UI resources stay active during gameplay.

## Design Constraints

* Single-binary native C++20 app with on-demand Direct2D + DirectWrite desktop UI destroyed completely when hidden to tray.
* Zero gameplay overlays, zero `Present` or swapchain hooks, zero Electron or WebView, and zero `ffmpeg.exe` subprocesses.
* Zero-copy GPU color conversion via `ID3D11VideoProcessor` (`BGRA8` Full Range to BT.709 Studio `NV12`).
* NVENC low-latency single-pass H.264 (`P4`, `LOW_LATENCY`, `enableLookahead=0`, `enableAQ=0`, `enableTemporalAQ=0`, 0 B-frames, 4-slot async pool).
* Mixed desktop loopback and microphone audio encoded once via Media Foundation `CLSID_AACMFTEncoder`.

## Build Instructions

Build requirements are Visual Studio Build Tools 2022 (`MSVC`), Windows SDK (`10.0.26100.0` or compatible), CMake `>= 3.20`, and Ninja. `CMakeLists.txt` sets `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT` to `Embedded` (`CMP0141`) so builds run without `mspdbsrv.exe` IPC sockets and links `clipper.exe` against native OS libraries (`d2d1`, `dwrite`, `windowscodecs`, `dwmapi`, `shcore`, `shell32`).

```cmd
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces three binaries inside `build/`.

* `build\clipper.exe` (`343,040` bytes `measured`)
* `build\clipper_bench.exe` (`203,264` bytes `measured`)
* `build\clipper_tests.exe` (`266,752` bytes `measured`)

## Usage

Launch `build\clipper.exe` from an interactive Windows desktop session. By default it starts background capture, places a dark-slate icon with a red center dot in the notification tray, opens the Direct2D desktop window (`ClipperSettingsWindow`), and loads or writes `clipper.ini` next to the executable.

Launching `clipper.exe` again while already running focuses and re-opens the window without spawning a duplicate capture session. Pass `--tray` or `--minimized` to start directly in the system tray without opening the window, or `--page=clips|capture|audio|hotkeys` to open a specific view.

### Desktop UI (`src/settings_window.{h,cpp}`, `src/clip_library.{h,cpp}`, `src/ui_icons.h`)

* **Left Navigation Rail & Live Buffer Card:** Custom vector icons switch between **Clips**, **Capture**, **Audio**, and **Hotkeys** views. The bottom card displays live engine state (`LIVE BUFFER` / `RECOVERING` / `FAULTED`), active resolution/FPS, replay RAM footprint, active hotkey keycaps, **Save Clip**, **Tray**, and **Quit**.
* **Clips:** Scans `output_dir` for `.mp4` replays, extracts MP4 container duration via `moov/mvhd` and video thumbnails on demand via `IShellItemImageFactory`, and provides click-to-play (`ShellExecuteW`), reveal in Explorer, and Recycle Bin delete (`SHFileOperationW`).
* **Capture:** Interactive display selector cards (`EnumerateCaptureMonitors()`), quick quality presets (**Competitive**, **High Quality**, **Compact**), `30 fps | 60 fps` segmented toggle, replay duration pills (`15..300` s), draggable VBR bitrate slider (`5..80` Mbps), and live ring RAM budget bar.
* **Audio:** Toggle cards for **Desktop Audio**, **Microphone**, and **Save Confirmation Chime** paired with live WASAPI packet and AAC frame counters.
* **Hotkeys:** 3D-styled keycap binding card (`F1..F24`, Numpad, Nav keys, or modifier + alphanumeric/OEM combos; `Esc` cancels, **Reset** restores `Ctrl+Alt+S`) and destination folder picker (`IFileOpenDialog`) + docked **Reset** / **Apply Changes** bar.

### Tray Icon Actions

* **Left-click or Double-click** re-opens the Settings window.
* **Right-click Menu** offers **Open Settings**, **Save Replay**, **Microphone** mute toggle, **Open Clips Folder**, and **Exit**.

### Command-Line Flags

| Flag | Effect |
| --- | --- |
| `--tray`, `--minimized` | Starts background capture directly in the system tray without opening the GUI |
| `--page=TAB` | Opens the desktop window on `clips`, `capture`, `audio`, or `hotkeys` |
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

* **Unit, Monitor, Hotkey, Clip Library, and Codec Suite (`measured`, `clipper_tests.exe`):** All 6 suites (`TestConfig`, `TestRingBuffer`, `TestMp4StructureAndCo64`, `TestRecorderComponentsAndLifecycle`, `TestEndToEndNvencAacAndSourceReader`, `TestClipLibrary`) passed with `Total failures: 0`. Synthesized 60-frame `1920x1080` H.264 + `48 kHz` AAC-LC MP4 decoded `60` video frames and `192,512` bytes of PCM (`rms=11249.7`) via `IMFSourceReader`, and `ProbeMp4DurationSeconds` read `1.003s`.
* **Live 3-Second Replay Self-Test (`measured`, `--self-test-clip 3`):** Captured `186` changed + `1` static frames (`183` encoded, `0` errors), video capture plus submit median `367.0 us` (`p95 = 562.0 us`), encoded `141` AAC frames, saved `2,007,490` bytes in `3.88 ms`, and verified `dur=3.017s`, `videoFrames=181`, and `pcmBytes=577,536` via `IMFSourceReader`.
* **Headless `--tray` Swapchain 3D Benchmark (`measured`, `--gpu-load-bench 3 --runs 2`):** Verified `--tray --fail-fast --ready-event` spawn and PID-scoped shutdown (`OFF p50 = 4.795 ms`, `209.3 FPS` vs `ON p50 = 4.836 ms`, `201.7 FPS`, `Delta p50 = +0.041 ms` / `+0.85%`, `0.233%` median CPU, `135.7 MB` median RAM, and `0 B` active disk writes).
* **GUI Window Lifecycle (`measured`):** Confirmed Direct2D `ClipperSettingsWindow` launches visible (`976x679`, `children=0`), destroys its HWND and D2D/WIC/GDI resources cleanly on `WM_CLOSE` while keeping `ClipperTrayHiddenWindow` alive, re-opens on second `clipper.exe` invocation, and exits cleanly with code `0` on tray `WM_CLOSE`.
