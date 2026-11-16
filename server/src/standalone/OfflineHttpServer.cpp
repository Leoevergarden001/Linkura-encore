#include "OfflineHttpServer.hpp"

#include "../RouteRegistry.hpp"
#include "MockPlatformStandalone.hpp"
#include "offline_api_mock_builtin.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace LinkuraLocal::Standalone {
    namespace {
        std::string Lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string Trim(const std::string& value) {
            const auto begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return {};
            const auto end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
            if (from.empty()) return;
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        std::string HttpDateNow() {
            const std::time_t now = std::time(nullptr);
            std::tm gmt{};
            gmtime_r(&now, &gmt);
            char buffer[64];
            std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &gmt);
            return buffer;
        }

        std::string ReadFile(const std::filesystem::path& path) {
            std::error_code ec;
            if (path.empty() || !std::filesystem::is_regular_file(path, ec)) return {};
            std::ifstream stream(path, std::ios::binary);
            std::ostringstream content;
            content << stream.rdbuf();
            return content.str();
        }

        // "/v1/user/login" -> <root>/v1/user/login<suffix>; rejects traversal.
        std::filesystem::path MockFile(const std::filesystem::path& root, const std::string& path,
                                       const char* suffix) {
            if (root.empty() || path.size() < 2 || path[0] != '/') return {};
            if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos) return {};
            return root / (path.substr(1) + suffix);
        }

        // Archive covers (thumbnail_image_url) and motion-capture replays
        // (archive_url) point at the official CDN, which no longer serves them.
        // Point a URL at this server's /a/ instead when <assetRoot> holds a
        // file at the same path, or whenever a remote asset base is configured
        // (the /a/ proxy forwards it to the resource server, which keeps the
        // replay data and has a cover for every archive). A cover that fails to
        // load is not just blank: the list reuses its cells and keeps the
        // previous archive's cover and date. Other URLs are left as they are.
        void RedirectLocalAssets(std::string& body, const ServerConfig& config) {
            static const std::string kCdn = "https://assets.link-like-lovelive.app/";
            if (config.assetRoot.empty() && config.remoteAssetBase.empty()) return;
            const std::string local = "http://127.0.0.1:" + std::to_string(config.port) + "/a/";
            size_t pos = 0;
            while ((pos = body.find(kCdn, pos)) != std::string::npos) {
                const size_t end = body.find('"', pos);
                if (end == std::string::npos) break;
                const std::string rest = body.substr(pos + kCdn.size(), end - pos - kCdn.size());
                const auto file = MockFile(config.assetRoot, "/" + rest, "");
                std::error_code ec;
                if (!config.remoteAssetBase.empty() || (!file.empty() && std::filesystem::is_regular_file(file, ec))) {
                    body.replace(pos, kCdn.size(), local);
                    pos += local.size();
                } else {
                    pos += kCdn.size();
                }
            }
        }

        const char* AssetContentType(const std::string& path) {
            const auto dot = path.rfind('.');
            const std::string ext = dot == std::string::npos ? std::string{} : Lower(path.substr(dot + 1));
            if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
            if (ext == "png") return "image/png";
            return "application/octet-stream";
        }

        // The iOS client reads its paid balance from jewel_paid_apple; the
        // backend keeps the wallet in jewel_paid_google. Mirror the value.
        void MirrorAppleWallet(std::string& headers) {
            const std::string google = "jewel_paid_google: ";
            const std::string apple = "jewel_paid_apple: ";
            const auto g = headers.find(google);
            const auto a = headers.find(apple);
            if (g == std::string::npos || a == std::string::npos) return;
            const auto gEnd = headers.find('\n', g);
            const auto value = headers.substr(g + google.size(), gEnd - g - google.size());
            const auto aEnd = headers.find('\n', a);
            headers.replace(a + apple.size(), aEnd - a - apple.size(), value);
        }

        // Header template expansion, as the Android HttpMock glue does.
        std::string ExpandHeaders(std::string headers, const ServerConfig& config) {
            MirrorAppleWallet(headers);
            ReplaceAll(headers, "${DATE_GMT}", HttpDateNow());
            if (!config.resVersion.empty()) {
                ReplaceAll(headers, "${RES_VERSION}", config.resVersion);
                return headers;
            }
            std::istringstream lines(headers);
            std::string line, kept;
            while (std::getline(lines, line)) {
                if (line.find("${RES_VERSION}") != std::string::npos) continue;
                kept += line;
                kept += '\n';
            }
            return kept;
        }

        bool SendAll(int fd, const std::string& data) {
            size_t sent = 0;
            while (sent < data.size()) {
                const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        // Buffered reader over a socket.
        class Reader {
        public:
            explicit Reader(int fd) : fd_(fd) {}

            bool Fill() {
                char chunk[16384];
                for (;;) {
                    const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) return false;
                    buffer_.append(chunk, static_cast<size_t>(n));
                    return true;
                }
            }

            bool ReadUntil(const std::string& delimiter, std::string& out, size_t limit) {
                for (;;) {
                    const auto pos = buffer_.find(delimiter);
                    if (pos != std::string::npos) {
                        out = buffer_.substr(0, pos);
                        buffer_.erase(0, pos + delimiter.size());
                        return true;
                    }
                    if (buffer_.size() > limit || !Fill()) return false;
                }
            }

            bool ReadExact(size_t count, std::string& out) {
                while (buffer_.size() < count) {
                    if (!Fill()) return false;
                }
                out.append(buffer_, 0, count);
                buffer_.erase(0, count);
                return true;
            }

        private:
            int fd_;
            std::string buffer_;
        };

        constexpr size_t kMaxHeaderBytes = 64 * 1024;
        constexpr size_t kMaxBodyBytes = 32 * 1024 * 1024;
    }

    HttpResponse HandleRequest(const HttpRequest& request, const ServerConfig& config) {
        HttpResponse response;
        const std::string& path = request.path;

        // Asset requests (the IPA's asset base URLs point here): serve
        // <assetRoot>/<path after /a/> when present, otherwise log the miss.
        if (path.rfind("/a/", 0) == 0) {
            const auto file = MockFile(config.assetRoot, path.substr(2), "");
            std::string content = ReadFile(file);
            std::error_code ec;
            if (!file.empty() && std::filesystem::is_regular_file(file, ec)) {
                Log::InfoFmt("[OfflineServer] asset %s -> %zu bytes", path.c_str(), content.size());
                response.headersText = std::string("content-type: ") + AssetContentType(path) + "\n";
                response.body = std::move(content);
                return response;
            }
            if (!config.remoteAssetBase.empty() && config.fetchRemote) {
                ServerConfig::RemoteResponse remote;
                const auto range = request.headers.find("range");
                std::string base = config.remoteAssetBase;
                while (!base.empty() && base.back() == '/') base.pop_back();
                const std::string url = base + path.substr(2);
                if (config.fetchRemote(url, range == request.headers.end() ? std::string{} : range->second, remote) &&
                    (remote.status == 200 || remote.status == 206)) {
                    Log::InfoFmt("[OfflineServer] asset %s -> proxied %d (%zu bytes)", path.c_str(), remote.status,
                                 remote.body.size());
                    response.status = remote.status;
                    response.reason = remote.status == 206 ? "Partial Content" : "OK";
                    response.headersText = std::string("content-type: ") + AssetContentType(path) +
                                           "\naccept-ranges: bytes\n";
                    if (!remote.contentRange.empty()) response.headersText += "content-range: " + remote.contentRange + "\n";
                    response.body = std::move(remote.body);
                    return response;
                }
                Log::WarnFmt("[OfflineServer] asset proxy failed: %s -> %d", path.c_str(), remote.status);
            }
            Log::WarnFmt("[OfflineServer] asset request not served: %s", request.target.c_str());
            // No body: the game's downloader writes whatever body it gets into
            // the asset's cache file regardless of status, then resumes from
            // that length with a Range request, so an error text would end up
            // as the start of the asset and fail its checksum for good.
            response.status = 404;
            response.reason = "Not Found";
            response.body.clear();
            return response;
        }

        std::string body = ReadFile(MockFile(config.mockRoot, path, ".json"));
        std::string headers;
        if (!body.empty()) {
            headers = ReadFile(MockFile(config.mockRoot, path, ".headers"));
            if (headers.empty()) headers = std::string(OfflineApiMockBuiltIn::DefaultHeadersView);
            Log::InfoFmt("[OfflineServer] %s -> mock file", path.c_str());
        } else {
            auto routed = HttpMock::ResolveRegisteredRoute(HttpMock::MockRequestContext{path, request.body});
            if (!routed.has_value()) {
                Log::WarnFmt("[OfflineServer] no route for %s %s (body %zu bytes)",
                             request.method.c_str(), path.c_str(), request.body.size());
                response.status = 404;
                response.reason = "Not Found";
                response.headersText = "content-type: application/json; charset=UTF-8\n";
                response.body = "{}";
                return response;
            }
            body = std::move(routed->body);
            headers = std::move(routed->headersText);
            if (path.rfind("/v1/archive/", 0) == 0) RedirectLocalAssets(body, config);
            response.status = routed->statusCode;
            response.reason = response.status == 200 ? "OK" : "Offline";
            Log::InfoFmt("[OfflineServer] %s -> %d (%zu bytes)", path.c_str(), response.status, body.size());
        }
        response.headersText = ExpandHeaders(std::move(headers), config);
        response.body = std::move(body);
        return response;
    }

    std::string SerializeResponse(const HttpResponse& response, bool keepAlive) {
        std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " + response.reason + "\r\n";
        bool hasContentType = false;
        std::istringstream lines(response.headersText);
        std::string line;
        while (std::getline(lines, line)) {
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = Trim(line.substr(0, colon));
            const std::string value = Trim(line.substr(colon + 1));
            const std::string lower = Lower(name);
            // Framing is ours; templates from captured traffic say "chunked".
            if (name.empty() || lower == "transfer-encoding" || lower == "content-length" ||
                lower == "connection") continue;
            if (lower == "content-type") hasContentType = true;
            out += name + ": " + value + "\r\n";
        }
        if (!hasContentType) out += "content-type: application/json; charset=UTF-8\r\n";
        out += "content-length: " + std::to_string(response.body.size()) + "\r\n";
        out += keepAlive ? "connection: keep-alive\r\n" : "connection: close\r\n";
        out += "\r\n";
        out += response.body;
        return out;
    }

    OfflineHttpServer::OfflineHttpServer(ServerConfig config) : config_(std::move(config)) {}

    OfflineHttpServer::~OfflineHttpServer() { Stop(); }

    namespace {
        int OpenListenSocket(std::uint16_t port, std::string* error) {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                if (error) *error = std::string("socket: ") + std::strerror(errno);
                return -1;
            }
            int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(fd, 64) != 0) {
                if (error) *error = std::string("bind/listen 127.0.0.1:") + std::to_string(port) + ": " +
                                    std::strerror(errno);
                ::close(fd);
                return -1;
            }
            return fd;
        }

        // iOS takes the listening socket of a suspended app away: accept() then
        // fails with ECONNABORTED (or EBADF) for good, so the socket is replaced.
        bool ListenSocketGone(int err) {
            return err == ECONNABORTED || err == EBADF || err == EINVAL || err == ENOTSOCK;
        }
    }

    bool OfflineHttpServer::Start(std::string* error) {
        if (running_) return true;
        const int fd = OpenListenSocket(config_.port, error);
        if (fd < 0) return false;
        listenFd_ = fd;
        running_ = true;
        acceptThread_ = std::thread([this] { AcceptLoop(); });
        Log::InfoFmt("[OfflineServer] listening on 127.0.0.1:%u", static_cast<unsigned>(config_.port));
        return true;
    }

    void OfflineHttpServer::Stop() {
        if (!running_.exchange(false)) return;
        const int fd = listenFd_.exchange(kStopped);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (acceptThread_.joinable()) acceptThread_.join();
    }

    void OfflineHttpServer::AcceptLoop() {
        bool failing = false;  // log a failure streak once, not every retry
        while (running_) {
            const int listenFd = listenFd_.load();
            if (listenFd == kStopped) break;
            const int fd = listenFd >= 0 ? ::accept(listenFd, nullptr, nullptr) : -1;
            if (fd < 0) {
                const int err = listenFd >= 0 ? errno : EBADF;
                if (err == EINTR) continue;
                if (!running_) break;
                if (!failing) {
                    Log::WarnFmt("[OfflineServer] accept failed: %s", std::strerror(err));
                    failing = true;
                }
                if (ListenSocketGone(err)) {
                    int expected = listenFd;
                    if (listenFd >= 0 && listenFd_.compare_exchange_strong(expected, -1)) ::close(listenFd);
                    std::string error;
                    const int fresh = OpenListenSocket(config_.port, &error);
                    if (fresh >= 0) {
                        expected = -1;
                        if (!listenFd_.compare_exchange_strong(expected, fresh)) {  // stopped meanwhile
                            ::close(fresh);
                            break;
                        }
                        Log::InfoFmt("[OfflineServer] listening again on 127.0.0.1:%u",
                                     static_cast<unsigned>(config_.port));
                        failing = false;
                        continue;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            failing = false;
#ifdef SO_NOSIGPIPE
            int yes = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            int noDelay = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
            std::thread([this, fd] { ServeConnection(fd); }).detach();
        }
    }

    void OfflineHttpServer::ServeConnection(int fd) {
        Reader reader(fd);
        for (;;) {
            std::string head;
            if (!reader.ReadUntil("\r\n\r\n", head, kMaxHeaderBytes)) break;
            HttpRequest request;
            std::istringstream lines(head);
            std::string requestLine;
            std::getline(lines, requestLine);
            {
                std::istringstream parts(Trim(requestLine));
                std::string version;
                parts >> request.method >> request.target >> version;
            }
            if (request.method.empty() || request.target.empty()) break;
            std::string line;
            while (std::getline(lines, line)) {
                const auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                request.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
            }
            const auto query = request.target.find('?');
            request.path = query == std::string::npos ? request.target : request.target.substr(0, query);
            // Absolute-form targets ("http://host/v1/...") from proxies.
            if (request.path.rfind("http://", 0) == 0) {
                const auto slash = request.path.find('/', 7);
                request.path = slash == std::string::npos ? "/" : request.path.substr(slash);
            }

            const auto expect = request.headers.find("expect");
            if (expect != request.headers.end() && Lower(expect->second) == "100-continue") {
                if (!SendAll(fd, "HTTP/1.1 100 Continue\r\n\r\n")) break;
            }

            bool ok = true;
            const auto te = request.headers.find("transfer-encoding");
            if (te != request.headers.end() && Lower(te->second).find("chunked") != std::string::npos) {
                for (;;) {
                    std::string sizeLine;
                    if (!reader.ReadUntil("\r\n", sizeLine, 1024)) { ok = false; break; }
                    const size_t size = std::strtoul(sizeLine.c_str(), nullptr, 16);
                    if (size == 0) {
                        std::string trailer;
                        reader.ReadUntil("\r\n", trailer, kMaxHeaderBytes);
                        break;
                    }
                    if (request.body.size() + size > kMaxBodyBytes ||
                        !reader.ReadExact(size, request.body)) { ok = false; break; }
                    std::string crlf;
                    if (!reader.ReadExact(2, crlf)) { ok = false; break; }
                }
            } else {
                const auto length = request.headers.find("content-length");
                if (length != request.headers.end()) {
                    const size_t size = std::strtoul(length->second.c_str(), nullptr, 10);
                    if (size > kMaxBodyBytes || !reader.ReadExact(size, request.body)) ok = false;
                }
            }
            if (!ok) break;

            const auto connection = request.headers.find("connection");
            const bool keepAlive = connection == request.headers.end() ||
                                   Lower(connection->second) != "close";
            HttpResponse response;
            try {
                response = HandleRequest(request, config_);
            } catch (const std::exception& e) {
                Log::ErrorFmt("[OfflineServer] handler threw for %s: %s", request.path.c_str(), e.what());
                response.status = 500;
                response.reason = "Internal Server Error";
                response.body = "{}";
            }
            if (!SendAll(fd, SerializeResponse(response, keepAlive)) || !keepAlive) break;
        }
        ::close(fd);
    }
}
