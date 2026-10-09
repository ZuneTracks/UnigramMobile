#pragma once

#include <string>

namespace Unigram {
namespace Native {
namespace Calls {

class UwpCameraCaptureControl {
public:
    virtual ~UwpCameraCaptureControl() = default;

    virtual bool SwitchToDevice(std::string deviceSelector) = 0;
};

}
}
}
