#include "TgCallsEngineFacade.h"

#include "Instance.h"
#include "InstanceImpl.h"

#include <string>

namespace Unigram {
namespace Native {
namespace Calls {

const wchar_t* GetFirstSupportedVersion() {
    static const std::wstring version = [] {
        tgcalls::Register<tgcalls::InstanceImpl>();
        const auto versions = tgcalls::Meta::Versions();
        return std::wstring(versions.front().begin(), versions.front().end());
    }();

    return version.c_str();
}

}
}
}
