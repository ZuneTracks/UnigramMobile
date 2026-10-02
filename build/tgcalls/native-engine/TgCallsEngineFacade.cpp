#include "TgCallsEngineFacade.h"

#include "InstanceImpl.h"

#include <string>

namespace Unigram {
namespace Native {
namespace Calls {

const wchar_t* GetFirstSupportedVersion() {
    static const std::wstring version = [] {
        const auto versions = tgcalls::InstanceImpl::GetVersions();
        return std::wstring(versions.front().begin(), versions.front().end());
    }();

    return version.c_str();
}

}
}
}
