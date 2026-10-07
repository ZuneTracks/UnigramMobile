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
    Unigram_26.9.6154.0_ARM_ModernTgCalls_AudioDeviceTrace.appx

Minimal ARM sideload ZIP:
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\
    Unigram_26.9.6154.0_ARM_ModernTgCalls_AudioDeviceTrace_Sideload.zip
APPX SHA-256:
FF3825C36D49F2C44AC2D4F80E28807895C1A2B12845688DD3184C56EA6B3322
ZIP SHA-256:
23D76538501AB4CACA9EBBBBE03D736395145E18033A3FAB3973975EE4AF348A
```

The ZIP contains the signed APPX, its public `.cer`, and only the ARM NET
Native, XAML, and VCLibs dependency APPXs. It contains no PFX, private key,
or source secret. The APPX was signature-verified and its manifest confirms
the side-by-side experimental identity
`49197Wirdschon.UnigramMobileTdlibExperimental`, version `26.9.6154.0`,
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

## 26.9.6154.0 — the crash is inside the platform audio stack

The `6153` trace from an outgoing W10M to Android call answered the question
`6153` was built to ask, and it cleared the audio device module of suspicion:

```text
voip.media|result=audio_device;created=1;init=0;playout_devices=2;recording_devices=3
voip.transport|result=state;state=Established
voip.media|result=remote_audio;state=Active
```

The module is created, initializes with code `0`, and enumerates two playout
and three recording devices, so device discovery works on Windows 10 Mobile.
The transport reaches `Established` and the peer reports its audio as active.

The decisive detail is how the log ends. `PushDiagnostics` opens, appends, and
closes the file on every single line, so the log is a flushed breadcrumb trail
and its last line is the last thing the process did. The trace stops mid-call
roughly 360 ms after `Established`, on an ordinary `voip.signaling` line, with
no `CallStateDiscarded`, no error, and no teardown. The process was killed
rather than the call being ended, which matches every earlier report that the
app closes immediately once a call is answered.

That window is exactly where `MediaManager::setIsConnected(true)` starts the
audio send and receive channels, which is what drives the audio device module
into `InitPlayout`, `StartPlayout`, `InitRecording`, and `StartRecording`. A
structured exception there terminates the process before managed code can log
anything, which is why every previous build produced a truncated trace.

`6154` instruments precisely that region. The bridge now returns a wrapper
around the platform module, derived from the upstream
`DefaultWrappedAudioDeviceModule` so that every method it does not override is
forwarded unchanged. The wrapper brackets each once-per-call lifecycle call
with a `phase=begin` and a `phase=end` breadcrumb carrying only a fixed step
name and the numeric result:

```text
voip.media|result=audio_device;step=start_playout;phase=begin
voip.media|result=audio_device;step=start_playout;phase=end;faulted=0;code=0
```

Each forwarded call also runs inside a structured exception guard. If the
platform audio stack raises an access violation, the guard reports
`faulted=1;code=-1` and returns instead of letting the process die. This names
the faulting step and, because the process survives, the remaining diagnostics
for that call are still written. A `phase=begin` with no matching `phase=end`
would instead mean the fault escaped the guard.

Catching an access violation and continuing can leave the audio stack in an
undefined state, so this is a diagnostic build only. The guard exists to
identify the faulting call, not to ship as a fix.

Because the wrapper is a forwarding subclass of the upstream wrapper rather
than a hand-written implementation of the interface, it cannot silently drop a
method. The media diagnostic allowance is raised to cover the extra breadcrumbs
for a whole call, and remains separate from the shared call budget.

The residual non-fatal `NullReferenceException` is still unattributed: .NET
Native reports both its message and its platform description as the resource
key `Arg_NullReferenceException`, with no stack and no inner exception, so the
`6153` description change could not name the throw site. It fires about 31 ms
after `CreateCall` returns and remains non-fatal.

Expected `6154` evidence from one call: a `step=`/`phase=` pair for each audio
device lifecycle call, and either a `faulted=1` line naming the faulting step,
or a complete set of `faulted=0` pairs proving the audio device stack is not
where the process dies.

## 26.9.6156.0 — the process dies inside `InitRecording`

The `6154` trace answered its question exactly. Every one of the bracketed
audio device lifecycle calls up to and including playout completed cleanly:

```text
voip.media|result=audio_device;step=init_playout;phase=end;faulted=0;code=0
voip.media|result=audio_device;step=start_playout;phase=end;faulted=0;code=0
voip.media|result=audio_device;step=init_recording;phase=begin
```

`init_recording` has a `phase=begin` and no `phase=end`, and the log stops
about 40 ms later. Playout therefore works end to end and the process dies in
the capture path. Because the structured exception guard around that call did
not report `faulted=1`, the fault did not happen on the thread that made the
call: UWP activates a capture endpoint through `ActivateAudioInterfaceAsync`,
whose completion runs on a thread pool thread that no `__except` of ours
covers.

The most likely cause is microphone consent. Searching the app shows that
nothing in the call path ever requests it — only `ChatRecordButton` does, for
voice messages, through `MediaCapture`. Playout needs no consent, which is why
it succeeds, and raw WASAPI capture activation is the first thing in a call
that does. The webrtc-uwp fork's capture activation handler does not appear to
tolerate a failed activation result.

So `6156` acquires consent before the engine is ever constructed.
`CallsService` starts an idempotent `MediaCapture` initialization as soon as a
call reaches `CallStatePending`, using `StreamingCaptureMode.Audio` and
`MediaCategory.Communications`, and disposes it immediately. The TDLib update
thread then waits up to eight seconds for that result before creating the
session. It never waits on the UI thread: `Handle(UpdateCall)` runs on TDLib's
native receive thread, so the `MediaCapture` work is marshalled to the UI
thread and awaited from the caller without any possibility of self-blocking.

If consent has not been granted the call is rejected with
`reason=microphone_unavailable` instead of proceeding into the fatal path. A
timeout is treated as a refusal for the same reason — the only case that times
out is a first-ever consent prompt still on screen — but the pending
acquisition is deliberately left in place so answering it makes the next call
work. A non-timeout failure clears the cached task so one transient error
cannot wedge every later call.

```text
voip.media|result=microphone;acquired=1
voip.media|result=microphone;acquired=0;hresult=0x80070005
```

### Reporting a fault that kills the process

Because the fault can land on a thread we do not control, `6156` also installs
a vectored exception handler in the native engine. Three details make it safe:

- It only reports while an audio device lifecycle call is in flight, tracked by
  an interlocked depth counter. A first-chance handler otherwise sees every
  exception in the process, and .NET Native raises `NullReferenceException` as
  an access violation, so an ungated handler would burn its report budget on
  benign managed exceptions and on the very access violations the sibling
  structured exception guard deliberately swallows.
- It ignores `EXCEPTION_STACK_OVERFLOW`, where running any handler on the
  remaining guard page would simply fault again.
- It always returns `EXCEPTION_CONTINUE_SEARCH`, so it changes no behaviour.

It writes to its own file, `Diagnostics\voip-fault.txt`, not to the main log. A
fault context can only append with raw file APIs, while the managed writer
tracks its own end-of-file offset and would write back over anything appended
behind its back. `CallsService` drains that file into the main log at the start
of the next call, tagging each recovered line `deferred=1`, so the user still
collects a single artifact. `CreateFileW` is unavailable in the UWP app
partition, so the handler uses `CreateFile2`; `AddVectoredExceptionHandler` is
likewise gated out of the app partition headers but the export is present and
links against the UWP import library once declared by hand.

### Artifacts

```text
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\Unigram_26.9.6156.0_ARM_ModernTgCalls_MicAcquire.appx
  57497091 bytes  SHA-256 B089503D464760B1A56636C61C7E800CAA25A603C44440FB6FE2E4EE503BF2C5
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\Unigram_26.9.6156.0_ARM_ModernTgCalls_MicAcquire_Sideload.zip
  64098545 bytes  SHA-256 92DDE730A150DEA4BC8200DF2E07EF31C21F117A11E53AFBE95ED58366EA4666
