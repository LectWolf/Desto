[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ReleaseDirectory,
    [string]$ExpectedVersion
)

$ErrorActionPreference = 'Stop'
$releaseRoot = [System.IO.Path]::GetFullPath($ReleaseDirectory)
$manifestPath = Join-Path $releaseRoot 'release-manifest.json'
$checksumsPath = Join-Path $releaseRoot 'SHA256SUMS.txt'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $checksumsPath -PathType Leaf)) {
    throw 'Release manifest or checksum file is missing.'
}

$manifest = Get-Content -Raw -Encoding utf8 -LiteralPath $manifestPath | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or $manifest.product -ne 'Desto' -or
    $manifest.version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' -or
    $manifest.commit -notmatch '^[0-9a-f]{40}$' -or
    $manifest.minimumWindowsBuild -lt 17763) {
    throw 'Release manifest fields are invalid.'
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedVersion)) {
    if ($ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -and
        $ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "Expected version is invalid: $ExpectedVersion"
    }
    if ($ExpectedVersion.Split('.').Count -eq 3) {
        if (-not $manifest.version.StartsWith("$ExpectedVersion.")) {
            throw "Manifest version $($manifest.version) does not match base version $ExpectedVersion."
        }
    } elseif ($manifest.version -ne $ExpectedVersion) {
        throw "Manifest version $($manifest.version) does not match $ExpectedVersion."
    }
}

$manifestNames = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($asset in $manifest.assets) {
    if (-not $manifestNames.Add([string]$asset.name)) {
        throw "Duplicate release asset: $($asset.name)"
    }
    $path = Join-Path $releaseRoot ([string]$asset.name)
    $resolved = [System.IO.Path]::GetFullPath($path)
    $prefix = $releaseRoot.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix,
            [System.StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "Release asset is missing or escapes the release directory: $($asset.name)"
    }
    $item = Get-Item -LiteralPath $resolved
    $hash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne ([string]$asset.sha256).ToLowerInvariant() -or
        $item.Length -ne [int64]$asset.size) {
        throw "Release asset verification failed: $($asset.name)"
    }
}

$checksumLines = Get-Content -LiteralPath $checksumsPath
foreach ($line in $checksumLines) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  ([^\\/]+)$') {
        throw "Invalid checksum line: $line"
    }
    $path = Join-Path $releaseRoot $Matches[2]
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Checksum target is missing: $($Matches[2])"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($actual -ne $Matches[1]) {
        throw "Checksum mismatch: $($Matches[2])"
    }
}

$installerName = "Desto-$($manifest.version)-win-x64-setup.exe"
if (-not $manifestNames.Contains($installerName)) {
    throw 'Required installer asset is missing from the manifest.'
}

[pscustomobject]@{
    Version = $manifest.version
    Commit = $manifest.commit
    Assets = $manifest.assets.Count
    Checksums = $checksumLines.Count
    Verified = $true
}
