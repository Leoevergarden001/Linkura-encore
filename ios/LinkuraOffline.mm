// LinkuraOffline.dylib — offline runtime for the iOS client.
//
// Loaded through an LC_LOAD_DYLIB command added to the game executable
// (see linkura-preservation `ios-offline-stage`). At load time it starts the
// loopback API server that the patched metadata points the game at, before
// UnityFramework issues its first request.
//
// Files (visible in the Files app / Finder because the staged Info.plist
// enables file sharing):
//   Documents/LinkuraOffline/server.log   request and error log
//   Documents/LinkuraOffline/config.json  optional {"port":52680,"resVersion":"...",
//                                          "remoteAssetBase":"https://<asset host>/a",
//                                          "remoteAssetKey":"<this device's key>"}
//                                          (with remoteAssetBase, <base>/version.json
//                                          overrides resVersion when reachable)
//   Documents/LinkuraOffline/mock_api/... optional per-path JSON overrides
//                                          (development build only)
// The player database lives in Library/Application Support/LinkuraOffline.

#import <Foundation/Foundation.h>
#include <chrono>
#include <future>
#include <memory>
#include <os/log.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include "standalone/MockPlatformStandalone.hpp"
#include "standalone/OfflineHttpServer.hpp"

namespace {
    std::mutex g_fileMutex;
    FILE* g_logFile = nullptr;
    os_log_t g_log = nullptr;
    std::unique_ptr<LinkuraLocal::Standalone::OfflineHttpServer> g_server;

    void LogSink(const char* level, const char* message) {
        os_log_with_type(g_log, level[0] == 'E' ? OS_LOG_TYPE_ERROR : OS_LOG_TYPE_DEFAULT,
                         "[LinkuraOffline] %{public}s %{public}s", level, message);
        std::lock_guard<std::mutex> lock(g_fileMutex);
        if (!g_logFile) return;
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", &local);
        std::fprintf(g_logFile, "%s %s %s\n", stamp, level, message);
        std::fflush(g_logFile);
    }

    NSURL* EnsureDirectory(NSSearchPathDirectory kind, NSString* name) {
        NSFileManager* files = NSFileManager.defaultManager;
        NSURL* base = [files URLForDirectory:kind inDomain:NSUserDomainMask
                           appropriateForURL:nil create:YES error:nil];
        if (!base) return nil;
        NSURL* directory = [base URLByAppendingPathComponent:name isDirectory:YES];
        [files createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:nil];
        return directory;
    }
}

// The game keeps its local-storage key (and other state) in encrypted Unity
// PlayerPrefs, which iOS stores in NSUserDefaults under the same hashed key
// names as Android. Documents/LinkuraOffline/storage-key.json uses the
// Android resource-pack format ({"entries": {"<url-encoded name>": "<url-
// encoded value>"}}); seed those entries before Unity reads PlayerPrefs so an
// imported Documents/D, Documents/M cache is recognised. A null value removes
// the entry (for example the source account's player id from an older setup).
static void SeedPlayerPrefs(NSURL* visible) {
    NSURL* file = [visible URLByAppendingPathComponent:@"storage-key.json"];
    NSData* raw = [NSData dataWithContentsOfURL:file];
    if (!raw) return;
    id parsed = [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];
    NSDictionary* entries = [parsed isKindOfClass:NSDictionary.class] ? parsed[@"entries"] : nil;
    if (![entries isKindOfClass:NSDictionary.class]) {
        LogSink("E", "storage-key.json has no entries");
        return;
    }
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    int changed = 0;
    for (NSString* name in entries) {
        id value = entries[name];
        if (![name isKindOfClass:NSString.class]) continue;
        NSString* key = [name stringByRemovingPercentEncoding] ?: name;
        if (value == NSNull.null) {
            if ([defaults objectForKey:key]) {
                [defaults removeObjectForKey:key];
                changed++;
            }
            continue;
        }
        if (![value isKindOfClass:NSString.class]) continue;
        NSString* decoded = [(NSString*)value stringByRemovingPercentEncoding] ?: value;
        if (![[defaults stringForKey:key] isEqualToString:decoded]) {
            [defaults setObject:decoded forKey:key];
            changed++;
        }
    }
    if (changed) [defaults synchronize];
    char line[128];
    std::snprintf(line, sizeof(line), "PlayerPrefs seed: %lu entries, %d updated",
                  static_cast<unsigned long>(entries.count), changed);
    LogSink("I", line);
}

