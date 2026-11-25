#include "xdll/xgameruntime/xgameruntime.hpp"

ABX_API std::uint32_t XDisplayTryEnableHdrMode(std::uint32_t preference, void* info) {
    static_cast<void>(info);
    AbxTrace("XDisplayTryEnableHdrMode(preference %u): HDR output is not enabled, the title renders SDR", preference);
    return 0;
}
