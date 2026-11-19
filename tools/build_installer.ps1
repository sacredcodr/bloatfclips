# Builds the Release exe and packs it into dist\bloatfclips-setup-<version>.exe with Inno Setup 6.
# Usage (from the repo root): powershell -ExecutionPolicy Bypass -File tools\build_installer.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "Visual Studio Build Tools 2022 not found: $vcvars" }
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup" }

$build = Join-Path $root 'build-release'
cmd /c "`"$vcvars`" >nul && cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$build`" --target clipper"
if ($LASTEXITCODE -ne 0) { throw "Release build failed" }

$exe = Join-Path $build 'bloatfclips.exe'
& $iscc "/DSourceExe=$exe" (Join-Path $root 'installer\bloatfclips.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compile failed" }
Get-ChildItem (Join-Path $root 'dist\*.exe') | Sort-Object LastWriteTime | Select-Object -Last 1 FullName, Length
