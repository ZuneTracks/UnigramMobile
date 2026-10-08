#include "ModernCallsBridge.h"
#include "TgCallsEngineFacade.h"
#include <windows.h>
#include <windows.applicationmodel.core.h>
#include <exception>
#include <windows.ui.core.h>
#include <windows.storage.streams.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace Platform;
using namespace Windows::Foundation::Collections;
using namespace Platform::Collections;

namespace Unigram {
namespace Native {
namespace Calls {
namespace Proof {

void EnsureUwpTlsSupport();

namespace {

MIDL_INTERFACE("905a0fe0-bc53-11df-8c49-001e4fc686da")
IBufferByteAccess : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Buffer(byte** value) = 0;
};

struct SessionHolder {
    Unigram::Native::Calls::CallSessionPtr session;
    WeakReference owner;
    Windows::UI::Core::CoreDispatcher^ dispatcher;
};

// Teardown normally drains in well under a second; the bound only exists so a wedged
// tgcalls stop cannot hang the thread that disposes the call.
constexpr int kTeardownTimeoutMilliseconds = 3000;

String^ ToPlatformString(const std::exception& error) {
    return ref new String(std::wstring(error.what(), error.what() + strlen(error.what())).c_str());
}

String^ ToPlatformString(const std::string& value) {
    return ref new String(std::wstring(value.begin(), value.end()).c_str());
}

std::vector<uint8_t> ToNativeBytes(IVector<unsigned char>^ values) {
    if (values == nullptr) {
        throw ref new InvalidArgumentException(L"A byte vector is required.");
    }

    auto result = std::vector<uint8_t>{};
    result.reserve(values->Size);
    for each (const auto value in values) {
        result.push_back(value);
    }
    return result;
}

Unigram::Native::Calls::NetworkType ToNativeNetworkType(NetworkType value) {
    return static_cast<Unigram::Native::Calls::NetworkType>(value);
}

AudioCallSession^ ResolveOwner(const std::shared_ptr<SessionHolder>& holder) {
    return holder->owner.Resolve<AudioCallSession>();
}

std::shared_ptr<SessionHolder>& GetSessionHolder(void* value) {
    if (value == nullptr) {
        throw ref new InvalidArgumentException(L"The audio call session has been disposed.");
    }
    return *static_cast<std::shared_ptr<SessionHolder>*>(value);
}

}

ReflectorEndpoint::ReflectorEndpoint() {
    PeerTag = ref new Vector<unsigned char>();
}

RtcServer::RtcServer() {
}

AudioCallConfiguration::AudioCallConfiguration() {
    EncryptionKey = ref new Vector<unsigned char>();
    ReflectorEndpoints = ref new Vector<ReflectorEndpoint^>();
    RtcServers = ref new Vector<RtcServer^>();
}

VideoFrame::VideoFrame() {
}

void AudioCallSession::ReportVideoFrameDeliveryFailure(int result) {
    try {
        VideoFrameDeliveryFailed(this, result);
    } catch (Platform::Exception^) {
        OutputDebugStringW(L"ModernCallsBridge video frame failure notification failed.\n");
    }
}

AudioCallSession^ AudioCallSession::Create(AudioCallConfiguration^ configuration) {
    if (configuration == nullptr) {
        throw ref new InvalidArgumentException(L"An audio call configuration is required.");
    }
    return ref new AudioCallSession(configuration);
}

AudioCallSession::AudioCallSession(AudioCallConfiguration^ configuration) : _holder(nullptr) {
    if (configuration->Version == nullptr || configuration->Version->IsEmpty()) {
        throw ref new InvalidArgumentException(L"A TgCalls protocol version is required.");
    }

    auto holder = std::make_shared<SessionHolder>();
    holder->owner = WeakReference(this);
    auto mainView = Windows::ApplicationModel::Core::CoreApplication::MainView;
    if (mainView != nullptr && mainView->CoreWindow != nullptr) {
        holder->dispatcher = mainView->CoreWindow->Dispatcher;
    }

    auto native = Unigram::Native::Calls::CallConfiguration{};
    native.version = configuration->Version->Data();
    native.initializationTimeout = configuration->InitializationTimeout;
    native.receiveTimeout = configuration->ReceiveTimeout;
    native.enableP2P = configuration->EnableP2P;
    native.allowTcp = configuration->AllowTcp;
    native.maxApiLayer = configuration->MaxApiLayer;
    native.isOutgoing = configuration->IsOutgoing;
    native.isVideo = configuration->IsVideo;
    native.cameraDeviceId = configuration->CameraDeviceId == nullptr
        ? L""
        : configuration->CameraDeviceId->Data();
    native.initialNetworkType = ToNativeNetworkType(configuration->InitialNetworkType);
    native.encryptionKey = ToNativeBytes(configuration->EncryptionKey);

    if (configuration->ReflectorEndpoints == nullptr || configuration->RtcServers == nullptr) {
        throw ref new InvalidArgumentException(L"Call server collections are required.");
    }

    for (unsigned int index = 0; index < configuration->ReflectorEndpoints->Size; index++) {
        const auto endpoint = configuration->ReflectorEndpoints->GetAt(index);
        if (endpoint == nullptr ||
            (endpoint->Ipv4Address == nullptr && endpoint->Ipv6Address == nullptr) ||
            endpoint->PeerTag == nullptr) {
            throw ref new InvalidArgumentException(L"Every reflector endpoint must include an address and peer tag.");
        }

        auto target = Unigram::Native::Calls::EndpointConfiguration{};
        target.id = endpoint->Id;
        target.ipv4 = endpoint->Ipv4Address->Data();
        target.ipv6 = endpoint->Ipv6Address == nullptr ? L"" : endpoint->Ipv6Address->Data();
        target.port = endpoint->Port;
        target.isTcp = endpoint->IsTcp;
        target.peerTag = ToNativeBytes(endpoint->PeerTag);
        native.endpoints.push_back(std::move(target));
    }

    for (unsigned int index = 0; index < configuration->RtcServers->Size; index++) {
        const auto server = configuration->RtcServers->GetAt(index);
        if (server == nullptr || server->Host == nullptr) {
            throw ref new InvalidArgumentException(L"Every RTC server must include a host.");
        }

        auto target = Unigram::Native::Calls::RtcServerConfiguration{};
        target.id = server->Id;
        target.host = server->Host->Data();
        target.port = server->Port;
        target.username = server->Username == nullptr ? L"" : server->Username->Data();
        target.password = server->Password == nullptr ? L"" : server->Password->Data();
        target.isTurn = server->IsTurn;
        target.isTcp = server->IsTcp;
        native.rtcServers.push_back(std::move(target));
    }

    auto callbacks = Unigram::Native::Calls::CallCallbacks{};
    const auto weakHolder = std::weak_ptr<SessionHolder>(holder);
    callbacks.stateChanged = [weakHolder](Unigram::Native::Calls::CallState state) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->StateChanged(owner, static_cast<CallState>(state));
            }
        }
    };
    callbacks.signalingData = [weakHolder](std::vector<uint8_t> data) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                auto result = ref new Vector<unsigned char>();
                for (const auto value : data) {
                    result->Append(value);
                }
                owner->SignalingData(owner, result);
            }
        }
    };
    callbacks.stopped = [weakHolder](bool completed) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->Stopped(owner, completed);
            }
        }
    };
    callbacks.signalBarsChanged = [weakHolder](int bars) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->SignalBarsChanged(owner, bars);
            }
        }
    };
    callbacks.audioLevelChanged = [weakHolder](float level) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->AudioLevelChanged(owner, level);
            }
        }
    };
    callbacks.remoteAudioStateChanged = [weakHolder](Unigram::Native::Calls::RemoteAudioState state) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->RemoteAudioStateChanged(owner, static_cast<RemoteAudioState>(state));
            }
        }
    };
    callbacks.remoteVideoStateChanged = [weakHolder](Unigram::Native::Calls::VideoState state) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->RemoteVideoStateChanged(owner, static_cast<VideoState>(state));
            }
        }
    };
    callbacks.videoCaptureFailed = [weakHolder] {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->VideoCaptureFailed(owner, nullptr);
            }
        }
    };
    callbacks.videoFrameReceived = [weakHolder](
            bool local,
            int width,
            int height,
            std::vector<uint8_t> pixels) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                auto framePixels = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
                const auto dispatcher = holder->dispatcher;
                if (dispatcher == nullptr) {
                    owner->ReportVideoFrameDeliveryFailure(E_HANDLE);
                    return;
                }
                try {
                    dispatcher->RunAsync(
                        Windows::UI::Core::CoreDispatcherPriority::Low,
                        ref new Windows::UI::Core::DispatchedHandler(
                            [weakHolder, local, width, height, framePixels] {
                                const auto dispatchedHolder = weakHolder.lock();
                                if (!dispatchedHolder) {
                                    return;
                                }

                                const auto dispatchedOwner = ResolveOwner(dispatchedHolder);
                                if (dispatchedOwner == nullptr) {
                                    return;
                                }

                                try {
                                    auto frame = ref new VideoFrame();
                                    frame->IsLocal = local;
                                    frame->Width = width;
                                    frame->Height = height;
                                    auto buffer = ref new Windows::Storage::Streams::Buffer(
                                        static_cast<unsigned int>(framePixels->size()));
                                    Microsoft::WRL::ComPtr<IBufferByteAccess> access;
                                    if (FAILED(reinterpret_cast<IInspectable*>(buffer)->QueryInterface(IID_PPV_ARGS(&access))) ||
                                        !access) {
                                        dispatchedOwner->ReportVideoFrameDeliveryFailure(E_NOINTERFACE);
                                        return;
                                    }
                                    byte* data = nullptr;
                                    if (FAILED(access->Buffer(&data)) || data == nullptr) {
                                        dispatchedOwner->ReportVideoFrameDeliveryFailure(E_FAIL);
                                        return;
                                    }
                                    std::memcpy(data, framePixels->data(), framePixels->size());
                                    buffer->Length = static_cast<unsigned int>(framePixels->size());
                                    frame->Pixels = buffer;
                                    dispatchedOwner->VideoFrameReceived(dispatchedOwner, frame);
                                } catch (Platform::Exception^ error) {
                                    dispatchedOwner->ReportVideoFrameDeliveryFailure(error->HResult);
                                } catch (const std::bad_alloc&) {
                                    dispatchedOwner->ReportVideoFrameDeliveryFailure(E_OUTOFMEMORY);
                                }
                            }));
                } catch (Platform::Exception^ error) {
                    owner->ReportVideoFrameDeliveryFailure(error->HResult);
                } catch (const std::bad_alloc&) {
                    owner->ReportVideoFrameDeliveryFailure(E_OUTOFMEMORY);
                }
            }
        }
    };
    callbacks.audioDeviceReport = [weakHolder](std::string report) {
        if (const auto holder = weakHolder.lock()) {
            if (const auto owner = ResolveOwner(holder)) {
                owner->AudioDeviceReport(owner, ToPlatformString(report));
            }
        }
    };

    try {
        holder->session = Unigram::Native::Calls::CreateCallSession(native, std::move(callbacks));
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }

    _holder = new std::shared_ptr<SessionHolder>(std::move(holder));
}

