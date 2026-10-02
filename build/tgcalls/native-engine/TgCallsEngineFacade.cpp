#include "TgCallsEngineFacade.h"

#include "Instance.h"
#include "InstanceImpl.h"

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

        tgcalls::Register<tgcalls::InstanceImpl>();

        auto session = CallSessionPtr(new CallSession(std::move(callbacks)));

        auto encryptionKey = std::make_shared<std::array<uint8_t, tgcalls::EncryptionKey::kSize>>();
        std::copy(
            configuration.encryptionKey.begin(),
            configuration.encryptionKey.end(),
            encryptionKey->begin());
        auto endpoints = std::vector<tgcalls::Endpoint>{};
        endpoints.reserve(configuration.endpoints.size());
        for (const auto& endpoint : configuration.endpoints) {
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
        rtcServers.reserve(configuration.rtcServers.size());
        for (const auto& server : configuration.rtcServers) {
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

        const auto weak = std::weak_ptr<CallSession>(session);
        auto descriptor = tgcalls::Descriptor{
            .version = ToUtf8(configuration.version),
            .config = {
                .initializationTimeout = configuration.initializationTimeout,
                .receiveTimeout = configuration.receiveTimeout,
                .enableP2P = configuration.enableP2P,
                .allowTCP = configuration.allowTcp,
                .enableAEC = true,
                .enableNS = true,
                .enableAGC = true,
                .enableVolumeControl = true,
                .maxApiLayer = configuration.maxApiLayer,
            },
            .endpoints = std::move(endpoints),
            .rtcServers = std::move(rtcServers),
            .initialNetworkType = ToTgCallsNetworkType(configuration.initialNetworkType),
            .encryptionKey = tgcalls::EncryptionKey(encryptionKey, configuration.isOutgoing),
            .stateUpdated = [weak](tgcalls::State state) {
                if (const auto strong = weak.lock()) {
                    strong->StateChanged(ToFacadeState(state));
                }
            },
            .signalingDataEmitted = [weak](const std::vector<uint8_t>& data) {
                if (const auto strong = weak.lock()) {
                    strong->SignalingData(data);
                }
            },
        };

        session->_instance = tgcalls::Meta::Create(descriptor.version, std::move(descriptor));
        if (!session->_instance) {
            throw std::invalid_argument("The requested TgCalls protocol version is not registered.");
        }

        return session;
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

    void Stop() {
        std::unique_lock<std::mutex> lock(_mutex);
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
    explicit CallSession(CallCallbacks callbacks) : _callbacks(std::move(callbacks)) {
    }

    void EnsureActive() const {
        if (!_instance || _stopping) {
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

    std::mutex _mutex;
    std::unique_ptr<tgcalls::Instance> _instance;
    CallCallbacks _callbacks;
    bool _stopping = false;
};

const wchar_t* GetFirstSupportedVersion() {
    static const std::wstring version = [] {
        tgcalls::Register<tgcalls::InstanceImpl>();
        const auto versions = tgcalls::Meta::Versions();
        return std::wstring(versions.front().begin(), versions.front().end());
    }();

    return version.c_str();
}

CallSessionPtr CreateCallSession(
    const CallConfiguration& configuration,
    CallCallbacks callbacks) {
    return CallSession::Create(configuration, std::move(callbacks));
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

void StopCallSession(const CallSessionPtr& session) {
    if (!session) {
        throw std::invalid_argument("The TgCalls session is unavailable.");
    }
    session->Stop();
}

}
}
}
