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
- `AudioCallSession` is configured before it starts the native instance.
  The app subscribes to state/signaling/stop events, assigns session ownership,
  then explicitly calls `Start()` so the first synchronous offer or answer
  cannot be lost. It accepts encrypted signaling, controls mute and network
  type, asynchronously stops, and reports native state, emitted signaling, and
  final-stop events.
- The bridge rejects missing versions, missing endpoint/server data, non-UTF-16
  strings, non-256-byte keys, and reflector peer tags other than 16 bytes.
  It neither logs nor persists keys, server credentials, or signaling.
- The C++/CX owner is weakly referenced from native callbacks and has
  deterministic disposal, so callback ownership cannot retain a stopped call
  session.

The bridge is included only in opt-in experimental APPXs, where
`CallsService` selects it after modern protocol-version negotiation. It has
not yet been device-tested with Android/iOS. The default build remains
`libtgvoip`-only, and negotiated modern calls never silently fall back to the
legacy transport after setup fails.

Current verified output hashes from the external proof root:

```text
573C65F6D07F933306E1F1DCC7BB6423550C28DA375BAF767BCF0D23549C247B  ModernCallsBridge.dll
54505BA880224F24E1BB9229C69E4AB5B534B6FD4847DCE9EF5A8CBE26B9224C  Unigram.Native.Calls.Proof.winmd
5829394098835DEA8A09811A8DECE1171B348301E426E9B83ACD386CB3E1AF38  Unigram.Native.Calls.Proof.pri
3E48E7CF39E911B7B25C73454C8277C2857866219D36E2FE0C70DA066BDD8814  native-engine\TgCallsEngine.lib
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

The app uses the audited version list from the pinned TgCalls source
(`2.7.7` and `5.0.0`) and advertises only those values. This avoids activating
the WebRTC bridge merely to build `CallProtocol`; bridge activation is
deferred until TDLib provides ready-state configuration. It selects the modern
transport only when the peer's `CallProtocol.LibraryVersions` has an exact
intersection with that list.
It maps Telegram reflector endpoints and TDLib WebRTC STUN/TURN server roles,
forwards `UpdateNewCallSignalingData` to the bridge, and sends emitted
signaling through `SendCallSignalingData`. If a modern version was negotiated
but server/configuration validation or bridge creation fails, the app records
a privacy-safe result and discards the call; it does not silently substitute
legacy `libtgvoip` after modern negotiation.

`CreateCall` and `AcceptCall` are created parameterlessly and populated through
property setters. Their generated modern TDLib constructors marshal a
`CallProtocol` object through a multi-argument WinMD activation factory, which
is not reliable on this C++/CX projection; `DiscardCall` uses only primitive
arguments and is not affected. The property-setter path avoids that ABI
boundary and is used for both outgoing and accepted calls.

Call actions record activation failures with privacy-safe diagnostics and show
an explicit failure dialog rather than failing silently. That makes the
remaining device-side test result observable without logging call keys,
signaling, user IDs, or message content.

Modern chat folders use the same parameterless/property-setter construction
for folder names, folder objects, and get/create/edit/delete requests. The
folder chat picker is enabled again for included and excluded chats; its
selected IDs are mapped to `ChatFolder` fields before the request is sent.

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
    Unigram_26.9.6153.0_ARM_ModernTgCalls_MediaDiagnostics.appx

Minimal ARM sideload ZIP:
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\
    Unigram_26.9.6153.0_ARM_ModernTgCalls_MediaDiagnostics_Sideload.zip
APPX SHA-256:
78D249C2ABB34592CC39620F4824EDCFE563C402425C1EFD3C703F4CF76B6B10
ZIP SHA-256:
2E7A118EB70AA0FAF3B1B23B0A36CA3F121EA15421129EDA2126B8CFD160777B
```

The ZIP contains the signed APPX, its public `.cer`, and only the ARM NET
Native, XAML, and VCLibs dependency APPXs. It contains no PFX, private key,
or source secret. The APPX was signature-verified and its manifest confirms
the side-by-side experimental identity
`49197Wirdschon.UnigramMobileTdlibExperimental`, version `26.9.6153.0`,
ARM architecture, and the existing native notification background entry point.

This remains a device-test package, not a released call fix. The 26.9.6148.0
trace proved that WinRT bridge activation succeeds, but the managed mapping
then rejected TDLib WebRTC servers as `invalid_webrtc_server` because their
64-bit IDs were incorrectly required to fit in a byte. TgCalls defines the
corresponding wire ID as an 8-bit value; its reference implementation
intentionally narrows TDLib's ID. That implementation also exposes Telegram
reflectors as ordered TURN servers, which the bridge had omitted.

Version 26.9.6149.0 mirrors that mapping: it narrows the WebRTC wire ID
without rejecting the server, adds reflector TURN entries using the reference
implementation's stable sorted ordinal, and accepts IPv6-only endpoints. It
records only aggregate mapped server counts before bridge creation; no
endpoint, credential, peer-tag, signaling, or key data is logged.

Version 26.9.6150.0 retains the corrected server mapping and serializes
call-window show/hide operations through the existing asynchronous mutex.
Rapid pending-call updates previously could concurrently create or dispose the
secondary compact view, producing an unhandled `NullReferenceException` before
call setup. The UI path now holds that mutex across creation, update, and
disposal; unexpected view failures are emitted as privacy-safe `voip.ui`
diagnostics with only the operation, HRESULT, and sanitized message. This does
not change TDLib call negotiation, bridge session creation, signaling, or the
legacy transport selection.

