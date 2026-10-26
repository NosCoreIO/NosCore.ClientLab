param(
    [string]$Client = 'C:\Program Files (x86)\Nostale\NosCore.exe',
    [string]$Work = 'C:\tools\re\work',
    [int]$MaxRounds = 25,
    [int]$Seconds = 25,
    [string[]]$ClientArgs = @('gf', '0')
)
$gen = Join-Path $Work 'recomp\gen'
$out = Join-Path $Work 'recomp\out'
$seeds = Join-Path $Work 'recomp\seeds.txt'
$log = Join-Path $out 'recomp.log'
if (-not (Test-Path $seeds)) { New-Item -ItemType File $seeds | Out-Null }

for ($round = 1; $round -le $MaxRounds; $round++) {
    Get-Process NosTaleRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
    $argList = @("`"$Client`"") + $ClientArgs
    $p = Start-Process (Join-Path $out 'NosTaleRecomp.exe') -ArgumentList $argList -PassThru -WorkingDirectory $out
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
        if ($text -match 'FATAL') { break }
    }
    Start-Sleep -Milliseconds 300
    Get-Process NosTaleRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
    $text = Get-Content $log -Raw -ErrorAction SilentlyContinue
    $m = [regex]::Match($text, 'no recompiled code for (?:guest address|indirect jump target) ([0-9a-f]{8})')
    if (-not $m.Success) {
        "round $round : no missing-code stop"
        $text
        break
    }
    $addr = $m.Groups[1].Value
    "round $round : missing $addr"
    if (Select-String -Path $seeds -Pattern "^$addr$" -Quiet) { "already seeded $addr, giving up"; $text; break }
    Add-Content $seeds $addr
    python (Join-Path $PSScriptRoot 'lift.py') (Join-Path $Work 'input\NosCore.exe') (Join-Path $Work 'functions.txt') $gen --seeds $seeds | Select-Object -Last 1
    & (Join-Path $PSScriptRoot 'build.ps1') -Gen $gen -Out $out | Select-Object -Last 1
}
