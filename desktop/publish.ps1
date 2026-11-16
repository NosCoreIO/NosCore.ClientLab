[CmdletBinding()]
param(
    [ValidateSet('win-x64', 'linux-x64', 'osx-x64', 'osx-arm64')]
    [string]$Runtime,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../dist'),
    [string]$Version = '0.1.0',
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$ExecutableName,
    [switch]$SkipChecksum
)
$ErrorActionPreference = 'Stop'
if (!$Runtime) { throw 'Specify -Runtime win-x64, linux-x64, osx-x64 or osx-arm64.' }
if (($Runtime.StartsWith('win-') -and !$IsWindows) -or
    ($Runtime.StartsWith('linux-') -and !$IsLinux) -or
    ($Runtime.StartsWith('osx-') -and !$IsMacOS)) {
    throw 'Publish on the target operating system so CefGlue includes the correct native browser files.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][a-zA-Z0-9.-]+)?$') { throw 'Use a semantic version.' }
$destination = [IO.Path]::GetFullPath($OutputDirectory)
$work = Join-Path $PSScriptRoot ('obj/publish/work-' + $Runtime + '-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $work 'payload'
$architecture = if ($Runtime.EndsWith('arm64')) { 'arm64' } else { 'x64' }
New-Item -ItemType Directory -Path $payload -Force | Out-Null

# The publish input is the launcher project only, never the repository, a client install or user data.
& dotnet publish (Join-Path $PSScriptRoot 'ClientLab.Launcher/ClientLab.Launcher.csproj') `
    -c Release -r $Runtime --self-contained true -o $payload `
    "-p:CefArchitecture=$architecture" "-p:Version=$Version" -p:DebugType=None -p:NuGetAudit=false -m:1
if ($LASTEXITCODE) { throw 'Publishing the desktop launcher failed.' }
$launcher = if ($IsWindows) { 'ClientLab.exe' } else { 'ClientLab' }
if (!(Test-Path (Join-Path $payload $launcher))) { throw 'The published launcher is missing.' }
$helper = if ($IsWindows) { 'Xilium.CefGlue.BrowserProcess.exe' } else { 'Xilium.CefGlue.BrowserProcess' }
if (!(Get-ChildItem $payload -Recurse -File -Filter $helper)) { throw 'The embedded browser subprocess is missing.' }
Copy-Item (Join-Path $PSScriptRoot '../LICENSE') (Join-Path $payload 'LICENSE.txt')
Copy-Item (Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.md') $payload

# Preserve Unix executable bits, and dereference framework symlinks into regular ZIP entries.
$archive = Join-Path $work 'launcher.zip'
$stream = [IO.File]::Create($archive)
$zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem $payload -Recurse -File -FollowSymlink | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($payload, $file.FullName).Replace('\', '/')
        if ($relative -match '(?i)(^|/)(settings\.json|active\.json|content|versions|generated|client|symbols-cache|.*gameforge-identity\.json)(/|$)' -or
            $relative -match '(?i)\.(nos|idb|i64|gpr)$') {
            throw "Unexpected user or client content in publish output: $relative"
        }
        $entry = $zip.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
        if (!$IsWindows) { $entry.ExternalAttributes = ([int][IO.File]::GetUnixFileMode($file.FullName) -shl 16) }
        $inputStream = [IO.File]::OpenRead($file.FullName)
        $outputStream = $entry.Open()
        try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
    }
} finally { $zip.Dispose(); $stream.Dispose() }

$bootstrap = Join-Path $work 'bootstrap'
& dotnet publish (Join-Path $PSScriptRoot 'ClientLab.Bootstrap/ClientLab.Bootstrap.csproj') `
    -c Release -r $Runtime -o $bootstrap "-p:LauncherPayload=$archive" "-p:Version=$Version" -p:NuGetAudit=false -m:1
if ($LASTEXITCODE) { throw 'Publishing the single executable failed.' }
$extension = if ($IsWindows) { '.exe' } else { '' }
$artifactName = if ($ExecutableName) { $ExecutableName } else { "ClientLab-$Version-$Runtime$extension" }
$artifact = Join-Path $destination $artifactName
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item (Join-Path $bootstrap $launcher) $artifact
if (!$IsWindows) {
    [IO.File]::SetUnixFileMode($artifact, [IO.UnixFileMode]::UserRead -bor [IO.UnixFileMode]::UserWrite -bor [IO.UnixFileMode]::UserExecute -bor
        [IO.UnixFileMode]::GroupRead -bor [IO.UnixFileMode]::GroupExecute -bor [IO.UnixFileMode]::OtherRead -bor [IO.UnixFileMode]::OtherExecute)
}
if (!$SkipChecksum) {
    $hash = (Get-FileHash $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($artifact))" | Set-Content "$artifact.sha256" -Encoding ascii
}
Write-Output "Release executable: $artifact"
