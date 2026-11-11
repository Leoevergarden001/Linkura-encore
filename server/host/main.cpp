// Host (macOS) build of the iOS offline server, for local testing:
//   linkura-offline-server --data DIR [--port N] [--res-version V] [--mock-root DIR]
// Runs until stdin closes (so a test harness can stop it), then exits.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "standalone/MockPlatformStandalone.hpp"
#include "standalone/OfflineHttpServer.hpp"

int main(int argc, char** argv) {
    LinkuraLocal::Standalone::ServerConfig config;
    std::string data;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--data") data = value;
        else if (key == "--port") config.port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
        else if (key == "--res-version") config.resVersion = value;
        else if (key == "--mock-root") config.mockRoot = value;
        else if (key == "--asset-root") config.assetRoot = value;
        else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return 2;
        }
    }
    if (data.empty()) {
        std::fprintf(stderr, "usage: %s --data DIR [--port N] [--res-version V] [--mock-root DIR]\n", argv[0]);
        return 2;
    }
    LinkuraLocal::Standalone::SetDataRoot(data);
    LinkuraLocal::Standalone::OfflineHttpServer server(config);
    std::string error;
    if (!server.Start(&error)) {
        std::fprintf(stderr, "start failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("READY %u\n", static_cast<unsigned>(server.Port()));
    std::fflush(stdout);
    std::string line;
    while (std::getline(std::cin, line)) {
    }
    server.Stop();
    return 0;
}