```

Expected evidence from one call: `result=microphone;acquired=1` followed by
`step=init_recording;phase=end`, which would confirm the diagnosis and leave
the remaining lifecycle calls visible for the first time. A clean
`acquired=0;hresult=` instead names the consent failure directly. If
`init_recording` still truncates the log, the drained `voip.fault` line on the
following call reports the exception code.

### Upstream comparison

`UnigramDev/Unigram`'s `Telegram.Native.Calls/VoipManager.cpp` was read as a
reference. It does not override `createAudioDeviceModule`, does not request
microphone consent, and builds a `tgcalls::Descriptor` and `Config` matching
ours field for field. That is a useful negative result: it rules out a
mis-integration of tgcalls, and it is consistent with the diagnosis, since raw
WASAPI capture activation does succeed unprompted on desktop UWP. Two
differences remain noted but unapplied: upstream sets `config.logPath` (which
makes tgcalls write a native log containing IP addresses, so it cannot be fed
into the shared diagnostics as-is), and its reflector `server.id` is 0-based
where ours is 1-based. The transport already reaches `Established`, so the
latter is not the cause of this crash.

## 26.9.6157.0 — microphone consent was never the problem

The `6156` trace settled it: `result=microphone;acquired=1` on every call, with
no prompt, because consent had already been granted. The crash is unchanged,
and the consent hypothesis is dead.

What `6156` did buy is the first direct evidence of the fault. The gated
vectored handler fired, and its records survived into the main log:

```text
voip.media|result=audio_device;step=init_recording;phase=begin
voip.fault|result=exception;code=0xC0000005;noncontinuable=0;deferred=1
```

An access violation, raised while an audio device lifecycle call was in flight,
not caught by the structured exception guard around that call.

That is still not conclusive, for two reasons. .NET Native raises every
ordinary null dereference as `0xC0000005`, and this app has a known benign
`NullReferenceException` that fires during the same window — so the code alone
cannot distinguish a real native fault from that. And a fatal error that is not
a structured exception, such as the `abort()` at the end of a WebRTC
`RTC_CHECK` failure, never reaches a vectored handler at all and would leave
exactly the same evidence: a log that simply stops.

`6157` closes both gaps.

### Locating the fault

The handler now reports where the fault is, not just that it happened:

```text
result=exception;code=0xC0000005;step=init_recording;same_thread=0;engine=1;offset=0x00ABCDEF;access=read;null_page=1;noncontinuable=0
```

- `step` names the lifecycle call in flight, recorded by the scope that gates
  the handler.
- `same_thread` compares the faulting thread against the thread driving the
  module, which decides whether the structured exception guard could ever have
  caught it.
- `engine` is whether the fault address lies inside this module, resolved
  against the linker-supplied `__ImageBase` and the image size read from the PE
  header — no module API is called from the fault context. `engine=1` means
  webrtc or tgcalls code; `engine=0` means the fault belongs to another module
  and is almost certainly the managed null dereference.
- `offset` is the module-relative address. It carries nothing about the install
  and resolves to a function against the build's map file, which is archived
  next to the artifacts as `ModernCallsBridge_26.9.6157.0.map` and is
  deliberately not packaged.
- `access` and `null_page` separate a null dereference from genuine memory
  corruption.

The project now links with `/MAP` so that offset is resolvable.

### Catching a termination that is not an exception

`signal(SIGABRT)`, `std::set_terminate`, `_set_purecall_handler` and
`_set_invalid_parameter_handler` each write a line to the same fault file:

```text
result=abort;step=init_recording;in_lifecycle=1
```

A WebRTC `RTC_CHECK` failure ends in `abort()` and is live in release builds,
unlike `RTC_DCHECK`. If that is what kills the process, this names it. Only a
`__fastfail` would still escape.

Sharing the push log from the diagnostics page now drains the fault file first,
so the shared file is complete without waiting for the next call to collect it.

### Artifacts

```text
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\Unigram_26.9.6157.0_ARM_ModernTgCalls_FaultLocate.appx
  57497252 bytes  SHA-256 96C511703B84C4CA418014A9C74E4561F216E82120EAE602ED603D28BF392F13
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\Unigram_26.9.6157.0_ARM_ModernTgCalls_FaultLocate_Sideload.zip
  64099016 bytes  SHA-256 2C78EEE43DB8619FC1C6F5CAD014A72DC7291B24A4B439DB51B24E415261B55F
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\ModernCallsBridge_26.9.6157.0.map
  14800124 bytes  SHA-256 AEED2B0A22A59C6F4335BAF36D4A82014AB548A2E43C5BA93FF2ADD235E63D8A
