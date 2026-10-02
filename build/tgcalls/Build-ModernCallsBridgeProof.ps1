[CmdletBinding()]
param(
    [string]$WebRtcRoot = 'C:\wrtcar\src',
    [string]$OutputRoot = (Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\bridge-probe')
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'bridge-probe\ModernCallsBridge.vcxproj'
$engineProject = Join-Path $PSScriptRoot 'native-engine\TgCallsEngine.vcxproj'
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$tgCallsRoot = Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\tgcalls'
$tgCallsPatch = Join-Path $PSScriptRoot 'patches\tgcalls-m123-winuwp-audio-device.patch'
$expectedTgCallsCommit = '1c236c09f8d8569fead14bd68000618a52051225'

if (-not (Test-Path -LiteralPath (Join-Path $WebRtcRoot 'out\msvc\uwp\Release\arm\obj\webrtc.lib'))) {
    throw "Release ARM webrtc.lib was not found below '$WebRtcRoot'."
}

if (-not (Test-Path -LiteralPath $tgCallsRoot)) {
    throw "The pinned TgCalls checkout was not found at '$tgCallsRoot'."
}

$actualTgCallsCommit = (& git -C $tgCallsRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read the TgCalls revision at '$tgCallsRoot'."
}

if ($actualTgCallsCommit -ne $expectedTgCallsCommit) {
    throw "Unexpected TgCalls revision '$actualTgCallsCommit'; expected '$expectedTgCallsCommit'."
}

Push-Location $tgCallsRoot
try {
    & git apply --reverse --check --ignore-whitespace $tgCallsPatch 2>$null
    if ($LASTEXITCODE -ne 0) {
        & git apply --3way --ignore-whitespace $tgCallsPatch
        if ($LASTEXITCODE -ne 0) {
            throw 'Unable to apply the TgCalls M123 audio-device compatibility patch.'
        }
    }
}
finally {
    Pop-Location
}

$engineOutputRoot = Join-Path $OutputRoot 'native-engine'
& $msbuild $engineProject /nologo /m /t:Rebuild /p:Configuration=Release /p:Platform=ARM /p:WebRtcRoot=$WebRtcRoot /p:TgCallsRoot=$tgCallsRoot /p:OutputDirectory=$engineOutputRoot
if ($LASTEXITCODE -ne 0) {
    throw "The ARM UWP TgCalls engine library failed with exit code $LASTEXITCODE."
}

& $msbuild $project /nologo /m /t:Rebuild /p:Configuration=Release /p:Platform=ARM /p:WebRtcRoot=$WebRtcRoot /p:TgCallsEngineOutputRoot=$engineOutputRoot /p:OutputDirectory=$OutputRoot
if ($LASTEXITCODE -ne 0) {
    throw "The ARM UWP C++/CX bridge proof failed with exit code $LASTEXITCODE."
}
