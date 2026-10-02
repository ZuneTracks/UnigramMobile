#include "ModernCallsBridge.h"

#include "rtc_base/checks.h"

using namespace Platform;

namespace Unigram {
namespace Native {
namespace Calls {
namespace Proof {

String^ Diagnostics::GetBuildInfo() {
    RTC_DCHECK(true);
    return "Modern TgCalls ARM UWP bridge proof";
}

}
}
}
}
