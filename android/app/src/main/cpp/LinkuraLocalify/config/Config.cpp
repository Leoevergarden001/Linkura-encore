#include <string>
#include "nlohmann/json.hpp"
#include "../Log.h"
#include <thread>
#include <fstream>
#include <unordered_map>
#include <sstream>
#include "version_compatibility.h"

namespace LinkuraLocal::Config {
    bool isConfigInit = false;

    bool dbgMode = false;
    bool enabled = true;
    bool renderHighResolution = true;
    bool fesArchiveUnlockTicket = false;
    bool lazyInit = true;
    bool replaceFont = true;
    bool textTest = false;
    bool dumpText = false;
    bool enableFreeCamera = false;
    bool memorizeFreeCameraPos = false;
    int targetFrameRate = 0;
    bool removeRenderImageCover = false;
    bool avoidCharacterExit = false;
    bool storyHideBackground = false;
    bool storyHideTransition = false;
    bool storyHideNonCharacter3d = false;
    bool storyHideDof = false;
    bool storyHideEffect = false;
    float storyNovelVocalTextDurationRate = 1.0f;
    float storyNovelNonVocalTextDurationRate = 1.0f;
    std::string storyReplaceContent;
    bool firstPersonCameraHideHead = true;
    bool firstPersonCameraHideHair = true;
    bool enableMotionCaptureReplay = true;
    bool enableInGameReplayDisplay = true;
    std::string motionCaptureResourceUrl = "https://assets.chocoie.com";
    int withliveOrientation = 2;
    bool lockRenderTextureResolution = false;
    int renderTextureLongSide = 3840;
    int renderTextureShortSide = 2160;
    bool hideCharacterBody = false;
    int renderTextureAntiAliasing = 0;
    bool unlockAfter = false;
    float cameraMovementSensitivity = 1.0f;
    float cameraVerticalSensitivity = 1.0f;
    float cameraFovSensitivity = 1.0f;
    float cameraRotationSensitivity = 1.0f;
    bool enableLegacyCompatibility = false;
    bool enableSetArchiveStartTime = false;
    int archiveStartTime = 0;
    bool filterMotionCaptureReplay = false;
    bool filterPlayableMotionCapture = false;
    bool avoidAccidentalTouch = true;
    std::string assetsUrlPrefix;
    bool hideCharacterShadow = false;
    int hideLiveStreamSceneItemsLevel = 0;
    bool hideLiveStreamCharacterItems = false;
    std::string localeCode = "ja-JP";

    // Offline API mock defaults (disabled by default).
    bool enableOfflineApiMock = false;
    bool dumpHttpMockJson = false;
    bool dumpSqlCipherKey = false;
    bool traceMasterImport = false;
    bool enableVirtualClock = false;
    int64_t virtualClockUnixTime = 0;
    std::string apiServerUrl;
    std::string customResVersion;
    
    // Archive configuration mapping: archives_id -> item data
    std::unordered_map<std::string, nlohmann::json> archiveConfigMap;
    // runtime
    VersionCompatibility::Version currentClientVersion;
    std::string currentResVersion;
    VersionCompatibility::Version latestClientVersion;
    std::string latestResVersion;
    bool enableLocale = false;
    
    // cache
    bool isLegacyVersionCached = false;
    bool legacyVersionResult = false;
    bool isFirstYearVersionCached = false;
    bool isFirstYearVersionResult = false;

    void LoadConfig(const std::string& configStr) {
        try {
            const auto config = nlohmann::json::parse(configStr);

            #define GetConfigItem(name) if (config.contains(#name)) name = config[#name]

            GetConfigItem(dbgMode);
            GetConfigItem(enabled);
            GetConfigItem(renderHighResolution);
            GetConfigItem(fesArchiveUnlockTicket);
            GetConfigItem(lazyInit);
            GetConfigItem(replaceFont);
            GetConfigItem(textTest);
            GetConfigItem(dumpText);
            GetConfigItem(targetFrameRate);
            GetConfigItem(enableFreeCamera);
            GetConfigItem(memorizeFreeCameraPos);
            GetConfigItem(removeRenderImageCover);
            GetConfigItem(avoidCharacterExit);
            GetConfigItem(storyHideBackground);
            GetConfigItem(storyHideTransition);
            GetConfigItem(storyHideNonCharacter3d);
            GetConfigItem(storyHideDof);
            GetConfigItem(storyHideEffect);
            GetConfigItem(storyNovelVocalTextDurationRate);
            GetConfigItem(storyNovelNonVocalTextDurationRate);
            GetConfigItem(storyReplaceContent);
            GetConfigItem(firstPersonCameraHideHead);
            GetConfigItem(firstPersonCameraHideHair);
            GetConfigItem(enableMotionCaptureReplay);
            GetConfigItem(enableInGameReplayDisplay);
            GetConfigItem(motionCaptureResourceUrl);
            GetConfigItem(withliveOrientation);
            GetConfigItem(lockRenderTextureResolution);
            GetConfigItem(renderTextureLongSide);
            GetConfigItem(renderTextureShortSide);
            GetConfigItem(hideCharacterBody);
            GetConfigItem(renderTextureAntiAliasing);
            GetConfigItem(unlockAfter);
            GetConfigItem(cameraMovementSensitivity);
            GetConfigItem(cameraVerticalSensitivity);
            GetConfigItem(cameraFovSensitivity);
            GetConfigItem(cameraRotationSensitivity);
            GetConfigItem(enableLegacyCompatibility);
            GetConfigItem(filterMotionCaptureReplay);
            GetConfigItem(filterPlayableMotionCapture);
            GetConfigItem(enableSetArchiveStartTime);
            GetConfigItem(archiveStartTime);
            GetConfigItem(avoidAccidentalTouch);
            GetConfigItem(assetsUrlPrefix);
            GetConfigItem(hideCharacterShadow);
            GetConfigItem(hideLiveStreamSceneItemsLevel);
            GetConfigItem(hideLiveStreamCharacterItems);
            GetConfigItem(localeCode);
            GetConfigItem(enableOfflineApiMock);
            GetConfigItem(dumpHttpMockJson);
            GetConfigItem(dumpSqlCipherKey);
            GetConfigItem(traceMasterImport);
            GetConfigItem(enableVirtualClock);
            GetConfigItem(virtualClockUnixTime);
            GetConfigItem(apiServerUrl);
            GetConfigItem(customResVersion);
            if (localeCode != "ja-JP") {
                enableLocale = true;
            }
        }
        catch (std::exception& e) {
            Log::ErrorFmt("LoadConfig error: %s", e.what());
        }
        isConfigInit = true;
    }
    