Version 26.9.6151.0 guards the call-page routing API. On the tested Windows
10 Mobile image, the Phone contract is present while
`AudioRoutingManager.GetDefault()` can return no manager. The old call-page
loaded/unloaded, route-click, and endpoint-change paths dereferenced that
unavailable manager. The page now retains a manager only when it is available,
hides the routing control otherwise, and unsubscribes from that same manager
during cleanup. The `6151` device trace proved the null reference occurred
before the call page's `Loaded` event, so this guard did not cover the actual
secondary-view fault.

Version 26.9.6152.0 also guards `TLWindowContext` title/status-bar
customization. It is instantiated while the compact secondary view is created,
before the `VoIPPage` content is loaded, and previously dereferenced
`ApplicationView.TitleBar` and `CoreApplicationViewTitleBar` unconditionally.
Those shell objects are absent in the Windows 10 Mobile compact-view context.
The call page now records ordered, privacy-safe `voip.ui` construction stages
as well. The `6151` trace reached modern bridge `Established` with
bidirectional signaling before the app restart; it did not show a native
bridge rejection.

Chat Folders is now device-verified: after registering `FoldersViewModel` and
`FolderViewModel` for modern TDLib, a folder created in the experimental app
with two group chats synchronized successfully with the regular Telegram
client.

Version 26.9.6153.0 makes the media layer observable. The `6152` trace proved
the app no longer restarts during a call and that the modern transport reaches
`Established` with bidirectional signaling, yet neither side had audio. Every
diagnostic up to that point described signaling and transport only, so an
audio-device failure and a media-routing failure were indistinguishable. The
bridge now reports four media facts to managed code:

- `voip.media result=audio_device` — whether the platform audio device module
  was created, its `Init` result, and the playout and recording device counts.
  TgCalls silently continues without audio when this module is null or fails to
  initialize, so this distinguishes an audio fault from a network fault.
- `voip.media result=audio_level` — a five-second summary of the engine's audio
  level callback: sample count, how many samples exceeded an audible threshold,
  and a quantised peak amplitude. TgCalls only runs its level timer when this
  callback is supplied, so it had never been active before. This value is
  deliberately labelled `active` and not `incoming`: the engine reports the
  larger of the local capture level and the decoded remote level, so room noise
  on the microphone alone can raise it and it cannot by itself separate the two
  directions.
- `voip.media result=remote_audio` — the peer's reported audio state.
- `voip.media result=signal_bars` — the engine's network quality estimate. This
  is derived purely from the outgoing send bandwidth estimate, so it is an
  upstream signal and is not evidence that media is being received.

Because the engine's own level callback is non-directional, each audio level
summary also carries the live state of the audio device module the bridge
supplies, as `created`, `recording`, and `playing` booleans. These are the
unambiguous answer to "the transport is established but nobody can hear
anything": `playing=0` means no playout is running regardless of what arrives
on the wire, and `recording=0` means nothing is being captured to send.

Media diagnostics are sampled for the entire duration of a call, so they draw
on a separate per-call allowance that is reset when a call is disposed. They
deliberately do not charge the shared call diagnostic budget, because a single
long call would otherwise exhaust it and silence the lifecycle diagnostics of
every later call in the same app session.

None of these carry audio content, device names, identifiers, hosts, or
credentials; only fixed keys, enum names, counts, booleans, and a quantised
amplitude. The audio device module the bridge now supplies is the same platform
default TgCalls would otherwise create for itself, so this adds visibility
without changing which device is used.

The same version records the platform description of an unhandled exception.
.NET Native discards a managed stack before it reaches the app's handler, and
the diagnostics sanitizer additionally redacted framework resource keys such as
`Arg_NullReferenceException` because they are long enough to look like opaque
tokens. Exception-shaped identifiers built only from letters and underscores
are now preserved, while anything bearing digits or hyphens still redacts. The
residual non-fatal null reference raised once per call in `6152` was therefore
unattributable and should now name its throw site.

`CallsService` also no longer swaps the two TgCalls timeouts: the connect
timeout maps to `initializationTimeout` and the packet timeout to
`receiveTimeout`, with non-zero fallbacks because the server reports both
options as zero. This is a correctness fix only. The pinned TgCalls tree never
reads either field — every implementation hard-codes a 20 second timeout — so
it cannot by itself change call behaviour and is not claimed as a call fix.

Required validation is experimental W10M to/from current Android and iOS audio
calls, including accept, outgoing signaling, mute, route changes, foreground
and background behavior, reconnect, rejection, and cleanup. Preserve the
legacy-W10M call regression test in both `UseModernTgCalls=false` and enabled
package configurations. Collect fresh privacy-safe diagnostics: a modern call
should now log `voip.ready result=creating` followed by either
`bridge_create`/`bridge_start` failure detail or the subsequent session
startup result. It should not emit an unhandled `CallStatePending`
null-reference error or restart when audio routing is unavailable; that state
is recorded as `voip.ui result=routing_unavailable` and route selection is
hidden. `6152` should log `view.title_bar result=unavailable` or complete the
`voip.ui` construction stages without crashing. `6153` should additionally log
one `voip.media result=audio_device` line per call and, once the transport is
established, recurring `voip.media result=audio_level` summaries carrying
`recording=` and `playing=` flags. This is not a claim that Android/iOS audio
is established; `6153` is a diagnostic build and is not expected to fix calls.