/// <summary>
/// Claims the holder atomically and drains the native session. Dispose can be reached
/// from the TDLib update thread and the tgcalls worker thread at the same time, so only
/// one caller may tear down and free it.
/// </summary>
Platform::String^ AudioCallSession::DrainSession() {
    auto holder = static_cast<std::shared_ptr<SessionHolder>*>(
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&_holder), nullptr));
    if (holder == nullptr) {
        return L"empty";
    }

    String^ result = L"empty";
    if (*holder && (*holder)->session) {
        try {
            // Blocking matters here: tgcalls stops asynchronously and destroys the audio
            // device on its own threads, so returning early would let the next call
            // activate a microphone this session still owned.
            result = ToPlatformString(Unigram::Native::Calls::StopCallSessionAndWait(
                (*holder)->session,
                kTeardownTimeoutMilliseconds));
        }
        catch (...) {
            result = L"faulted";
        }
    }

    delete holder;
    return result;
}

Platform::String^ AudioCallSession::Teardown() {
    return DrainSession();
}

AudioCallSession::~AudioCallSession() {
    // Managed code calls Teardown first and reads the outcome from its return value, so
    // nothing has to be stored on the instance and read back after the destructor has
    // run. This stays as a safety net for a Dispose that skipped Teardown.
    DrainSession();
}

