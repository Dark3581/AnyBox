#include "xdll/gameinput/gameinput.hpp"

#include "xdll/gameinput/Log.hpp"
#include "xdll/gameinput/Objects.hpp"

namespace {

void ToAbxLog(const char* line) {
    AbxLog("gameinput", "%s", line);
}

}

ABX_API HRESULT GameInputCreate(void** gameInput) {
    abx::gameinput::SetLogSink(ToAbxLog);
    return abx::gameinput::Create(gameInput);
}
