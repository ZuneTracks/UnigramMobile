# TgCalls ARM UWP proof

This directory contains the reproducible native-source proof for the
experimental ARM UWP private-call transport. It does not package a TgCalls
binary, change `CallProtocol`, or replace the proven legacy `libtgvoip`
fallback.

## Source pin

The source checkout is intentionally external to this repository:

```text
%LOCALAPPDATA%\UnigramTdlibExperiment\tgcalls
```

It is pinned to:

```text
TelegramMessenger/tgcalls@1c236c09f8d8569fead14bd68000618a52051225
```

That revision's `InstanceImpl` registers the actual current private-call
versions `2.7.7` (protocol V0) and `5.0.0` (protocol V1). Those values must
not be advertised by the experimental app until a matching native transport
has been built and packaged.

## Reproducible probe

From the app repository, run:

```powershell
.\build\tgcalls\Test-TgCallsUwpArm.ps1
```

The probe initializes the installed VS 2017 v141 ARM compiler environment and
compiles `tgcalls\Instance.cpp`, then attempts the real V0/V1
`tgcalls\InstanceImpl.cpp` unit. It verifies the exact source commit before
compiling and removes its temporary object files.

At the pinned revision, metadata compilation succeeds, while the private-call
engine stops at:

```text
fatal error C1083: Cannot open include file: 'rtc_base/logging.h'
```

This is expected: the upstream checkout contains neither WebRTC headers nor
ARM UWP WebRTC libraries, and it has no UWP CMake or Visual Studio project.
The `tgcalls\platform\uwp` sources are not a build system; their capture code
also belongs to the deferred video scope.

The installed vcpkg revision (`98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8`)
rejects `webrtc:arm-uwp` during its dry-run dependency plan because the port
supports desktop Windows, not UWP. The current `UnigramDev/Unigram`
`Telegram.Native.Calls` component confirms the required modern bridge exists,
but its dedicated UWP WebRTC port deliberately supports only x64 and ARM64.
Neither route supplies an ARM Windows Mobile library.

After supplying a source-compatible WebRTC checkout, repeat the probe with:

```powershell
.\build\tgcalls\Test-TgCallsUwpArm.ps1 -WebRtcRoot C:\path\to\webrtc\src
```

The default probe deliberately uses the app's v141 ARM compiler. The pinned
M123 Abseil source rejects Visual Studio 2017, so that check currently proves
that the modern engine cannot be built in the app's existing native toolset.
It must not be bypassed by editing the upstream version check. To evaluate a
separate VS 2022 bridge toolset after the WebRTC Release ARM build exists,
pass its environment script and host/target pair explicitly:

```powershell
.\build\tgcalls\Test-TgCallsUwpArm.ps1 `
    -WebRtcRoot C:\wrtcar\src `
    -VcVarsAllPath 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat' `
    -VcVarsArchitecture x64_arm