```

### What the audio device module actually is

Worth recording, because it narrows where the fault can be. The pinned WebRTC
selects `AudioDeviceWindowsCore` from `win/audio_device_core_win.cc`, and that
file contains no `WINUWP`, `WINRT` or `WINAPI_FAMILY` conditionals at all: it
is stock desktop WASAPI code running in an app container. `InitRecording`
itself is wholly synchronous and starts no threads, which makes a
`same_thread=0` report significant if it appears. Its one early exit is
`InitRecordingDMO`, reached only when built-in AEC is enabled; `_dmo` is null
unless `CLSID_CWMAudioAEC` could be created, and `EnableBuiltInAEC` refuses to
enable the flag in that case, so the DMO path should be unreachable here. The
`RTC_DCHECK(_dmo)` guarding it is compiled out in release.

### Resolving a reported offset

`Resolve-FaultOffset.ps1` turns an `offset=` value from a `voip.fault` line into
the function it landed in. The offset is a relative virtual address, and a
linker map lists every function by absolute address next to the preferred load
address, so the answer is the last entry at or below the offset.

```powershell
cd build\tgcalls
.\Resolve-FaultOffset.ps1 -Offset 0x000afbe0 `
    -MapPath "$env:LOCALAPPDATA\UnigramTdlibExperiment\artifacts\ModernCallsBridge_26.9.6157.0.map"
```

```text
Function ?InitRecording@AudioDeviceWindowsCore@webrtc@@UAAHXZ
Starts   0x000AFBDC  (+0x4 into the function)
Object   webrtc:audio_device_core_win.obj
```

Use the map archived for the build that produced the log; a map from any other
build resolves to the wrong function.

## 26.9.6158.0 — the crash was managed all along

The 26.9.6157.0 run reported this, and it ends the search for a native fault:

```text
result=exception;code=0xC0000005;step=init_playout;same_thread=0;engine=0;access=read;null_page=1
result=exception;code=0xC0000005;step=init_recording;same_thread=0;engine=0;access=write;null_page=1
```

`engine=0` means the faulting address is outside `ModernCallsBridge.dll`, which
is where all of WebRTC and tgcalls live, so neither of these is a native audio
fault. `null_page=1` on both, and the first one fired while `init_playout` was
in flight — a step that went on to complete with `faulted=0;code=0`. A fault the
process survives is not the fault that kills it.

What these actually are is how .NET Native raises a `NullReferenceException`:
a hardware access violation against the null page. The main log carries the
matching `hresult=0x80004003` entries. So the crash is a managed exception, and
the reason nothing was ever recorded is that it never reached a handler:
`Application.UnhandledException` only observes the UI thread, and the log shows
no `app.unhandled` line for the fatal one, unlike the benign ones a few seconds
earlier.

That leaves exactly one mechanism. TDLib's own native callbacks were contained
long ago — `ProtoService.OnResult`, `TdHandler.OnResult` and
`TdCompletionSource.OnResult` each catch and record, precisely because a managed
exception escaping a reverse P/Invoke reaches
`RhpFailFastForPInvokeExceptionPreemp` and terminates the process with no
managed stack. The tgcalls events were the last boundary of that shape left
unguarded: `StateChanged`, `SignalingData`, `Stopped`, `AudioDeviceReport`,
`SignalBarsChanged`, `AudioLevelChanged` and `RemoteAudioStateChanged` are all
raised on native worker threads, and every one of them ran straight into
application code.

