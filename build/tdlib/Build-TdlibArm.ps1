[CmdletBinding()]
param(
    [string]$WorkRoot = (Join-Path $env:LOCALAPPDATA "UnigramTdlibExperiment"),
    [string]$VisualStudioPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
    [ValidateSet("Dependencies", "Generate", "Configure", "Build", "All")]
    [string]$Stage = "All"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$tdlibCommit = "022d60202e446ad1287b9fb68e687c8a0760788b"
$tdlibVersion = "1.8.66"
$vcpkgCommit = "45f9f39362a4c52e2b1fbe57b7e649db7f3d96d4"
$manifestRoot = $PSScriptRoot
$tripletRoot = Join-Path $PSScriptRoot "triplets"
$overlayRoot = Join-Path $PSScriptRoot "overlays"
$patchRoot = Join-Path $PSScriptRoot "patches"
$tdlibRoot = Join-Path $WorkRoot "tdlib"
$vcpkgRoot = Join-Path $WorkRoot "vcpkg"
$installedRoot = Join-Path $WorkRoot "vcpkg_installed"
$hostInstalledRoot = Join-Path $WorkRoot "vcpkg_host_installed"
$nativeBuild = Join-Path $WorkRoot "build-native"
$uwpBuild = Join-Path $WorkRoot "build-uwp-arm"

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE."
    }
}

function Initialize-PinnedCheckout {
    param(
        [Parameter(Mandatory)]
        [string]$Path,
        [Parameter(Mandatory)]
        [string]$Remote,
        [Parameter(Mandatory)]
        [string]$Commit
    )

    if (-not (Test-Path (Join-Path $Path ".git"))) {
        New-Item -ItemType Directory -Force -Path $Path | Out-Null
        Invoke-Checked -FilePath git -Arguments @("-C", $Path, "init")
        Invoke-Checked -FilePath git -Arguments @("-C", $Path, "remote", "add", "origin", $Remote)
    }

    $head = & git -C $Path rev-parse HEAD 2>$null
    if ($LASTEXITCODE -ne 0 -or $head -ne $Commit) {
        Invoke-Checked -FilePath git -Arguments @("-C", $Path, "fetch", "--depth", "1", "origin", $Commit)
        Invoke-Checked -FilePath git -Arguments @("-C", $Path, "checkout", "--detach", "FETCH_HEAD")
    }

    $head = (& git -C $Path rev-parse HEAD).Trim()
    if ($head -ne $Commit) {
        throw "Pinned checkout mismatch at $Path. Expected $Commit, found $head."
    }
}

function Get-CMake {
    $cmake = (& (Join-Path $vcpkgRoot "vcpkg.exe") fetch cmake).Trim().Split([Environment]::NewLine)[-1]
    if (-not (Test-Path $cmake)) {
        throw "vcpkg did not return a usable CMake path."
    }
    return $cmake
}

function Apply-PinnedPatches {
    $stampPath = Join-Path $WorkRoot "tdlib-patches-applied.txt"
    $applied = @()
    if (Test-Path -LiteralPath $stampPath -PathType Leaf) {
        $applied = @(Get-Content -LiteralPath $stampPath)
    }

    foreach ($patch in Get-ChildItem $patchRoot -Filter "*.patch" | Sort-Object Name) {
        if ($applied -contains $patch.Name) {
            continue
        }

        & git -C $tdlibRoot apply --check $patch.FullName 2>$null
        if ($LASTEXITCODE -eq 0) {
            Invoke-Checked -FilePath git -Arguments @("-C", $tdlibRoot, "apply", $patch.FullName)
        } else {
            & git -C $tdlibRoot apply --reverse --check $patch.FullName 2>$null
            if ($LASTEXITCODE -ne 0) {
                throw "Pinned patch cannot be applied cleanly: $($patch.Name). Delete the external work root and rebuild from the pinned checkout."
            }
        }

        Add-Content -LiteralPath $stampPath -Value $patch.Name -Encoding ASCII
    }
}

