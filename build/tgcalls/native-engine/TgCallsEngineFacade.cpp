#include "TgCallsEngineFacade.h"

#include "Instance.h"
#include "InstanceImpl.h"

#include "api/task_queue/task_queue_factory.h"
#include "modules/audio_device/include/audio_device.h"
#include "platform/PlatformInterface.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <stdexcept>
#include <utility>

#include <windows.h>
#include <stdlib.h>

namespace Unigram {
namespace Native {
namespace Calls {
namespace {

extern "C" {
PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
}

// Supplied by the linker at the base of the module this code is linked into, which lets
// a fault address be classified and turned into a map-file offset without calling any
// module API from a fault context.
extern "C" IMAGE_DOS_HEADER __ImageBase;

// A fault handler cannot allocate, take a lock or call into the CRT, so the diagnostics
// path is captured once up front into a fixed buffer and the report is assembled by hand.
// It writes to its own file rather than the managed diagnostics log, because the managed
// writer tracks its own end-of-file offset and would write back over an appended record.
wchar_t g_crashDiagnosticsPath[MAX_PATH] = {};
volatile LONG g_crashDiagnosticsBudget = 8;
// Only faults raised while the audio device module is being driven are recorded. A
// vectored handler sees every first-chance exception in the process, and .NET Native
// raises an access violation for each ordinary null dereference, so an ungated handler
// would spend its budget on benign exceptions long before the fatal one arrives.
volatile LONG g_audioDeviceLifecycleDepth = 0;
// Recorded so a report can say whether the fault landed on the thread that is driving
// the audio device module, or on a pool thread the structured exception guard cannot see.
volatile LONG g_audioDeviceLifecycleThread = 0;
const char* volatile g_audioDeviceLifecycleStep = nullptr;
PVOID g_crashDiagnosticsHandle = nullptr;
INIT_ONCE g_crashDiagnosticsOnce = INIT_ONCE_STATIC_INIT;

struct AudioDeviceLifecycleScope {
    explicit AudioDeviceLifecycleScope(const char* step) {
        g_audioDeviceLifecycleStep = step;
        InterlockedExchange(&g_audioDeviceLifecycleThread,
            static_cast<LONG>(GetCurrentThreadId()));
        InterlockedIncrement(&g_audioDeviceLifecycleDepth);
    }
    ~AudioDeviceLifecycleScope() { InterlockedDecrement(&g_audioDeviceLifecycleDepth); }
};

void AppendHex32(char* buffer, int& length, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    buffer[length++] = '0';
    buffer[length++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        buffer[length++] = digits[(value >> shift) & 0xF];
    }
}

void AppendLiteral(char* buffer, int& length, const char* text) {
    while (*text != '\0') {
        buffer[length++] = *text++;
    }
}

bool IsFatalExceptionCode(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return true;
    default:
        // A stack overflow is deliberately absent: the handler would run on the
        // exhausted stack and fault again before writing anything.
        return false;
    }
}

void WriteFaultLine(const char* line, int length) {
    const HANDLE file = CreateFile2(
        g_crashDiagnosticsPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        OPEN_ALWAYS,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
}

bool ClaimFaultReport() {
    if (InterlockedDecrement(&g_crashDiagnosticsBudget) < 0) {
        InterlockedIncrement(&g_crashDiagnosticsBudget);
        return false;
    }
    return true;
}

const char* CurrentLifecycleStep() {
    const char* step = g_audioDeviceLifecycleStep;
    return step == nullptr ? "unknown" : step;
}

// A fatal error that does not travel as a structured exception never reaches a vectored
// handler. WebRTC's RTC_CHECK failures end in abort(), a pure virtual call or an invalid
// CRT parameter terminate just as abruptly, and all of them would leave exactly the same
// evidence as the crash under investigation: a log that simply stops.
void ReportAbruptTermination(const char* result) {
    if (g_crashDiagnosticsPath[0] == L'\0' || !ClaimFaultReport()) {
        return;
    }

    char line[160];
    int length = 0;
    AppendLiteral(line, length, "result=");
    AppendLiteral(line, length, result);
    AppendLiteral(line, length, ";step=");
    AppendLiteral(line, length, CurrentLifecycleStep());
    AppendLiteral(line, length, ";in_lifecycle=");
    line[length++] = (g_audioDeviceLifecycleDepth > 0) ? '1' : '0';
    AppendLiteral(line, length, "\r\n");
    WriteFaultLine(line, length);
}

void AbortReporter(int) { ReportAbruptTermination("abort"); }
void TerminateReporter() { ReportAbruptTermination("terminate"); }
void PureCallReporter() { ReportAbruptTermination("purecall"); }

void InvalidParameterReporter(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    ReportAbruptTermination("invalid_parameter");
}

LONG CALLBACK FatalExceptionReporter(EXCEPTION_POINTERS* pointers) {
    if (pointers == nullptr ||
        pointers->ExceptionRecord == nullptr ||
        g_audioDeviceLifecycleDepth <= 0 ||
        !IsFatalExceptionCode(pointers->ExceptionRecord->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (!ClaimFaultReport()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const char* step = CurrentLifecycleStep();

    // The exception code alone cannot name the failure, because .NET Native raises an
    // access violation for every ordinary null dereference and this app has a known
    // benign one. What separates them is where the fault is: an offset inside this
    // module is webrtc or tgcalls code and can be resolved against the build's map
    // file, while anything outside it is another module's problem. An offset within a
    // module carries no information about the install, so it stays privacy-safe.
    const auto address = reinterpret_cast<uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        base + static_cast<uintptr_t>(__ImageBase.e_lfanew));
    const auto size = static_cast<uintptr_t>(headers->OptionalHeader.SizeOfImage);
    const bool inModule = address >= base && address < base + size;

    char line[256];
    int length = 0;
    AppendLiteral(line, length, "result=exception;code=");
    AppendHex32(line, length, static_cast<uint32_t>(pointers->ExceptionRecord->ExceptionCode));
    AppendLiteral(line, length, ";step=");
    AppendLiteral(line, length, step);
    AppendLiteral(line, length, ";same_thread=");
    line[length++] = (static_cast<LONG>(GetCurrentThreadId()) == g_audioDeviceLifecycleThread) ? '1' : '0';
    AppendLiteral(line, length, ";engine=");
    line[length++] = inModule ? '1' : '0';
    if (inModule) {
        AppendLiteral(line, length, ";offset=");
        AppendHex32(line, length, static_cast<uint32_t>(address - base));
    }

    // For an access violation the operation and whether the target was a null page
    // separate a managed null dereference from genuine memory corruption.
    if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        pointers->ExceptionRecord->NumberParameters >= 2) {
        const auto operation = pointers->ExceptionRecord->ExceptionInformation[0];
        AppendLiteral(line, length, ";access=");
        AppendLiteral(line, length, operation == 0 ? "read" : (operation == 1 ? "write" : "execute"));
        AppendLiteral(line, length, ";null_page=");
        line[length++] = (pointers->ExceptionRecord->ExceptionInformation[1] < 0x10000) ? '1' : '0';
    }

    AppendLiteral(line, length, ";noncontinuable=");
    line[length++] = (pointers->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? '1' : '0';
    AppendLiteral(line, length, "\r\n");

    WriteFaultLine(line, length);

    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL CALLBACK InstallFatalExceptionReporter(PINIT_ONCE, PVOID, PVOID*) {
    // The header gates this out of the app partition, but the export exists and is
    // permitted for store apps; declaring it keeps the UWP surface otherwise untouched.
    g_crashDiagnosticsHandle = AddVectoredExceptionHandler(1, FatalExceptionReporter);
    signal(SIGABRT, AbortReporter);
    std::set_terminate(TerminateReporter);
    _set_purecall_handler(PureCallReporter);
    _set_invalid_parameter_handler(InvalidParameterReporter);
    return TRUE;
}

// The audio device module lifecycle calls below run once per call and are the last
// thing to execute before the process has been observed to die mid-call, so each one
// is bracketed by a breadcrumb and executed under a structured exception guard. The
// guard converts an access violation inside the platform audio stack into a reported
// failure code instead of terminating the app, which both names the faulting step and
// lets the rest of the call continue so the remaining diagnostics can still be
// collected. These helpers take no objects requiring unwinding, which is what allows
// __try to be used here.

using AdmPlainMethod = int32_t (webrtc::AudioDeviceModule::*)();
using AdmBoolOutMethod = int32_t (webrtc::AudioDeviceModule::*)(bool*);
using AdmConstBoolOutMethod = int32_t (webrtc::AudioDeviceModule::*)(bool*) const;
using AdmBoolInMethod = int32_t (webrtc::AudioDeviceModule::*)(bool);
using AdmWindowsDeviceMethod = int32_t (webrtc::AudioDeviceModule::*)(webrtc::AudioDeviceModule::WindowsDeviceType);

__declspec(noinline) int32_t InvokeGuarded(
        webrtc::AudioDeviceModule* impl,
        AdmPlainMethod method,
        int* faulted) {
    __try {
        return (impl->*method)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        return -1;
    }
}

__declspec(noinline) int32_t InvokeGuarded(
        webrtc::AudioDeviceModule* impl,
        AdmBoolOutMethod method,
        bool* argument,
        int* faulted) {
    __try {
        return (impl->*method)(argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        return -1;
    }
}

__declspec(noinline) int32_t InvokeGuarded(
        const webrtc::AudioDeviceModule* impl,
        AdmConstBoolOutMethod method,
        bool* argument,
        int* faulted) {
    __try {
        return (impl->*method)(argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        return -1;
    }
}

__declspec(noinline) int32_t InvokeGuarded(
        webrtc::AudioDeviceModule* impl,
        AdmBoolInMethod method,
        bool argument,
        int* faulted) {
    __try {
        return (impl->*method)(argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        return -1;
    }
}

__declspec(noinline) int32_t InvokeGuarded(
        webrtc::AudioDeviceModule* impl,
        AdmWindowsDeviceMethod method,
        webrtc::AudioDeviceModule::WindowsDeviceType argument,
        int* faulted) {
    __try {
        return (impl->*method)(argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *faulted = 1;
        return -1;
    }
}

/// <summary>
/// Forwards every audio device module call to the real platform module, reporting only
/// fixed step names and numeric result codes. No device names, identifiers, or audio
/// content are read or reported.
/// </summary>
class DiagnosticAudioDeviceModule : public tgcalls::DefaultWrappedAudioDeviceModule {
public:
    DiagnosticAudioDeviceModule(
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> impl,
        std::function<void(const std::string&)> report)
        : tgcalls::DefaultWrappedAudioDeviceModule(impl)
        , _raw(impl.get())
        , _report(std::move(report)) {
    }

    int32_t Init() override { return Guarded("init", &webrtc::AudioDeviceModule::Init); }
    int32_t Terminate() override { return Guarded("terminate", &webrtc::AudioDeviceModule::Terminate); }
    int32_t InitSpeaker() override { return Guarded("init_speaker", &webrtc::AudioDeviceModule::InitSpeaker); }
    int32_t InitMicrophone() override { return Guarded("init_microphone", &webrtc::AudioDeviceModule::InitMicrophone); }
    int32_t InitPlayout() override { return Guarded("init_playout", &webrtc::AudioDeviceModule::InitPlayout); }
    int32_t InitRecording() override { return Guarded("init_recording", &webrtc::AudioDeviceModule::InitRecording); }
    int32_t StartPlayout() override { return Guarded("start_playout", &webrtc::AudioDeviceModule::StartPlayout); }
    int32_t StopPlayout() override { return Guarded("stop_playout", &webrtc::AudioDeviceModule::StopPlayout); }
    int32_t StartRecording() override { return Guarded("start_recording", &webrtc::AudioDeviceModule::StartRecording); }
    int32_t StopRecording() override { return Guarded("stop_recording", &webrtc::AudioDeviceModule::StopRecording); }

    int32_t PlayoutIsAvailable(bool* available) override {
        return Guarded("playout_available", &webrtc::AudioDeviceModule::PlayoutIsAvailable, available);
    }

    int32_t RecordingIsAvailable(bool* available) override {
        return Guarded("recording_available", &webrtc::AudioDeviceModule::RecordingIsAvailable, available);
    }

    using tgcalls::DefaultWrappedAudioDeviceModule::SetPlayoutDevice;
    using tgcalls::DefaultWrappedAudioDeviceModule::SetRecordingDevice;

    int32_t SetPlayoutDevice(WindowsDeviceType device) override {
        return Guarded(
            "set_playout_device",
            static_cast<AdmWindowsDeviceMethod>(&webrtc::AudioDeviceModule::SetPlayoutDevice),
            device);
    }

    int32_t SetRecordingDevice(WindowsDeviceType device) override {
        return Guarded(
            "set_recording_device",
            static_cast<AdmWindowsDeviceMethod>(&webrtc::AudioDeviceModule::SetRecordingDevice),
            device);
    }

    int32_t StereoPlayoutIsAvailable(bool* available) const override {
        return GuardedConst("stereo_playout_available", &webrtc::AudioDeviceModule::StereoPlayoutIsAvailable, available);
    }

    int32_t StereoRecordingIsAvailable(bool* available) const override {
        return GuardedConst("stereo_recording_available", &webrtc::AudioDeviceModule::StereoRecordingIsAvailable, available);
    }

    int32_t SetStereoPlayout(bool enable) override {
        return Guarded("set_stereo_playout", &webrtc::AudioDeviceModule::SetStereoPlayout, enable);
    }

    int32_t SetStereoRecording(bool enable) override {
        return Guarded("set_stereo_recording", &webrtc::AudioDeviceModule::SetStereoRecording, enable);
    }

private:
    template <typename Method, typename... Args>
    int32_t Guarded(const char* step, Method method, Args... arguments) const {
        AudioDeviceLifecycleScope scope(step);
        Report(std::string("step=") + step + ";phase=begin");
        int faulted = 0;
        const auto result = InvokeGuarded(_raw, method, arguments..., &faulted);
        Report(std::string("step=") + step + ";phase=end;faulted=" + std::to_string(faulted) +
            ";code=" + std::to_string(result));
        return result;
    }

    template <typename Method, typename... Args>
    int32_t GuardedConst(const char* step, Method method, Args... arguments) const {
        AudioDeviceLifecycleScope scope(step);
        Report(std::string("step=") + step + ";phase=begin");
        int faulted = 0;
        const auto result = InvokeGuarded(
            static_cast<const webrtc::AudioDeviceModule*>(_raw), method, arguments..., &faulted);
        Report(std::string("step=") + step + ";phase=end;faulted=" + std::to_string(faulted) +
            ";code=" + std::to_string(result));
        return result;
    }

    void Report(const std::string& value) const {
        if (_report) {
            _report(value);
        }
    }

    webrtc::AudioDeviceModule* _raw;
    std::function<void(const std::string&)> _report;
};

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const auto length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length == 0) {
        throw std::invalid_argument("The call configuration contains invalid UTF-16 text.");
    }

    std::string result(length, '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr) == 0) {
        throw std::invalid_argument("The call configuration contains invalid UTF-16 text.");
    }

    return result;
}

tgcalls::NetworkType ToTgCallsNetworkType(NetworkType value) {
    return static_cast<tgcalls::NetworkType>(value);
}

CallState ToFacadeState(tgcalls::State value) {
    return static_cast<CallState>(value);
}

RemoteAudioState ToFacadeAudioState(tgcalls::AudioState value) {
    return value == tgcalls::AudioState::Active
        ? RemoteAudioState::Active
        : RemoteAudioState::Muted;
}

}

class CallSession final : public std::enable_shared_from_this<CallSession> {
public:
    static CallSessionPtr Create(
        const CallConfiguration& configuration,
        CallCallbacks callbacks) {
        if (configuration.version.empty()) {
            throw std::invalid_argument("A TgCalls protocol version is required.");
        }
        if (configuration.encryptionKey.size() != tgcalls::EncryptionKey::kSize) {
            throw std::invalid_argument("The TgCalls encryption key must contain exactly 256 bytes.");
        }

        return CallSessionPtr(new CallSession(configuration, std::move(callbacks)));
    }

    void Start() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_started) {
            throw std::logic_error("The TgCalls session has already started.");
        }

        tgcalls::Register<tgcalls::InstanceImpl>();

        auto encryptionKey = std::make_shared<std::array<uint8_t, tgcalls::EncryptionKey::kSize>>();
        std::copy(
            _configuration.encryptionKey.begin(),
            _configuration.encryptionKey.end(),
            encryptionKey->begin());
        auto endpoints = std::vector<tgcalls::Endpoint>{};
        endpoints.reserve(_configuration.endpoints.size());
        for (const auto& endpoint : _configuration.endpoints) {
            if (endpoint.peerTag.size() != 16) {
                throw std::invalid_argument("Every reflector endpoint peer tag must contain exactly 16 bytes.");
            }

            auto target = tgcalls::Endpoint{};
            target.endpointId = endpoint.id;
            target.host.ipv4 = ToUtf8(endpoint.ipv4);
            target.host.ipv6 = ToUtf8(endpoint.ipv6);
            target.port = endpoint.port;
            target.type = endpoint.isTcp
                ? tgcalls::EndpointType::TcpRelay
                : tgcalls::EndpointType::UdpRelay;
            std::copy(endpoint.peerTag.begin(), endpoint.peerTag.end(), target.peerTag);
            endpoints.push_back(std::move(target));
        }

        auto rtcServers = std::vector<tgcalls::RtcServer>{};
        rtcServers.reserve(_configuration.rtcServers.size());
        for (const auto& server : _configuration.rtcServers) {
            auto target = tgcalls::RtcServer{};
            target.id = server.id;
            target.host = ToUtf8(server.host);
            target.port = server.port;
            target.login = ToUtf8(server.username);
            target.password = ToUtf8(server.password);
            target.isTurn = server.isTurn;
            target.isTcp = server.isTcp;
            rtcServers.push_back(std::move(target));
        }

        const auto weak = std::weak_ptr<CallSession>(shared_from_this());
        auto descriptor = tgcalls::Descriptor{
            .version = ToUtf8(_configuration.version),
            .config = {
                .initializationTimeout = _configuration.initializationTimeout,
                .receiveTimeout = _configuration.receiveTimeout,
                .enableP2P = _configuration.enableP2P,
                .allowTCP = _configuration.allowTcp,
                .enableAEC = true,
                .enableNS = true,
                .enableAGC = true,
                .enableVolumeControl = true,
                .maxApiLayer = _configuration.maxApiLayer,
            },
            .endpoints = std::move(endpoints),
            .rtcServers = std::move(rtcServers),
            .initialNetworkType = ToTgCallsNetworkType(_configuration.initialNetworkType),
            .encryptionKey = tgcalls::EncryptionKey(encryptionKey, _configuration.isOutgoing),
            .stateUpdated = [weak](tgcalls::State state) {
                if (const auto strong = weak.lock()) {
                    strong->StateChanged(ToFacadeState(state));
                }
            },
            .signalBarsUpdated = [weak](int bars) {
                if (const auto strong = weak.lock()) {
                    strong->SignalBarsChanged(bars);
                }
            },
            .audioLevelUpdated = [weak](float level) {
                if (const auto strong = weak.lock()) {
                    strong->AudioLevelChanged(level);
                }
            },
            .remoteMediaStateUpdated = [weak](tgcalls::AudioState audio, tgcalls::VideoState) {
                if (const auto strong = weak.lock()) {
                    strong->RemoteAudioStateChanged(ToFacadeAudioState(audio));
                }
            },
            .signalingDataEmitted = [weak](const std::vector<uint8_t>& data) {
                if (const auto strong = weak.lock()) {
                    strong->SignalingData(data);
                }
            },
            .createAudioDeviceModule = [weak](webrtc::TaskQueueFactory* factory)
                    -> webrtc::scoped_refptr<webrtc::AudioDeviceModule> {
                // MediaManager falls back to the same platform default when this returns
                // null, so creating it here changes no behaviour; it exists purely to make
                // an audio device failure observable instead of silently muting the call.
                auto report = std::string("created=0");
                auto module = webrtc::AudioDeviceModule::Create(
                    webrtc::AudioDeviceModule::kPlatformDefaultAudio,
                    factory);
                if (module) {
                    const auto initialized = module->Init();
                    report = "created=1;init=" + std::to_string(initialized);
                    if (initialized == 0) {
                        report += ";playout_devices=" + std::to_string(module->PlayoutDevices());
                        report += ";recording_devices=" + std::to_string(module->RecordingDevices());
                    }
                }

                webrtc::scoped_refptr<webrtc::AudioDeviceModule> result = module;
                if (module) {
                    result = rtc::make_ref_counted<DiagnosticAudioDeviceModule>(
                        module,
                        [weak](const std::string& step) {
                            if (const auto strong = weak.lock()) {
                                strong->AudioDeviceReport(step);
                            }
                        });
                }

                if (const auto strong = weak.lock()) {
                    strong->RetainAudioDeviceModule(module);
                    strong->AudioDeviceReport(report);
                }
                return result;
            },
        };

        _instance = tgcalls::Meta::Create(descriptor.version, std::move(descriptor));
        if (!_instance) {
            throw std::invalid_argument("The requested TgCalls protocol version is not registered.");
        }

        _started = true;
    }

    void ReceiveSignalingData(std::vector<uint8_t> data) {
        std::lock_guard<std::mutex> lock(_mutex);
        EnsureActive();
        _instance->receiveSignalingData(data);
    }

    void SetMuted(bool value) {
        std::lock_guard<std::mutex> lock(_mutex);
        EnsureActive();
        _instance->setMuteMicrophone(value);
    }

    void SetNetworkType(NetworkType value) {
        std::lock_guard<std::mutex> lock(_mutex);
        EnsureActive();
        _instance->setNetworkType(ToTgCallsNetworkType(value));
    }

    /// <summary>
    /// Reads the live recording and playout state of the retained audio device module.
    /// Reported as fixed keys with boolean values only.
    /// </summary>
    std::string AudioDeviceStatus() {
        webrtc::scoped_refptr<webrtc::AudioDeviceModule> module;
        {
            std::lock_guard<std::mutex> lock(_audioDeviceMutex);
            module = _audioDeviceModule;
        }

        if (!module) {
            return "created=0";
        }

        return std::string("created=1;recording=") + (module->Recording() ? "1" : "0") +
            ";playing=" + (module->Playing() ? "1" : "0");
    }

    void Stop() {
        std::unique_lock<std::mutex> lock(_mutex);
        if (!_started) {
            return;
        }
        if (_stopping) {
            return;
        }
        EnsureActive();
        _stopping = true;
        const auto self = shared_from_this();
        _instance->stop([self](tgcalls::FinalState) {
            CallCallbacks callbacks;
            {
                std::lock_guard<std::mutex> completionLock(self->_mutex);
                self->_instance.reset();
                callbacks = self->_callbacks;
            }
            if (callbacks.stopped) {
                callbacks.stopped(true);
            }
        });
    }

private:
    explicit CallSession(
        const CallConfiguration& configuration,
        CallCallbacks callbacks)
        : _configuration(configuration), _callbacks(std::move(callbacks)) {
    }

    void EnsureActive() const {
        if (!_started || !_instance || _stopping) {
            throw std::logic_error("The TgCalls session is not active.");
        }
    }

    void StateChanged(CallState state) {
        if (_callbacks.stateChanged) {
            _callbacks.stateChanged(state);
        }
    }

    void SignalingData(const std::vector<uint8_t>& data) {
        if (_callbacks.signalingData) {
            _callbacks.signalingData(data);
        }
    }

    void SignalBarsChanged(int bars) {
        if (_callbacks.signalBarsChanged) {
            _callbacks.signalBarsChanged(bars);
        }
    }

    void AudioLevelChanged(float level) {
        if (_callbacks.audioLevelChanged) {
            _callbacks.audioLevelChanged(level);
        }
    }

    void RemoteAudioStateChanged(RemoteAudioState state) {
        if (_callbacks.remoteAudioStateChanged) {
            _callbacks.remoteAudioStateChanged(state);
        }
    }

    void AudioDeviceReport(const std::string& report) {
        if (_callbacks.audioDeviceReport) {
            _callbacks.audioDeviceReport(report);
        }
    }

    void RetainAudioDeviceModule(webrtc::scoped_refptr<webrtc::AudioDeviceModule> module) {
        std::lock_guard<std::mutex> lock(_audioDeviceMutex);
        _audioDeviceModule = std::move(module);
    }

    std::mutex _mutex;
    std::mutex _audioDeviceMutex;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> _audioDeviceModule;
    CallConfiguration _configuration;
    std::unique_ptr<tgcalls::Instance> _instance;
    CallCallbacks _callbacks;
    bool _started = false;
    bool _stopping = false;
};

const wchar_t* GetFirstSupportedVersion() {
    static const std::wstring version = GetSupportedVersions().front();
    return version.c_str();
}

void EnableCrashDiagnostics(const std::wstring& diagnosticsFilePath) {
    if (diagnosticsFilePath.empty() || diagnosticsFilePath.size() >= MAX_PATH) {
        return;
    }

    std::memcpy(
        g_crashDiagnosticsPath,
        diagnosticsFilePath.c_str(),
        (diagnosticsFilePath.size() + 1) * sizeof(wchar_t));
    InitOnceExecuteOnce(&g_crashDiagnosticsOnce, InstallFatalExceptionReporter, nullptr, nullptr);
}

std::vector<std::wstring> GetSupportedVersions() {
    static const std::vector<std::wstring> versions = [] {
        tgcalls::Register<tgcalls::InstanceImpl>();
        const auto registeredVersions = tgcalls::Meta::Versions();
        auto result = std::vector<std::wstring>{};
        result.reserve(registeredVersions.size());
        for (const auto& version : registeredVersions) {
            result.emplace_back(version.begin(), version.end());
        }
        return result;
    }();

    return versions;
}

CallSessionPtr CreateCallSession(
    const CallConfiguration& configuration,
    CallCallbacks callbacks) {
    return CallSession::Create(configuration, std::move(callbacks));
}

void StartCallSession(const CallSessionPtr& session) {
    if (!session) {
        throw std::invalid_argument("The TgCalls session is unavailable.");
    }
    session->Start();
}

void ReceiveSignalingData(const CallSessionPtr& session, std::vector<uint8_t> data) {
    if (!session) {
        throw std::invalid_argument("The TgCalls session is unavailable.");
    }
    session->ReceiveSignalingData(std::move(data));
}

void SetMuted(const CallSessionPtr& session, bool value) {
    if (!session) {
        throw std::invalid_argument("The TgCalls session is unavailable.");
    }
    session->SetMuted(value);
}

void SetNetworkType(const CallSessionPtr& session, NetworkType value) {
    if (!session) {
        throw std::invalid_argument("The TgCalls session is unavailable.");
    }
    session->SetNetworkType(value);
}

std::string GetAudioDeviceStatus(const CallSessionPtr& session) {
    if (!session) {
        return "created=0";
    }
    return session->AudioDeviceStatus();
}

void StopCallSession(const CallSessionPtr& session) {
    if (!session) {
        throw std::invalid_argument("The TgCalls session is unavailable.");
    }
    session->Stop();
}

}
}
}
