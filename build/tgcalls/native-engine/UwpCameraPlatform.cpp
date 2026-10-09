#include "platform/PlatformInterface.h"
#include "Instance.h"
#include "VideoCaptureInterface.h"
#include "VideoCapturerInterface.h"

#include "api/media_stream_interface.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "media/base/video_broadcaster.h"
#include "modules/video_capture/video_capture_factory.h"
#include "modules/video_coding/codecs/h264/win/decoder/h264_decoder_mf_impl.h"
#include "modules/video_coding/codecs/h264/win/encoder/h264_encoder_mf_impl.h"
#include "pc/video_track_source.h"
#include "pc/video_track_source_proxy.h"
#include "rtc_base/logging.h"
#include "rtc_base/ref_counted_object.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace tgcalls {
namespace {

constexpr int kPreferredWidth = 1280;
constexpr int kPreferredHeight = 720;
constexpr int kPreferredFps = 30;
// The Windows 10 Mobile capture backend delivers the Lumia sensor's landscape
// buffer without device orientation. Advertise the portrait correction through
// both the V2 media state and WebRTC frame metadata.
constexpr auto kPortraitRotation = webrtc::kVideoRotation_270;

bool IsH264(const webrtc::SdpVideoFormat& format) {
    return format.name == "H264";
}

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(), [](char first, char second) {
            return std::tolower(static_cast<unsigned char>(first)) ==
                std::tolower(static_cast<unsigned char>(second));
        });
}

std::vector<webrtc::SdpVideoFormat> H264Formats() {
    return {
        webrtc::SdpVideoFormat("H264", {
            { "level-asymmetry-allowed", "1" },
            { "packetization-mode", "1" },
            { "profile-level-id", "42e01f" },
        }),
    };
}

class H264OnlyEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return H264Formats();
    }

    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(
            const webrtc::SdpVideoFormat& format) override {
        return IsH264(format)
            ? std::make_unique<webrtc::H264EncoderMFImpl>()
            : nullptr;
    }
};

class H264OnlyDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return H264Formats();
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
            const webrtc::Environment&,
            const webrtc::SdpVideoFormat& format) override {
        return CreateVideoDecoder(format);
    }

    std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(
            const webrtc::SdpVideoFormat& format) override {
        return IsH264(format)
            ? std::make_unique<webrtc::H264DecoderMFImpl>()
            : nullptr;
    }
};

class UwpCameraTrackSource : public webrtc::VideoTrackSource {
public:
    UwpCameraTrackSource()
        : VideoTrackSource(/*remote=*/false)
        , _broadcaster(std::make_shared<rtc::VideoBroadcaster>()) {
    }

    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink() const {
        return _broadcaster;
    }

private:
    rtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
        return _broadcaster.get();
    }

    std::shared_ptr<rtc::VideoBroadcaster> _broadcaster;
};

std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> GetSourceSink(
        const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>& source) {
    const auto proxy = static_cast<webrtc::VideoTrackSourceProxy*>(source.get());
    const auto internal = static_cast<UwpCameraTrackSource*>(proxy->internal());
    return internal->sink();
}

