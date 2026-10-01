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

#### `Visual.RelativeSizeAdjustment` is reported present but is unusable

Device tracing showed `DropShadowEx.Attach` completing successfully
(`shadow.attach|step=done`) immediately before the `MainPage` fault, which
isolated the throw to the statement after it: `RelativeSizeAdjustment`.
That property lives on `IVisual2`, introduced in UniversalApiContract v5
(Windows 10 1709), so on this Windows 10 Mobile build the interface query fails
and the setter faults with a bare `NullReferenceException` under .NET Native.

The same property was also the reason **tapping a chat did nothing**:
`ChatView.xaml` instantiates `controls:StickerPanel` eagerly, that constructor
set `RelativeSizeAdjustment`, and the XAML parser wrapped the resulting
`NullReferenceException` as a `XamlParseException` (`HRESULT 0x802B000A`), which
aborted navigation to the chat page with no visible error.

**`ApiInformation.IsPropertyPresent` cannot be used to gate this property.** The
first fix probed it per member, and the device capability line proved the probe
is a false positive: `visual_relative_size=True` on a `universal_contract=4`
device, while the setter still failed with `hresult=0x80004003` (`E_POINTER`) at
all five converted call sites. `DropShadowEx.SetRelativeSize` therefore gates on
`ApiInfo.IsUniversalApiContract5Present` instead, which is also the convention
the rest of `ApiInfo` already follows for contract v5 members — the per-member
probes there are deliberately commented out in favour of the contract check.
When the contract is absent the shadow visual is sized from the host element and
kept in sync through `SizeChanged`, so the shadows still render.
The `#else` (non-`MODERN_TDLIB`) branches keep the original statements.

The call sites converted are `MainPage..ctor`, `StickerPanel..ctor`,
`EmojiDrawer..ctor`, `AnimationDrawer..ctor`, `StickerDrawer..ctor` and
`ChatBackgroundPresenter.UpdateBlurred`. The first sweep missed the three
drawers because the search glob did not recurse past one directory level; the
`EmojiDrawer` constructor was what actually aborted chat navigation.

Because the XAML parser turns any throwing constructor into a navigation
failure, the decorative composition in `StickerPanel`, `EmojiDrawer`,
`AnimationDrawer` and `StickerDrawer` is additionally wrapped in a reporting
`try`/`catch` (`drawer.construct|drawer=...`). These controls are instantiated
eagerly by `ChatView.xaml`, so a single unavailable composition member in any of
them would otherwise make chats impossible to open.

#### Supergroup chats crashed on the pinned-message path

Opening certain supergroups terminated the app. The diagnostics correlated the
crash precisely: `SupergroupFullInfo` → `ChatAdministrators` →
`GetChatMessageCount` → `app.unhandled` 16 ms later, then a fresh
`startup.app|stage=configure`. Chats without a pinned message were unaffected,
which is why one group crashed consistently and another never did.

`DialogViewModel.LoadPinnedMessagesSliceAsync` is an `async void` method, so any
fault inside it goes straight to `Application.UnhandledException` — which under
.NET Native receives an exception whose stack has **already been discarded** —
and takes the process down. That is why all four `app.unhandled` entries carried
no `stack=` field and could not be localised from the log alone.

The method is now a thin wrapper that awaits
`LoadPinnedMessagesSliceCoreAsync` inside a reporting `try`/`catch`
(`pinned.load`). A failure now records a usable stack and leaves the chat open
without its pinned-message banner instead of terminating the app.

`PushDiagnostics.WriteException` additionally falls back to a sanitised
`Exception.ToString()` as `detail=` when `StackTrace` is empty, so a stripped
unhandled exception still carries whatever locator information remains.

On device that guard worked as intended and produced the stack the unhandled
handler could not: the fault is in `MessagePinned.UpdateMessage`, reached from
`ChatView.UpdatePinnedMessage`. It is raised when `GetChatMessageCount` reports
a non-zero count, so only chats that actually have a pinned message enter the
banner's cross-fade — confirming the one-group-crashes-and-one-does-not split.

`MessagePinned.UpdateMessage` now takes a non-animated path when
`ApiInfo.CanUseDirectComposition` is false. On those devices `MessagePinnedLine`
and `NumericTextBlock` both disable themselves in their constructors, so the
cross-fade animates nothing and only risks the scoped batch and visual
animations it has to set up first. The method already branched on the same
capability to decide the title text, so this extends an existing distinction
rather than introducing one. `ShowHide` additionally falls back to toggling
`Visibility` when `InitializeParent` has not run, because
`ElementCompositionPreview.GetElementVisual(null)` throws out through WinRT.

