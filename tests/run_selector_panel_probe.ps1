param(
 [string]$QtRoot='D:\Tools\Qt\static-toolchains\6.11.2-x64\Qt',
 [string]$VcVars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
 [string]$Ninja='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
 [string]$CMake='C:\Program Files\CMake\bin\cmake.exe'
)
$ErrorActionPreference='Stop'
if (!$env:PI_SCRATCH_DIR) { throw 'Set PI_SCRATCH_DIR to an external temporary directory' }
$work=Join-Path $env:PI_SCRATCH_DIR 'selector-panel-reproducible'
& py -3 (Join-Path $PSScriptRoot 'selector_panel_probe.py') $work
if($LASTEXITCODE -ne 0){throw 'Probe generation failed'}
$commands=@"
@echo off
call "$VcVars" >nul
if errorlevel 1 exit /b 1
"$CMake" -S "$work" -B "$work\build" -G Ninja -DCMAKE_MAKE_PROGRAM="$Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QtRoot"
if errorlevel 1 exit /b 1
"$CMake" --build "$work\build" -j4
if errorlevel 1 exit /b 1
set QT_QPA_PLATFORM=windows
"$work\build\selector_panel_probe.exe"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $work 'run.cmd'),$commands)
& cmd.exe /c (Join-Path $work 'run.cmd')
if($LASTEXITCODE -ne 0){throw 'Selector panel production-code regression failed'}
