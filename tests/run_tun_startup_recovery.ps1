param([string]$VcVars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference='Stop'
if (!$env:PI_SCRATCH_DIR) { throw 'Set PI_SCRATCH_DIR to an external temporary directory' }
$repo=Split-Path $PSScriptRoot -Parent
$work=Join-Path $env:PI_SCRATCH_DIR 'tun-recovery-policy'
New-Item -ItemType Directory -Force $work | Out-Null
$commands=@"
@echo off
call "$VcVars" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /I"$repo" /Fo"$work\policy.obj" /Fe"$work\policy.exe" "$repo\tests\tun_startup_recovery.cpp"
if errorlevel 1 exit /b 1
"$work\policy.exe"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $work 'run.cmd'),$commands)
& cmd.exe /c (Join-Path $work 'run.cmd')
if($LASTEXITCODE-ne 0){throw 'TUN startup recovery policy regression failed'}
