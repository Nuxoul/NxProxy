param([string]$Root=(Split-Path -Parent $PSScriptRoot),[string]$Scratch=$env:PI_SCRATCH_DIR)
$ErrorActionPreference='Stop'
if (!$Scratch) {throw 'Pass -Scratch: all build/artifact files must be outside the repository'}
New-Item -ItemType Directory -Force $Scratch | Out-Null
# Extract exact production method bodies, not a SQL reimplementation.
$cpp=[IO.File]::ReadAllText("$Root\src\database\ProfilesRepo.cpp")
$header=[IO.File]::ReadAllText("$Root\include\database\ProfilesRepo.h")
$utf8=New-Object Text.UTF8Encoding($false)
function Method([string]$name) {
 $match=[regex]::Match($cpp,"(?m)^    [^\r\n]*ProfilesRepo::$name\(")
 if (!$match.Success) {throw "Missing method $name"}
 $open=$cpp.IndexOf('{',$match.Index);$depth=1;$end=$open+1
 while($depth -gt 0 -and $end -lt $cpp.Length) {if($cpp[$end] -eq '{'){$depth++};if($cpp[$end] -eq '}'){$depth--};$end++}
 if($depth -ne 0){throw "Unbalanced method $name"}
 $cpp.Substring($match.Index,$end-$match.Index)
}
$methods=(@('ProfilesRepo','createTables','profilesColumnExists','saveToDatabase','Save','SaveTestResults') | ForEach-Object {Method $_}) -join "`n"
[IO.File]::WriteAllText("$Scratch\o1_methods.inc",$methods,$utf8)
$start=$header.IndexOf('    struct TestResult {');$end=$header.IndexOf('    class ProfilesRepo {')
if($start -lt 0 -or $end -le $start){throw 'Missing result records'}
[IO.File]::WriteAllText("$Scratch\o1_records.inc",$header.Substring($start,$end-$start),$utf8)
$vc='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vc`" >nul && set" | ForEach-Object {if($_ -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$qt='D:\Tools\Qt\6.11.2\msvc2022_64'
$flags=@('/nologo','/c','/std:c++20','/EHsc','/O2','/MT','/utf-8','/Zc:__cplusplus','/DNOMINMAX',"/I$Root","/I$Scratch","/I$Root\3rdparty\SQLiteCpp\include","/I$qt\include","/I$qt\include\QtCore","/I$qt\mkspecs\win32-msvc")
$objects=@();$sources=@("$Root\tests\test_results_sqlite.cpp")
foreach($s in @('Backup','Column','Database','Exception','Statement','Transaction')){$sources+="$Root\3rdparty\SQLiteCpp\src\$s.cpp"}
foreach($src in $sources){$obj="$Scratch\$([IO.Path]::GetFileNameWithoutExtension($src)).obj";& cl @flags $src "/Fo$obj";if($LASTEXITCODE -ne 0){throw "Compile failed: $src"};$objects+=$obj}
& cl /nologo /c /O2 /MT "$Root\3rdparty\SQLiteCpp\src\sqlite3.c" "/Fo$Scratch\sqlite3.obj"
if($LASTEXITCODE -ne 0){throw 'SQLite C compile failed'};$objects+="$Scratch\sqlite3.obj"
& link /nologo /OUT:"$Scratch\o1_sqlite.exe" @objects "$qt\lib\Qt6Core.lib"
if($LASTEXITCODE -ne 0){throw 'Link failed'}
$env:PATH="$qt\bin;$env:PATH"
& "$Scratch\o1_sqlite.exe" | Tee-Object -FilePath "$Scratch\o1_benchmark.csv"
if($LASTEXITCODE -ne 0){throw 'Regression/benchmark failed'}