```

With `-WebRtcRoot`, the probe adds the pinned Abseil and generated-output
include roots plus the UWP WebRTC platform definitions from the ARM GN build.
This only validates C++ source compilation; a separate C++/CX WinMD project
must still prove toolset/link/runtime compatibility before integration.

M123 adds string device-selection members to `AudioDeviceModule`. The pinned
TgCalls wrapper already delegates these members but incorrectly limits that
delegation to its desktop UWP macro. The probe applies
`patches\tgcalls-m123-winuwp-audio-device.patch` to expose the same existing
delegation on UWP ARM; it neither changes device selection semantics nor adds
a fallback.

The required WebRTC build must target Windows Store ARM with the app's
Windows 10 SDK 18362 constraints and provide the matching headers, generated
configuration, static libraries, and UWP-compatible audio/network
implementations. It must be built or ported from source; headers alone do not
constitute a transport proof.

## Integration gate

Only after the probe and a linked ARM C++/CX WinMD proof pass may a new
component be introduced. It must expose audio-only lifecycle, encryption,
reflector/WebRTC server configuration, signaling, routing, state callbacks,
and a version list; `CallsService` must select it only for the versions it
actually implements. The existing ARM `libtgvoip` wrapper stays in place as
the validated W10M legacy fallback. Video remains out of scope.

TgCalls is LGPL-3.0. Before distributing any resulting APPX, preserve the
upstream license and source pin, publish required notices and corresponding
source for modifications, and complete a licensing review of the component
linkage and redistribution terms.

## ARM WebRTC source build

`UnigramDev/webrtc-uwp` carries the needed `winuwp` platform changes, but its
published archives intentionally omit 32-bit ARM. The external
`UnigramDev/deps@6ce0019e1e5ea4e06ab3bc21651242567774a4e0` build script
pins the UWP fork at
`801b01361857fd9afd40f0efd29034b0e48001c7` and its WebRTC source branch at
`branch-heads/6312`.

Run the experimental wrapper with VS 2022 ARM C++ Build Tools installed:

```powershell
.\build\tgcalls\Build-WebRtcUwpArm.ps1
```

It verifies the external build-script revision, selects a Visual Studio
instance with the ARM workload, and injects `arm` into the script's published
x64/ARM64 architecture allow-list only in a temporary copy. The external
script and its pinned upstream source remain unmodified. The default build
root is deliberately short (`C:\wrtcar`): Chromium's third-party checkout
contains paths that exceed Windows' legacy limit under the longer
`%LOCALAPPDATA%` location. It downloads `depot_tools`, synchronizes
approximately 20 GB of WebRTC dependencies, then generates
`target_os="winuwp"` and `target_cpu="arm"` before building `webrtc.lib`.
This operation can take several hours.

Chromium M123's generic Rust configuration rejects Windows ARM before GN sees
that WebRTC's standalone build defaults Rust off. The wrapper explicitly
passes `enable_rust=false` and applies the small, guarded
`patches\webrtc-m123-winuwp-arm-no-rust.patch`: it permits ARM only when Rust
is disabled and continues to reject it if a dependency enables Rust. This does
not disable SCTP, WebRTC signaling, or any C++ audio transport component.

The corresponding Chromium runtime-DLL staging helper has no ARM case even
though the UWP compiler toolchain does. The wrapper applies
`patches\webrtc-m123-winuwp-arm-skip-runtime-copy.patch` to bypass that
desktop-oriented copy only for the static UWP ARM library; the ARM compiler
environment still comes from `setup_toolchain.py`.

MSVC cannot assemble the fork's GNU ARMv7/NEON routines or compile their
GCC-style inline assembly. For this proof only, the wrapper selects WebRTC's
existing scalar paths with `arm_version=6 arm_use_neon=false` and applies
`patches\webrtc-m123-winuwp-arm-scalar-audio.patch`. The scalar PFFFT
configuration also reveals an upstream GN list-assignment error; the wrapper
applies `patches\webrtc-m123-winuwp-arm-scalar-pffft.patch` to preserve the
Windows math-constant define while adding `PFFFT_SIMD_DISABLE`. These changes
control WebRTC optimization feature selection only; they do not change the
`target_cpu` (`arm`), UWP architecture, audio codec support, SCTP, signaling,
or the eventual APPX architecture. They trade performance for a correctly
compiled native ARM proof and require audio performance/device validation
before distribution.

Opus also enables an ARM runtime-dispatched GNU assembly routine whenever its
target is Windows ARM. `patches\webrtc-m123-winuwp-arm-scalar-opus.patch`
disables that assembly-only selection for UWP ARM, retaining Opus's existing
portable C implementation in `pitch.c`. This is the same scalar-proof
constraint as the common-audio changes, not a change to Opus support or media
encryption.

The scalar configuration retains `WEBRTC_ARCH_ARM` for generic ARM behavior,
but MSVC cannot compile the GNU extended assembly used only to control the
floating-point denormal mode. The wrapper therefore applies
`patches\webrtc-m123-winuwp-arm-msvc-denormal.patch`, which leaves denormal
control unsupported for 32-bit MSVC ARM and uses WebRTC's existing no-op
fallback. It does not affect media encryption, audio codecs, signaling, or
call lifecycle.

The same compiler limitation reaches BoringSSL's optimized GAS sources.
`patches\webrtc-m123-winuwp-arm-boringssl-no-asm.patch` selects BoringSSL's
existing `OPENSSL_NO_ASM` configuration for UWP ARM, matching its established
ARM64/sanitizer fallback. The portable C cryptographic implementation remains
in use; this is a performance tradeoff that needs device validation, not a
substitute or a reduction in encryption behavior.

The proof intentionally excludes AV1 and libvpx by passing
`enable_libaom=false rtc_build_libvpx=false rtc_libvpx_build_vp9=false`.
`patches\webrtc-m123-winuwp-arm-audio-only-libaom.patch` makes the standalone
root target honor those existing configuration options instead of
unconditionally including the AV1, VP8, and VP9 factory adapters.
`patches\webrtc-m123-winuwp-arm-audio-only-libvpx.patch` preserves the public
VP8/VP9 target labels for dependency-graph compatibility but omits their
libvpx-dependent implementation sources whenever `rtc_build_libvpx=false`.
The fork does not supply the required ARM-generated libvpx headers
(`vpx_config.h` and `vpx/vp8*.h`), so this is an explicit audio-only build,
not a codec fallback. AV1, VP8, VP9, and all broader video-call work remain
deferred.

The generated library is only a native proof input. It must still compile and
link with the audio-only C++/CX TgCalls bridge, activate from its WinMD, and
complete device interoperability before it can replace any packaged transport.

## Linked ARM WinMD bridge proof

`Build-ModernCallsBridgeProof.ps1` now builds the complete Release|ARM
linkage proof:

```powershell
.\build\tgcalls\Build-ModernCallsBridgeProof.ps1
```

It first builds the pinned TgCalls core as a native v143 C++20 static library,
then links that library and the generated ARM `webrtc.lib` into a separate
v143 C++/CX UWP Runtime Component. This split is required because modern
WebRTC headers use `generic`, which is reserved by C++/CX and therefore cannot
be compiled under `/ZW`.

The external proof output root is:

```text
%LOCALAPPDATA%\UnigramTdlibExperiment\bridge-probe
```

Successful builds produce:

```text
ModernCallsBridge.dll
Unigram.Native.Calls.Proof.winmd
Unigram.Native.Calls.Proof.pri
native-engine\TgCallsEngine.lib
```

`ModernCallsBridge.dll` is an ARM AppContainer DLL. `Diagnostics::GetBuildInfo()`
registers `InstanceImpl` with TgCalls' `Meta` registry, then obtains the first
supported version through that actual native-engine path; it is not a
hard-coded version string.

The proof now also exports an audio-only lifecycle surface:

- `AudioCallConfiguration` carries the registered protocol version, timeouts,
  P2P/TCP settings, API layer, direction, network type, 256-byte encryption
  key, reflector endpoints, and STUN/TURN server entries.
- `AudioCallSession` creates the native instance, accepts encrypted signaling,
  controls mute and network type, asynchronously stops, and reports native
  state, emitted signaling, and final-stop events.
- The bridge rejects missing versions, missing endpoint/server data, non-UTF-16
  strings, non-256-byte keys, and reflector peer tags other than 16 bytes.
  It neither logs nor persists keys, server credentials, or signaling.
- The C++/CX owner is weakly referenced from native callbacks and has
  deterministic disposal, so callback ownership cannot retain a stopped call
  session.

This is still an external build proof, not an app payload. It has not yet been
selected by `CallsService`, included in an APPX, or device-tested with
Android/iOS. The legacy ARM `libtgvoip` fallback therefore remains the only
packaged call transport.

Current verified output hashes from the external proof root:

```text
AD8289C5008C5530A2330D7F1BC82E56B1750073E1458D1293E25C6AD19926E9  ModernCallsBridge.dll
9432A7AB2A65C0D7D6A9724691B4155283DAA84BD997D6CA8D5612BAABC3890D  Unigram.Native.Calls.Proof.winmd
5829394098835DEA8A09811A8DECE1171B348301E426E9B83ACD386CB3E1AF38  Unigram.Native.Calls.Proof.pri
F155A1BB87D63385898124E5642DEC5B7EF29C1A6EFC055D6D47D09E15F0A5FA  native-engine\TgCallsEngine.lib
```

The engine's `AudioOnlyPlatform.cpp` deliberately reports empty video encoder
and decoder factory formats, reports no video encoding support, and exposes no
capture/source implementation. It retains Opus audio and the native
peer-connection setup required by TgCalls, while avoiding the unavailable ARM
libvpx codecs and Windows Mobile video-capture surface. Video remains
explicitly unsupported.

The source closure includes TgCalls' reflector relay, reflector port, and raw
TCP port implementations. It also requires
`webrtc-m123-winuwp-arm-boringssl-tls.patch`: BoringSSL's Windows code emits
x86-decorated linker `/INCLUDE` names for non-x64 architectures, whereas ARM
uses undecorated symbols. The wrapper applies the narrow ARM directive fix,
rebuilds `thread_win.obj`, and preserves BoringSSL's real thread-exit callback
and TLS directory instead of suppressing either linker dependency.

Any later app integration must package the resulting bridge conditionally,
retain `libtgvoip` for validated legacy Windows 10 Mobile calls, and must not
advertise modern TgCalls versions until device interoperability is verified.

## Experimental app integration

The experimental app project has an explicit `UseModernTgCalls` build property.
It defaults to `false`, so normal experimental ARM packages continue to
advertise the legacy empty call-library list and use only `libtgvoip`.

Set it only for the dedicated interoperability package:

```powershell
$env:SolutionDir = "$PWD\Unigram\"
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
    .\Unigram\Unigram\Unigram.csproj `
    /m /t:Build /p:Configuration=Release /p:Platform=ARM `
    /p:UseModernTgCalls=true /p:GenerateAppxPackageOnBuild=true
