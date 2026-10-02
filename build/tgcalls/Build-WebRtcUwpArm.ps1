[CmdletBinding()]
param(
    [string]$DependencyScriptsRoot = (Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\webrtc-uwp-deps'),
    [string]$Root = (Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\webrtc-uwp-arm'),
    [ValidateSet('Release', 'Debug')][string[]]$Configuration = @('Release', 'Debug'),
    [switch]$SkipAcquire
)

$ErrorActionPreference = 'Stop'

$expectedScriptsCommit = '6ce0019e1e5ea4e06ab3bc21651242567774a4e0'
$buildScript = Join-Path $DependencyScriptsRoot 'webrtc\build.ps1'

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "The pinned WebRTC UWP build script was not found at '$buildScript'."
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
