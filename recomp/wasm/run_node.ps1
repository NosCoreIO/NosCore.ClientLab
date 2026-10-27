param(
    [string]$Client = 'C:/Program Files (x86)/Nostale/NosCore.exe',
    [int]$Seconds = 120,
    [switch]$NoBuild
)
$env:EMSDK = 'C:\tools\emsdk'
$env:EM_CONFIG = 'C:\tools\emsdk\.emscripten'
$work = 'C:\tools\re\work\wasm'
if (-not $NoBuild) {
    python (Join-Path $PSScriptRoot 'compile_all.py') --link 2>&1 | Select-String -Pattern 'compiled|linked|error' | ForEach-Object { $_.Line }
}
$node = (Get-ChildItem C:\tools\emsdk\node -Recurse -Filter node.exe | Select-Object -First 1).FullName
$p = Start-Process $node -ArgumentList 'nostale.js', "`"$Client`"", 'gf', '0' -PassThru -NoNewWindow -WorkingDirectory $work `
    -RedirectStandardError (Join-Path $work 'stderr.txt') -RedirectStandardOutput (Join-Path $work 'stdout.txt')
$p.WaitForExit($Seconds * 1000) | Out-Null
if (-not $p.HasExited) { "still running after $Seconds s"; Stop-Process -Id $p.Id -Force }
Get-Content (Join-Path $work 'stderr.txt') | Select-String -NotMatch '^\s+trace' | Select-Object -Last 12 | ForEach-Object { $_.Line }
