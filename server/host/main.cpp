// Host (macOS) build of the iOS offline server, for local testing:
//   linkura-offline-server --data DIR [--port N] [--res-version V] [--mock-root DIR]
//     [--asset-root DIR] [--remote-asset-base URL [--remote-asset-key K] [--remote-chunk-bytes N]]
//     [--res-version-file PATH] [--motion-replays on|off]
// The asset proxy fetches with curl (the iOS build uses NSURLSession); the
// key goes in an x-linkura-key header. --res-version-file is read for every
// response (a stand-in for the version the remote asset base publishes).
// Runs until stdin closes (so a test harness can stop it), then exits.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "RouteRegistry.hpp"
#include "standalone/MockPlatformStandalone.hpp"
#include "standalone/OfflineHttpServer.hpp"

namespace {
    std::string ReadAll(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    std::string Quote(const std::string& value) {
        std::string out = "'";
        for (char c : value) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return out + "'";
    }

    bool CurlFetch(const std::string& key, const std::string& url, const std::string& range,
                   LinkuraLocal::Standalone::ServerConfig::RemoteResponse& out) {
        char headers[] = "/tmp/linkura-host-hXXXXXX";
        char body[] = "/tmp/linkura-host-bXXXXXX";
        const int hf = ::mkstemp(headers), bf = ::mkstemp(body);
        if (hf < 0 || bf < 0) return false;
        ::close(hf);
        ::close(bf);
        std::string command = "curl -s --max-time 120 -D " + Quote(headers) + " -o " + Quote(body);
        if (!range.empty()) command += " -H " + Quote("Range: " + range);
        if (!key.empty()) command += " -H " + Quote("x-linkura-key: " + key);
        command += " " + Quote(url);
        const int rc = std::system(command.c_str());
        const std::string head = ReadAll(headers);
        out.body = ReadAll(body);
        ::unlink(headers);
        ::unlink(body);
        if (rc != 0 || head.empty()) return false;
        std::istringstream lines(head);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("HTTP/", 0) == 0) {
                out.status = std::atoi(line.substr(line.find(' ') + 1).c_str());
                out.contentRange.clear();
            } else if (line.size() > 14 && (line.rfind("Content-Range:", 0) == 0 || line.rfind("content-range:", 0) == 0)) {
                out.contentRange = line.substr(line.find(':') + 1);
                while (!out.contentRange.empty() && out.contentRange.front() == ' ') out.contentRange.erase(0, 1);
            }
        }
        return out.status != 0;
    }
}

int main(int argc, char** argv) {
    LinkuraLocal::Standalone::ServerConfig config;
    std::string data, remoteKey, versionFile;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const std::string value = argv[i + 1];
        if (key == "--data") data = value;
        else if (key == "--port") config.port = static_cast<std::uint16_t>(std::atoi(value.c_str()));
        else if (key == "--res-version") config.resVersion = value;
        else if (key == "--mock-root") config.mockRoot = value;
        else if (key == "--asset-root") config.assetRoot = value;
        else if (key == "--remote-asset-base") config.remoteAssetBase = value;
        else if (key == "--remote-asset-key") remoteKey = value;
        else if (key == "--remote-chunk-bytes") config.remoteChunkBytes = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "--res-version-file") versionFile = value;
        else if (key == "--motion-replays") LinkuraLocal::HttpMock::SetMotionReplaysEnabled(value != "off");
        else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return 2;
        }
    }
    if (data.empty()) {
        std::fprintf(stderr, "usage: %s --data DIR [--port N] [--res-version V] [--mock-root DIR]\n", argv[0]);
        return 2;
    }
    if (!config.remoteAssetBase.empty()) {
        config.fetchRemote = [remoteKey](const std::string& url, const std::string& range,
                                         LinkuraLocal::Standalone::ServerConfig::RemoteResponse& out) {
            return CurlFetch(remoteKey, url, range, out);
        };
    }
    if (!versionFile.empty()) {
        config.resVersionSource = [versionFile] {
            std::string v = ReadAll(versionFile);
            while (!v.empty() && (v.back() == '\n' || v.back() == ' ')) v.pop_back();
            return v;
        };
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