// Development remote (LinkuraRemote.mm); weak so builds without it link.
extern "C" __attribute__((weak)) void LinkuraRemoteStart(NSURL* directory);
void LinkuraPushStart();
void LinkuraOfflineLog(const char* level, const char* message) { LogSink(level, message); }

// Asset proxy for config.json "remoteAssetBase": the game may only use plain
// HTTP towards loopback (Unity rejects other insecure URLs), so the local
// server fetches missing assets from the resource server with NSURLSession.
// config.json "remoteAssetKey" (one per device) is sent as x-linkura-key; the
// asset host (R2 behind Cloudflare) refuses requests without a valid key.
static std::string g_remoteAssetKey;
static NSURLSession* AssetProxySession() {
    static NSURLSession* session;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURLSessionConfiguration* configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        configuration.HTTPMaximumConnectionsPerHost = 8;
        configuration.URLCache = nil;
        configuration.timeoutIntervalForRequest = 120;
        session = [NSURLSession sessionWithConfiguration:configuration];
    });
    return session;
}

static bool FetchRemoteAsset(const std::string& url, const std::string& range,
                             LinkuraLocal::Standalone::ServerConfig::RemoteResponse& out) {
    @autoreleasepool {
        NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!target) return false;
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:target
                                                               cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                           timeoutInterval:120];
        if (!range.empty()) [request setValue:[NSString stringWithUTF8String:range.c_str()] forHTTPHeaderField:@"Range"];
        if (!g_remoteAssetKey.empty()) {
            [request setValue:[NSString stringWithUTF8String:g_remoteAssetKey.c_str()] forHTTPHeaderField:@"x-linkura-key"];
        }
        __block NSData* body = nil;
        __block NSHTTPURLResponse* http = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[AssetProxySession() dataTaskWithRequest:request
                                completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            body = data;
            if ([response isKindOfClass:NSHTTPURLResponse.class]) http = (NSHTTPURLResponse*)response;
            if (error) {
                char line[512];
                std::snprintf(line, sizeof(line), "asset proxy: %s (%s %ld) %s", url.c_str(),
                              error.domain.UTF8String ?: "?", static_cast<long>(error.code),
                              error.localizedDescription.UTF8String ?: "");
                LogSink("W", line);
                id path = error.userInfo[@"_NSURLErrorNWPathKey"];
                NSString* detail = [NSString stringWithFormat:@"asset proxy path: %@ | reason: %@",
                                    path ?: @"-", error.userInfo[@"_kCFStreamErrorCodeKey"] ?: @"-"];
                LogSink("W", detail.UTF8String ?: "");
            }
            dispatch_semaphore_signal(done);
        }] resume];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (!http) return false;
        out.status = static_cast<int>(http.statusCode);
        NSString* contentRange = http.allHeaderFields[@"Content-Range"];
        if ([contentRange isKindOfClass:NSString.class]) out.contentRange = contentRange.UTF8String;
        if (body) out.body.assign(static_cast<const char*>(body.bytes), body.length);
        return true;
    }
}