`GuardModernCallback` now wraps all seven. It contains the exception and records
`voip.callback|result=error;name=<callback>;type=<exception>`, which both stops
the fail-fast and names the offending callback on the next run. The callback
name and exception type are API surface, not user data.

Note that the truncation at `init_recording` was always a coincidence of
timing rather than a cause: the ADM call is simply the last thing written
before an unrelated thread takes the process down.

### Artifacts

```text
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\Unigram_26.9.6158.0_ARM_ModernTgCalls_CallbackGuard.appx
  57500419 bytes  SHA-256 9ADABB9366BA071731722DC5697C3EAB3D57131E173E23D431B9CD9A4AA03E38
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\Unigram_26.9.6158.0_ARM_ModernTgCalls_CallbackGuard_Sideload.zip
  64105253 bytes  SHA-256 4B180124F62F8A01F1829626664D8BBF88DE9E7D478FA5401614AA3B7F02A867
%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\ModernCallsBridge_26.9.6158.0.map
  14800124 bytes  SHA-256 7B20BA631CCF66A9A707D406A21D3EF774E9FCB8AD3C5DC7EFAC252DA978819E
```

### Two defects the guard exposed

Containing the callbacks made two pre-existing races matter, so both were fixed
in the same change.

`DisposeModernCall` is reached from the tgcalls worker thread through the
session's stopped event and from the TDLib update thread when the call is
discarded, and it previously null-checked `_modernController`, disposed it, and
only then cleared the field. Two unsynchronised callers could each pass that
check and dispose the same native session twice; `AudioCallSession`'s destructor
blocks in `StopCallSession` before `delete holder`, so a double close is a heap
corruption rather than a no-op. The field is now claimed with
`Interlocked.Exchange` and the call id and starting flag are cleared before
`Dispose` runs, so a throwing dispose can no longer leave a dead call installed
— which, with the exception now swallowed, would otherwise have made the service
reject every later call until the app was restarted.

The callback failure line is also recorded only once per callback name per call.
A null dereference recurs on every invocation, and audio levels arrive ten times
a second, so reporting each one would have drained the process-wide call
diagnostic budget within seconds and silenced the rest of the log on exactly the
run being read.

## 26.9.6160.0 — the second call, and who owns the microphone

The callback guard in 6158 paid off, but not in the way it was meant to. No
`voip.callback` line appeared anywhere in the device log, so the fatal fault was
never a managed exception in the seven tgcalls callbacks. What the log showed
instead settles the question the previous five builds were circling:

    19:17:06.722  init_recording;phase=begin
    19:17:06.907  init_recording;phase=end;faulted=0;code=0
    19:17:06.915  start_recording;phase=begin
    19:17:06.947  start_recording;phase=end;faulted=0;code=0

The first call of the process completed the entire audio pipeline, reached
`Established`, saw `remote_audio Active`, and ran for thirteen seconds. The audio
device module, `InitRecording`, and the WASAPI capture path are not broken, and
never were. The crash came from a *second* call, accepted twelve seconds later,
which died at `init_recording;phase=begin`. The first call had no
`CallStateDiscarded` and no `stopped` event anywhere in the log: it was never
torn down.

### Stop is asynchronous, and nothing downstream of it is synchronous either

`CallSession::Stop` called `_instance->stop(completion)` and the C++/CX
destructor then freed its holder and returned. Reading the pinned tgcalls source
shows how little of the teardown that actually performs:

* `InstanceImpl::stop` posts to the manager thread, which posts to the network
  thread, which posts back to the manager thread, which finally runs the
  completion inside `_mediaManager->perform`. It never stops the media pipeline.
* `~ThreadLocalObject` does not block. It `PostTask`s the held object's reset to
  its thread and returns, so `_instance.reset()` only *queues* `~Manager`, which
  in turn only queues `~MediaManager`.
* `~MediaManager` is the thing that does the real work: it drops the channels to
  `kNetworkDown`, calls `SetSend(false)` and `SetPlayout(false)`, and only then
  releases the audio device module.

So the capture endpoint stays owned across two further thread hops after the
destructor has returned. Windows 10 Mobile allows a single capture owner, and
the stock desktop WASAPI code has no handling for contention, so the second
call's `InitRecording` activated an endpoint the first call still held. That is
an access violation inside the audio stack, on a tgcalls thread, outside this
module — which is exactly the `engine=0`, `same_thread=0`, no-managed-frame
signature every one of these crashes has carried.

### What 6160 does

Teardown is now synchronous from the caller's point of view, but the shutdown
itself is left where tgcalls already does it correctly:

* The session retains the *wrapper* it hands to tgcalls rather than the platform
  module underneath it, and drops that reference inside the stop completion. The
  last reference is then `~MediaManager`'s, so tgcalls tears the device down in
  its own order, with the channels already stopped.
* `DiagnosticAudioDeviceModule`'s destructor terminates the platform module and
  signals the session. Because it runs only once every tgcalls owner has let go,
  it is the first provably-idle moment, and it is the honest answer to "is the
  microphone free".
* `StopCallSessionAndWait` blocks until the session has stopped *and* that signal
  has arrived, so the next call cannot start against a device the previous one
  owns.
* A stop completion that never arrives is now survivable. tgcalls drops it
  silently when the manager has already gone, which would pin the device for the
  life of the process, so a timeout forces the instance reset and waits out a
  short grace period.

