param(
    [Parameter(Mandatory)] [string] $ClientExe,
    [string] $ToolsDir = "C:\tools\re",
    [string] $WorkDir = "C:\tools\re\work",
    [string] $GhidraDir = "C:\Program Files\ghidra_12.0.4_PUBLIC"
)
$ErrorActionPreference = 'Stop'
$idrDir = Join-Path $ToolsDir 'idr'
$dhrakeDir = Join-Path $ToolsDir 'dhrake'
$inputDir = Join-Path $WorkDir 'input'
New-Item -ItemType Directory -Force $idrDir, $dhrakeDir, $inputDir, (Join-Path $WorkDir 'ghidra'), (Join-Path $WorkDir 'decomp') | Out-Null

if (-not (Test-Path (Join-Path $idrDir 'Idr.exe'))) {
    foreach ($f in 'Idr.exe', 'Icons.dll', 'dis.dll') {
        gh api "repos/crypto2011/IDR/contents/bin/$f" -H 'Accept: application/vnd.github.raw' > (Join-Path $idrDir $f)
    }
}
if (-not (Test-Path (Join-Path $idrDir 'kb7.bin'))) {
    python -m pip install -q gdown
    python -c "import gdown; gdown.download(id='1bvkbANJW9GH9MgCslBBmwPPgiSiDcqVd', output=r'$idrDir\kb7.7z'); gdown.download(id='1clSRwEIMO5581qUubqmMNTprpNQp77as', output=r'$idrDir\syskb7.bin')"
    & 'C:\Program Files\7-Zip\7z.exe' x -y "-o$idrDir" (Join-Path $idrDir 'kb7.7z') | Out-Null
}
if (-not (Test-Path (Join-Path $dhrakeDir 'DhrakeInit.java'))) {
    foreach ($f in 'DhrakeInit.java', 'DhrakeParseClass.java') {
        $source = (gh api "repos/huettenhain/dhrake/contents/$f" -H 'Accept: application/vnd.github.raw') -join "`n"
        [IO.File]::WriteAllText((Join-Path $dhrakeDir $f), $source, (New-Object Text.UTF8Encoding $false))
    }
}
Copy-Item (Join-Path $PSScriptRoot 'ghidra\ExportDecompiled.java') $dhrakeDir -Force

# analyzeHeadless.bat cannot handle "(x86)" in paths, so analyse a local copy.
$exe = Join-Path $inputDir 'NosCore.exe'
Copy-Item $ClientExe $exe -Force

python -m pip install -q pywinauto
python (Join-Path $PSScriptRoot 'idr_export.py') (Join-Path $idrDir 'Idr.exe') $exe

& (Join-Path $GhidraDir 'support\analyzeHeadless.bat') (Join-Path $WorkDir 'ghidra') NosTaleClient `
    -import $exe -overwrite -processor x86:LE:32:default -cspec borlanddelphi `
    -scriptPath $dhrakeDir `
    -postScript DhrakeInit.java ([IO.Path]::ChangeExtension($exe, '.idc')) `
    -postScript ExportDecompiled.java (Join-Path $WorkDir 'decomp')
