#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Unigram {
namespace Native {
namespace Calls {

enum class CallState {
    WaitInit,
    WaitInitAck,
    Established,
    Failed,
    Reconnecting,
};

enum class NetworkType {
    Unknown,
    Gprs,
    Edge,
    ThirdGeneration,
    Hspa,
    Lte,
    WiFi,
    Ethernet,
    OtherHighSpeed,
    OtherLowSpeed,
    OtherMobile,
    Dialup,
};

struct EndpointConfiguration {
    int64_t id = 0;
    std::wstring ipv4;
    std::wstring ipv6;
    uint16_t port = 0;
    bool isTcp = false;
    std::vector<uint8_t> peerTag;
};

struct RtcServerConfiguration {
    uint8_t id = 0;
    std::wstring host;
    uint16_t port = 0;
    std::wstring username;
    std::wstring password;
    bool isTurn = false;
    bool isTcp = false;
};

struct CallConfiguration {
    std::wstring version;
    double initializationTimeout = 0.;
    double receiveTimeout = 0.;
    bool enableP2P = false;
    bool allowTcp = false;
    int maxApiLayer = 0;
    bool isOutgoing = false;
    NetworkType initialNetworkType = NetworkType::Unknown;
    std::vector<uint8_t> encryptionKey;
    std::vector<EndpointConfiguration> endpoints;
    std::vector<RtcServerConfiguration> rtcServers;
};

class CallSession;
using CallSessionPtr = std::shared_ptr<CallSession>;

struct CallCallbacks {
    std::function<void(CallState)> stateChanged;
    std::function<void(std::vector<uint8_t>)> signalingData;
    std::function<void(bool)> stopped;
};

const wchar_t* GetFirstSupportedVersion();
std::vector<std::wstring> GetSupportedVersions();
CallSessionPtr CreateCallSession(
    const CallConfiguration& configuration,
    CallCallbacks callbacks);
void StartCallSession(const CallSessionPtr& session);
void ReceiveSignalingData(const CallSessionPtr& session, std::vector<uint8_t> data);
void SetMuted(const CallSessionPtr& session, bool value);
void SetNetworkType(const CallSessionPtr& session, NetworkType value);
void StopCallSession(const CallSessionPtr& session);

}
}
}
