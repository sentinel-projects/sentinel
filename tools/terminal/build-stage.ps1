# Build and stage isolated development artifacts; never install or publish.
param(
    [Parameter(Mandatory=$true, Position=0)][string]$Target,
    [Parameter(Mandatory=$true, Position=1)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$allowed = @('x86_64-unknown-linux-gnu','aarch64-unknown-linux-gnu','x86_64-apple-darwin','aarch64-apple-darwin','x86_64-pc-windows-gnu','x86_64-pc-windows-msvc','aarch64-pc-windows-msvc')
if ($Target -cnotin $allowed) { throw 'Unsupported target; see the development staging matrix.' }
if ($Destination -notmatch '^[A-Za-z]:[\\/]') { throw 'Destination must be an absolute local drive path.' }
if (Test-Path -LiteralPath $Destination) { throw 'Output already exists; use a new directory.' }
$Repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$versionLine = Get-Content -LiteralPath (Join-Path $Repo 'CMakeLists.txt') | Where-Object { $_ -cmatch '^set\(SENTINEL_APP_VERSION "([^"]+)"' } | Select-Object -First 1
$versionMatch = [regex]::Match([string]$versionLine, '^set\(SENTINEL_APP_VERSION "([A-Za-z0-9.+-]+)"')
if (!$versionMatch.Success) { throw 'Invalid canonical application version.' }
$Version = $versionMatch.Groups[1].Value
$Suffix = if ($Target -match '-windows-') { '.exe' } else { '' }
$OldTarget = $env:CARGO_TARGET_DIR
$OldVersion = $env:SENTINEL_APP_VERSION
try {
    if (!$env:CARGO_TARGET_DIR) { $env:CARGO_TARGET_DIR = Join-Path $Repo 'cli/target' }
    if (![System.IO.Path]::IsPathRooted($env:CARGO_TARGET_DIR)) { $env:CARGO_TARGET_DIR = Join-Path $Repo $env:CARGO_TARGET_DIR }
    $env:SENTINEL_APP_VERSION = $Version
    $LockBefore = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Repo 'cli/Cargo.lock')).Hash.ToLowerInvariant()
    Push-Location -LiteralPath $Repo
    try {
        & cargo build --manifest-path (Join-Path $Repo 'cli/Cargo.toml') --release --locked --target $Target -p sentinel-cli -p sentinel-daemon-dev
        if ($LASTEXITCODE -ne 0) { throw "Cargo build failed with exit code $LASTEXITCODE." }
    } finally { Pop-Location }
    $Source = Join-Path $env:CARGO_TARGET_DIR "$Target/release"
    $CliName = "sentinel$Suffix"
    $DaemonName = "sentinel-daemon-dev$Suffix"
    foreach ($Name in @($CliName, $DaemonName)) {
        if (!(Test-Path -LiteralPath (Join-Path $Source $Name) -PathType Leaf)) { throw "Missing build artifact: $Name" }
    }
    New-Item -ItemType Directory -Path $Destination -ErrorAction Stop | Out-Null
    foreach ($Directory in @('bin','share/sentinel/core-warden','share/licenses/sentinel')) {
        New-Item -ItemType Directory -Path (Join-Path $Destination $Directory) -Force | Out-Null
    }
    Copy-Item -LiteralPath (Join-Path $Source $CliName) -Destination (Join-Path $Destination "bin/$CliName")
    Copy-Item -LiteralPath (Join-Path $Source $DaemonName) -Destination (Join-Path $Destination "bin/$DaemonName")
    Copy-Item -LiteralPath (Join-Path $Repo 'assets/core-warden/state-contract.json') -Destination (Join-Path $Destination 'share/sentinel/core-warden/')
    Copy-Item -LiteralPath (Join-Path $Repo 'assets/core-warden/animations') -Destination (Join-Path $Destination 'share/sentinel/core-warden/') -Recurse
    Copy-Item -LiteralPath (Join-Path $Repo 'LICENSE') -Destination (Join-Path $Destination 'share/licenses/sentinel/')
    Copy-Item -LiteralPath (Join-Path $Repo 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $Destination 'share/licenses/sentinel/')
    $Utf8 = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $Destination 'README.txt'), 'Development evaluation only. No signing, installer, production daemon, Qt profile migration, model or native platform certification is included. Asset integrity is not covered by manifest.json.', $Utf8)
    $LockAfter = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Repo 'cli/Cargo.lock')).Hash.ToLowerInvariant()
    if ($LockBefore -cne $LockAfter) { throw 'Cargo.lock changed during staging; discard the incomplete output.' }
    $Manifest = [ordered]@{
        schema_version = 1
        app_version = $Version
        target = $Target
        stage = 'development-evaluation'
        artifacts = @(
            [ordered]@{ path = "bin/$CliName"; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Destination "bin/$CliName")).Hash.ToLowerInvariant() },
            [ordered]@{ path = "bin/$DaemonName"; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Destination "bin/$DaemonName")).Hash.ToLowerInvariant() }
        )
        lockfile_sha256 = $LockAfter
        assets_integrity = 'not-verified'
    }
    [System.IO.File]::WriteAllText((Join-Path $Destination 'manifest.json'), ($Manifest | ConvertTo-Json -Depth 5), $Utf8)
    Write-Output $Destination
} finally {
    $env:CARGO_TARGET_DIR = $OldTarget
    $env:SENTINEL_APP_VERSION = $OldVersion
}
