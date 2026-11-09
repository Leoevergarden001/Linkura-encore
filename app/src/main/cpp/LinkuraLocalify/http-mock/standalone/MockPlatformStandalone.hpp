#pragma once

// Platform surface of the offline backend for builds without the Android
// hook framework (iOS in-app server, macOS host tests).

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace LinkuraLocal::Log {
    void Info(const char* msg);
    void InfoFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void Warn(const char* msg);
    void WarnFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void Error(const char* msg);
    void ErrorFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    void Debug(const char* msg);
    void DebugFmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}

namespace LinkuraLocal::Local {
    // Directory whose parent holds HasuKikaisann.sqlite3 (same layout as the
    // Android files/linkura-local directory).
    std::filesystem::path GetBasePath();
}

namespace LinkuraLocal::HookShare::Shareable {
    extern std::string currentArchiveId;
}

namespace LinkuraLocal::Standalone {
    // Root data directory; must be set before the first backend call.
    void SetDataRoot(const std::filesystem::path& root);
    const std::filesystem::path& DataRoot();

    // Log sink: called with a level tag ("I", "W", "E", "D") and the message.
    using LogSink = void (*)(const char* level, const char* message);
    void SetLogSink(LogSink sink);
}
