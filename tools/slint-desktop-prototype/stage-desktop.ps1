# Build an unsigned native Windows Rust desktop candidate.
param(
    [Parameter(Mandatory=$true, Position=0)][string]$Target,
    [Parameter(Mandatory=$true, Position=1)][string]$Destination
)
$ErrorActionPreference = 'Stop'
if ($Target -cnotin @('x86_64-pc-windows-msvc', 'aarch64-pc-windows-msvc')) {
    throw 'Unsupported Windows desktop stage target.'
}
if ($Destination -notmatch '^[A-Za-z]:[\\/]') { throw 'Destination must be an absolute local drive path.' }
$Destination = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists.' }

$parent = Split-Path -Parent $Destination
if (!(Test-Path -LiteralPath $parent -PathType Container)) { throw 'Destination parent must exist.' }
$hostInfo = & rustc -vV
if ($LASTEXITCODE -ne 0 -or $hostInfo -cnotcontains "host: $Target") {
    throw 'This helper stages native Rust host builds only.'
}
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$cliLock = Join-Path $repo 'cli/Cargo.lock'
$desktopLock = Join-Path $PSScriptRoot 'Cargo.lock'
function Get-Digest([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

$cliDigest = Get-Digest $cliLock
$desktopDigest = Get-Digest $desktopLock
$versionLines = @(Get-Content -LiteralPath (Join-Path $repo 'CMakeLists.txt') |
    Where-Object { $_ -cmatch '^set\(SENTINEL_APP_VERSION "[A-Za-z0-9.+-]+"' })
if ($versionLines.Count -ne 1) { throw 'Cannot determine application version.' }
$version = [regex]::Match($versionLines[0], '"([A-Za-z0-9.+-]+)"').Groups[1].Value
$oldTarget = $env:CARGO_TARGET_DIR
$oldVersion = $env:SENTINEL_APP_VERSION
$stage = Join-Path $parent ('.sentinel-desktop-stage-' + [guid]::NewGuid().ToString('N'))
$published = $false
try {
    $env:CARGO_TARGET_DIR = if ($oldTarget) { [IO.Path]::GetFullPath($oldTarget) } else { Join-Path $repo 'cli/target' }
    $env:SENTINEL_APP_VERSION = $version
    & cargo build --manifest-path (Join-Path $PSScriptRoot 'Cargo.toml') --release --locked --target $Target
    if ($LASTEXITCODE -ne 0) { throw "Desktop build failed with exit code $LASTEXITCODE." }

    New-Item -ItemType Directory -Path $stage | Out-Null
    $terminal = Join-Path $stage 'terminal'
    & (Join-Path $repo 'tools/terminal/build-stage.ps1') $Target $terminal | Out-Null
    $bin = Join-Path $stage 'bin'
    New-Item -ItemType Directory -Path $bin | Out-Null
    $desktop = Join-Path $bin 'sentinel-slint-prototype.exe'
    Copy-Item -LiteralPath (Join-Path $env:CARGO_TARGET_DIR "$Target/release/sentinel-slint-prototype.exe") -Destination $desktop
    if ((Get-Digest $cliLock) -cne $cliDigest -or (Get-Digest $desktopLock) -cne $desktopDigest) {
        throw 'A lockfile changed during staging.'
    }
    $manifest = [ordered]@{
        schema_version = 2
        stage = 'development-desktop-evaluation'
        app_version = $version
        target = $Target
        artifacts = @([ordered]@{ path = 'bin/sentinel-slint-prototype.exe'; sha256 = Get-Digest $desktop })
        cli_lockfile_sha256 = $cliDigest
        desktop_lockfile_sha256 = $desktopDigest
        terminal_stage = 'terminal'
        terminal_manifest_sha256 = Get-Digest (Join-Path $terminal 'manifest.json')
        assets_integrity = 'not-verified'
    }
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText((Join-Path $stage 'desktop-manifest.json'), ($manifest | ConvertTo-Json -Depth 5), $utf8)
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), 'Unsigned Windows evaluation stage. Start the daemon separately with an isolated development profile, then pass its named pipe to Slint using --socket.', $utf8)
    & python (Join-Path $PSScriptRoot 'verify-desktop-stage.py') $stage --expected-version $version --cli-lockfile $cliLock --desktop-lockfile $desktopLock --runtime-assets-source (Join-Path $repo 'assets/core-warden')
    if ($LASTEXITCODE -ne 0) { throw 'Windows desktop stage verification failed.' }
    [IO.Directory]::Move($stage, $Destination)
    $published = $true
    Write-Output $Destination
} finally {
    $env:CARGO_TARGET_DIR = $oldTarget
    $env:SENTINEL_APP_VERSION = $oldVersion
    if (!$published -and (Test-Path -LiteralPath $stage)) {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