__attribute__((constructor))
static void LinkuraOfflineStart() {
    @autoreleasepool {
        g_log = os_log_create("org.linkura.offline", "server");
        NSURL* visible = EnsureDirectory(NSDocumentDirectory, @"LinkuraOffline");
        NSURL* data = EnsureDirectory(NSApplicationSupportDirectory, @"LinkuraOffline");
        if (visible) {
            NSString* logPath = [visible URLByAppendingPathComponent:@"server.log"].path;
            g_logFile = std::fopen(logPath.fileSystemRepresentation, "a");
        }
        LinkuraLocal::Standalone::SetLogSink(LogSink);
        if (!data) {
            LogSink("E", "Application Support directory unavailable; offline server not started");
            return;
        }
        LinkuraLocal::Standalone::SetDataRoot(data.path.fileSystemRepresentation);

        LinkuraLocal::Standalone::ServerConfig config;
        if (visible) {
#if defined(LINKURA_DEV_BUILD)
            // Development only: a leftover override (for example an empty
            // login bonus or mission list) must never shadow a release route.
            config.mockRoot = [visible URLByAppendingPathComponent:@"mock_api"].path.fileSystemRepresentation;
#endif
            config.assetRoot = [visible URLByAppendingPathComponent:@"assets"].path.fileSystemRepresentation;
            NSData* raw = [NSData dataWithContentsOfURL:[visible URLByAppendingPathComponent:@"config.json"]];
            id parsed = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
            if ([parsed isKindOfClass:NSDictionary.class]) {
                NSDictionary* settings = parsed;
                if ([settings[@"port"] isKindOfClass:NSNumber.class]) {
                    config.port = static_cast<std::uint16_t>([settings[@"port"] unsignedShortValue]);
                }
                if ([settings[@"remoteAssetKey"] isKindOfClass:NSString.class]) {
                    g_remoteAssetKey = [settings[@"remoteAssetKey"] UTF8String];
                }
                if ([settings[@"remoteAssetBase"] isKindOfClass:NSString.class]) {
                    config.remoteAssetBase = [settings[@"remoteAssetBase"] UTF8String];
                    config.fetchRemote = FetchRemoteAsset;
                    // The asset base publishes its resource version (<base>/version.json,
                    // {"resVersion": "..."}), so a new build on the asset host needs no
                    // config change on the device. Fetched once at startup; responses
                    // wait for it until 6 s after launch at most, then fall back to the
                    // configured resVersion.
                    auto remoteVersion = std::make_shared<std::promise<std::string>>();
                    std::shared_future<std::string> versionReady = remoteVersion->get_future().share();
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
                    const std::string versionUrl = [&] {
                        std::string b = config.remoteAssetBase;
                        while (!b.empty() && b.back() == '/') b.pop_back();
                        return b + "/version.json";
                    }();
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                        LinkuraLocal::Standalone::ServerConfig::RemoteResponse out;
                        std::string version;
                        if (FetchRemoteAsset(versionUrl, std::string{}, out) && out.status == 200) {
                            NSData* json = [NSData dataWithBytes:out.body.data() length:out.body.size()];
                            id doc = [NSJSONSerialization JSONObjectWithData:json options:0 error:nil];
                            if ([doc isKindOfClass:NSDictionary.class] &&
                                [doc[@"resVersion"] isKindOfClass:NSString.class]) {
                                version = [doc[@"resVersion"] UTF8String];
                            }
                        }
                        char line[512];
                        std::snprintf(line, sizeof(line), "remote resVersion %s -> %s (http %d)", versionUrl.c_str(),
                                      version.empty() ? "(none, using config.json)" : version.c_str(), out.status);
                        LogSink("I", line);
                        remoteVersion->set_value(version);
                    });
                    config.resVersionSource = [versionReady, deadline]() -> std::string {
                        if (versionReady.wait_until(deadline) != std::future_status::ready) return {};
                        return versionReady.get();
                    };
                    // Startup self-test: can this app reach the resource server and the internet?
                    const std::string base = config.remoteAssetBase;
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                                   dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                        for (const std::string& probe : {base, std::string("http://captive.apple.com/hotspot-detect.html")}) {
                            LinkuraLocal::Standalone::ServerConfig::RemoteResponse out;
                            const bool ok = FetchRemoteAsset(probe, "bytes=0-0", out);
                            char line[512];
                            std::snprintf(line, sizeof(line), "network self-test %s -> %s %d", probe.c_str(),
                                          ok ? "response" : "no response", out.status);
                            LogSink("I", line);
                        }
                    });
                }
                if ([settings[@"resVersion"] isKindOfClass:NSString.class]) {
                    config.resVersion = [settings[@"resVersion"] UTF8String];
                }
            }
        }
        if (visible) SeedPlayerPrefs(visible);
        if (visible && LinkuraRemoteStart) LinkuraRemoteStart(visible);
        LinkuraPushStart();
        NSString* version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        char banner[256];
        std::snprintf(banner, sizeof(banner), "LinkuraOffline start: client %s, port %u, resVersion '%s'",
                      version.UTF8String ?: "?", static_cast<unsigned>(config.port), config.resVersion.c_str());
        LogSink("I", banner);

        g_server = std::make_unique<LinkuraLocal::Standalone::OfflineHttpServer>(config);
        std::string error;
        if (!g_server->Start(&error)) {
            LogSink("E", ("offline server failed: " + error).c_str());
        }
    }
}
