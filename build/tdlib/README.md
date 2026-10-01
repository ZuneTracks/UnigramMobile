# Experimental TDLib 1.8.66 ARM UWP build

This directory contains reproducible inputs for an experimental C++/CX ARM UWP
build. Upstream source and compiled output stay outside the application
repository.

## Pinned inputs

- TDLib: `022d60202e446ad1287b9fb68e687c8a0760788b` (declares 1.8.66)
- vcpkg: `45f9f39362a4c52e2b1fbe57b7e649db7f3d96d4`
- OpenSSL: 3.5.7, from tag `openssl-3.5.7`
- zlib: 1.3.2, vcpkg port revision 2
- Generator host: Visual Studio 2022 Build Tools
- Compiler/toolset: Visual Studio v143 ARM (`14.44.35207`) for the ARM UWP build;
  v141 (`14.16.27023`) for vcpkg dependencies and the x64 host code generator
- Windows SDK: 10.0.18362.0
- UWP dependencies: dynamic CRT and dynamic libraries

### Why v143 for the ARM UWP build

TDLib 1.8.66 is 2026-era C++. Building it with v141 (MSVC 14.16, 2017) produced a
binary that corrupted the process heap during `TdDb::init_sqlite` /
`init_message_db`, which surfaced on device as an LFH double-free
(`0xC0000374`) when .NET Native freed an `HSTRING` on the update callback. Every
layer of the C++/CX string path was verified correct by disassembly, so the
fault was in code generation rather than in source.

v143 also matches the generation of `vccorlib140_app.dll` shipped by the
`Microsoft.VCLibs.140.00` **14.0.33519.0** framework package the app actually
runs against, so the inline `Platform::String` code compiled into
`Telegram.Td.dll` matches the runtime DLL implementing it.

v143 has full C++/CX ARM32 store support (`lib\arm\store\vccorlib.lib` and an
ARM `cl.exe` are both present in 14.44.35207). The generated WinMD is
byte-identical between the two toolsets, so the projected API surface is
unchanged.

Use `-Toolset v141` to reproduce the previous build for comparison.

The OpenSSL overlay is based on the pinned vcpkg port recipe because the vcpkg
registry at the pinned commit does not contain OpenSSL 3.5.7. Its source archive
SHA-512 is pinned in `overlays/openssl/portfile.cmake`. UWP builds disable only
the OpenSSL Windows certificate-store provider, which calls desktop-only
`CertOpenSystemStoreW`; TDLib does not use that provider.

`patches/0001-qualify-cx-field-types.patch` fixes the C++/CX generator to fully
qualify reference types in properties. This avoids a v141 name-hiding error when
an object has both a `Message` property and a later `Array<Message^>` property;
it does not change the projected WinMD API.

`patches/0002-project-legacy-web-page.patch` keeps the legacy `WebPage` projected
name for the modern `LinkPreview` object and exposes typed media accessors over
the direct `LinkPreviewType` variants. This is a narrow compatibility surface
for the existing message renderer; unsupported preview categories still retain
their modern typed `Type` object.

## Build

From a PowerShell prompt:

```powershell
.\build\tdlib\Build-TdlibArm.ps1 -WorkRoot C:\tdlib-build
```

The work root must be outside this repository. The script verifies both Git
commits, installs only the pinned manifest dependencies, generates the C++/CX
API with a native v141 build, configures WindowsStore ARM against SDK 18362,
and builds only `tddotnet`. Use `-VisualStudioPath` if the Build Tools instance
is installed elsewhere.

The script imports explicit developer environments before invoking vcpkg. This
avoids vcpkg selecting another same-version Visual Studio instance that does not
have the ARM compiler installed. Dependency and host-tool stages stay pinned to
`-vcvars_ver=14.16`; the ARM UWP stages follow the selected `-Toolset`.

Switching `-Toolset` clears the ARM UWP CMake cache automatically, because CMake
refuses to reuse a cache generated with a different toolset.

The existing machine-installed `Telegram.Td.UWP` Extension SDK is never changed.
Integration must consume proof-build files from the external work root, leaving
the stable SDK as a reproducible rollback path.

To verify an existing proof output and print stable SHA-256 values:

```powershell
.\build\tdlib\Verify-TdlibArm.ps1 -OutputRoot C:\tdlib-build\build-uwp-arm\RelWithDebInfo
```

Build the app only with the verified output:

```powershell
msbuild .\Unigram\Unigram\Unigram.csproj /p:Configuration=Release /p:Platform=ARM `
  /p:ModernTdlibRoot=C:\tdlib-build\build-uwp-arm\RelWithDebInfo
