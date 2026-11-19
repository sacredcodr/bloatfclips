# Builds the Release exe and packs it into dist\bloatfclips-setup-<version>.exe with Inno Setup 6.
# Usage (from the repo root): powershell -ExecutionPolicy Bypass -File tools\build_installer.ps1
# Used both locally and by .github/workflows/build.yml, which signs in between:
#   -ExeOnly      build build-release\bloatfclips.exe and stop
#   -Exe <path>   skip the build and package this (already signed) exe
param([switch]$ExeOnly, [string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# Reuse an already-initialised MSVC environment (CI), otherwise find vcvars64.bat via vswhere.
$envPrefix = ''
if (-not $env:VCINSTALLDIR) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vcvars = $null
    if (Test-Path $vswhere) {
        $vcvars = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
    }
    if (-not $vcvars) { throw 'MSVC not found. Install Visual Studio Build Tools 2022 with the C++ workload.' }
    $envPrefix = "`"$vcvars`" >nul && "
}

if (-not $Exe) {
    $build = Join-Path $root 'build-release'
    cmd /c "$envPrefix cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build `"$build`""
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
    $Exe = Join-Path $build 'bloatfclips.exe'
    if ($ExeOnly) { return Get-Item $Exe | Select-Object FullName, Length }
}

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup' }

$exePath = (Resolve-Path $Exe).Path
& $iscc /Q "/DSourceExe=$exePath" (Join-Path $root 'installer\bloatfclips.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compile failed' }

# Portable build: just the exe in a ZIP. Unzip anywhere writable and run; settings go next to it.
$version = (Get-Item $exePath).VersionInfo.ProductVersion
$stage = Join-Path $root 'build-release\portable\bloatfclips'
Remove-Item -Recurse -Force (Split-Path $stage) -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exePath (Join-Path $stage 'bloatfclips.exe')
Compress-Archive -Path $stage -DestinationPath (Join-Path $root "dist\bloatfclips-$version-portable.zip") -Force

Get-ChildItem (Join-Path $root 'dist') | Select-Object Name, Length
