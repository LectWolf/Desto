[CmdletBinding()]
param(
    [string]$Version,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [switch]$RequireClean,
    [switch]$Development,
    [int]$BuildNumber = -1
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $repositoryRoot 'build'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot 'dist'
}
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$releaseRoot = Join-Path $buildRoot 'apps\Release'

$cmakeText = Get-Content -Raw (Join-Path $repositoryRoot 'CMakeLists.txt')
$cmakeVersionMatch = [regex]::Match(
    $cmakeText, 'project\(Desto VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $cmakeVersionMatch.Success) {
    throw 'Unable to read the Desto version from CMakeLists.txt.'
}
$projectVersion = $cmakeVersionMatch.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($Version)) { $Version = $projectVersion }
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version must use major.minor.patch: $Version"
}
if ($Version -ne $projectVersion) {
    throw "Release version $Version does not match project version $projectVersion."
}

$commit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') {
    throw 'Unable to resolve the release commit.'
}
$dirty = -not [string]::IsNullOrWhiteSpace(
    ((& git -C $repositoryRoot status --porcelain) -join "`n"))
if ($RequireClean -and $dirty) {
    throw 'Official release assets must be built from a clean working tree.'
}

$buildNumberPath = Join-Path $repositoryRoot '.desto-build-number'
$buildNumber = if (Test-Path -LiteralPath $buildNumberPath) {
    [int](Get-Content -Raw -LiteralPath $buildNumberPath).Trim()
} else { 0 }
$effectiveBuildNumber = if ($BuildNumber -ge 0) { $BuildNumber } else { $buildNumber }
$fullVersion = if ($Development) { "$Version.$effectiveBuildNumber" } else { $Version }

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
& (Join-Path $repositoryRoot 'installer\BuildInstaller.ps1') `
    -Version $Version -BuildDirectory $buildRoot -OutputDirectory $outputRoot `
    -BuildNumber $effectiveBuildNumber -Development:$Development
$installerPath = Join-Path $outputRoot "Desto-$fullVersion-win-x64-setup.exe"

$thirdPartyNoticeSource = Join-Path $repositoryRoot 'THIRD-PARTY-NOTICES.md'
$thirdPartyNoticePath = Join-Path $outputRoot 'THIRD-PARTY-NOTICES.md'
Copy-Item -LiteralPath $thirdPartyNoticeSource -Destination $thirdPartyNoticePath -Force

$builtAt = (& git -C $repositoryRoot show -s --format=%cI $commit).Trim()
$cmakeVersion = ((& cmake --version)[0] -replace '^cmake version\s+', '').Trim()
$executableVersion = (Get-Item (Join-Path $releaseRoot 'Desto.exe')).VersionInfo.ProductVersion
$buildInfo = [ordered]@{
    schemaVersion = 1
    product = 'Desto'
    version = $fullVersion
    baseVersion = $Version
    buildNumber = $effectiveBuildNumber
    executableVersion = $executableVersion
    commit = $commit
    workingTreeDirty = $dirty
    sourceCommitTime = $builtAt
    architecture = 'x64'
    minimumWindowsBuild = 17763
    cmakeVersion = $cmakeVersion
}
$buildInfoPath = Join-Path $outputRoot 'BUILDINFO.json'
$buildInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $buildInfoPath -Encoding utf8

$assetPaths = @($installerPath)
$assets = foreach ($path in $assetPaths) {
    $item = Get-Item -LiteralPath $path
    [ordered]@{
        name = $item.Name
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        size = $item.Length
    }
}
$manifest = [ordered]@{
    schemaVersion = 1
    product = 'Desto'
    version = $fullVersion
    baseVersion = $Version
    buildNumber = $effectiveBuildNumber
    commit = $commit
    minimumWindowsBuild = 17763
    assets = $assets
}
$manifestPath = Join-Path $outputRoot 'release-manifest.json'
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding utf8

$checksumTargets = @($installerPath, $buildInfoPath,
    $manifestPath, $thirdPartyNoticePath)
$checksumLines = foreach ($path in $checksumTargets) {
    $item = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $($item.Name)"
}
$checksumPath = Join-Path $outputRoot 'SHA256SUMS.txt'
$checksumLines | Set-Content -LiteralPath $checksumPath -Encoding ascii

Get-Item -LiteralPath @(
    $installerPath,
    $manifestPath,
    $buildInfoPath,
    $checksumPath,
    $thirdPartyNoticePath
)
