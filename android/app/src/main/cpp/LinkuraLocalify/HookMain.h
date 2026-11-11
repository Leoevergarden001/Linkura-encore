#pragma once

#include <set>
#include <unordered_set>
#include "../platformDefine.hpp"
#include "config/Config.hpp"
#include <nlohmann/json.hpp>
#include "Log.h"
#include "Plugin.h"
#include "UnityResolve/UnityResolve.hpp"
#include "Il2cppUtils.hpp"
#include "JniBridge.h"


extern std::unordered_set<void*> hookedStubs;

extern bool IsNativeObjectAlive(void* obj);

// Offline fixes for the 5.1 client (scene changes, Home, missions, async
// exceptions) plus development diagnostics installed only with dbgMode.
namespace LinkuraLocal::HookOfflineFixes {
    void Install(HookInstaller* hookInstaller);
    bool ChangeSceneForOfflineLogin(int sceneId);
}

#include <vector>
#include <cstdint>

namespace LinkuraLocal::HookLiveRender {
    // With×MEETS screen orientation values of Config::withliveOrientation.
    enum struct LiveScreenOrientation {
        Landscape,
        Portrait
    };
}

namespace LinkuraLocal::HookShare {
    void Install(HookInstaller* hookInstaller);
    std::string replaceUriHost(const std::string& external_link, const std::string& assets_url);
    namespace Shareable {
        struct ArchiveData {
            std::string id;
            long long duration;
        };
        enum RenderScene {
            None,
            FesLive,
            WithLive,
            Story
        };
        enum SetPlayPosition_State {
            Nothing,
            UpdateReceived
        };
        extern std::unordered_map<std::string, ArchiveData> archiveData;
        extern void* realtimeRenderingArchiveControllerCache;
        extern float realtimeRenderingArchivePositionSeconds;
        extern std::string currentArchiveId;
        extern float currentArchiveDuration;
        extern RenderScene renderScene;
        extern SetPlayPosition_State setPlayPositionState;

        // Function declarations (implementations in HookShare.cpp)
        void resetRenderScene();
        bool renderSceneIsNone();
        bool renderSceneIsFesLive();
        bool renderSceneIsWithLive();
        bool renderSceneIsStory();
        
        // Template function for better type safety and performance
        template<RenderScene scene>
        constexpr bool isRenderScene() {
            return renderScene == scene;
        }
    }
}
namespace LinkuraLocal::HookSqlCipher {
    void Install(HookInstaller* hookInstaller);
    void InstallManaged(HookInstaller* hookInstaller);
}
namespace LinkuraLocal::HookVirtualClock {
    void Install(HookInstaller* hookInstaller);
}
