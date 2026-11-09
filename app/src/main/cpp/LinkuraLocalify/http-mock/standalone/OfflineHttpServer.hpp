#pragma once

// Loopback HTTP/1.1 front end for the offline API backend.
//
// On iOS the game cannot be hooked at runtime (no inline patching on a
// non-jailbroken device, and UnityFramework exports no il2cpp_* API), so the
// IPA's API base URL is rewritten to http://127.0.0.1:<port> and this server,
// running inside the injected dylib, answers the game's real HTTP requests
// with the same RouteRegistry/HttpMockBackend used by the Android hooks.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

namespace LinkuraLocal::Standalone {
    struct ServerConfig {
        std::uint16_t port = 52680;
        // Value for ${RES_VERSION} in response headers. Empty: the
        // x-res-version header is left out entirely.
        std::string resVersion;
        // Optional overrides: <mockRoot>/<api path>.json (and .headers),
        // e.g. mock_api/v1/user/login.json, take precedence over routes.
        std::filesystem::path mockRoot;
    };

    struct HttpRequest {
        std::string method;
        std::string target;   // request target as sent (path + query)
        std::string path;     // target without query string
        std::map<std::string, std::string> headers;  // lower-case names
        std::string body;
    };

    struct HttpResponse {
        int status = 200;
        std::string reason = "OK";
        std::string headersText;  // "name: value" lines
        std::string body;
    };

    // Pure request handler (no sockets); used by the server and by tests.
    HttpResponse HandleRequest(const HttpRequest& request, const ServerConfig& config);

    // Serialize a response (Content-Length framing, keep-alive aware).
    std::string SerializeResponse(const HttpResponse& response, bool keepAlive);

    class OfflineHttpServer {
    public:
        explicit OfflineHttpServer(ServerConfig config);
        ~OfflineHttpServer();
        // Binds 127.0.0.1:<port> and starts the accept thread.
        bool Start(std::string* error = nullptr);
        void Stop();
        std::uint16_t Port() const { return config_.port; }

    private:
        void AcceptLoop();
        void ServeConnection(int fd);

        ServerConfig config_;
        int listenFd_ = -1;
        std::atomic<bool> running_{false};
        std::thread acceptThread_;
    };
}
