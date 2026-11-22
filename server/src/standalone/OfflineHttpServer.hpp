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
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace LinkuraLocal::Standalone {
    struct ServerConfig {
        std::uint16_t port = 52680;
        // Value for ${RES_VERSION} in response headers. Empty: the
        // x-res-version header is left out entirely.
        std::string resVersion;
        // Optional: called for every response that needs ${RES_VERSION}; a
        // non-empty result replaces resVersion (e.g. the version the remote
        // asset base publishes, fetched at startup). May block briefly.
        std::function<std::string()> resVersionSource;
        // Optional overrides: <mockRoot>/<api path>.json (and .headers),
        // e.g. mock_api/v1/user/login.json, take precedence over routes.
        std::filesystem::path mockRoot;
        // Asset files served for /a/<path> (the rewritten asset base URLs),
        // e.g. assets/raw/oj/oj4rf2oo3u37dl5nkqrn5zfcyy for a catalog.
        std::filesystem::path assetRoot;
        // Asset proxy. Unity only lets the game make plain-HTTP requests to
        // loopback, so assets that are not in <assetRoot> can be fetched by
        // this server instead: GET <remoteAssetBase>/<path after /a/> through
        // fetchRemote (the platform's HTTP client), forwarding Range.
        std::string remoteAssetBase;
        struct RemoteResponse {
            int status = 0;
            std::string contentRange;
            std::string body;
        };
        std::function<bool(const std::string& url, const std::string& range, RemoteResponse& out)> fetchRemote;
        // Proxied assets are fetched and sent on in pieces of at most this
        // many bytes (Range requests; the next piece is fetched while the
        // current one is sent), so a large asset (story movies are up to
        // 418 MB) never sits in memory whole: at most two pieces per transfer.
        std::size_t remoteChunkBytes = 4u << 20;
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
        // Streamed body (proxied assets): when set, body is empty and the
        // response carries streamLength bytes, produced by stream(). stream
        // calls write for each piece and returns false if the body could not
        // be completed (the connection is then closed so the client retries).
        std::uint64_t streamLength = 0;
        std::function<bool(const std::function<bool(const std::string&)>& write)> stream;
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
        // -1: no socket (being reopened), kStopped: Stop() ran.
        static constexpr int kStopped = -2;
        std::atomic<int> listenFd_{-1};
        std::atomic<bool> running_{false};
        std::thread acceptThread_;
    };
}
