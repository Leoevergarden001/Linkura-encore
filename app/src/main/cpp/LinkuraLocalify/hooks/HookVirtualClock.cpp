#include "../HookMain.h"

#ifndef GKMS_WINDOWS

#include "../config/Config.hpp"

#include <cstdint>

namespace LinkuraLocal::HookVirtualClock {
    namespace {
        // System.DateTime stores ticks since 0001-01-01. The high bits encode
        // DateTimeKind; return UTC for UtcNow and local-kind for Now, while
        // retaining the same wall-clock instant as the configured Pack clock.
        constexpr int64_t kUnixEpochTicks = 621355968000000000LL;
        constexpr int64_t kTicksPerSecond = 10000000LL;
        constexpr uint64_t kKindUtc = 0x4000000000000000ULL;
        constexpr uint64_t kKindLocal = 0x8000000000000000ULL;
        constexpr uint64_t kTicksMask = 0x3fffffffffffffffULL;

        uint64_t FixedDateTimeData(uint64_t kind) {
            const int64_t unixSeconds = Config::virtualClockUnixTime;
            // DateTime's valid range is conservative; a malformed config falls
            // through to the original runtime rather than wrapping an integer.
            if (unixSeconds < -62135596800LL || unixSeconds > 253402300799LL) {
                return 0;
            }
            const int64_t ticks = kUnixEpochTicks + unixSeconds * kTicksPerSecond;
            return (static_cast<uint64_t>(ticks) & kTicksMask) | kind;
        }

        DEFINE_HOOK(uint64_t, DateTime_get_Now, (void* method)) {
            if (Config::enableVirtualClock) {
                const auto fixed = FixedDateTimeData(kKindLocal);
                if (fixed != 0) return fixed;
            }
            return DateTime_get_Now_Orig(method);
        }

        DEFINE_HOOK(uint64_t, DateTime_get_UtcNow, (void* method)) {
            if (Config::enableVirtualClock) {
                const auto fixed = FixedDateTimeData(kKindUtc);
                if (fixed != 0) return fixed;
            }
            return DateTime_get_UtcNow_Orig(method);
        }
    }  // namespace

    void Install(HookInstaller*) {
        for (auto* assembly : UnityResolve::assembly) {
            if (!assembly) continue;
            auto* dateTime = assembly->Get("DateTime", "System");
            if (!dateTime) continue;
            for (const auto* method : dateTime->methods) {
                if (!method || !method->function || !method->args.empty()) continue;
                if (method->name == "get_Now") {
                    ADD_HOOK(DateTime_get_Now, method->function);
                    Log::InfoFmt("[VirtualClock] hooked %s:System.DateTime.get_Now", assembly->name.c_str());
                } else if (method->name == "get_UtcNow") {
                    ADD_HOOK(DateTime_get_UtcNow, method->function);
                    Log::InfoFmt("[VirtualClock] hooked %s:System.DateTime.get_UtcNow", assembly->name.c_str());
                }
            }
        }
    }
}  // namespace LinkuraLocal::HookVirtualClock

#else

namespace LinkuraLocal::HookVirtualClock { void Install(HookInstaller*) {} }

#endif