An earlier revision of this fix stopped and terminated the device directly in the
stop completion. That was wrong for an instructive reason: the completion runs
*inside* a live `MediaManager::perform` callback, on the media thread, while
`webrtc::Call` and the send channel are still streaming into that device. It
would have moved the access violation from the second call's `InitRecording` to
the first call's teardown and looked like progress.

### Two lifetime bugs the blocking teardown exposed

Making the caller wait turned two latent races into near-deterministic ones.

The outcome used to be stored on the session and read back through a
`TeardownResult` property *after* `Dispose`. A C++/CX destructor is `Dispose`, and
reading a member of an instance whose destructor has run is undefined — the
handle is dangling, and a managed `catch` does not help, because C++/CX generates
no `RO_E_CLOSED` guard unless the class writes one. Worse for a diagnostic that
must never leak, a reused `String^` buffer would have put arbitrary heap bytes in
`result=`. Teardown is now an explicit `Teardown()` call that *returns* the token,
so nothing is read off a disposed instance.

The stop completion also published the stop and woke the waiter *before* raising
the `stopped` callback, so the disposing thread could destroy the owning object
while the native event was still being raised on its stack. The callback is now
raised first and the stop published after. For the same reason the managed
`stopped` handler no longer disposes the call: it runs on the completion thread,
inside the event raise, and disposal there would tear down the object mid-raise.
It does not need to any more — the device is released by the native side before
that callback runs, so disposal can wait for `CallStateDiscarded` or for the
stale-session sweep when the next call arrives.

### Reading the result

`voip.teardown` reports a fixed token and a coarse duration bucket:

    voip.teardown|result=drained;transport=modern_tgcalls;elapsed_ms=0
    voip.teardown|result=stale;transport=modern_tgcalls

`drained` means the device was confirmed destroyed before the next call could
start. `timeout` means it was not, and the next call is at risk. `stale` is the
sweep releasing a previous session that TDLib never discarded. A
`voip.transport|result=stopped` line should now appear for every call that ends,
which it never did before.

### Correction after device testing

The heading above frames the fault as a second-call problem. Device testing of
26.9.6160.0 disproved that framing and it is left in place only so the reasoning
history stays honest.

Both crashes in the 26.9.6160.0 log happen on the **first** call of a freshly started
process, in both directions, with no second session anywhere in the segment. The
overlapping-call reading explained the 26.9.6158.0 log but is not the general cause,
and the 26.9.6158.0 call that survived now looks like a timing fluke rather than
evidence that the capture path is sound.

The teardown defects 26.9.6160.0 fixed were real and were verified against the pinned
upstream source, so the change is kept. It is unfalsified rather than confirmed: the
log contains no `voip.teardown` and no `voip.transport|result=stopped`, so the process
died before any of the new code ran.

## 26.9.6161.0 — proving where the process actually dies

Every build so far concluded "the process dies inside `InitRecording`" from the absence
of a `step=init_recording;phase=end` line. That inference does not hold.

`phase=begin` and `phase=end` are both written by **managed** code: the device module
wrapper raises a report, the bridge marshals it across the WinMD boundary, and
`CallsService` writes the line. A death anywhere in that window produces exactly the
same evidence, so the missing `phase=end` is equally consistent with

* a fault inside the native `InitRecording`,
* a fault inside the managed report that follows it, and
* an unrelated death on another thread that merely happened to land in the window.

The 26.9.6160.0 log actively argues against the first reading. The process survived
65 ms past `step=init_recording;phase=begin` and answered a TDLib request in that time
(`tdlib.result|type=Ok`), which is not the shape of an instantaneous fault inside a
WASAPI call.

Two gaps made this impossible to settle:

1. **The vectored handler cannot see the death.** It only observes structured
   exceptions. When a managed exception escapes a reverse call the runtime ends the
   process with a fail-fast, which bypasses every handler, so that death writes nothing
   at all. `result=abort`, `terminate`, `purecall` and `invalid_parameter` are all
   absent from the log and the fault budget was never exhausted, so whatever ends the
   process is not something the existing instrumentation can record.
2. **A fault address was classified only against this module.** `engine=0` says the
   faulting instruction is not in `ModernCallsBridge.dll`, which does not distinguish
   the platform audio stack from managed code — and those need opposite fixes.

### What this build adds

**A crash-proof in-flight marker.** A 64-byte file-backed section holds the step
currently in flight. Updating it is a memory write, not a system call, so it is cheap
enough not to perturb the timing of the fault, and because the section is file-backed
the memory manager still writes the last value to disk no matter how the process ends —
fail-fast, stack overflow or outright termination included. The next launch reports it
as `result=inflight_at_exit;step=<step>:<stage>` where the stage is one of

| stage | meaning |
| --- | --- |
| `report_begin` | died in the managed `phase=begin` report |
| `native` | died inside the real audio device module call |
| `report_end` | died in the managed `phase=end` report |

This separates the three readings above in a single device test.

**Fault address classification against the app binary.** The runtime compiles managed
code into the app executable, so a faulting instruction inside it is a managed null
dereference rather than a platform failure. Faults now carry `app=0|1`, plus an
`offset=` when the address falls in either known module. The module ranges are captured
once at install time, away from any fault context, because resolving a module from a
handler would take the loader lock the faulting thread may already hold. The fault
budget rises from 8 to 24 so a repeating fault no longer hides later ones.

