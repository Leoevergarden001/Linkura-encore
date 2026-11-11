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
//   Documents/LinkuraOffline/config.json  optional {"port":52680,"resVersion":"..."}
//   Documents/LinkuraOffline/mock_api/... optional per-path JSON overrides
// The player database lives in Library/Application Support/LinkuraOffline.

#import <Foundation/Foundation.h>
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
// imported Documents/D, Documents/M cache is recognised.
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
        if (![name isKindOfClass:NSString.class] || ![value isKindOfClass:NSString.class]) continue;
        NSString* key = [name stringByRemovingPercentEncoding] ?: name;
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
            config.mockRoot = [visible URLByAppendingPathComponent:@"mock_api"].path.fileSystemRepresentation;
            config.assetRoot = [visible URLByAppendingPathComponent:@"assets"].path.fileSystemRepresentation;
            NSData* raw = [NSData dataWithContentsOfURL:[visible URLByAppendingPathComponent:@"config.json"]];
            id parsed = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
            if ([parsed isKindOfClass:NSDictionary.class]) {
                NSDictionary* settings = parsed;
                if ([settings[@"port"] isKindOfClass:NSNumber.class]) {
                    config.port = static_cast<std::uint16_t>([settings[@"port"] unsignedShortValue]);
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
