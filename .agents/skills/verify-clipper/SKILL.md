---
name: verify-clipper
description: Build, unit-test, live-capture verify, benchmark, and clean up the Clipper Windows instant-replay recorder binaries.
---

# Verify Clipper

Use this procedure to verify `clipper` after code or build changes on Windows with an NVIDIA NVENC GPU and VS Build Tools 2022 installed.

## Launch

Configure and build Release binaries inside the repository root (`c:\Users\yugi\Desktop\recorder`). `CMakeLists.txt` enables `Embedded` MSVC debug info (`CMP0141`) so Ninja builds run without `mspdbsrv.exe` failures.

```cmd
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build c:\Users\yugi\Desktop\recorder\build
```

Check binary sizes after building.

```powershell
Get-ChildItem c:\Users\yugi\Desktop\recorder\build\*.exe | Select-Object Name, Length
```

## Doctor

Check prerequisite process state before running live benchmarks.

```powershell
Get-Process clipper -ErrorAction SilentlyContinue
```

Stop any stale `bloatfclips.exe` instance before running `clipper_bench.exe --gpu-load-bench`. If live DXGI Desktop Duplication fails with `0x80070005` (`E_ACCESSDENIED`) or exit code `2` inside a restricted LOW-integrity terminal sandbox, re-run the command in an interactive MEDIUM-integrity desktop session outside the sandbox.

## Drive

Run the automated test binary first (works inside the sandbox).

```powershell
c:\Users\yugi\Desktop\recorder\build\clipper_tests.exe
```

Run the live 3-second replay capture and `IMFSourceReader` decode-back check in a desktop session.

```powershell
c:\Users\yugi\Desktop\recorder\build\clipper_bench.exe --self-test-clip 3 --out c:\Users\yugi\Desktop\recorder\build\test_out\live_replay.mp4
c:\Users\yugi\Desktop\recorder\build\clipper_bench.exe --self-test-clip 2 --fps=120 --out c:\Users\yugi\Desktop\recorder\build\test_out\live_replay_120fps.mp4
```

Run the 5-round alternating 3D swapchain GPU load benchmark (launches `bloatfclips.exe --tray --fail-fast` headlessly). Close heavy background GPU apps first if checking sub-millisecond baseline jitter.

```powershell
c:\Users\yugi\Desktop\recorder\build\clipper_bench.exe --gpu-load-bench 3 --runs 5
```

If testing tray startup directly, pass `--tray --fail-fast --ready-event=<UniqueName>` to `build\bloatfclips.exe` and terminate only the matching child process PID via `WM_CLOSE`.

When verifying UI changes, launch `build\bloatfclips.exe` without `--tray` and verify `ClipperSettingsWindow` opens (`IsWindowVisible`, `1000x640`, `children=0`), destroys itself cleanly on `WM_CLOSE` while leaving `ClipperTrayHiddenWindow` running, re-opens on a second `bloatfclips.exe` invocation, and exits cleanly when `WM_CLOSE` is sent to `ClipperTrayHiddenWindow`.

## Evidence

Record and verify every pass gate below before reporting success.

* `cmake --build build` exits `0` with zero compiler warnings.
* `clipper_tests.exe` prints all 6 `PASS` lines (`TestConfig`, `TestRingBuffer`, `TestMp4StructureAndCo64`, `TestRecorderComponentsAndLifecycle`, `TestEndToEndNvencAacAndSourceReader`, `TestClipLibrary`) and `Total failures: 0`.
* `clipper_bench.exe --self-test-clip 3` (60 fps) and `--self-test-clip 2 --fps=120` print `PASS SelfTestClip` with `recoveries=0`, `err=0` on both video and audio, valid MP4 duration, `videoFrames > 0`, and `pcmBytes > 0` (remove temporary `live_replay_120fps.mp4` afterward).
* `clipper_bench.exe --gpu-load-bench 3 --runs 5` exits `0` with `max active disk write=0 B` and prints CPU, RAM, and frame-time medians across all alternating rounds.

## Cleanup

Confirm zero orphaned tray daemon processes remain running and verify the output clip exists cleanly on disk.

```powershell
Get-Process clipper -ErrorAction SilentlyContinue
Get-Item c:\Users\yugi\Desktop\recorder\build\test_out\live_replay.mp4 | Select-Object Name, Length
```