**A self-arming capture bypass.** If the recovered marker names a capture step, the next
call runs the device module with `InitRecording`, `StartRecording` and `StopRecording`
reporting success without touching the platform module, logged as
`phase=skipped;reason=prior_fault`. A call that then survives proves the capture path is
what ends the process, and the user gets audio in one direction while we find out. It
arms only after a death inside a capture step, so a death anywhere else leaves the
device module completely untouched and adds no noise.

Nothing here changes the call path on a healthy first call. This build is instrumentation
plus a fallback that only a crash can switch on.

### Also established this round

* The build links WebRTC's **desktop** Core Audio module, `audio_device_core_win.cc`,
  inside an app container. This is worth noting as a structural risk, but it is not yet
  implicated: device enumeration, speaker and microphone init, both stereo queries,
  `init_playout` and `start_playout` all return `code=0`.
* `_builtInAecEnabled` is false — nothing in WebRTC or tgcalls calls `EnableBuiltInAEC`,
  so `InitRecording` takes the plain WASAPI path and never touches the voice-capture DSP
  that does not exist on Windows 10 Mobile. That branch is ruled out.
* Diagnostic budget exhaustion is ruled out as a cause of the log stopping: the crashing
  segments wrote 31 and 32 media lines against a budget of 192.

## 26.9.6161.0 result — the recording path is not the cause

The in-flight marker worked and returned a clear answer on the first device test.

### What the device reported

```
voip.media|step=init_recording;phase=begin        20:59:38.1336
voip.media|step=init_recording;phase=end;faulted=0;code=0   20:59:39.1893
voip.media|step=init_recording;phase=begin/end  (the duplicate init)
voip.media|step=start_recording;phase=begin      20:59:39.2013
<process death>
voip.fault|result=inflight_at_exit;step=start_recording:native;capture_skipped=1
```

Three conclusions follow directly, and they retire a theory each earlier build was
built on.

1. **`InitRecording` succeeds.** It returns `code=0`, taking 1.05 seconds. Every
   previous build concluded the process died inside it purely because the log stopped
   after `phase=begin`. It did not. The 1-second duration is why that line was so often
   the last one written.
2. **The process died with `start_recording` in flight, in the `native` stage** — not in
   the managed reporting path. That rules out the managed reverse-callback theory for
   this death.
3. **But `StartRecording` is almost certainly a bystander.** It does very little: it
   creates the capture thread and then blocks in
   `WaitForSingleObject(_hCaptureStartedEvent, 1000)`. A step is marked in flight for the
   whole of that wait, so any death on any thread during that second is attributed to it.

### The bypass experiment settled it

The same build armed a capture bypass on the next launch. The following call ran with
`init_recording` and `start_recording` skipped entirely, reached
`voip.transport|state=Established` — **and the process still died**, this time leaving
no marker at all, because no audio device step was in flight.

So:

* Skipping the entire recording path does not keep the call alive. The recording path is
  not the cause.
* A death with no step in flight is invisible to the fault reporter, because the vectored
  handler was gated on `g_audioDeviceLifecycleDepth > 0`. That gate is the reason the
  fatal event has never been captured.

The capture bypass is retired in 26.9.6162.0. It cannot help, and leaving it armed would
remove the microphone from every subsequent call and make a surviving call impossible to
recognise.

### What this leaves

The killer runs on a thread the instrumentation does not cover, during a window the fault
reporter was not armed for. Playout is started and the transport is established in every
failing run, so the engine's own media threads are live in both the capture and the
no-capture case.

## 26.9.6163.0 — arming the instrumentation where the death actually happens

Four changes, in order of expected value. (Built as 6162 first; that build was never
released, because review found its wider reporting window could spend the log budget on
benign faults and drop the fatal one. 6163 is 6162 plus the corrections below.)

### 1. `CoreApplication.UnhandledErrorDetected` (diagnostic *and* candidate fix)

The app subscribed to `Application.UnhandledException` only, which observes the UI thread
and nothing else. The call engine raises its failures on WebRTC's threads, so a managed
failure there was never going to appear — on .NET Native it fail-fasts the process
outright, writing nothing.

`CoreApplication.UnhandledErrorDetected` is the one managed hook that sees those.
`Propagate()` rethrows the error on the handler's thread so it can be recorded, and
catching it marks the error handled. If the crash is a managed exception escaping a
native callback, this both names it and stops it being fatal.

The handler records every error but only *absorbs* a `NullReferenceException`, the shape
this app is already known to raise once per call and survive. Anything else is logged and
then rethrown, leaving it unhandled. Suppressing an arbitrary failure inside a native
callback would resume the caller with half-applied invariants and convert a crash that
names its cause into a later one that does not. Log lines are
`app.unhandled.core.suppressed` and `app.unhandled.core.fatal`.

### 2. Fault reporting armed for the whole call

`g_callActiveDepth` is incremented once the tgcalls instance exists and is decremented at
teardown (`Stop()`'s completion and `ForceRelease()`, with `~CallSession` as a backstop —
tgcalls holds a strong reference in its stop completion, so destruction alone would close
the window at an unspecified later time). The vectored handler now reports while either an
audio device step is on the stack **or** a call is live. A new `in_step=` field preserves
the old distinction, and when no step is live the report says `step=none;same_thread=0`
rather than naming a device call that finished long ago.