```

When enabled, the project rebuilds the external bridge proof, requires its
ARM WinMD/DLL/PRI output, references it as a runtime component, and packages:

```text
Unigram.Native.Calls.Proof.winmd
ModernCallsBridge.dll
Unigram.Native.Calls.Proof.pri
```

The app reads the registered versions from the bridge and advertises only
those values. It selects the modern transport only when the peer's
`CallProtocol.LibraryVersions` has an exact intersection with that registry.
It maps Telegram reflector endpoints and TDLib WebRTC STUN/TURN server roles,
forwards `UpdateNewCallSignalingData` to the bridge, and sends emitted
signaling through `SendCallSignalingData`. If a modern version was negotiated
but server/configuration validation or bridge creation fails, the app records
a privacy-safe result and discards the call; it does not silently substitute
legacy `libtgvoip` after modern negotiation.

Legacy peers without a matching library version continue on the existing
`libtgvoip` route. The existing call page remains the legacy controller UI:
modern transport state updates the connecting/established tone lifecycle, but
per-device volume selection and preferred-relay reporting are not yet exposed
by the modern bridge. They are explicitly outside this audio-only proof and
must be completed before treating the opt-in package as a general release.

Current verified opt-in package output:

```text
APPX:
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\
    Unigram_26.9.6139.0_ARM_ModernTgCalls_Test.appx

Minimal ARM sideload ZIP:
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\
    Unigram_26.9.6139.0_ARM_ModernTgCalls_Test_Sideload.zip
APPX SHA-256:
90C95398B04C4C120D9851FF8B7F461534167977E5CAAA380661334A4CB6570C
ZIP SHA-256:
7D440D5DEEF2C44BD1CBF722DE00EF29B45F3BA59EE6F413FF4B773D79720D3C
```

The ZIP contains the signed APPX, its public `.cer`, and only the ARM NET
Native, XAML, and VCLibs dependency APPXs. It contains no PFX, private key,
or source secret. The APPX was signature-verified and its manifest confirms
the side-by-side experimental identity
`49197Wirdschon.UnigramMobileTdlibExperimental`, version `26.9.6139.0`,
ARM architecture, and the existing native notification background entry point.

This remains a device-test package, not a released call fix. Required
validation is experimental W10M to/from current Android and iOS audio calls,
including accept, outgoing signaling, mute, route changes, foreground and
background behavior, reconnect, rejection, and cleanup. Preserve the
legacy-W10M call regression test in both `UseModernTgCalls=false` and enabled
package configurations.