class UwpCameraCapturer final : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    explicit UwpCameraCapturer(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink)
        : _sink(std::move(sink)) {
    }

    ~UwpCameraCapturer() override {
        Stop();
    }

    void SetState(VideoState state) {
        if (_state.load(std::memory_order_acquire) == state) {
            return;
        }

        _state.store(state, std::memory_order_release);
        if (state == VideoState::Active) {
            Start();
        } else {
            Stop();
        }
    }

    void SetDeviceId(std::string deviceId) {
        if (_requestedDeviceId == deviceId) {
            return;
        }

        Stop();
        _requestedDeviceId = std::move(deviceId);
        if (_state.load(std::memory_order_acquire) == VideoState::Active) {
            Start();
        }
    }

    void SetPreferredCaptureAspectRatio(float aspectRatio) {
        _aspectRatio.store(aspectRatio, std::memory_order_release);
    }

    void SetOnFatalError(std::function<void()> error) {
        _error = std::move(error);
        if (_failed && _error) {
            _error();
        }
    }

    std::pair<int, int> resolution() const {
        return _dimensions;
    }

    void OnFrame(const webrtc::VideoFrame& frame) override {
        if (_state.load(std::memory_order_acquire) != VideoState::Active || _sink == nullptr) {
            return;
        }

        const auto aspectRatio = _aspectRatio.load(std::memory_order_acquire);
        if (aspectRatio <= 0.001f) {
            ForwardPortraitFrame(frame);
            return;
        }

        const auto originalWidth = frame.width();
        const auto originalHeight = frame.height();
        auto width = originalWidth > aspectRatio * originalHeight
            ? static_cast<int>(aspectRatio * originalHeight)
            : originalWidth;
        auto height = originalWidth > aspectRatio * originalHeight
            ? originalHeight
            : static_cast<int>(originalWidth / aspectRatio);

        if ((width >= originalWidth && height >= originalHeight) || width <= 0 || height <= 0) {
            ForwardPortraitFrame(frame);
            return;
        }

        width &= ~1;
        height &= ~1;
        const auto left = (originalWidth - width) / 2;
        const auto top = (originalHeight - height) / 2;
        auto buffer = webrtc::I420Buffer::Create(width, height);
        buffer->CropAndScaleFrom(
            *frame.video_frame_buffer()->ToI420(), left, top, width, height);

        _sink->OnFrame(
            webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(buffer)
                .set_rotation(kPortraitRotation)
                .set_timestamp_us(frame.timestamp_us())
                .set_id(frame.id())
                .build());
    }

private:
    void ForwardPortraitFrame(const webrtc::VideoFrame& frame) {
        auto builder = webrtc::VideoFrame::Builder()
            .set_video_frame_buffer(frame.video_frame_buffer())
            .set_rotation(kPortraitRotation)
            .set_timestamp_us(frame.timestamp_us())
            .set_id(frame.id());
        if (frame.has_update_rect()) {
            builder.set_update_rect(frame.update_rect());
        }
        _sink->OnFrame(builder.build());
    }

    void Start() {
        _failed = false;
        const auto info = std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo>(
            webrtc::VideoCaptureFactory::CreateDeviceInfo());
        if (!info) {
            Fail();
            return;
        }

        const auto count = info->NumberOfDevices();
        if (count <= 0) {
            Fail();
            return;
        }

        const auto getId = [&info](int index) {
            constexpr size_t kLengthLimit = 256;
            char name[kLengthLimit] = {};
            char id[kLengthLimit] = {};
            return info->GetDeviceName(index, name, kLengthLimit, id, kLengthLimit) == 0
                ? std::string(id)
                : std::string();
        };

        auto preferredId = std::string();
        for (auto index = 0; index != count; ++index) {
            const auto id = getId(index);
            if (EqualsIgnoreCase(_requestedDeviceId, id) ||
                (preferredId.empty() && (_requestedDeviceId.empty() || _requestedDeviceId == "default"))) {
                preferredId = id;
            }
        }

        if (Start(info.get(), preferredId)) {
            return;
        }

        for (auto index = 0; index != count; ++index) {
            if (Start(info.get(), getId(index))) {
                return;
            }
        }

        Fail();
    }

    bool Start(webrtc::VideoCaptureModule::DeviceInfo* info, const std::string& deviceId) {
        if (deviceId.empty()) {
            return false;
        }

        _module = webrtc::VideoCaptureFactory::Create(deviceId.c_str());
        if (!_module) {
            RTC_LOG(LS_ERROR) << "Failed to create UWP camera capture.";
            return false;
        }

        _module->RegisterCaptureDataCallback(this);
        auto requested = webrtc::VideoCaptureCapability();
        requested.videoType = webrtc::VideoType::kI420;
        requested.width = kPreferredWidth;
        requested.height = kPreferredHeight;
        requested.maxFPS = kPreferredFps;
        info->GetBestMatchedCapability(_module->CurrentDeviceName(), requested, _capability);
        if (_capability.width == 0 || _capability.height == 0 || _capability.maxFPS == 0) {
            _capability.width = kPreferredWidth;
            _capability.height = kPreferredHeight;
            _capability.maxFPS = kPreferredFps;
        }

        if (_module->StartCapture(_capability) != 0) {
            RTC_LOG(LS_ERROR) << "Failed to start UWP camera capture.";
            Stop();
            return false;
        }

        _dimensions = { _capability.width, _capability.height };
        return true;
    }

    void Stop() {
        _failed = false;
        if (!_module) {
            return;
        }

        _module->StopCapture();
        _module->DeRegisterCaptureDataCallback();
        _module = nullptr;
    }

    void Fail() {
        _failed = true;
        if (_error) {
            _error();
        }
    }

    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _sink;
    rtc::scoped_refptr<webrtc::VideoCaptureModule> _module;
    webrtc::VideoCaptureCapability _capability;
    std::atomic<VideoState> _state{VideoState::Inactive};
    std::string _requestedDeviceId;
    std::pair<int, int> _dimensions;
    std::function<void()> _error;
    std::atomic<float> _aspectRatio{0.0f};
    bool _failed = false;
};

