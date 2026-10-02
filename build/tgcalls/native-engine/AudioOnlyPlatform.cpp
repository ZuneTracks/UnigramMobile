#include "platform/PlatformInterface.h"
#include "VideoCapturerInterface.h"

#include "api/environment/environment.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace tgcalls {
namespace {

class AudioOnlyVideoEncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {};
    }

    CodecSupport QueryCodecSupport(
            const webrtc::SdpVideoFormat&,
            absl::optional<std::string>) const override {
        return {};
    }

    std::unique_ptr<webrtc::VideoEncoder> CreateVideoEncoder(
            const webrtc::SdpVideoFormat&) override {
        return nullptr;
    }
};

class AudioOnlyVideoDecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {};
    }

    CodecSupport QueryCodecSupport(
            const webrtc::SdpVideoFormat&,
            bool) const override {
        return {};
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
            const webrtc::Environment&,
            const webrtc::SdpVideoFormat&) override {
        return nullptr;
    }

    std::unique_ptr<webrtc::VideoDecoder> CreateVideoDecoder(
            const webrtc::SdpVideoFormat&) override {
        return nullptr;
    }
};

class AudioOnlyPlatform final : public PlatformInterface {
public:
    std::unique_ptr<webrtc::VideoEncoderFactory> makeVideoEncoderFactory(
            bool,
            bool) override {
        return std::make_unique<AudioOnlyVideoEncoderFactory>();
    }

    std::unique_ptr<webrtc::VideoDecoderFactory> makeVideoDecoderFactory() override {
        return std::make_unique<AudioOnlyVideoDecoderFactory>();
    }

    bool supportsEncoding(const std::string&) override {
        return false;
    }

    rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> makeVideoSource(
            rtc::Thread*,
            rtc::Thread*) override {
        return nullptr;
    }

    void adaptVideoSource(
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>,
            int,
            int,
            int) override {
    }

    std::unique_ptr<VideoCapturerInterface> makeVideoCapturer(
            rtc::scoped_refptr<webrtc::VideoTrackSourceInterface>,
            std::string,
            std::function<void(VideoState)>,
            std::function<void(PlatformCaptureInfo)>,
            std::shared_ptr<PlatformContext>,
            std::pair<int, int>&) override {
        return nullptr;
    }
};

}

std::unique_ptr<PlatformInterface> CreatePlatformInterface() {
    return std::make_unique<AudioOnlyPlatform>();
}

}