### 3. A mapped fault slot, so a fault outside a device step costs nothing

Widening the window admits the benign managed access violation this app raises on every
call. Writing each one to the log would spend the 24-entry budget long before the fatal
fault arrived — and an empty log is indistinguishable from a clean run — and would run
blocking file I/O on WebRTC's real-time threads, which the fault handler is explicitly not
allowed to do.

So faults raised outside a device step are written to a fixed 192-byte slot in the same
file-backed section as the in-flight marker. Each fault overwrites the last, costs no
system call and claims no budget, and because the memory manager writes dirty pages back
regardless of how the process ends, the value that survives is by construction the last
fault before the death. The next launch emits it as
`voip.fault|result=last_fault_before_exit;...`.

Faults *inside* a device step still go straight to the log as before. Their budget is now
restored at the start of every call, and the first drop within a call emits
`result=budget_exhausted` so a truncated record is never read as a clean one.

### 4. App lifecycle logging

`Suspending`, `Resuming`, `EnteredBackground`, `LeavingBackground`. Windows 10 Mobile
terminates an app that holds audio across a suspend, and in the log that is
indistinguishable from a crash. These lines separate "we faulted" from "the OS took us
out" — a theory that is still open and that nothing so far excludes.

### How to read the next log

Read these in order; the first that matches names the layer the death happens in.

* `app.unhandled.core.fatal` or `app.unhandled.core.suppressed` present → the crash is a
  managed failure off the UI thread, now named by type and stack. `suppressed` means the
  process was allowed to continue, so if the call also survives, this was the cause. This
  is the best case.
* `voip.fault|result=last_fault_before_exit;...` → the last fault the dead process saw
  while a call was live, recovered from the mapped slot. `engine=1` is bridge/WebRTC code
  and `offset=` resolves against the archived map file via `Resolve-FaultOffset.ps1`;
  `app=1` is .NET Native compiled managed code; `null_page=1` with `app=1` is a managed
  null dereference.
* `voip.fault|result=exception;...;in_step=1` → a fault raised while an audio device call
  was actually on the stack.
* `app.lifecycle|event=suspending` just before the death → the OS is suspending the app
  during the call and this was never a crash.
* `result=budget_exhausted` → reports were dropped; the record is incomplete rather than
  clean.
* Still nothing → the death is not an exception, not managed, and not a suspend, which
  points at an OS-level kill of the app container.

The capture bypass is retired: it was tested on device and the process died anyway, so it
only removed the microphone and made a surviving call impossible to recognise. The mic is
live again in this build.

### 26.9.6163.0 artifacts

Under `%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\`.

| File | SHA-256 |
| --- | --- |
| `Unigram_26.9.6163.0_ARM_ModernTgCalls_FaultSlot_Sideload.zip` | `D25092C1D702A6B11311A9D6A8371F0AC59CAFEC455E0544B156D15051CBB047` |
| `Unigram_26.9.6163.0_ARM_ModernTgCalls_FaultSlot.appx` | `FA290B13E7E97293F977B308177DAE452BAE66450B5072CC5B5F6F17CCB2B7BE` |
| `Unigram_26.9.6163.0_ARM_ModernTgCalls_FaultSlot.cer` | `5D891C3D3F5DF85A556C01BD5BA58C6837A736776E4D781CDEE9790060A72B85` |
| `ModernCallsBridge_26.9.6163.0.map` | `4B6D2DA99ECA13C09F184B05D79CE18235CF7B70A696773D111D6E6E80DAE67C` |

Packed `ModernCallsBridge.dll` `07A573AF8341252D61E9DCD6D0EA4F41354E60C31759454A6E9C8C06FE23A30F`,
identical to the freshly built binary. The map file is kept out of the package and is only
needed to resolve an `offset=` from this exact build.

## 26.9.6164.0 - the ABI mismatch, and why nothing was ever logged

This build stops adding observers. The 6163 log finally said something decisive, and what
it pointed at is a build configuration defect rather than a bug in the call code.

### What 6163 proved

Two separate results, both useful:

* The per-call managed exception is named at last: a `NullReferenceException` in
  `VoIPPage.OnSizeChanged`. It was suppressed, the process survived it by roughly 2.3
  seconds, and the call carried on normally through `AcceptCall`, `CallStateReady`,
  transport setup and the whole audio device sequence. It is a real bug, fixed in this
  build, but it was never the crash.
* The fatal death raises **no exception at all** within the filter we were using, and no
  `app.lifecycle|event=suspending` precedes it. The mapped fault slot was empty, so there
  was not even an out-of-step fault to recover. The log simply stops between
  `start_playout;phase=begin` and the relaunch.

### Root cause

`TgCallsEngine.vcxproj` compiled the tgcalls sources and this facade with a **different
ABI than the `webrtc.lib` they link against**. Comparing the project's defines against
`C:\wrtcar\src\out\msvc\uwp\Release\arm\obj\modules\audio_device\audio_device_api.ninja`
(the library was configured `is_debug = false`):

| Define | webrtc.lib | bridge, before | Effect of the mismatch |
| --- | --- | --- | --- |
| `NDEBUG` | set | **missing** | `RTC_DCHECK_IS_ON` 0 vs **1** |
| `_HAS_EXCEPTIONS=0` | set | missing | MSVC STL ODR mismatch |
| `ABSL_ALLOCATOR_NOTHROW=1` | set | missing | abseil container ABI |
| `WEBRTC_LIBRARY_IMPL` | set | missing | export and trace linkage |
| `WEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=0` | set | missing | trace handler indirection |
| `WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE` | set | missing | gates the real ADM in headers |

`NDEBUG` is not cosmetic here. It selects `RTC_DCHECK_IS_ON`, and `webrtc::SequenceChecker`
is a full mutex-and-thread-reference object when that is on and an **empty stub** when it
is off. Every WebRTC type embedding one - `AudioDeviceBuffer`, `cricket::BaseChannel`,
`TaskQueueBase`, `Mutex` - therefore has a different `sizeof` and different member offsets
on each side of the boundary. We were constructing and driving exactly those objects
across it.

That explains every symptom that previously had no explanation: a death with no exception,
no suspend and no stable location; `start_recording` in 6161 and `start_playout` in 6163;
and why bypassing the capture path never helped. It was never a step. It was memory
corruption, and the step on the marker was only whatever happened to be in flight.

### Why a failed check was invisible

`rtc_base/checks.cc` ends `FatalLog` with:

```cpp
#if defined(WEBRTC_WIN)
  DebugBreak();
