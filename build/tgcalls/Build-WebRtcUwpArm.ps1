[CmdletBinding()]
param(
    [string]$DependencyScriptsRoot = (Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\webrtc-uwp-deps'),
    [string]$Root = 'C:\wrtcar',
    [ValidateSet('Release', 'Debug')][string[]]$Configuration = @('Release', 'Debug'),
    [switch]$SkipAcquire
)

$ErrorActionPreference = 'Stop'

$expectedScriptsCommit = '6ce0019e1e5ea4e06ab3bc21651242567774a4e0'
$buildScript = Join-Path $DependencyScriptsRoot 'webrtc\build.ps1'
$rustPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-no-rust.patch'
$runtimePatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-skip-runtime-copy.patch'
$audioAssemblyPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-scalar-audio.patch'
$pffftPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-scalar-pffft.patch'
$denormalPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-msvc-denormal.patch'
$boringSslPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-boringssl-no-asm.patch'
$audioOnlyLibaomPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-audio-only-libaom.patch'
$audioOnlyLibvpxPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-audio-only-libvpx.patch'
$opusPatch = Join-Path $PSScriptRoot 'patches\webrtc-m123-winuwp-arm-scalar-opus.patch'

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "The pinned WebRTC UWP build script was not found at '$buildScript'."
}

if (-not (Test-Path -LiteralPath $rustPatch)) {
    throw "The ARM UWP Rust compatibility patch was not found at '$rustPatch'."
}

if (-not (Test-Path -LiteralPath $runtimePatch)) {
    throw "The ARM UWP runtime-copy compatibility patch was not found at '$runtimePatch'."
}

if (-not (Test-Path -LiteralPath $audioAssemblyPatch)) {
    throw "The ARM UWP scalar-audio compatibility patch was not found at '$audioAssemblyPatch'."
}

if (-not (Test-Path -LiteralPath $pffftPatch)) {
    throw "The ARM UWP scalar PFFFT compatibility patch was not found at '$pffftPatch'."
}

if (-not (Test-Path -LiteralPath $denormalPatch)) {
    throw "The ARM UWP MSVC denormal compatibility patch was not found at '$denormalPatch'."
}

if (-not (Test-Path -LiteralPath $boringSslPatch)) {
    throw "The ARM UWP BoringSSL no-assembly patch was not found at '$boringSslPatch'."
}

if (-not (Test-Path -LiteralPath $audioOnlyLibaomPatch)) {
    throw "The ARM UWP audio-only libaom compatibility patch was not found at '$audioOnlyLibaomPatch'."
}

if (-not (Test-Path -LiteralPath $audioOnlyLibvpxPatch)) {
    throw "The ARM UWP audio-only libvpx compatibility patch was not found at '$audioOnlyLibvpxPatch'."
}

if (-not (Test-Path -LiteralPath $opusPatch)) {
    throw "The ARM UWP scalar Opus compatibility patch was not found at '$opusPatch'."
}

$actualScriptsCommit = (& git -C $DependencyScriptsRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read the WebRTC UWP build-script revision at '$DependencyScriptsRoot'."
}

if ($actualScriptsCommit -ne $expectedScriptsCommit) {
    throw "Unexpected WebRTC UWP build-script revision '$actualScriptsCommit'; expected '$expectedScriptsCommit'."
}

$scriptText = Get-Content -LiteralPath $buildScript -Raw
$architectureValidation = "[ValidateSet('x64', 'arm64')]"
$visualStudioLookup = "`$path = & `$vswhere -version '[17.0,19.0)' -latest -products * -property installationPath"

if (-not $scriptText.Contains($architectureValidation) -or -not $scriptText.Contains($visualStudioLookup)) {
    throw 'The pinned WebRTC UWP build script no longer has the expected architecture or Visual Studio lookup.'
}

$scriptText = $scriptText.Replace(
    $architectureValidation,
    "[ValidateSet('x64', 'arm', 'arm64')]"
)
$scriptText = $scriptText.Replace(
    $visualStudioLookup,
    "`$path = & `$vswhere -version '[17.0,19.0)' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath"
)
$scriptText = $scriptText.Replace(
    'target_cpu=\`"$a\`""',
    'target_cpu=\`"$a\`" enable_rust=false arm_version=6 arm_use_neon=false enable_libaom=false rtc_build_libvpx=false rtc_libvpx_build_vp9=false"'
)
$scriptText = $scriptText.Replace(
    '$vs = Get-VisualStudio',
    "Use-Patch (Join-Path `$Src 'build') '$rustPatch'`r`nUse-Patch (Join-Path `$Src 'build') '$runtimePatch'`r`nUse-Patch `$Src '$audioAssemblyPatch'`r`nUse-Patch (Join-Path `$Src 'third_party') '$pffftPatch'`r`nUse-Patch `$Src '$denormalPatch'`r`nUse-Patch (Join-Path `$Src 'third_party') '$boringSslPatch'`r`nUse-Patch (Join-Path `$Src 'third_party') '$opusPatch'`r`nUse-Patch `$Src '$audioOnlyLibaomPatch'`r`nUse-Patch `$Src '$audioOnlyLibvpxPatch'`r`n`r`n`$vs = Get-VisualStudio"
)

if (-not $scriptText.Contains('enable_rust=false arm_version=6 arm_use_neon=false enable_libaom=false rtc_build_libvpx=false rtc_libvpx_build_vp9=false') -or -not $scriptText.Contains($rustPatch) -or -not $scriptText.Contains($runtimePatch) -or -not $scriptText.Contains($audioAssemblyPatch) -or -not $scriptText.Contains($pffftPatch) -or -not $scriptText.Contains($denormalPatch) -or -not $scriptText.Contains($boringSslPatch) -or -not $scriptText.Contains($opusPatch) -or -not $scriptText.Contains($audioOnlyLibaomPatch) -or -not $scriptText.Contains($audioOnlyLibvpxPatch)) {
    throw 'Unable to apply the ARM UWP compatibility configuration to the temporary build script.'
}

$temporaryBuildScript = Join-Path (Split-Path -Parent $buildScript) ('.copilot-arm-' + [Guid]::NewGuid().ToString('N') + '.ps1')
[System.IO.File]::WriteAllText($temporaryBuildScript, $scriptText, [System.Text.UTF8Encoding]::new($false))

try {
    $arguments = @{
        Root = $Root
        Arch = @('arm')
        Configuration = $Configuration
    }
    if ($SkipAcquire) {
        $arguments.SkipAcquire = $true
    }

    & $temporaryBuildScript @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "The ARM UWP WebRTC build failed with exit code $LASTEXITCODE."
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryBuildScript) {
        Remove-Item -LiteralPath $temporaryBuildScript -Force
    }
}
