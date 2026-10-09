param(
    [string]$BuildDir = 'build-local',
    [string]$QtRoot = 'D:\Tools\Qt\6.11.2\msvc2022_64',
    [string]$VcVars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat',
    [int]$Repeat = 1
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
$scratch = $env:PI_SCRATCH_DIR
if (-not $scratch) { $scratch = [IO.Path]::GetTempPath() }
$exe = Join-Path $scratch 'rpc-transport.exe'
$obj = Join-Path $scratch 'rpc-transport.obj'
$commandFile = Join-Path $scratch 'rpc-transport-build.cmd'
$includes = @($repo, $build, "$repo\3rdparty\simple-protobuf\include",
    "$repo\3rdparty\SQLiteCpp\include", "$repo\3rdparty\SQLiteCpp\sqlite3",
    "$QtRoot\include", "$QtRoot\include\QtCore", "$QtRoot\include\QtNetwork",
    "$QtRoot\include\QtGui", "$QtRoot\include\QtWidgets", "$QtRoot\mkspecs\win32-msvc")
$includeArgs = ($includes | ForEach-Object { '/I"' + $_ + '"' }) -join ' '
$compile = 'cl /nologo /std:c++20 /EHsc /O2 /MT /utf-8 /Zc:__cplusplus /permissive- ' +
    '/DNOMINMAX /DUNICODE /D_UNICODE /DQT_NO_DEBUG /D_CRT_SECURE_NO_WARNINGS ' +
    $includeArgs + ' /Fo"' + $obj + '" /Fe"' + $exe + '" "' +
    "$repo\tests\rpc_transport.cpp" + '" /link /LIBPATH:"' + "$QtRoot\lib" +
    '" "' + "$build\myproto.lib" + '" Qt6Core.lib Qt6Network.lib Qt6Gui.lib Qt6Widgets.lib'
$commands = "@echo off`r`ncall `"$VcVars`" >nul`r`n" +
    "if errorlevel 1 exit /b 1`r`ncmake --build `"$build`" --target myproto`r`n" +
    "if errorlevel 1 exit /b 1`r`n$compile`r`n"
[IO.File]::WriteAllText($commandFile, $commands)
& cmd.exe /c $commandFile
if ($LASTEXITCODE -ne 0) { throw 'RPC standalone build failed' }
$previousPath = $env:PATH
try {
    $env:PATH = "$QtRoot\bin;$previousPath"
    for ($i = 0; $i -lt $Repeat; ++$i) {
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "RPC standalone regression failed at repetition $i" }
    }
} finally { $env:PATH = $previousPath }
