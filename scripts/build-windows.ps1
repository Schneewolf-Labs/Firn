# Build and test with VS 2022, even when PATH contains an older CMake.
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [string]$SDL2Dir,
    [string]$ToolchainFile,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 256)][int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$sourceDir = Split-Path $PSScriptRoot -Parent

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

$candidates = @()
$onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($onPath) { $candidates += $onPath.Source }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $candidates += & $vswhere -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
}
$cmake = $null
foreach ($candidate in $candidates) {
    $versionText = & $candidate --version
    if ($LASTEXITCODE -eq 0 -and ($versionText -join "`n") -match 'cmake version (\d+\.\d+\.\d+)') {
        if ([version]$Matches[1] -ge [version]'3.22.0') { $cmake = $candidate; break }
    }
}
if (-not $cmake) { throw 'CMake 3.22+ is required. Install the C++ CMake tools in Visual Studio 2022.' }
$toolDir = Split-Path $cmake -Parent
Write-Host "Using $cmake"

# Resolve relative paths from the caller's directory, including paths with spaces.
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)
$configure = @('-S', $sourceDir, '-B', $BuildDir, '-G', 'Visual Studio 17 2022', '-A', 'x64')
if ($SDL2Dir -and $ToolchainFile) { throw 'Choose either -SDL2Dir or -ToolchainFile.' }
if ($SDL2Dir) {
    $configure += "-DSDL2_DIR=$((Resolve-Path -LiteralPath $SDL2Dir).Path)"
} elseif ($ToolchainFile) {
    $configure += "-DCMAKE_TOOLCHAIN_FILE=$((Resolve-Path -LiteralPath $ToolchainFile).Path)"
} elseif (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
    # An existing build keeps its dependency selection. For a new one, prefer
    # a single unpacked SDL VC package, then an existing vcpkg installation.
    $localSDL = @(Get-ChildItem -Path "$sourceDir/build-deps/SDL2-*/cmake/SDL2Config.cmake" -ErrorAction SilentlyContinue)
    if ($localSDL.Count -eq 1) {
        $configure += "-DSDL2_DIR=$($localSDL[0].DirectoryName)"
    } elseif ($env:VCPKG_INSTALLATION_ROOT) {
        $configure += "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
    } else {
        throw 'Supply -SDL2Dir (the SDL VC package cmake folder) or -ToolchainFile (vcpkg.cmake).'
    }
}
Invoke-Checked $cmake $configure
Invoke-Checked $cmake @('--build', $BuildDir, '--config', $Configuration, '--parallel', "$Jobs")
Invoke-Checked (Join-Path $toolDir 'ctest.exe') @('--test-dir', $BuildDir, '-C', $Configuration, '--output-on-failure', '--no-tests=error')
if ($Package) {
    if ($Configuration -eq 'Debug') { throw 'Use Release or RelWithDebInfo for a redistributable package.' }
    Invoke-Checked (Join-Path $toolDir 'cpack.exe') @('--config', (Join-Path $BuildDir 'CPackConfig.cmake'), '-G', 'ZIP', '-C', $Configuration, '-B', $BuildDir)
}