    void LoadArchiveConfig(const std::string& configStr) {
        try {
            const auto config = nlohmann::json::parse(configStr);
            
            // Clear existing archive config map
            archiveConfigMap.clear();
            
            // Check if config is an array of archive items
            if (config.is_array()) {
                for (const auto& item : config) {
                    if (item.contains("archives_id")) {
                        std::string archivesId = item["archives_id"];
                        archiveConfigMap[archivesId] = item;
//                        Log::VerboseFmt("Loaded archive config for ID: %s", archivesId.c_str());
                    }
                }
            } else {
                Log::Error("Invalid archive config format. Expected an array of archive items.");
                return;
            }
            
            Log::InfoFmt("Archive config loaded successfully. Total items: %zu", archiveConfigMap.size());
        } catch (const std::exception& e) {
            Log::ErrorFmt("LoadArchiveConfig error: %s", e.what());
        }
    }
    
    std::vector<int> parseVersion(const std::string& version) {
        std::vector<int> parts;
        std::stringstream ss(version);
        std::string part;
        
        while (std::getline(ss, part, '.')) {
            if (!part.empty()) {
                try {
                    parts.push_back(std::stoi(part));
                } catch (const std::exception&) {
                    // Skip invalid parts
                }
            }
        }
        
        return parts;
    }
    
    int compareVersions(const std::string& version1, const std::string& version2) {
        auto v1Parts = parseVersion(version1);
        auto v2Parts = parseVersion(version2);
        
        size_t maxLength = std::max(v1Parts.size(), v2Parts.size());
        
        for (size_t i = 0; i < maxLength; i++) {
            int v1Part = (i < v1Parts.size()) ? v1Parts[i] : 0;
            int v2Part = (i < v2Parts.size()) ? v2Parts[i] : 0;
            Log::DebugFmt("Comparing version parts: v1Part=%d, v2Part=%d", v1Part, v2Part);
            if (v1Part < v2Part) return -1;
            if (v1Part > v2Part) return 1;
        }
        
        return 0;
    }

    bool checkVersionCompatibility(const std::string& rule, const VersionCompatibility::Version& version) {
        try {
            VersionCompatibility::VersionChecker checker(rule);
            return checker.checkCompatibility(version);
        } catch (const std::exception& e) {
            Log::ErrorFmt("Version compatibility check error: %s", e.what());
            return false;
        }
    }
    
    std::string getVersionRuleDescription(const std::string& rule) {
        try {
            VersionCompatibility::VersionChecker checker(rule);
            return checker.toHumanReadable();
        } catch (const std::exception& e) {
            Log::ErrorFmt("Version rule description error: %s", e.what());
            return rule; // Fallback to original rule
        }
    }

    std::string getRecommendVersion(const std::string& rule) {
        try {
            VersionCompatibility::VersionChecker checker(rule);
            return checker.getRecommendVersion();
        } catch (const std::exception& e) {
            Log::ErrorFmt("Version recommend error: %s", e.what());
            return ""; // Return empty string on error
        }
    }

    bool isLegacyMrsVersion() {
        if (isLegacyVersionCached) return legacyVersionResult;
        // Use the new version compatibility checker: version < 4.0.1
        legacyVersionResult = checkVersionCompatibility("< 4.0.1", currentClientVersion);
        isLegacyVersionCached = true;
        Log::DebugFmt("Legacy MRS version check: %s < 4.0.1 = %s",
                      currentClientVersion.toString().c_str(), legacyVersionResult ? "true" : "false");
        return legacyVersionResult;
    }

    bool isFirstYearVersion() {
        if (isFirstYearVersionCached) return isFirstYearVersionResult;
        // Use the new version compatibility checker: version < 2.0.0
        isFirstYearVersionResult = checkVersionCompatibility("< 2.0.0", currentClientVersion);
        isFirstYearVersionCached = true;
        Log::DebugFmt("First year version check: %s < 2.0.0 = %s",
                      currentClientVersion.toString().c_str(), isFirstYearVersionResult ? "true" : "false");
        return isFirstYearVersionResult;
    }

    bool isLatestVersion() {
        return currentResVersion == latestResVersion;
    }

    std::string GetOfflineResourceVersion() {
        if (!customResVersion.empty()) return customResVersion;
        if (!latestResVersion.empty()) return latestResVersion;
        return currentResVersion;
    }
}