```

The project defaults to modern TDLib and rejects `UseModernTdlib=false`.
The verifier is also invoked during MSBuild with the expected version and
commit hard-coded, so changing build properties cannot select another TDLib
version. To rebuild from a clean pinned source checkout, delete the external
`C:\tdlib-build` work root and rerun the script; never substitute files inside
the output directory manually.

When building from Visual Studio, no separate TDLib command is required. The
project automatically invokes the pinned build as an incremental MSBuild target
when its external output is missing or older than the checked-in TDLib build
inputs. The default work root is
`%LOCALAPPDATA%\UnigramTdlibExperiment`; it contains the upstream checkout,
dependencies, intermediate builds, and final proof output, and is not committed
to this repository. Once the verified manifest is current, ordinary solution
builds reuse the output and only run the lightweight verifier.

## Application port status

The native proof build completes and produces `Telegram.Td.dll` and
`Telegram.Td.winmd`. The application project in this experimental branch is modern-TDLib-only:
`UseModernTdlib` defaults to `true`, and a build fails if it is overridden to
`false`. The legacy `Telegram.Td.UWP` Extension SDK is therefore not selectable
from this branch.
`UpdateManifest.ps1` selects the stable package identity/display name for the
default configuration and the isolated experimental identity when that property
is enabled. Modern bundles are ARM-only so they cannot accidentally include
unsupported x64 TDLib payloads. The experimental `uap:VisualElements` label is
unmistakable; the top-level `Properties/DisplayName` remains the localized
`Unigram Mobile` resource required by APPX validation.

The modern schema is not source-compatible with the 26.8 application surface.
The experimental build explicitly disables VoIP, nearby chats, and chat-folder
editing. Those areas are gated at compile time and emit a diagnostic event when
invoked. Notification registration continues to use
`RegisterDevice(DeviceTokenWindowsPush)` and preserves the existing
`PushReceiverId` session mapping and native background-task entry point.
The package manifest explicitly registers `Telegram.Td.Client` against
`Telegram.Td.dll`; the TDLib component PRI is also added as an APPX payload.
This is required for WinRT activation of the external C++/CX proof payload.

The modern Release ARM managed/XAML compile and signed APPX packaging complete
with the pinned WinMD and native payload when the ignored local
`Constants.Secret.cs` and test signing certificate are present. The PFX and
public CER remain local-only; the current test certificate thumbprint is
`B5E7EBF1650558A9D670BD1C5B9F82E24A213434`. The generated APPX and
`.appxupload` files are under `Unigram\Unigram\bin\ARM\Release\Upload` and
`Unigram\Unigram\AppPackages`. Modern chat/message reporting is compile-time
disabled until the server-driven `ReportChatResult` option flow is implemented.
Microsoft App Center (Analytics and Crashes) is also compile-time disabled on
this branch. Its UWP session tracker reaches
`Windows.System.Diagnostics.ProcessDiagnosticInfo` from a background thread, and
that WinRT server faults on Windows 10 Mobile 15254. Because the fault surfaces
inside a managed-to-COM call, .NET Native cannot marshal it and fail-fasts the
process with code `0x1007` a few seconds after launch. Keeping it enabled would
additionally publish this experimental build's telemetry, including the
signed-in user id, into the production App Center application. Unhandled and
unobserved exceptions are written to the local privacy-filtered diagnostics log
instead.
Device installation, fresh login, push, and Live Tile validation remain
outstanding.

### Composition degradations on device

`MainPage` could not be constructed on Windows 10 Mobile: the constructor threw
a `NullReferenceException` inside `DropShadowEx.Attach(FolderShadow, ...)`, which
left the app showing only the flyout menu. Three purely decorative composition
sites now report the failure and continue instead of taking the page down. Each
one logs to the diagnostics file so the degradation is visible rather than
silent, and none of them affect chat list, messaging, or notification behaviour:

- `MainPage..ctor` folder drop shadow - `main.construct.folder_shadow`.
- `MainPage..ctor` page header inset clip - `main.construct.page_header_visual`.
- `MainPage.ShowHideArchive` show/hide animation, skipped when the chat list
  template is not realized yet so `VisualTreeHelper.GetChild` returns null -
  `main.archive|result=skipped`. The end state is applied directly, so archive
  visibility stays correct; only the transition is dropped.

`DropShadowEx.Attach` additionally traces its first four calls per process
(`shadow.attach|step=...`) to identify which WinRT composition call returns null.
The budget exists because the diagnostics file is deleted once it reaches its
size cap, and unbounded tracing would evict the startup records. All of this is
gated behind `MODERN_TDLIB`; the non-modern configuration keeps the original
unguarded statements.

`Build-TdlibArm.ps1` writes `TdlibBuildManifest.txt` beside the proof payload.
It records the pinned TDLib version/commit and hashes the generated WinMD and
implementation DLL. `Verify-TdlibArm.ps1` rejects missing or mismatched
metadata, and the application build requires that verification manifest before
it can resolve the TDLib reference. This prevents a stale, floating, or
manually substituted TDLib payload from reaching the APPX.

The experimental project explicitly excludes the portable
`System.Numerics.Vectors` packages and selects the matching UWP reference
assemblies from `Microsoft.NETCore.UniversalWindowsPlatform` 6.2.10 for
compile and native interop generation. The final ARM package includes the
corresponding ARM AOT runtime pair (`4.1.4.0` and `4.0.4.0`) through explicit
APPX payload items. This avoids both the portable `4.1.1.0`/`4.0.1.0` pair
and an app/device split where the native image requests `4.1.4.0` but the
device resolves another manifest (`0x80131040`).