function Import-VcVars {
    param(
        [Parameter(Mandatory)]
        [ValidateSet("x64", "x64_arm")]
        [string]$Architecture
    )

    $vcvars = Join-Path $VisualStudioPath "VC\Auxiliary\Build\vcvarsall.bat"
    $command = "call `"$vcvars`" $Architecture 10.0.18362.0 -vcvars_ver=14.16 >nul && set"
    $environment = & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall failed for $Architecture with code $LASTEXITCODE."
    }

    foreach ($line in $environment) {
        $separator = $line.IndexOf("=")
        if ($separator -gt 0) {
            $name = $line.Substring(0, $separator)
            $value = $line.Substring($separator + 1)
            Set-Item -Path "env:$name" -Value $value
        }
    }
}

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
if (-not (Test-Path (Join-Path $VisualStudioPath "VC\Auxiliary\Build\vcvarsall.bat"))) {
    throw "Visual Studio C++ tools were not found at $VisualStudioPath."
}
$visualStudioInstaller = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
if (Test-Path (Join-Path $visualStudioInstaller "vswhere.exe")) {
    $env:PATH = "$visualStudioInstaller;$env:PATH"
}

Initialize-PinnedCheckout $tdlibRoot "https://github.com/tdlib/td.git" $tdlibCommit
Initialize-PinnedCheckout $vcpkgRoot "https://github.com/microsoft/vcpkg.git" $vcpkgCommit
Apply-PinnedPatches

$vcpkg = Join-Path $vcpkgRoot "vcpkg.exe"
if (-not (Test-Path $vcpkg)) {
    Invoke-Checked -FilePath (Join-Path $vcpkgRoot "bootstrap-vcpkg.bat") -Arguments @("-disableMetrics")
}

if ($Stage -in @("Dependencies", "All")) {
    Import-VcVars "x64"
    Invoke-Checked -FilePath $vcpkg -Arguments @(
        "install",
        "gperf:x64-windows",
        "--overlay-triplets", $tripletRoot,
        "--x-install-root", $hostInstalledRoot
    )

    Import-VcVars "x64_arm"
    Invoke-Checked -FilePath $vcpkg -Arguments @(
        "install",
        "--triplet", "arm-uwp-dynamic-v141",
        "--host-triplet", "x64-windows",
        "--overlay-triplets", $tripletRoot,
        "--overlay-ports", $overlayRoot,
        "--x-manifest-root", $manifestRoot,
        "--x-install-root", $installedRoot
    )
}

$cmake = Get-CMake

if ($Stage -in @("Generate", "All")) {
    Import-VcVars "x64"
    $env:PATH = "$(Join-Path $hostInstalledRoot 'x64-windows\tools\gperf');$env:PATH"
    New-Item -ItemType Directory -Force -Path $nativeBuild | Out-Null
    Invoke-Checked -FilePath $cmake -Arguments @(
        "-S", $tdlibRoot,
        "-B", $nativeBuild,
        "-G", "Visual Studio 17 2022",
        "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioPath",
        "-A", "x64",
        "-T", "v141",
        "-DTD_GENERATE_SOURCE_FILES=ON",
        "-DTD_ENABLE_DOTNET=CX"
    )
    Invoke-Checked -FilePath $cmake -Arguments @(
        "--build", $nativeBuild,
        "--config", "Release",
        "--target", "prepare_cross_compiling",
        "--", "/m"
    )
}

if ($Stage -in @("Configure", "All")) {
    Import-VcVars "x64_arm"
    New-Item -ItemType Directory -Force -Path $uwpBuild | Out-Null
    Invoke-Checked -FilePath $cmake -Arguments @(
        "-S", $tdlibRoot,
        "-B", $uwpBuild,
        "-G", "Visual Studio 17 2022",
        "-DCMAKE_GENERATOR_INSTANCE=$VisualStudioPath",
        "-A", "ARM",
        "-T", "v141",
        "-DCMAKE_SYSTEM_NAME=WindowsStore",
        "-DCMAKE_SYSTEM_VERSION=10.0.18362.0",
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake')",
        "-DVCPKG_TARGET_TRIPLET=arm-uwp-dynamic-v141",
        "-DVCPKG_INSTALLED_DIR=$installedRoot",
        "-DVCPKG_OVERLAY_TRIPLETS=$tripletRoot",
        "-DVCPKG_OVERLAY_PORTS=$overlayRoot",
        "-DTD_ENABLE_DOTNET=CX",
        "-DTD_ENABLE_LTO=OFF",
        "-DTD_ENABLE_MULTI_PROCESSOR_COMPILATION=ON"
    )
}

if ($Stage -in @("Build", "All")) {
    Import-VcVars "x64_arm"
    Invoke-Checked -FilePath $cmake -Arguments @(
        "--build", $uwpBuild,
        "--config", "RelWithDebInfo",
        "--target", "tddotnet",
        "--", "/m"
    )

    $outputRoot = Join-Path $uwpBuild "RelWithDebInfo"
    $manifest = @(
        "TDLIB_VERSION=$tdlibVersion",
        "TDLIB_COMMIT=$tdlibCommit",
        "VCPKG_COMMIT=$vcpkgCommit",
        "OPENSSL_VERSION=3.5.7",
        "ZLIB_VERSION=1.3.2",
        "TELEGRAM_TD_DLL_SHA256=$((Get-FileHash (Join-Path $outputRoot 'Telegram.Td.dll') -Algorithm SHA256).Hash)",
        "TELEGRAM_TD_WINMD_SHA256=$((Get-FileHash (Join-Path $outputRoot 'Telegram.Td.winmd') -Algorithm SHA256).Hash)"
    )
    Set-Content -LiteralPath (Join-Path $outputRoot "TdlibBuildManifest.txt") -Value $manifest -Encoding ASCII
}

Write-Output "TDLib version: $tdlibVersion"
Write-Output "TDLib commit: $tdlibCommit"
Write-Output "vcpkg commit: $vcpkgCommit"
Write-Output "ARM UWP output: $(Join-Path $uwpBuild 'RelWithDebInfo')"