Because the exception unwinds through a WinRT frame no managed stack survives to
name the exact statement, so `UpdateMessage` and `ShowHide` emit budgeted
`pinned.ui|step=` traces. The budget (60 entries per process) exists for the
same reason as the `shadow.attach` one: the diagnostics file is deleted at its
size cap, and unbounded tracing would evict the records that matter.

#### Opening a chat crashed while its history streamed in

With the pinned-message fault contained, the same chat still terminated the app
a few seconds later, after a long run of `tdlib.result|type=Message` entries —
message rendering rather than the banner.

`ChatView.OnCollectionChanged` animates the messages around an insert or a
removal, and both of its loops start at `panel.FirstCacheIndex`:

```csharp
for (int i = panel.FirstCacheIndex; i <= args.NewStartingIndex; i++)
{
    var container = Messages.ContainerFromIndex(i) as SelectorItem;
    var child = VisualTreeHelper.GetChild(container, 0) as UIElement;
```

`FirstCacheIndex` is `-1` until the panel has been measured, and
`ContainerFromIndex` returns null for any index the panel has not realised.
`VisualTreeHelper.GetChild` is a WinRT call, so a null container does not raise
a catchable managed exception — it throws across a native frame, and .NET Native
tears the process down there.

That is also why `App.OnUnhandledException` could not save it. The handler
already sets `e.Handled = true`, but `Handled` only suppresses termination for
exceptions the XAML framework can unwind; once the throw has crossed a native
callback frame the process is going down regardless. This is the reason faults
have to be caught at their managed source rather than centrally, and it is worth
remembering before trusting the global handler again.

Both loops now skip unrealised containers and containers with no visual child,
and `OnCollectionChanged` — another `async void` — delegates to
`OnCollectionChangedCoreAsync` inside a reporting `try`/`catch`
(`messages.collection`). The cost of a skipped entry is one missing slide
animation.

#### Composition members verified as already gated

A recursive sweep of every `CreateShapeVisual`, `CreateLinearGradientBrush` and
`CreateSpringVector3Animation` call site confirmed that all of them except
`StorageChart` are already behind `ApiInfo.CanUseDirectComposition`,
`ApiInfo.IsFullExperience` or `IsUniversalApiContract7Present`, or are dead code
(`FileButton.OnPauseToPlay` / `OnPlayToPause` are commented out at their call
sites). `ChatActionIndicator` is gated inside `UpdateAction`, and
`MessagePinnedLine`, `NumericTextBlock` and `ProgressBarRing` all null-check the
fields their guarded constructors leave unset.

`StorageChart` was the one genuinely unguarded user: its constructor called
`CreateShapeVisual` unconditionally and `ArrangeOverride`, `SetItems` and
`Update` dereferenced the result. It is now gated the same way, with null checks
on each member, so the storage page renders without the ring chart rather than
faulting.

A later re-sweep found one more: `ChatView.Autocomplete_SizeChanged` called
`CreateSpringVector3Animation` unconditionally. Spring animations are
UniversalApiContract v7, and the device reports `create_spring_vector3=False`,
so the autocomplete list would have faulted as it grew while typing. It is now
gated on `ApiInfo.CanUseDirectComposition`.

Two related sites are deliberately left alone. `ColorSlider` has one
`CreateSpringVector3Animation` block behind `if (false)` (dead) and another
gated on `IsUniversalApiContract5Present`. That second guard is the wrong
contract — the API is v7, so it would still fault on a contract v5 or v6 device
— but it is correctly skipped on this contract v4 device, sits in the photo
editor rather than on any in-scope path, and is pre-existing. It is recorded
here rather than changed.

#### Calls are intentionally unavailable

`voip.disabled|result=unsupported;feature=experimental_tdlib` in the diagnostics
is expected, not a regression. VoIP is one of the features explicitly disabled
for this experimental build (see the disabled-feature list above); the modern
TDLib call API was out of the ported scope. Placing or receiving a call logs
that line and does nothing else.

#### Device capability reporting

`ApiInfo.WriteCapabilities` runs once per process from the `App` constructor and
records `device.capabilities|universal_contract=...` along with the presence of
the individual composition members this port depends on. Moving to a newer TDLib
did not change the OS, but it did exercise code paths whose availability had not
been tested on this hardware, and .NET Native reports an unavailable WinRT member
as a bare `NullReferenceException` rather than a typed error. The line contains
API surface metadata only - no user data, identifiers, or paths.

#### Navigation failure reporting
`FrameFacade.Navigate` records the target page name and the full exception
(`navigate.page|page=...`) before rethrowing. A page whose XAML fails to load is
otherwise only visible as an unhandled HRESULT with no stack and no indication
of which page was involved.

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
