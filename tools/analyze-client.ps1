param(
    [Parameter(Mandatory)] [string] $ClientExe,
    [Parameter(Mandatory)] [string] $WorkDir,
    [string] $GhidraDir = 'C:\Program Files\ghidra_12.0.4_PUBLIC',
    [string] $MetadataRoot = (Join-Path $PSScriptRoot '../symbols')
)
$ErrorActionPreference = 'Stop'
$image = (Resolve-Path -LiteralPath $ClientExe).Path
$digest = (Get-FileHash -LiteralPath $image -Algorithm MD5).Hash.ToLowerInvariant()
$work = [IO.Path]::GetFullPath((Join-Path $WorkDir $digest))
New-Item -ItemType Directory -Force $work | Out-Null
$pinned = Join-Path $work 'client.exe'
Copy-Item -LiteralPath $image -Destination $pinned -Force
$analyzer = Join-Path $GhidraDir ('support/' + $(if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'analyzeHeadless.bat' } else { 'analyzeHeadless' }))
$scripts = Join-Path $PSScriptRoot 'ghidra'
foreach ($configuration in @('borlanddelphi', 'default')) {
    $compilerSpec = if ($configuration -eq 'default') { 'windows' } else { $configuration }
    $arguments = @($work, ('ClientLab-' + $configuration), '-import', $pinned, '-overwrite', '-processor', 'x86:LE:32:default', '-cspec', $compilerSpec,
        '-scriptPath', $scripts, '-postScript', 'ExportDelphiMethods.java', (Join-Path $work ($configuration + '-methods.txt')),
        '-postScript', 'ExportFunctions.java', (Join-Path $work ($configuration + '-functions.txt')))
    & $analyzer @arguments
    if ($LASTEXITCODE -ne 0) { throw "Ghidra analysis failed for $configuration; existing metadata was not replaced." }
}
if ((Get-FileHash -LiteralPath $pinned -Algorithm MD5).Hash.ToLowerInvariant() -ne $digest) { throw 'The pinned analysis image changed.' }
$functions = [IO.File]::ReadAllLines((Join-Path $work 'borlanddelphi-functions.txt'))
if ($functions.Count -eq 0) { throw 'Ghidra did not produce any functions.' }
$known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in $functions) { [void]$known.Add(($line -split '\s+')[0]) }
$additional = @([IO.File]::ReadLines((Join-Path $work 'default-functions.txt')) | ForEach-Object {
    $entry = ($_ -split '\s+')[0]; if (-not $known.Contains($entry)) { $entry }
})
$target = [IO.Path]::GetFullPath((Join-Path $MetadataRoot $digest))
New-Item -ItemType Directory -Force $target | Out-Null
[IO.File]::WriteAllLines((Join-Path $target 'ghidra-functions.txt'), (@("# Ghidra Borland Delphi function ranges, enriched with ExportDelphiMethods.java.", "# Exact client MD5 $digest. Analysis metadata only; no executable bytes.") + $functions))
[IO.File]::WriteAllLines((Join-Path $target 'ghidra-seeds.txt'), (@("# Supplemental Ghidra default-analysis entries for exact client MD5 $digest.") + $additional))
Write-Output "Analysis metadata: $target"
Write-Output 'Review and validate the generated runtime before committing the metadata. No executable or generated client C is copied into symbols.'
