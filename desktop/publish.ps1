[CmdletBinding()]
param(
    [ValidateSet('win-x64', 'linux-x64', 'osx-x64', 'osx-arm64')]
    [string]$Runtime,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../dist'),
    [string]$Version = '0.1.0',
    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$ExecutableName,
    [switch]$SkipChecksum,
    # macOS only: the codesign identity for ClientLab.app. "-" is an ad-hoc signature (unsigned distribution);
    # a "Developer ID Application: ..." identity from the keychain signs it for notarization.
    [string]$MacSigningIdentity = '-',
    # Where the release that carries this build's browser archive serves its assets.
    [string]$BrowserBaseUrl
)
$ErrorActionPreference = 'Stop'
if (!$Runtime) { throw 'Specify -Runtime win-x64, linux-x64, osx-x64 or osx-arm64.' }
if (($Runtime.StartsWith('win-') -and !$IsWindows) -or
    ($Runtime.StartsWith('linux-') -and !$IsLinux) -or
    ($Runtime.StartsWith('osx-') -and !$IsMacOS)) {
    throw 'Publish on the target operating system so CefGlue includes the correct native browser files.'
}
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][a-zA-Z0-9.-]+)?$') { throw 'Use a semantic version.' }
if (!$BrowserBaseUrl) { $BrowserBaseUrl = "https://github.com/NosCoreIO/NosCore.ClientLab/releases/download/v$Version/" }
if ($BrowserBaseUrl -notmatch '^https://') { throw 'The browser archive must be served over HTTPS.' }
$destination = [IO.Path]::GetFullPath($OutputDirectory)
$work = Join-Path $PSScriptRoot ('obj/publish/work-' + $Runtime + '-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $work 'payload'
$architecture = if ($Runtime.EndsWith('arm64')) { 'arm64' } else { 'x64' }
$browserList = Join-Path $work 'browser-files.txt'
New-Item -ItemType Directory -Path $payload -Force | Out-Null

# The publish input is the launcher project only, never the repository, a client install or user data.
$project = Join-Path $PSScriptRoot 'ClientLab.Launcher/ClientLab.Launcher.csproj'
$publishArguments = @('-c', 'Release', '-r', $Runtime, '--self-contained', 'true', '-o', $payload,
    "-p:CefArchitecture=$architecture", "-p:Version=$Version", '-p:DebugType=None', '-p:NuGetAudit=false', "-p:BrowserFileList=$browserList", '-m:1')
& dotnet publish $project @publishArguments
if ($LASTEXITCODE) {
    # CefGlue's macOS packaging occasionally misses one Chromium library in the build output (a different one each
    # time, MSB3030). One clean retry; a failure that repeats is real.
    Write-Warning 'Publishing failed; retrying once from a clean build output.'
    $output = Join-Path $PSScriptRoot "ClientLab.Launcher/bin/Release/net10.0/$Runtime"
    if (Test-Path $output) { Remove-Item $output -Recurse -Force }
    Remove-Item $payload -Recurse -Force; New-Item -ItemType Directory -Path $payload -Force | Out-Null
    & dotnet publish $project @publishArguments
    if ($LASTEXITCODE) { throw 'Publishing the desktop launcher failed.' }
}
$launcher = if ($IsWindows) { 'ClientLab.exe' } else { 'ClientLab' }
if (!(Test-Path (Join-Path $payload $launcher))) { throw 'The published launcher is missing.' }
Copy-Item (Join-Path $PSScriptRoot '../LICENSE') (Join-Path $payload 'LICENSE.txt')
Copy-Item (Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.md') $payload

# Preserve Unix executable bits, and dereference framework symlinks into regular ZIP entries.
function Write-Archive([string]$Folder, [string]$Archive) {
    $stream = [IO.File]::Create($Archive)
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem $Folder -Recurse -File -FollowSymlink | Sort-Object FullName) {
            $relative = [IO.Path]::GetRelativePath($Folder, $file.FullName).Replace('\', '/')
            if ($relative -match '(?i)(^|/)(settings\.json|active\.json|content|versions|generated|client|symbols-cache|.*gameforge-identity\.json)(/|$)' -or
                $relative -match '(?i)\.(nos|idb|i64|gpr)$') {
                throw "Unexpected user or client content in publish output: $relative"
            }
            $entry = $zip.CreateEntry($relative, [IO.Compression.CompressionLevel]::Optimal)
            # A fixed time keeps the archive byte-identical between releases with the same Chromium, so its checksum
            # (which launchers cache it by) does not change and players do not download it again.
            $entry.LastWriteTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
            if (!$IsWindows) { $entry.ExternalAttributes = ([int][IO.File]::GetUnixFileMode($file.FullName) -shl 16) }
            $inputStream = [IO.File]::OpenRead($file.FullName)
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    } finally { $zip.Dispose(); $stream.Dispose() }
}

# Chromium (about two thirds of the launcher) is its own release asset: the launcher downloads it once, on first start,
# and keeps it across launcher updates (Services/BrowserRuntime.cs). The launcher project lists the files it published
# from the CEF packages; they move, at the same relative paths, to the browser archive.
$browser = Join-Path $work 'browser'
foreach ($line in Get-Content $browserList) {
    $relative = $line.Trim()
    if (!$relative) { continue }
    $source = Join-Path $payload $relative
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $target = Join-Path $browser $relative
    New-Item -ItemType Directory -Path (Split-Path $target) -Force | Out-Null
    Move-Item -LiteralPath $source -Destination $target
}
Get-ChildItem $payload -Recurse -Directory | Sort-Object { $_.FullName.Length } -Descending |
    Where-Object { !(Get-ChildItem $_.FullName -Force) } | Remove-Item
$helper = if ($IsWindows) { 'Xilium.CefGlue.BrowserProcess.exe' } else { 'Xilium.CefGlue.BrowserProcess' }
if (!(Test-Path $browser) -or !(Get-ChildItem $browser -Recurse -File -Filter $helper)) { throw 'The embedded browser subprocess is missing.' }
$leftover = Get-ChildItem $payload -Recurse -File | Where-Object { $_.Name -match '^(libcef\.(dll|so|dylib)|Chromium Embedded Framework|icudtl\.dat|Xilium\.CefGlue\.BrowserProcess(\.exe)?)$' }
if ($leftover) { throw "Chromium files were left in the launcher: $($leftover.Name -join ', ')" }
$browserName = "ClientLab-browser-$Version-$Runtime.zip"
$browserArchive = Join-Path $destination $browserName
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Write-Archive $browser $browserArchive
$browserHash = (Get-FileHash $browserArchive -Algorithm SHA256).Hash.ToLowerInvariant()
if (!$SkipChecksum) { "$browserHash  $browserName" | Set-Content "$browserArchive.sha256" -Encoding ascii }
[ordered]@{ name = $browserName; url = $BrowserBaseUrl.TrimEnd('/') + '/' + $browserName; sha256 = $browserHash; size = (Get-Item $browserArchive).Length } |
    ConvertTo-Json | Set-Content (Join-Path $payload 'browser.json') -Encoding utf8
Write-Output "Browser archive: $browserArchive"

# The launcher itself travels inside the bootstrap as a Brotli-compressed tar: about a third smaller than a ZIP of the
# same files, and the bootstrap unpacks it in under a second. Symbolic links are stored as the files they point to.
$archive = Join-Path $work 'launcher.tar.br'
$stream = [IO.File]::Create($archive)
$brotli = [IO.Compression.BrotliStream]::new($stream, [IO.Compression.CompressionLevel]::SmallestSize)
$tar = [System.Formats.Tar.TarWriter]::new($brotli, [System.Formats.Tar.TarEntryFormat]::Pax, $false)
try {
    foreach ($file in Get-ChildItem $payload -Recurse -File -FollowSymlink | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($payload, $file.FullName).Replace('\', '/')
        if ($relative -match '(?i)(^|/)(settings\.json|active\.json|content|versions|generated|client|symbols-cache|.*gameforge-identity\.json)(/|$)' -or
            $relative -match '(?i)\.(nos|idb|i64|gpr)$') {
            throw "Unexpected user or client content in publish output: $relative"
        }
        $entry = [System.Formats.Tar.PaxTarEntry]::new([System.Formats.Tar.TarEntryType]::RegularFile, $relative)
        $entry.Mode = if ($IsWindows) { [IO.UnixFileMode]'UserRead, UserWrite, GroupRead, OtherRead' } else { [IO.File]::GetUnixFileMode($file.FullName) }
        $entry.ModificationTime = [DateTimeOffset]::new(2000, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
        $entry.DataStream = [IO.File]::OpenRead($file.FullName)
        try { $tar.WriteEntry($entry) } finally { $entry.DataStream.Dispose() }
    }
} finally { $tar.Dispose(); $brotli.Dispose(); $stream.Dispose() }

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

# macOS players get ClientLab.app in a disk image. The bare executable above stays a release asset: the launcher's
# self-updater downloads it and replaces Contents/MacOS/ClientLab inside the installed app.
if ($IsMacOS) {
    $appName = 'ClientLab.app'
    $staging = Join-Path $work 'dmg'
    $app = Join-Path $staging $appName
    $contents = Join-Path $app 'Contents'
    New-Item -ItemType Directory -Path (Join-Path $contents 'MacOS'), (Join-Path $contents 'Resources') -Force | Out-Null
    Copy-Item $artifact (Join-Path $contents 'MacOS/ClientLab')
    & chmod 755 (Join-Path $contents 'MacOS/ClientLab')
    $iconEntry = ''
    $png = Join-Path $PSScriptRoot 'ClientLab.Launcher/Assets/noscore.png'
    $iconset = Join-Path $work 'ClientLab.iconset'
    try {
        New-Item -ItemType Directory -Path $iconset -Force | Out-Null
        foreach ($size in 16, 32, 128, 256, 512) {
            & sips -z $size $size $png --out (Join-Path $iconset "icon_${size}x${size}.png") | Out-Null
            & sips -z ($size * 2) ($size * 2) $png --out (Join-Path $iconset "icon_${size}x${size}@2x.png") | Out-Null
        }
        & iconutil -c icns $iconset -o (Join-Path $contents 'Resources/ClientLab.icns')
        if (!$LASTEXITCODE) { $iconEntry = "  <key>CFBundleIconFile</key><string>ClientLab</string>`n" }
    } catch { Write-Warning "No app icon: $_" }
    $shortVersion = ($Version -split '[-+]')[0]
    @"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>io.noscore.clientlab</string>
  <key>CFBundleName</key><string>ClientLab</string>
  <key>CFBundleDisplayName</key><string>ClientLab</string>
  <key>CFBundleExecutable</key><string>ClientLab</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$shortVersion</string>
  <key>CFBundleVersion</key><string>$Version</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
$iconEntry</dict>
</plist>
"@ | Set-Content (Join-Path $contents 'Info.plist') -Encoding utf8
    $signArguments = @('--force', '--deep', '-s', $MacSigningIdentity)
    if ($MacSigningIdentity -ne '-') { $signArguments += @('--options', 'runtime', '--timestamp') }
    & codesign @signArguments $app
    if ($LASTEXITCODE) { throw 'Signing ClientLab.app failed.' }
    & ln -s /Applications (Join-Path $staging 'Applications')
    $dmg = Join-Path $destination "ClientLab-$Version-$Runtime.dmg"
    if (Test-Path $dmg) { Remove-Item $dmg -Force }
    # hdiutil intermittently reports "Resource busy" on CI runners (Spotlight or XProtect scanning the new image).
    for ($attempt = 1; ; $attempt++) {
        & hdiutil create -volname ClientLab -srcfolder $staging -ov -format UDZO $dmg
        if (!$LASTEXITCODE) { break }
        if ($attempt -ge 5) { throw 'Creating the disk image failed.' }
        Write-Warning "hdiutil failed (attempt $attempt); retrying."
        Start-Sleep -Seconds (5 * $attempt)
    }
    if (!$SkipChecksum) {
        $hash = (Get-FileHash $dmg -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $([IO.Path]::GetFileName($dmg))" | Set-Content "$dmg.sha256" -Encoding ascii
    }
    Write-Output "Disk image: $dmg"
}
