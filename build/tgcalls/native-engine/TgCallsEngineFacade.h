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

enum class RemoteAudioState {
    Muted,
    Active,
};

struct CallCallbacks {
    std::function<void(CallState)> stateChanged;
    std::function<void(std::vector<uint8_t>)> signalingData;
    std::function<void(bool)> stopped;
    std::function<void(int)> signalBarsChanged;
    std::function<void(float)> audioLevelChanged;
    std::function<void(RemoteAudioState)> remoteAudioStateChanged;
    // Reports the outcome of creating the platform audio device module as a short
    // diagnostic string built only from fixed keys, result codes and device counts.
    // It never carries a device name, identifier or path.
    std::function<void(std::string)> audioDeviceReport;
};

const wchar_t* GetFirstSupportedVersion();
std::vector<std::wstring> GetSupportedVersions();
/// <summary>
/// Installs a process-wide handler that appends the code of a fatal hardware exception to
/// the given diagnostics file. The per-call guards around the audio device only cover the
/// thread that calls into it, but the platform completes capture activation on a pool
/// thread, so a fault there kills the process with nothing recorded. The handler writes
/// with raw file APIs and no allocation or locking because it runs in a fault context, and
/// it records only an exception code -- never an address, a module or a path.
/// </summary>
void EnableCrashDiagnostics(const std::wstring& diagnosticsFilePath);
/// <summary>
/// Returns the live state of the platform audio device module as a short diagnostic
/// string built only from fixed keys and boolean or count values. Whether the module is
/// actually recording and playing is the one unambiguous answer to "the transport is
/// established but nobody can hear anything"; the engine's own level callback reports the
/// larger of the local and remote levels and so cannot distinguish the two directions.
/// </summary>
std::string GetAudioDeviceStatus(const CallSessionPtr& session);
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
