#include "TgCallsEngineFacade.h"

#include "Instance.h"
#include "InstanceImpl.h"

#include "api/task_queue/task_queue_factory.h"
#include "modules/audio_device/include/audio_device.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <stdexcept>
#include <utility>

#include <windows.h>

namespace Unigram {
namespace Native {
namespace Calls {
namespace {

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

                if (const auto strong = weak.lock()) {
                    strong->RetainAudioDeviceModule(module);
                    strong->AudioDeviceReport(report);
                }
                return module;
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
