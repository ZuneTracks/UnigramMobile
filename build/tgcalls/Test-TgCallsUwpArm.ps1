[CmdletBinding()]
param(
    [string]$TgCallsRoot = (Join-Path $env:LOCALAPPDATA 'UnigramTdlibExperiment\tgcalls'),
    [string]$WebRtcRoot,
    [string]$VcVarsAllPath = 'C:\Program Files (x86)\Microsoft Visual Studio\2017\Professional\VC\Auxiliary\Build\vcvarsall.bat'
)

$ErrorActionPreference = 'Stop'

$expectedCommit = '1c236c09f8d8569fead14bd68000618a52051225'
$instanceSource = Join-Path $TgCallsRoot 'tgcalls\Instance.cpp'
$engineSource = Join-Path $TgCallsRoot 'tgcalls\InstanceImpl.cpp'

if (-not (Test-Path -LiteralPath $instanceSource) -or -not (Test-Path -LiteralPath $engineSource)) {
    throw "TgCalls source is incomplete: expected '$instanceSource' and '$engineSource'."
}

if (-not (Test-Path -LiteralPath $VcVarsAllPath)) {
    throw "The VS 2017 ARM environment script was not found at '$VcVarsAllPath'."
}

$actualCommit = (& git -C $TgCallsRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read the TgCalls Git revision at '$TgCallsRoot'."
}

if ($actualCommit -ne $expectedCommit) {
    throw "Unexpected TgCalls revision '$actualCommit'; expected '$expectedCommit'."
}

$includeArguments = @("/I `"$TgCallsRoot`"")
if ($WebRtcRoot) {
    if (-not (Test-Path -LiteralPath (Join-Path $WebRtcRoot 'rtc_base\logging.h'))) {
        throw "WebRTC root '$WebRtcRoot' does not contain rtc_base\logging.h."
    }

    $includeArguments += "/I `"$WebRtcRoot`""
}

$probeDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('UnigramTgCallsProbe-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $probeDirectory | Out-Null

try {
    $metadataObject = Join-Path $probeDirectory 'Instance.obj'
    $engineObject = Join-Path $probeDirectory 'InstanceImpl.obj'
    $includeCommand = $includeArguments -join ' '

    $metadataCommand = "call `"$VcVarsAllPath`" x86_arm >nul && cd /d `"$TgCallsRoot`" && cl /nologo /EHsc /std:c++17 /c $includeCommand /Fo`"$metadataObject`" tgcalls\Instance.cpp"
    & $env:ComSpec /d /c $metadataCommand
    if ($LASTEXITCODE -ne 0) {
        throw "The ARM metadata compilation probe failed."
    }

    $engineCommand = "call `"$VcVarsAllPath`" x86_arm >nul && cd /d `"$TgCallsRoot`" && cl /nologo /EHsc /std:c++17 /c $includeCommand /Fo`"$engineObject`" tgcalls\InstanceImpl.cpp"
    & $env:ComSpec /d /c $engineCommand
    if ($LASTEXITCODE -ne 0) {
        throw "The ARM private-call engine compilation probe failed. Supply a compatible WebRTC source root with -WebRtcRoot before attempting wrapper integration."
    }

    Write-Host "TgCalls ARM UWP source probes passed for $actualCommit."
}
finally {
    if (Test-Path -LiteralPath $probeDirectory) {
        Remove-Item -LiteralPath $probeDirectory -Recurse -Force
    }
}
