#include "ModernCallsBridge.h"

#include "TgCallsEngineFacade.h"

#include <string>

using namespace Platform;

namespace Unigram {
namespace Native {
namespace Calls {
namespace Proof {

void EnsureUwpTlsSupport();

String^ Diagnostics::GetBuildInfo() {
    EnsureUwpTlsSupport();
    return ref new String((L"Modern TgCalls ARM UWP bridge proof: " +
        std::wstring(Unigram::Native::Calls::GetFirstSupportedVersion())).c_str());
}

}
}
}
}