class UwpCameraCapturerInterface final : public VideoCapturerInterface {
public:
    UwpCameraCapturerInterface(
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
            std::string deviceId,
            std::function<void(VideoState)> stateUpdated,
            std::pair<int, int>& outResolution)
        : _source(std::move(source))
        , _sink(GetSourceSink(_source))
        , _stateUpdated(std::move(stateUpdated)) {
        _capturer = std::make_unique<UwpCameraCapturer>(_sink);
        _capturer->SetDeviceId(std::move(deviceId));
        _capturer->SetState(VideoState::Active);
        outResolution = _capturer->resolution();
    }

    void setState(VideoState state) override {
        _capturer->SetState(state);
        if (_stateUpdated) {
            _stateUpdated(state);
        }
    }

    void setPreferredCaptureAspectRatio(float aspectRatio) override {
        _capturer->SetPreferredCaptureAspectRatio(aspectRatio);
    }

    void setUncroppedOutput(
            std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) override {
        if (_uncroppedSink) {
            _source->RemoveSink(_uncroppedSink.get());
        }
        _uncroppedSink = std::move(sink);
        if (_uncroppedSink) {
            _source->AddOrUpdateSink(_uncroppedSink.get(), rtc::VideoSinkWants());
        }
    }

    int getRotation() override {
        return 270;
    }

    void setOnFatalError(std::function<void()> error) override {
        _capturer->SetOnFatalError(std::move(error));
    }

private:
    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> _source;
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _sink;
    std::unique_ptr<UwpCameraCapturer> _capturer;
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _uncroppedSink;
    std::function<void(VideoState)> _stateUpdated;
};

class UwpCameraPlatform final : public PlatformInterface {
public:
    std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory(
            bool,
            bool) override {
        return std::make_unique<H264OnlyEncoderFactory>();
    }

    std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override {
        return std::make_unique<H264OnlyDecoderFactory>();
    }

    bool supportsEncoding(const std::string& codecName) override {
        return codecName == cricket::kH264CodecName;
    }

    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(
            rtc::Thread* signalingThread,
            rtc::Thread* workerThread) override {
        const auto source = rtc::scoped_refptr<UwpCameraTrackSource>(
            new rtc::RefCountedObject<UwpCameraTrackSource>());
        return source
            ? webrtc::VideoTrackSourceProxy::Create(signalingThread, workerThread, source)
            : nullptr;
    }

    void adaptVideoSource(
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>,
            int,
            int,
            int) override {
    }

    std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source,
            std::string deviceId,
            std::function<void(VideoState)> stateUpdated,
            std::function<void(PlatformCaptureInfo)>,
            std::shared_ptr<PlatformContext>,
            std::pair<int, int>& outResolution) override {
        return std::make_unique<UwpCameraCapturerInterface>(
            std::move(source),
            std::move(deviceId),
            std::move(stateUpdated),
            outResolution);
    }
};

}

std::unique_ptr<PlatformInterface> CreatePlatformInterface() {
    return std::make_unique<UwpCameraPlatform>();
}

}