#endif
  abort();
```

`DebugBreak()` raises `EXCEPTION_BREAKPOINT` (`0x80000003`). `IsFatalExceptionCode` did not
accept that code, so the handler returned `EXCEPTION_CONTINUE_SEARCH`, nothing else
handled it, and the process died leaving no record whatsoever. With DCHECKs wrongly
enabled in our translation units, any thread-affinity assertion - and `AudioDeviceBuffer`
is covered in them - would kill the process exactly this silently.

### Changes

1. **`TgCallsEngine.vcxproj`** now defines `NDEBUG`, `WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE`,
   `WEBRTC_LIBRARY_IMPL`, `WEBRTC_NON_STATIC_TRACE_EVENT_HANDLERS=0` and
   `ABSL_ALLOCATOR_NOTHROW=1`, matching the library.
2. **`IsFatalExceptionCode`** gained `IsAlwaysFatalExceptionCode`, covering
   `EXCEPTION_BREAKPOINT`, `STATUS_STACK_BUFFER_OVERRUN`, `STATUS_INVALID_CRUNTIME_PARAMETER`,
   `STATUS_HEAP_CORRUPTION` and `STATUS_ASSERTION_FAILURE`. These are fatal by construction,
   so they are written to the log wherever they occur and mirrored into the mapped slot.
   `RTC_CHECK` stays live even with `NDEBUG`, so this remains necessary.
   C++ exceptions (`0xE06D7363`) are still excluded: this facade throws
   `std::invalid_argument` as ordinary argument validation.
3. **`VoIPPage.OnSizeChanged`** returns early when disposed or before the composition
   visuals exist, and null-checks the named elements it transforms.
4. **Review fixes.** The fault slot is cleared in `DisarmFaultReporting` so a clean call
   cannot leave a stale record to be reported as `last_fault_before_exit` on the next
   launch; `PublishFaultSlot` takes a single-writer claim and publishes its first byte last
   so a torn record reads as empty rather than as a different fault; the
   `budget_exhausted` notice moved into `ClaimFaultReport` so the abrupt termination
   handlers are covered too; and the recovered line no longer carries two `result=` keys.

### Residual risk

`_HAS_EXCEPTIONS=0` is still **not** matched, because this facade throws `std::invalid_argument`
and `std::logic_error` as part of its contract with the managed layer. Mixing the two
settings is formally unsupported. If calls still fail after this build, converting those
throws to status returns and matching the flag is the next step. `NTDDI_VERSION=NTDDI_WIN10_RS2`
is also ours alone, deliberately, to keep the Windows 10 Mobile API surface.

### How to read the next log

1. `voip.fault|result=exception;code=0x80000003` → a WebRTC `RTC_CHECK` failed, and the
   `offset=` now resolves to the exact function through `ModernCallsBridge_26.9.6164.0.map`.
2. No crash, call connects → the ABI mismatch was the whole story.
3. A crash with still nothing logged → `_HAS_EXCEPTIONS` is the remaining mismatch; see
   residual risk above.

### 26.9.6164.0 artifacts

Under `%LOCALAPPDATA%\UnigramTdlibExperiment\artifacts\`.

| File | SHA-256 |
| --- | --- |
| `Unigram_26.9.6164.0_ARM_ModernTgCalls_AbiFix_Sideload.zip` | `DA0519CD872049AB4373B1EE3D254E5B59112158320760C0B9754943F660A965` |
| `Unigram_26.9.6164.0_ARM_ModernTgCalls_AbiFix.appx` | `5BF4999A3323566FAF74A610C38A48BCED20C255A6987AEFCC251DD6EA4456A0` |
| `Unigram_26.9.6164.0_ARM_ModernTgCalls_AbiFix.cer` | `5D891C3D3F5DF85A556C01BD5BA58C6837A736776E4D781CDEE9790060A72B85` |
| `ModernCallsBridge_26.9.6164.0.map` | `4832B120B62014C3511E00F360CBF86DB88813D41BE1FA4BD3D72C27569AA7DC` |

Packed `ModernCallsBridge.dll` `1AF6A2AE57EAC31970C1A487841267566E1C027864C14572ED08E3A43BB65D45`,
identical to the freshly built binary.
