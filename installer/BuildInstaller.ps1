[CmdletBinding()]
param(
    [string]$Version,
    [string]$BuildDirectory,
    [string]$OutputDirectory,
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
$releaseRoot = Join-Path $buildRoot 'apps\Release'
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)

if ([string]::IsNullOrWhiteSpace($Version)) {
    $cmakeText = Get-Content -Raw (Join-Path $repositoryRoot 'CMakeLists.txt')
    $match = [regex]::Match($cmakeText, 'project\(Desto VERSION ([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $match.Success) {
        throw 'Unable to read the Desto version from CMakeLists.txt.'
    }
    $Version = $match.Groups[1].Value
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version must use major.minor.patch: $Version"
}

$buildNumberPath = Join-Path $repositoryRoot '.desto-build-number'
if ($BuildNumber -lt 0) {
    $previousBuildNumber = 0
    if (Test-Path -LiteralPath $buildNumberPath -PathType Leaf) {
        $rawBuildNumber = (Get-Content -Raw -LiteralPath $buildNumberPath).Trim()
        if ($rawBuildNumber -notmatch '^\d+$') {
            throw "Invalid build number in $buildNumberPath"
        }
        $previousBuildNumber = [int]$rawBuildNumber
    }
    $BuildNumber = $previousBuildNumber + 1
}
if ($BuildNumber -lt 0) {
    throw 'Build number must be non-negative.'
}
Set-Content -LiteralPath $buildNumberPath -Value $BuildNumber -Encoding ascii
$fullVersion = "$Version.$BuildNumber"

& cmake -S $repositoryRoot -B $buildRoot "-DDESTO_BUILD_NUMBER=$BuildNumber"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}
& cmake --build $buildRoot --config Release --parallel 4
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed with exit code $LASTEXITCODE."
}

$requiredFiles = @(
    (Join-Path $releaseRoot 'Desto.exe')
)
foreach ($requiredFile in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Missing Release artifact: $requiredFile"
    }
}

$isccCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
)
$iscc = $isccCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } |
    Select-Object -First 1
if (-not $iscc) {
    $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($command) { $iscc = $command.Source }
}
if (-not $iscc) {
    throw 'Inno Setup 6 is required. Install it with: winget install JRSoftware.InnoSetup'
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$scriptPath = Join-Path $PSScriptRoot 'Desto.iss'
& $iscc "/DMyAppVersion=$fullVersion" "/DSourceDir=$releaseRoot" "/DOutputDir=$outputRoot" $scriptPath
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup failed with exit code $LASTEXITCODE."
}

$installerPath = Join-Path $outputRoot "Desto-$fullVersion-win-x64-setup.exe"
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Installer was not created: $installerPath"
}
Get-Item -LiteralPath $installerPath
