# Narrow Windows regression harness; does not configure/build the GUI or write into build-local.
# Uses existing supporting objects; recompiles generation, DB repositories and entry in scratch.
param([string]$Repo = 'D:\Code\NxProxy', [string]$Scratch = $env:PI_SCRATCH_DIR)
$ErrorActionPreference = 'Stop'
if (!$Scratch) { throw 'Set PI_SCRATCH_DIR or pass -Scratch (an external temporary directory).' }
$work = Join-Path $Scratch 'selector-resource-harness'
New-Item -ItemType Directory -Force $work | Out-Null
$ninja = [IO.File]::ReadAllText((Join-Path $Repo 'build-local\build.ninja'))
$match = [regex]::Match($ninja, '(?m)^build CMakeFiles\\Throne.dir\\Unity\\unity_20_cxx.cxx.obj:.*\r?\n(?:.*\r?\n)*?  DEFINES = (.*)\r?\n  FLAGS = (.*)\r?\n  INCLUDES = (.*)')
if (!$match.Success) { throw 'Unexpected build-local compile rule; configure a baseline build first.' }
$flags = $match.Groups[1].Value.Trim()+' '+$match.Groups[2].Value.Trim()+' '+$match.Groups[3].Value.Trim()
$flags += ' /DSELECTOR_RESOURCE_STANDALONE /Gy /Gw /nologo /c'
$unity1 = [IO.File]::ReadAllText((Join-Path $Repo 'build-local\CMakeFiles\Throne.dir\Unity\unity_1_cxx.cxx'))
$mainLine = '#include "'+($Repo.Replace('\','/'))+'/src/main.cpp"'
if (($unity1.Split([string[]]@($mainLine), [StringSplitOptions]::None)).Length -ne 2) { throw 'Unexpected unity main content; abort.' }
$unity1 = $unity1.Replace($mainLine, '#include "'+$Repo.Replace('\','/')+'/tests/selector_resource_generation.cpp"')
[IO.File]::WriteAllText((Join-Path $work 'entry.cpp'), $unity1)
[IO.File]::WriteAllText((Join-Path $work 'entry.rsp'), $flags+' /Fo"'+$work+'\entry.obj" "'+$work+'\entry.cpp"')
[IO.File]::WriteAllText((Join-Path $work 'generate.rsp'), $flags+' /Fo"'+$work+'\generate.obj" "'+$Repo+'\build-local\CMakeFiles\Throne.dir\Unity\unity_20_cxx.cxx"')
[IO.File]::WriteAllText((Join-Path $work 'database.rsp'), $flags+' /Fo"'+$work+'\database.obj" "'+$Repo+'\build-local\CMakeFiles\Throne.dir\Unity\unity_23_cxx.cxx"')
$linkRule = [regex]::Match($ninja, '(?m)^build Throne.exe: [^ ]+ (.*?) \| ')
$libs = [regex]::Match($ninja, '(?m)^  LINK_LIBRARIES = (.*)').Groups[1].Value.Trim()
if (!$linkRule.Success -or !$libs) { throw 'Unexpected link rule; abort.' }
$objects = $linkRule.Groups[1].Value.Split(' ') | Where-Object { $_ -notmatch 'Unity\\unity_(1|20|23)_cxx.cxx.obj$' } | ForEach-Object { '"'+(Join-Path "$Repo\build-local" $_)+'"' }
$libs = $libs.Replace('myproto.lib', '"'+$Repo+'\build-local\myproto.lib"').Replace('qhotkey.lib','"'+$Repo+'\build-local\qhotkey.lib"')
[IO.File]::WriteAllText((Join-Path $work 'link.rsp'), '/nologo /SUBSYSTEM:CONSOLE /OPT:REF /OPT:ICF /OUT:"'+$work+'\selector_resource.exe" "'+$work+'\entry.obj" "'+$work+'\generate.obj" "'+$work+'\database.obj" '+($objects -join ' ')+' '+$libs)
$cmd = @"
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul
cl @"$work\entry.rsp" >"$work\compile.log" 2>&1
if errorlevel 1 exit /b 1
cl @"$work\generate.rsp" >>"$work\compile.log" 2>&1
if errorlevel 1 exit /b 1
cl @"$work\database.rsp" >>"$work\compile.log" 2>&1
if errorlevel 1 exit /b 1
link @"$work\link.rsp" >"$work\link.log" 2>&1
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $work 'build.cmd'), $cmd)
& cmd /c (Join-Path $work 'build.cmd')
if ($LASTEXITCODE) { throw "Compile/link failed: $work\compile.log, $work\link.log" }
$env:PATH = 'D:\Tools\Qt\6.11.2\msvc2022_64\bin;'+$env:PATH
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = 'D:\Tools\Qt\6.11.2\msvc2022_64\plugins'
& (Join-Path $work 'selector_resource.exe') 2>&1 | Tee-Object (Join-Path $work 'results.log')
if ($LASTEXITCODE) { throw 'Selector generation regression failed.' }
