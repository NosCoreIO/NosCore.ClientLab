param(
    [string]$Gen = 'C:\tools\re\work\recomp\gen',
    [string]$Out = 'C:\tools\re\work\recomp\out',
    [switch]$Full
)
$ErrorActionPreference = 'Stop'
$runtime = Join-Path $PSScriptRoot 'runtime'
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars32.bat'

$stale = Get-ChildItem $Gen -Filter *.c | Where-Object {
    $o = Join-Path $obj ($_.BaseName + '.obj')
    $Full -or -not (Test-Path $o) -or (Get-Item $o).LastWriteTime -lt $_.LastWriteTime
}
$list = Join-Path $Out 'stale.rsp'
$stale | ForEach-Object { '"' + $_.FullName + '"' } | Set-Content -Encoding ascii $list

$cmds = @("call `"$vcvars`" >nul 2>nul")
if ($stale) {
    "compiling $($stale.Count) generated files"
    $cmds += "cl /nologo /c /MP /W0 /GS- /Gy /O1 /I`"$runtime`" /Fo`"$obj\\`" @`"$list`" || exit /b 1"
}
$cmds += "cl /nologo /c /W3 /GS- /O2 /Zi /I`"$runtime`" /Fo`"$obj\\`" /Fd`"$obj\rt.pdb`" `"$runtime\rt.c`" `"$runtime\rt_host.c`" || exit /b 1"
$cmds += "link /nologo /DEBUG /OUT:`"$Out\NosTaleRecomp.exe`" /SUBSYSTEM:CONSOLE /BASE:0x30000000 /DYNAMICBASE:NO /FIXED /NXCOMPAT /LARGEADDRESSAWARE `"$obj\*.obj`" user32.lib kernel32.lib psapi.lib || exit /b 1"
$bat = Join-Path $Out 'build.bat'
$cmds -join "`r`n" | Set-Content -Encoding ascii $bat
cmd /c "`"$bat`" 2>&1" | Where-Object { $_ -notmatch '^(fn_\d+|table|rt|rt_host)\.c$|^Generating Code' }
if ($LASTEXITCODE) { throw "build failed" }
"built $Out\NosTaleRecomp.exe"
