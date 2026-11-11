#include "MockPlatformStandalone.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace LinkuraLocal::Standalone {
    namespace {
        std::filesystem::path g_root = std::filesystem::temp_directory_path() / "LinkuraOffline";
        LogSink g_sink = nullptr;
        std::mutex g_logMutex;
    }

    void SetDataRoot(const std::filesystem::path& root) { g_root = root; }
    const std::filesystem::path& DataRoot() { return g_root; }
    void SetLogSink(LogSink sink) { g_sink = sink; }

    static void Emit(const char* level, const char* message) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_sink) {
            g_sink(level, message);
        } else {
            std::fprintf(stderr, "[%s] %s\n", level, message);
        }
    }

    static void EmitV(const char* level, const char* fmt, va_list args) {
        char buffer[4096];
        std::vsnprintf(buffer, sizeof(buffer), fmt, args);
        Emit(level, buffer);
    }
}

namespace LinkuraLocal::Log {
    using LinkuraLocal::Standalone::Emit;
    using LinkuraLocal::Standalone::EmitV;

    void Info(const char* msg) { Emit("I", msg); }
    void Warn(const char* msg) { Emit("W", msg); }
    void Error(const char* msg) { Emit("E", msg); }
    void Debug(const char* msg) { Emit("D", msg); }

#define LINKURA_LOG_FMT(NAME, LEVEL)          \
    void NAME(const char* fmt, ...) {         \
        va_list args;                         \
        va_start(args, fmt);                  \
        EmitV(LEVEL, fmt, args);              \
        va_end(args);                         \
    }
    LINKURA_LOG_FMT(InfoFmt, "I")
    LINKURA_LOG_FMT(WarnFmt, "W")
    LINKURA_LOG_FMT(ErrorFmt, "E")
    LINKURA_LOG_FMT(DebugFmt, "D")
#undef LINKURA_LOG_FMT
}

namespace LinkuraLocal::Local {
    std::filesystem::path GetBasePath() {
        return LinkuraLocal::Standalone::DataRoot() / "linkura-local";
    }
}

namespace LinkuraLocal::HookShare::Shareable {
    std::string currentArchiveId;
}
