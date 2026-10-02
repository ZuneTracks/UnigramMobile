[CmdletBinding()]
param(
    [string]$WebRtcRoot = 'C:\wrtcar\src',
    [string]$OutputRoot = (Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\bridge-probe')
)

$ErrorActionPreference = 'Stop'
$project = Join-Path $PSScriptRoot 'bridge-probe\ModernCallsBridge.vcxproj'
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'

if (-not (Test-Path -LiteralPath (Join-Path $WebRtcRoot 'out\msvc\uwp\Release\arm\obj\webrtc.lib'))) {
    throw "Release ARM webrtc.lib was not found below '$WebRtcRoot'."
}

& $msbuild $project /nologo /m /t:Rebuild /p:Configuration=Release /p:Platform=ARM /p:WebRtcRoot=$WebRtcRoot /p:OutputDirectory=$OutputRoot
if ($LASTEXITCODE -ne 0) {
    throw "The ARM UWP C++/CX bridge proof failed with exit code $LASTEXITCODE."
}
