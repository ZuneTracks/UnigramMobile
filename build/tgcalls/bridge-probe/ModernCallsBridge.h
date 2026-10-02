#pragma once

#include <collection.h>
#include <memory>

namespace Unigram {
namespace Native {
namespace Calls {
namespace Proof {

public enum class NetworkType {
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

public enum class CallState {
    WaitInit,
    WaitInitAck,
    Established,
    Failed,
    Reconnecting,
};

public ref class ReflectorEndpoint sealed {
public:
    ReflectorEndpoint();

    property long long Id;
    property Platform::String^ Ipv4Address;
    property Platform::String^ Ipv6Address;
    property unsigned short Port;
    property bool IsTcp;
    property Windows::Foundation::Collections::IVector<unsigned char>^ PeerTag;
};

public ref class RtcServer sealed {
public:
    RtcServer();

    property unsigned char Id;
    property Platform::String^ Host;
    property unsigned short Port;
    property Platform::String^ Username;
    property Platform::String^ Password;
    property bool IsTurn;
    property bool IsTcp;
};

public ref class AudioCallConfiguration sealed {
public:
    AudioCallConfiguration();

    property Platform::String^ Version;
    property double InitializationTimeout;
    property double ReceiveTimeout;
    property bool EnableP2P;
    property bool AllowTcp;
    property int MaxApiLayer;
    property bool IsOutgoing;
    property NetworkType InitialNetworkType;
    property Windows::Foundation::Collections::IVector<unsigned char>^ EncryptionKey;
    property Windows::Foundation::Collections::IVector<ReflectorEndpoint^>^ ReflectorEndpoints;
    property Windows::Foundation::Collections::IVector<RtcServer^>^ RtcServers;
};

public ref class AudioCallSession sealed {
public:
    static AudioCallSession^ Create(AudioCallConfiguration^ configuration);

    virtual ~AudioCallSession();

    void ReceiveSignalingData(Windows::Foundation::Collections::IVector<unsigned char>^ data);
    void SetMuted(bool value);
    void SetNetworkType(NetworkType value);
    void Stop();

    event Windows::Foundation::EventHandler<CallState>^ StateChanged;
    event Windows::Foundation::EventHandler<Windows::Foundation::Collections::IVector<unsigned char>^>^ SignalingData;
    event Windows::Foundation::EventHandler<bool>^ Stopped;

private:
    AudioCallSession(AudioCallConfiguration^ configuration);
    void* _holder;
};

public ref class Diagnostics sealed {
public:
    static Platform::String^ GetBuildInfo();
};

}
}
}
}