void AudioCallSession::Start() {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::StartCallSession(holder->session);
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::ReceiveSignalingData(IVector<unsigned char>^ data) {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::ReceiveSignalingData(holder->session, ToNativeBytes(data));
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::SetMuted(bool value) {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::SetMuted(holder->session, value);
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

bool AudioCallSession::SupportsVideo() {
    try {
        const auto holder = GetSessionHolder(_holder);
        return Unigram::Native::Calls::SupportsVideo(holder->session);
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::SetVideoState(VideoState value) {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::SetVideoState(
            holder->session,
            static_cast<Unigram::Native::Calls::VideoState>(value));
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::SwitchVideoCaptureDevice(String^ deviceId) {
    if (deviceId == nullptr || deviceId->IsEmpty()) {
        throw ref new InvalidArgumentException(L"A camera device identifier is required.");
    }

    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::SwitchVideoCaptureDevice(holder->session, deviceId->Data());
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::SetVideoOutputEnabled(bool local, bool enabled) {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::SetVideoOutputEnabled(holder->session, local, enabled);
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::AcknowledgeVideoFrame(bool local) {
    if (_holder == nullptr) {
        return;
    }

    const auto holder = GetSessionHolder(_holder);
    Unigram::Native::Calls::AcknowledgeVideoFrame(holder->session, local);
}

String^ AudioCallSession::SetAudioOutputEndpoint(bool speakerphone) {
    try {
        const auto holder = GetSessionHolder(_holder);
        return ToPlatformString(Unigram::Native::Calls::SetAudioOutputEndpoint(
            holder->session,
            speakerphone));
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

void AudioCallSession::SetNetworkType(NetworkType value) {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::SetNetworkType(holder->session, ToNativeNetworkType(value));
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

String^ AudioCallSession::GetAudioDeviceStatus() {
    if (_holder == nullptr) {
        return ref new String(L"created=0");
    }

    try {
        const auto holder = GetSessionHolder(_holder);
        return ToPlatformString(Unigram::Native::Calls::GetAudioDeviceStatus(holder->session));
    } catch (const std::exception&) {
        return ref new String(L"created=0");
    }
}

void AudioCallSession::Stop() {
    try {
        const auto holder = GetSessionHolder(_holder);
        Unigram::Native::Calls::StopCallSession(holder->session);
    } catch (const std::exception& error) {
        throw ref new InvalidArgumentException(ToPlatformString(error));
    }
}

String^ Diagnostics::GetBuildInfo() {
    EnsureUwpTlsSupport();
    return ref new String((L"Modern TgCalls ARM UWP bridge proof: " +
        std::wstring(Unigram::Native::Calls::GetFirstSupportedVersion())).c_str());
}

IVector<String^>^ Diagnostics::GetSupportedVersions() {
    EnsureUwpTlsSupport();

    auto result = ref new Vector<String^>();
    for (const auto& version : Unigram::Native::Calls::GetSupportedVersions()) {
        result->Append(ref new String(version.c_str()));
    }
    return result;
}

void Diagnostics::EnableCrashDiagnostics(String^ diagnosticsFilePath) {
    if (diagnosticsFilePath == nullptr || diagnosticsFilePath->IsEmpty()) {
        return;
    }

    Unigram::Native::Calls::EnableCrashDiagnostics(std::wstring(diagnosticsFilePath->Data()));
}

}
}
}
}
