#include "../HookMain.h"
#include <chrono>
#include <thread>
#include <set>
#include <map>
#include <vector>
#include <future>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>
namespace LinkuraLocal::HookDebug {
    using Il2cppString = UnityResolve::UnityType::String;

    namespace {
        void* preservedSceneChanger = nullptr;
        Il2cppUtils::MethodInfo* sceneChangerChangeSceneMethod = nullptr;
        Il2cppUtils::MethodInfo* sceneChangerSetIsInTransitionMethod = nullptr;
        Il2cppUtils::FieldInfo* sceneChangerStateField = nullptr;
        Il2cppUtils::FieldInfo* homeInitializeStateField = nullptr;
        Il2cppUtils::FieldInfo* homePrepareStateField = nullptr;
        Il2cppUtils::FieldInfo* gachaExecuteStateField = nullptr;
        Il2cppUtils::FieldInfo* gachaStartAnimationStateField = nullptr;
        thread_local bool inGachaExecute = false;
        uintptr_t gachaIl2cppBase = 0;
        std::atomic<bool> pendingOfflineLoadingCoverRelease{false};
        std::atomic<bool> pendingOfflineFooterTransitionRelease{false};
        std::atomic<uint64_t> sceneChangeGeneration{0};

        void LogOfflineSceneState(const char* event, void* sceneChanger) {
            if (!Config::enableOfflineApiMock || !sceneChanger) return;
            auto* klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "", "SceneChanger");
            auto* field = klass ? klass->Get<UnityResolve::Field>("<IsInTransition>k__BackingField") : nullptr;
            if (!field || field->static_field || !field->type || field->type->name != "System.Boolean") return;
            bool transitioning = false;
            std::memcpy(&transitioning, static_cast<const char*>(sceneChanger) + field->offset, sizeof(transitioning));
            auto* controllerField = klass->Get<UnityResolve::Field>("_currentSceneController");
            void* controller = nullptr;
            if (controllerField && !controllerField->static_field) {
                std::memcpy(&controller, static_cast<const char*>(sceneChanger) + controllerField->offset,
                            sizeof(controller));
            }
            const char* controllerName = "<null>";
            if (controller) {
                const auto* object = static_cast<const Il2cppUtils::Il2CppObject*>(controller);
                const auto* controllerClass = static_cast<const Il2cppUtils::Il2CppClassHead*>(object->klass);
                if (controllerClass && controllerClass->name) controllerName = controllerClass->name;
            }
            Log::InfoFmt("[HomeFlow] %s IsInTransition=%d controller=%s(%p)",
                         event, transitioning, controllerName, controller);
        }

        std::vector<void*> FindAllObjects(UnityResolve::Class* klass) {
            if (!klass) return {};

            auto objects = klass->FindObjectsByType<void*>();
            if (!objects.empty()) return objects;

            auto* coreModule = UnityResolve::Get("UnityEngine.CoreModule.dll");
            auto* resourcesClass = coreModule
                    ? coreModule->Get("Resources", "UnityEngine")
                    : nullptr;
            auto* findAll = resourcesClass
                    ? resourcesClass->Get<UnityResolve::Method>(
                            "FindObjectsOfTypeAll", {"System.Type"})
                    : nullptr;
            if (findAll) {
                auto* all = findAll->Invoke<UnityResolve::UnityType::Array<void*>*>(
                        klass->GetType());
                if (all) objects = all->ToVector();
            }
            return objects;
        }

        std::string GetGameObjectHierarchyPath(UnityResolve::UnityType::GameObject* gameObject) {
            if (!gameObject) return "<null>";
            std::vector<std::string> names;
            auto* transform = gameObject->GetTransform();
            for (size_t depth = 0; transform && depth < 12; ++depth) {
                auto* current = transform->GetGameObject();
                if (current) names.emplace_back(current->GetName());
                transform = transform->GetParent();
            }
            std::string path;
            for (auto it = names.rbegin(); it != names.rend(); ++it) {
                if (!path.empty()) path += '/';
                path += *it;
            }
            return path;
        }

        void DismissOfflineTutorialOverlay() {
            auto* canvasClass = Il2cppUtils::GetClass(
                    "UnityEngine.UIModule.dll", "UnityEngine", "Canvas");
            if (!canvasClass) canvasClass = Il2cppUtils::GetClass(
                    "UnityEngine.CoreModule.dll", "UnityEngine", "Canvas");
            if (!canvasClass) return;
            size_t dismissed = 0;
            for (auto* instance : FindAllObjects(canvasClass)) {
                auto* canvas = reinterpret_cast<UnityResolve::UnityType::Component*>(instance);
                auto* gameObject = canvas ? canvas->GetGameObject() : nullptr;
                if (!gameObject || !gameObject->GetActiveInHierarchy()) continue;
                const auto path = GetGameObjectHierarchyPath(gameObject);
                if (path.find("TutorialManager(Clone)") == std::string::npos) continue;
                Log::InfoFmt("[TutorialFlow] dismissing offline tutorial overlay path=%s",
                             path.c_str());
                Il2cppUtils::il2cpp_SetGameObjectActive(gameObject, false);
                ++dismissed;
            }
            Log::InfoFmt("[TutorialFlow] dismissed offline tutorial overlays=%zu", dismissed);
        }

        void ReleaseOfflineLoadingCover() {
            auto* assemblyCSharp = UnityResolve::Get("Assembly-CSharp.dll");
            auto* canvasFadeManagerClass = assemblyCSharp
                    ? assemblyCSharp->Get("CanvasFadeManager", "*")
                    : nullptr;
            if (!canvasFadeManagerClass) {
                Log::Warn("[HomeFlow] CanvasFadeManager is unavailable; cannot release loading cover");
                return;
            }

            auto* setForceAllFadeOut = canvasFadeManagerClass->Get<UnityResolve::Method>(
                    "SetForceAllFadeOut", {});
            if (!setForceAllFadeOut) {
                Log::Warn("[HomeFlow] CanvasFadeManager.SetForceAllFadeOut is unavailable");
                return;
            }

            auto managers = FindAllObjects(canvasFadeManagerClass);
            size_t released = 0;
            for (auto* manager : managers) {
                if (!manager || !Il2cppUtils::IsNativeObjectAlive(manager)) continue;
                setForceAllFadeOut->Invoke<void>(manager);
                ++released;
            }
            Log::InfoFmt("[HomeFlow] released offline loading cover managers=%zu", released);
        }

        void ReleaseOfflineCapturedTransitionFrame() {
            auto* graphicClass = Il2cppUtils::GetClass(
                    "UnityEngine.UI.dll", "UnityEngine.UI", "Graphic");
            if (!graphicClass) return;
            size_t released = 0;
            for (auto* instance : FindAllObjects(graphicClass)) {
                auto* graphic = reinterpret_cast<UnityResolve::UnityType::Component*>(instance);
                auto* gameObject = graphic ? graphic->GetGameObject() : nullptr;
                if (!gameObject || !gameObject->GetActiveInHierarchy()) continue;
                const auto path = GetGameObjectHierarchyPath(gameObject);
                if (path.find("/CanvasFade/Capture/") == std::string::npos) continue;
                auto* transform = gameObject->GetTransform();
                for (size_t depth = 0; transform && depth < 12; ++depth) {
                    auto* current = transform->GetGameObject();
                    if (current && current->GetName() == "Capture") {
                        Il2cppUtils::il2cpp_SetGameObjectActive(current, false);
                        ++released;
                        break;
                    }
                    transform = transform->GetParent();
                }
            }
            Log::InfoFmt("[RenderFlow] released captured transition frames=%zu", released);
        }

        int ReadAsyncState(void* stateMachine, Il2cppUtils::FieldInfo* field) {
            if (!stateMachine || !field) return INT32_MIN;
            int value = INT32_MIN;
            std::memcpy(&value,
                        reinterpret_cast<const char*>(stateMachine) + field->offset - 16,
                        sizeof(value));
            return value;
        }

    }

    DEFINE_HOOK(void, Internal_LogException, (void* ex, void* obj)) {
        Internal_LogException_Orig(ex, obj);
        static auto Exception_ToString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
        Log::LogUnityLog(ANDROID_LOG_ERROR, "UnityLog - Internal_LogException:\n%s", Exception_ToString->Invoke<Il2cppString*>(ex)->ToString().c_str());
    }

    DEFINE_HOOK(void, Internal_Log, (int logType, int logOption, UnityResolve::UnityType::String* content, void* context)) {
        Internal_Log_Orig(logType, logOption, content, context);
        // 2022.3.21f1
        Log::LogUnityLog(ANDROID_LOG_VERBOSE, "Internal_Log:\n%s", content->ToString().c_str());
    }

    // 👀
    DEFINE_HOOK(void, CoverImageCommandReceiver_Awake, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("CoverImageCommandReceiver_Awake HOOKED");
        if (Config::removeRenderImageCover) return;
        CoverImageCommandReceiver_Awake_Orig(self, method);
    }
    // 👀　work for both als and mrs
    DEFINE_HOOK(void, CharacterVisibleReceiver_SetupExistCharacter, (Il2cppUtils::Il2CppObject* self,int character, void* method)) {
        Log::DebugFmt("CharacterVisibleReceiver_SetupExistCharacter HOOKED");
        if (Config::avoidCharacterExit) return;
        CharacterVisibleReceiver_SetupExistCharacter_Orig(self, character, method);
    }
    // old Config::enableLegacyCompatibility
    DEFINE_HOOK(void, CharacterVisibleReceiver_UpdateAvatarVisibility, (Il2cppUtils::Il2CppObject* self, bool isVisible, void* method)) {
        Log::DebugFmt("CharacterVisibleReceiver_UpdateAvatarVisibility HOOKED");
        if (Config::avoidCharacterExit) isVisible = true;
        CharacterVisibleReceiver_UpdateAvatarVisibility_Orig(self, isVisible, method);
    }
    // old Config::enableLegacyCompatibility
    DEFINE_HOOK(void, MRS_AppsCoverScreen_SetActiveCoverImage, (Il2cppUtils::Il2CppObject* self, bool isActive, void* method)) {
        Log::DebugFmt("AppsCoverScreen_SetActiveCoverImage HOOKED");
        if (Config::removeRenderImageCover) isActive = false;
        MRS_AppsCoverScreen_SetActiveCoverImage_Orig(self, isActive, method);
    }

    DEFINE_HOOK(Il2cppString*, Hailstorm_AssetDownloadJob_get_UrlBase, (Il2cppUtils::Il2CppObject* self, void* method)) {
        auto base = Hailstorm_AssetDownloadJob_get_UrlBase_Orig(self, method);
        if (!Config::assetsUrlPrefix.empty()) {
            base = Il2cppString::New(HookShare::replaceUriHost(base->ToString(), Config::assetsUrlPrefix));
        }
        return base;
    }

    DEFINE_HOOK(void, FootShadowManipulator_OnInstantiate, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("FootShadowManipulator_OnInstantiate HOOKED");
        if (Config::hideCharacterShadow) return;
        FootShadowManipulator_OnInstantiate_Orig(self, method);
    }

    // character　item
    DEFINE_HOOK(void, ItemManipulator_OnInstantiate, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("ItemManipulator_OnInstantiate HOOKED");
        if (Config::hideLiveStreamCharacterItems) return;
        ItemManipulator_OnInstantiate_Orig(self, method);
    }

    enum HideLiveStreamSceneItemMode {
        None,
        /**
         * Simply hide live scene item, useful for with meets.\n
         */
        Lite,
        /**
         * Simply hide live scene item, useful for with meets.\n
         * Will remove static items for fes live.
         * And will try to hide static scene object
         */
        Normal,
        /**
         * Will try to remove dynamic items for fes live.
         */
        Strong,
        /**
         * Will remove timeline for fes live, light render, position control, dynamic camera will be useless.
         */
        Ultimate
    };

    // For example: Whiteboard, photo in with meets
    DEFINE_HOOK(void, ScenePropManipulator_OnInstantiate, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("ScenePropManipulator_OnInstantiate HOOKED");
        if (Config::hideLiveStreamSceneItemsLevel >= HideLiveStreamSceneItemMode::Lite) return;
        ScenePropManipulator_OnInstantiate_Orig(self, method);
    }

    static void hideGameObjectRecursive(UnityResolve::UnityType::GameObject* gameObject, int current_level, int max_level,
                                 const std::string& prefix = "", std::set<void*>* visited = nullptr, bool debug = true) {
        if (!gameObject || current_level > max_level) {
            return;
        }

        // 防止无限递归
        std::set<void*> local_visited;
        if (!visited) {
            visited = &local_visited;
        }

        void* gameObjectPtr = static_cast<void*>(gameObject);
        if (visited->find(gameObjectPtr) != visited->end()) {
            if (debug) Log::VerboseFmt("%s[L%d] GameObject: %s (ALREADY_VISITED - skipping)", prefix.c_str(), current_level, gameObject->GetName().c_str());
            return;
        }
        visited->insert(gameObjectPtr);

        auto objName = gameObject->GetName();
        auto transform = gameObject->GetTransform();

        // 递归处理子对象 - 深度优先，先隐藏子节点
        if (current_level < max_level && transform) {
            const auto childCount = transform->GetChildCount();
            if (childCount > 0) {
                for (int i = 0; i < childCount; i++) {
                    auto childTransform = transform->GetChild(i);
                    if (childTransform) {
                        auto childGameObject = childTransform->GetGameObject();
                        if (childGameObject && childGameObject != gameObject) { // 避免自引用
                            std::string newPrefix = prefix + "  ";
                            hideGameObjectRecursive(childGameObject, current_level + 1, max_level, newPrefix, visited, debug);
                        }
                    }
                }
            }
        }

        // 隐藏当前GameObject (在隐藏子节点之后)
        if (transform) {
            if (debug) Log::VerboseFmt("%s[L%d] Hiding GameObject: %s", prefix.c_str(), current_level, objName.c_str());
            Il2cppUtils::SetTransformRenderActive(transform, false, objName, debug);
        }
    }

    // return struct value, should use &scene as ptr.
    DEFINE_HOOK(void*, SceneManager_GetSceneByName, (Il2cppString * sceneName, void* method)) {
        Log::DebugFmt("SceneManager_GetSceneByName HOOKED: %s", sceneName->ToString().c_str());
        auto scene = SceneManager_GetSceneByName_Orig(sceneName, method);
        Log::DebugFmt("SceneManager_GetSceneByName HOOKED: %s Finished", sceneName->ToString().c_str());
        static auto Scene_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "Scene");
        static auto Scene_getRootGameObjects = Scene_klass->Get<UnityResolve::Method>("GetRootGameObjects", {});
        static auto Transform_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
        if (sceneName->ToString().starts_with("3d_stage")) {
//            Log::DebugFmt("Scene_getRootGameObjects is at %p", Scene_getRootGameObjects);
            if (Scene_getRootGameObjects) {
                auto gameObjects = Scene_getRootGameObjects->Invoke<UnityResolve::UnityType::Array<UnityResolve::UnityType::GameObject*>*>(&scene);
//                auto gameObjects = Scene_getRootGameObjects(scene);
//                Log::DebugFmt("gameObjects is at %p", gameObjects);
                auto gameObjectsVector = gameObjects->ToVector();
                for (auto object : gameObjectsVector) {
                    auto name = object->GetName();
                    Log::DebugFmt("SceneManager_GetSceneByName game object: %s", name.c_str());
                    switch ((HideLiveStreamSceneItemMode) Config::hideLiveStreamSceneItemsLevel) {
                        case HideLiveStreamSceneItemMode::Normal:
                            if (name.starts_with("Sc")) {
                                hideGameObjectRecursive(object, 0, 12);
                            }
                            break;
                        case HideLiveStreamSceneItemMode::Strong: // 也许可以通过循环实时隐藏，或者是根据阻止timeline显示对应的object
                        case HideLiveStreamSceneItemMode::Ultimate:
                            hideGameObjectRecursive(object, 0, 12);
                            break;
                        default:
                            break;
                    }
                }
            }



        }
        return scene;
    }

    DEFINE_HOOK(void, TimelineCommandReceiver_Awake, (void* self, void* method)) {
        Log::DebugFmt("TimelineCommandReceiver_Awake HOOKED");
        // 可以根据阻止timeline显示对应的object
        if (Config::hideLiveStreamSceneItemsLevel == HideLiveStreamSceneItemMode::Ultimate) return;
        TimelineCommandReceiver_Awake_Orig(self, method);
    }

    DEFINE_HOOK(int32_t, ManagerParams_get_SeatsCount, (Il2cppUtils::Il2CppObject* self, void* method)) {
        auto result = ManagerParams_get_SeatsCount_Orig(self, method);
        Log::DebugFmt("ManagerParams_get_SeatsCount HOOKED: %d", result);
        switch ((HideLiveStreamSceneItemMode) Config::hideLiveStreamSceneItemsLevel) {
            case HideLiveStreamSceneItemMode::Strong: // 也许可以通过循环实时隐藏，或者是根据阻止timeline显示对应的object
            case HideLiveStreamSceneItemMode::Ultimate:
                result = 0;
                break;
            default:
                break;
        }
        return result;
    }
#pragma region draft
    DEFINE_HOOK(void, LiveSceneController_InitializeSceneAsync, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("LiveSceneController_InitializeSceneAsync HOOKED");
        LiveSceneController_InitializeSceneAsync_Orig(self, method);
//        Il2cppUtils::GetClass
//        static auto SceneControllerBase_klass = Il2cppUtils::GetClass("Core.dll", "", "SceneControllerBase`1");
//        static auto view_field = SceneControllerBase_klass->Get<UnityResolve::Field>("_view");
////        Log::DebugFmt("view_field is at: %p", view_field);
//        auto liveSceneView = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Component*>(self, view_field);
//        Log::DebugFmt("liveSceneView is at: %p", liveSceneView);
    }

    DEFINE_HOOK(void*, SceneControllerBase_GetSceneView, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("SceneControllerBase_GetSceneView HOOKED");
        return SceneControllerBase_GetSceneView_Orig(self, method);
    }

    static bool is_first_load = true;

    DEFINE_HOOK(UnityResolve::UnityType::Array<Il2cppString*>*, LiveSceneControllerLogic_FindAssetPaths, (Il2cppUtils::Il2CppObject* self, void* locationsRecord, void* timelinesRecords, void* charactersRecords, void* costumesRecords, void* method)) {
        Log::DebugFmt("LiveSceneControllerLogic_FindAssetPaths HOOKED");
        auto result = LiveSceneControllerLogic_FindAssetPaths_Orig(self, locationsRecord, timelinesRecords, charactersRecords, costumesRecords, method);
        if (is_first_load) {
            is_first_load = false;
            return result;
        }
        // Convert to vector for safer filtering
//        auto originalVector = result->ToVector();
//        std::vector<Il2cppString*> filteredVector;
//
//        // Filter out stage assets
//        for (auto str : originalVector) {
//            if (str) {
//                Log::DebugFmt("Load asset path: %s", str->ToString().c_str());
//                if (!str->ToString().contains("prop")) {
//                    filteredVector.push_back(str);
//                } else {
//                    Log::DebugFmt("Filtering out prop asset path: %s", str->ToString().c_str());
//                    filteredVector.push_back(str);
//                }
//            }
//        }
//
//        // Create new Array with filtered data
//        static auto Il2cppString_klass = Il2cppUtils::GetClass("mscorlib.dll", "System", "String");
//        if (Il2cppString_klass && !filteredVector.empty()) {
//            auto newResult = UnityResolve::UnityType::Array<Il2cppString*>::New(Il2cppString_klass, filteredVector.size());
//            if (newResult) {
//                newResult->Insert(filteredVector.data(), filteredVector.size());
//                return newResult;
//            }
//        }
//
        // Fallback: return original if filtering failed
        return result;
    }

    DEFINE_HOOK(void*, LiveSceneControllerLogic_LoadLocationAssets, (Il2cppUtils::Il2CppObject* self, void* locationsRecord, void* method)) {
        Log::DebugFmt("LiveSceneControllerLogic_LoadLocationAssets HOOKED");
        return LiveSceneControllerLogic_LoadLocationAssets_Orig(self, locationsRecord, method);
    }


    UnityResolve::UnityType::GameObject* viewObjectCache = nullptr;
    std::atomic<bool> analysisScheduled{false};
    std::atomic<bool> analysisRunning{false};
    std::future<void> analysisFuture;
    
    DEFINE_HOOK(void, LiveSceneControllerLogic_ctor, (Il2cppUtils::Il2CppObject* self, void* param, void* loader, UnityResolve::UnityType::GameObject* view, void* addSceneProvider, void* method)) {
        Log::DebugFmt("LiveSceneControllerLogic_ctor HOOKED");
        LiveSceneControllerLogic_ctor_Orig(self, param, loader, view, addSceneProvider, method);
        Log::DebugFmt("LiveSceneControllerLogic_ctor HOOKED Finished, ready for following handlers");
        viewObjectCache = view;
    }

    DEFINE_HOOK(void*, SceneChanger_AddSceneAsync, (Il2cppUtils::Il2CppObject* self, Il2cppString* sceneName, bool ignoreEditorSceneManager, void* method)) {
        const auto name = sceneName ? sceneName->ToString() : "<null>";
        Log::InfoFmt("[LoginFlow] SceneChanger.AddSceneAsync enter scene=%s ignoreEditor=%s self=%p",
                     name.c_str(), ignoreEditorSceneManager ? "true" : "false", self);
        auto result = SceneChanger_AddSceneAsync_Orig(self, sceneName, ignoreEditorSceneManager, method);
        Log::InfoFmt("[LoginFlow] SceneChanger.AddSceneAsync return scene=%s result=%p",
                     name.c_str(), result);
        return result;
    }

    DEFINE_HOOK(void, SceneChanger_ChangeScene, (void* self, int sceneId,
            void* beforeSceneParam, void* afterSceneParam,
            bool flag1, bool flag2, bool flag3, void* method)) {
        preservedSceneChanger = self;
        sceneChangeGeneration.fetch_add(1, std::memory_order_relaxed);
        LogOfflineSceneState("ChangeScene entry", self);
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] ChangeScene enter sceneId=%d flags=%d,%d,%d", sceneId, flag1, flag2, flag3);
        }
        SceneChanger_ChangeScene_Orig(self, sceneId, beforeSceneParam, afterSceneParam,
                                      flag1, flag2, flag3, method);
        if (Config::enableOfflineApiMock) Log::InfoFmt("[HomeFlow] ChangeScene return sceneId=%d", sceneId);
        LogOfflineSceneState("ChangeScene exit", self);
    }

    DEFINE_HOOK(void, SceneChanger_set_IsInTransition, (void* self, bool value, void* method)) {
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] SceneChanger.set_IsInTransition value=%d", value);
        }
        SceneChanger_set_IsInTransition_Orig(self, value, method);
    }

    // The normal online bootstrap completes the transition after the scene
    // state machine terminates. Offline entry and footer navigation both
    // bypass that callback, so they must perform the matching release only
    // after the original state machine has reached its terminal state.
    void CompleteOfflineTransition(void* sceneChanger, const char* event) {
        if (!sceneChanger || !SceneChanger_set_IsInTransition_Orig
                || !sceneChangerSetIsInTransitionMethod) {
            Log::Warn("[HomeFlow] cannot complete offline scene transition: setter unavailable");
            return;
        }
        SceneChanger_set_IsInTransition_Orig(
                sceneChanger, false, sceneChangerSetIsInTransitionMethod);
        LogOfflineSceneState(event, sceneChanger);
    }

    DEFINE_HOOK(void, CommonFooter_OnClickMenuButtonMoveSceneId,
                (void* self, int sceneId, void* method)) {
        const auto generationBefore = sceneChangeGeneration.load(std::memory_order_relaxed);
        if (Config::enableOfflineApiMock) Log::InfoFmt("[HomeFlow] menu navigation enter sceneId=%d", sceneId);
        CommonFooter_OnClickMenuButtonMoveSceneId_Orig(self, sceneId, method);
        if (Config::enableOfflineApiMock
                && sceneChangeGeneration.load(std::memory_order_relaxed) != generationBefore) {
            // The footer dispatched a real scene change. Its online completion
            // callback is not reached in offline mode, so remember to release
            // the transition when that exact state machine terminates.
            pendingOfflineFooterTransitionRelease.store(true, std::memory_order_release);
        }
        if (Config::enableOfflineApiMock) Log::InfoFmt("[HomeFlow] menu navigation return sceneId=%d", sceneId);
    }

    DEFINE_HOOK(void, HomeCalendar_SetData, (void* self, void* plans, void* method)) {
        if (Config::enableOfflineApiMock) Log::InfoFmt("[HomeFlow] HomeCalendar.SetData enter plans=%p", plans);
        HomeCalendar_SetData_Orig(self, plans, method);
        if (Config::enableOfflineApiMock) Log::Info("[HomeFlow] HomeCalendar.SetData return");
    }

    DEFINE_HOOK(void, UniTaskBuilder_SetException, (void* self, void* exception, void* method)) {
        // Awaited UniTask faults need not pass through Unity's Debug logger.
        // Forward unchanged; this diagnostic must not turn failures into success.
        if (Config::enableOfflineApiMock && exception) {
            auto* toString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
            if (toString) {
                auto* message = toString->Invoke<Il2cppString*>(exception);
                if (message) Log::LogUnityLog(ANDROID_LOG_ERROR, "[HomeFlow] UniTask fault:\n%s", message->ToString().c_str());
            }
        }
        UniTaskBuilder_SetException_Orig(self, exception, method);
    }

    DEFINE_HOOK(void, GachaSceneController_SetPhase,
                (void* self, int phase, void* info, void* result, void* pack, void* method)) {
        Log::InfoFmt("[GachaFlow] SetPhase phase=%d info=%p result=%p pack=%p", phase, info, result, pack);
        GachaSceneController_SetPhase_Orig(self, phase, info, result, pack, method);
    }

    DEFINE_HOOK(void, GachaSceneController_OnGachaAsync_MoveNext, (void* self, void* method)) {
        Log::InfoFmt("[GachaFlow] OnGachaAsync.MoveNext enter self=%p", self);
        GachaSceneController_OnGachaAsync_MoveNext_Orig(self, method);
        Log::InfoFmt("[GachaFlow] OnGachaAsync.MoveNext return self=%p", self);
    }

    DEFINE_HOOK(void, GachaSceneController_OnGachaExecute_MoveNext, (void* self, void* method)) {
        Log::InfoFmt("[GachaFlow] OnGachaExecute.MoveNext enter state=%d self=%p", ReadAsyncState(self, gachaExecuteStateField), self);
        inGachaExecute = true;
        GachaSceneController_OnGachaExecute_MoveNext_Orig(self, method);
        inGachaExecute = false;
        Log::InfoFmt("[GachaFlow] OnGachaExecute.MoveNext return state=%d self=%p", ReadAsyncState(self, gachaExecuteStateField), self);
    }

    DEFINE_HOOK(bool, GachaApiResult_get_Success, (void* self, void* method)) {
        const bool success = GachaApiResult_get_Success_Orig(self, method);
        if (inGachaExecute) Log::InfoFmt("[GachaFlow] API result success=%d object=%p", success, self);
        if (!success && self) {
            void* exception = nullptr;
            std::memcpy(&exception, static_cast<const char*>(self) + 0x18, sizeof(exception));
            if (exception) {
                auto* toString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
                auto* message = toString ? toString->Invoke<Il2cppString*>(exception) : nullptr;
                if (message) Log::LogUnityLog(ANDROID_LOG_ERROR, "[GachaFlow] API result fault:\n%s", message->ToString().c_str());
            }
        }
        return success;
    }

    DEFINE_HOOK(void, GachaNullReferenceRaise, ()) {
        if (inGachaExecute && gachaIl2cppBase) {
            const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            Log::ErrorFmt("[GachaFlow] null reference callsite offset=0x%lx", static_cast<unsigned long>(caller - gachaIl2cppBase - 4));
        }
        GachaNullReferenceRaise_Orig();
    }

    DEFINE_HOOK(void, UniTaskVoidBuilder_SetException, (void* self, void* exception, void* method)) {
        if (exception) {
            auto* toString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
            if (toString) {
                auto* message = toString->Invoke<Il2cppString*>(exception);
                if (message) Log::LogUnityLog(ANDROID_LOG_ERROR, "[GachaFlow] UniTaskVoid fault:\n%s", message->ToString().c_str());
            }
        }
        UniTaskVoidBuilder_SetException_Orig(self, exception, method);
    }

    // IL2CPP emits an AOT copy of this builder used by the game assembly;
    // the UniTask.dll method pointer alone does not intercept that copy.
    DEFINE_HOOK(void, GachaAotBuilder_SetException, (void* self, void* exception, void* method)) {
        if (exception) {
            auto* toString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
            auto* message = toString ? toString->Invoke<Il2cppString*>(exception) : nullptr;
            if (message) Log::LogUnityLog(ANDROID_LOG_ERROR, "[GachaFlow] AOT async fault:\n%s", message->ToString().c_str());
        }
        GachaAotBuilder_SetException_Orig(self, exception, method);
    }

    DEFINE_HOOK(void, AsyncVoidBuilder_SetException, (void* self, void* exception, void* method)) {
        if (inGachaExecute && exception) {
            auto* toString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
            auto* message = toString ? toString->Invoke<Il2cppString*>(exception) : nullptr;
            if (message) Log::LogUnityLog(ANDROID_LOG_ERROR, "[GachaFlow] AsyncVoid fault:\n%s", message->ToString().c_str());
        }
        AsyncVoidBuilder_SetException_Orig(self, exception, method);
    }

    DEFINE_HOOK(void, GachaSceneController_StartGachaAnimationAsync_MoveNext, (void* self, void* method)) {
        Log::InfoFmt("[GachaFlow] StartGachaAnimationAsync.MoveNext enter state=%d self=%p",
                     ReadAsyncState(self, gachaStartAnimationStateField), self);
        GachaSceneController_StartGachaAnimationAsync_MoveNext_Orig(self, method);
        Log::InfoFmt("[GachaFlow] StartGachaAnimationAsync.MoveNext return state=%d self=%p",
                     ReadAsyncState(self, gachaStartAnimationStateField), self);
    }

    DEFINE_HOOK(void, GachaSceneController_EndGachaAnimationAsync_MoveNext, (void* self, void* method)) {
        Log::InfoFmt("[GachaFlow] EndGachaAnimationAsync.MoveNext enter self=%p", self);
        GachaSceneController_EndGachaAnimationAsync_MoveNext_Orig(self, method);
        Log::InfoFmt("[GachaFlow] EndGachaAnimationAsync.MoveNext return self=%p", self);
    }

    DEFINE_HOOK(void, GachaResultNewUI_InitUI, (void* self, void* info, int execType, void* result, void* method)) {
        Log::InfoFmt("[GachaFlow] Result.InitUI execType=%d info=%p result=%p", execType, info, result);
        GachaResultNewUI_InitUI_Orig(self, info, execType, result, method);
    }

    bool ChangeSceneForOfflineLogin(int sceneId) {
        if (!preservedSceneChanger || !SceneChanger_ChangeScene_Orig
                || !sceneChangerChangeSceneMethod) {
            Log::Error("[LoginFlow] preserved SceneChanger is unavailable");
            return false;
        }

        // The stock title completion callback refreshes the persistent footer
        // after the resource bootstrap and closes any menu before changing
        // scene.  The retired bootstrap never reaches that callback offline,
        // so reproduce these two local-only operations before the preserved
        // SceneChanger transition.
        if (auto* commonFooterClass = Il2cppUtils::GetClass(
                    "Assembly-CSharp.dll", "Tecotec", "CommonFooter")) {
            auto footers = FindAllObjects(commonFooterClass);
            auto* initAfterMasterLoad = commonFooterClass->Get<UnityResolve::Method>(
                    "InitAfterMasterLoad", {});
            auto* closeMenu = commonFooterClass->Get<UnityResolve::Method>(
                    "CloseMenu", {});
            for (auto* footer : footers) {
                if (!footer || !Il2cppUtils::IsNativeObjectAlive(footer)) continue;
                if (initAfterMasterLoad) {
                    initAfterMasterLoad->Invoke<void>(footer);
                }
                if (closeMenu) {
                    closeMenu->Invoke<void>(footer);
                }
            }
        } else {
            Log::Warn("[LoginFlow] CommonFooter unavailable during offline login");
        }

        pendingOfflineLoadingCoverRelease.store(true, std::memory_order_release);
        SceneChanger_ChangeScene_Orig(preservedSceneChanger, sceneId,
                                      nullptr, nullptr, false, false, false,
                                      sceneChangerChangeSceneMethod);
        return true;
    }

    DEFINE_HOOK(void, SceneChanger_ChangeSceneSequenceAsync_MoveNext, (void* self, void* method)) {
        const auto stateBefore = ReadAsyncState(self, sceneChangerStateField);
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] SceneChange sequence enter state=%d", stateBefore);
        }
        SceneChanger_ChangeSceneSequenceAsync_MoveNext_Orig(self, method);
        const auto stateAfter = ReadAsyncState(self, sceneChangerStateField);
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] SceneChange sequence return state=%d", stateAfter);
            LogOfflineSceneState("SceneChange sequence return", preservedSceneChanger);
        }
        if (stateAfter == -2) {
            if (pendingOfflineLoadingCoverRelease.exchange(false, std::memory_order_acq_rel)) {
                LogOfflineSceneState("offline Home sequence terminal", preservedSceneChanger);
                CompleteOfflineTransition(preservedSceneChanger, "offline Home transition completed");
                ReleaseOfflineLoadingCover();
                ReleaseOfflineCapturedTransitionFrame();
                // The online bootstrap supplies tutorial progress and closes
                // this global overlay.  The offline mock deliberately has no
                // network tutorial service, so leaving it active keeps a
                // previously rendered Home frame above every later scene.
                DismissOfflineTutorialOverlay();
            } else if (pendingOfflineFooterTransitionRelease.exchange(
                    false, std::memory_order_acq_rel)) {
                LogOfflineSceneState("offline footer sequence terminal", preservedSceneChanger);
                DismissOfflineTutorialOverlay();
                ReleaseOfflineCapturedTransitionFrame();
                CompleteOfflineTransition(
                        preservedSceneChanger, "offline footer transition completed");
            }
        }
    }

    DEFINE_HOOK(void*, SceneChanger_AddSceneViewAsync, (void* self, int sceneId,
            void* sceneParam, bool isAdditive, void* cancellationToken, void* method)) {
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] AddSceneViewAsync enter sceneId=%d additive=%d", sceneId, isAdditive);
        }
        auto result = SceneChanger_AddSceneViewAsync_Orig(self, sceneId, sceneParam, isAdditive,
                                                           cancellationToken, method);
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] AddSceneViewAsync return sceneId=%d task=%p", sceneId, result);
        }
        return result;
    }

    DEFINE_HOOK(void, HomeSceneController_InitializeSceneAsync_MoveNext, (void* self, void* method)) {
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] Home.InitializeSceneAsync enter state=%d",
                         ReadAsyncState(self, homeInitializeStateField));
        }
        HomeSceneController_InitializeSceneAsync_MoveNext_Orig(self, method);
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] Home.InitializeSceneAsync return state=%d",
                         ReadAsyncState(self, homeInitializeStateField));
        }
    }

    DEFINE_HOOK(void, HomeSceneController_PrepareChangeSceneAsync_MoveNext, (void* self, void* method)) {
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] Home.PrepareChangeSceneAsync enter state=%d",
                         ReadAsyncState(self, homePrepareStateField));
        }
        HomeSceneController_PrepareChangeSceneAsync_MoveNext_Orig(self, method);
        if (Config::enableOfflineApiMock) {
            Log::InfoFmt("[HomeFlow] Home.PrepareChangeSceneAsync return state=%d",
                         ReadAsyncState(self, homePrepareStateField));
        }
    }

    DEFINE_HOOK(void, TitleSceneController_InitializeSceneAsync_MoveNext, (void* self, void* method)) {
        Log::InfoFmt("[LoginFlow] TitleSceneController.<InitializeSceneAsync>d__*.MoveNext enter self=%p", self);
        TitleSceneController_InitializeSceneAsync_MoveNext_Orig(self, method);
        Log::InfoFmt("[LoginFlow] TitleSceneController.<InitializeSceneAsync>d__*.MoveNext return self=%p", self);
    }

    DEFINE_HOOK(void, TitleSceneController_OnClickFirstScreen, (void* self, void* method)) {
        Log::InfoFmt("[LoginFlow] TitleSceneController.OnClickFirstScreen enter self=%p", self);
        TitleSceneController_OnClickFirstScreen_Orig(self, method);
        Log::InfoFmt("[LoginFlow] TitleSceneController.OnClickFirstScreen return self=%p", self);
    }

    DEFINE_HOOK(void, TitleSceneController_OnClickMenuButton, (void* self, void* method)) {
        Log::InfoFmt("[LoginFlow] TitleSceneController.OnClickMenuButton enter self=%p", self);
        TitleSceneController_OnClickMenuButton_Orig(self, method);
        Log::InfoFmt("[LoginFlow] TitleSceneController.OnClickMenuButton return self=%p", self);
    }

    DEFINE_HOOK(void, TitleSceneController_OnClickStartButton, (void* self, void* method)) {
        Log::InfoFmt("[LoginFlow] TitleSceneController.OnClickStartButton enter self=%p", self);
        TitleSceneController_OnClickStartButton_Orig(self, method);
        Log::InfoFmt("[LoginFlow] TitleSceneController.OnClickStartButton return self=%p", self);
    }

    DEFINE_HOOK(void*, SceneManager_LoadSceneAsync_Name, (Il2cppString* sceneName, void* method)) {
        const auto name = sceneName ? sceneName->ToString() : "<null>";
        Log::InfoFmt("[LoginFlow] SceneManager.LoadSceneAsync(name) enter scene=%s", name.c_str());
        auto result = SceneManager_LoadSceneAsync_Name_Orig(sceneName, method);
        Log::InfoFmt("[LoginFlow] SceneManager.LoadSceneAsync(name) return scene=%s result=%p",
                     name.c_str(), result);
        return result;
    }

    DEFINE_HOOK(void*, SceneManager_LoadSceneAsync_NameMode, (Il2cppString* sceneName,
            int mode, void* method)) {
        const auto name = sceneName ? sceneName->ToString() : "<null>";
        Log::InfoFmt("[LoginFlow] SceneManager.LoadSceneAsync(name,mode) enter scene=%s mode=%d",
                     name.c_str(), mode);
        auto result = SceneManager_LoadSceneAsync_NameMode_Orig(sceneName, mode, method);
        Log::InfoFmt("[LoginFlow] SceneManager.LoadSceneAsync(name,mode) return scene=%s result=%p",
                     name.c_str(), result);
        return result;
    }

    DEFINE_HOOK(void, SceneManager_LoadScene_Name, (Il2cppString* sceneName, void* method)) {
        const auto name = sceneName ? sceneName->ToString() : "<null>";
        Log::InfoFmt("[LoginFlow] SceneManager.LoadScene(name) enter scene=%s", name.c_str());
        SceneManager_LoadScene_Name_Orig(sceneName, method);
        Log::InfoFmt("[LoginFlow] SceneManager.LoadScene(name) return scene=%s", name.c_str());
    }

    DEFINE_HOOK(void, SceneManager_LoadScene_NameMode, (Il2cppString* sceneName,
            int mode, void* method)) {
        const auto name = sceneName ? sceneName->ToString() : "<null>";
        Log::InfoFmt("[LoginFlow] SceneManager.LoadScene(name,mode) enter scene=%s mode=%d",
                     name.c_str(), mode);
        SceneManager_LoadScene_NameMode_Orig(sceneName, mode, method);
        Log::InfoFmt("[LoginFlow] SceneManager.LoadScene(name,mode) return scene=%s mode=%d",
                     name.c_str(), mode);
    }

    DEFINE_HOOK(void, SceneManager_LoadScene_IndexMode, (int sceneIndex, int mode, void* method)) {
        Log::InfoFmt("[LoginFlow] SceneManager.LoadScene(index,mode) enter index=%d mode=%d",
                     sceneIndex, mode);
        SceneManager_LoadScene_IndexMode_Orig(sceneIndex, mode, method);
        Log::InfoFmt("[LoginFlow] SceneManager.LoadScene(index,mode) return index=%d mode=%d",
                     sceneIndex, mode);
    }

    // lag
    DEFINE_HOOK(void*, LiveSceneController_PrepareChangeSceneAsync, (Il2cppUtils::Il2CppObject* self,void* token, void* method)) {
        Log::DebugFmt("LiveSceneController_PrepareChangeSceneAsync HOOKED");
        return LiveSceneController_PrepareChangeSceneAsync_Orig(self, token, method);
    }

    void printChildren(UnityResolve::UnityType::Transform* obj, const std::string& obj_name) {
        const auto childCount = obj->GetChildCount();
        for (int i = 0;i < childCount; i++) {
            auto child = obj->GetChild(i);
            const auto childName = child->GetName();
            Log::VerboseFmt("%s child: %s", obj_name.c_str(), childName.c_str());
        }
    }

    void printGameObjectComponentsRecursive(UnityResolve::UnityType::GameObject* gameObject, int current_level, int max_level, 
                                           UnityResolve::Class* component_klass = nullptr, const std::string& prefix = "",
                                           std::set<void*>* visited = nullptr) {
        if (!gameObject || current_level > max_level) {
            return;
        }
        
        // 防止无限递归
        std::set<void*> local_visited;
        if (!visited) {
            visited = &local_visited;
        }
        
        void* gameObjectPtr = static_cast<void*>(gameObject);
        if (visited->find(gameObjectPtr) != visited->end()) {
            Log::DebugFmt("%s[L%d] GameObject: %s (ALREADY_VISITED - skipping)", prefix.c_str(), current_level, gameObject->GetName().c_str());
            return;
        }
        visited->insert(gameObjectPtr);
        
        auto objName = gameObject->GetName();
        Log::DebugFmt("%s[L%d] GameObject: %s", prefix.c_str(), current_level, objName.c_str());

        // 使用自定义类型或默认Component类型
        static auto defaultComponent_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
        auto target_klass = component_klass ? component_klass : defaultComponent_klass;
        
        if (target_klass) {
            // 获取指定类型的组件
            auto components = gameObject->GetComponents<UnityResolve::UnityType::Component*>(target_klass);
            Log::DebugFmt("%s  ├─ Components[%s]: %d found", prefix.c_str(), 
                         component_klass ? component_klass->name.c_str() : "Component", components.size());
            
            for (size_t i = 0; i < components.size(); i++) {
                auto component = components[i];
                if (component) {
                    auto componentName = component->GetName();
                    // 获取组件类型信息 - 但不递归进入组件的GameObject
                    Log::DebugFmt("%s  │  ├─ [%d] %s (ptr: %p)", prefix.c_str(), i, componentName.c_str(), component);
                }
            }
        }
        
        // 递归处理子对象 - 优化子对象获取逻辑
        if (current_level < max_level) {
            auto transform = gameObject->GetTransform();
            std::vector<UnityResolve::UnityType::GameObject*> childGameObjects;
            
            // 方法1：直接通过Transform获取子对象
            if (transform) {
                const auto childCount = transform->GetChildCount();
                if (childCount > 0) {
                    Log::DebugFmt("%s  └─ Direct children: %d", prefix.c_str(), childCount);
                    for (int i = 0; i < childCount; i++) {
                        auto childTransform = transform->GetChild(i);
                        if (childTransform) {
                            auto childGameObject = childTransform->GetGameObject();
                            if (childGameObject && childGameObject != gameObject) { // 避免自引用
                                childGameObjects.push_back(childGameObject);
                            }
                        }
                    }
                }
            }
            
            // 方法2：如果没有直接子对象，使用GetComponentsInChildren (但限制层数)
            if (childGameObjects.empty() && current_level < 2) {
                static auto GameObject_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
                if (GameObject_klass) {
                    auto allChildGameObjects = gameObject->GetComponentsInChildren<UnityResolve::UnityType::GameObject*>(GameObject_klass, true);
                    if (allChildGameObjects.size() > 1) { // 大于1因为包含自己
                        Log::DebugFmt("%s  └─ Children via GetComponentsInChildren: %d", prefix.c_str(), std::min(5, (int)(allChildGameObjects.size() - 1)));
                        // 跳过第一个(自己)，添加其余子对象，但限制数量
                        for (size_t i = 1; i < allChildGameObjects.size() && i <= 5; i++) {
                            if (allChildGameObjects[i] && allChildGameObjects[i] != gameObject) {
                                childGameObjects.push_back(allChildGameObjects[i]);
                            }
                        }
                    }
                }
            }
            
            // 递归处理所有找到的子对象
            for (size_t i = 0; i < childGameObjects.size(); i++) {
                auto childGameObject = childGameObjects[i];
                if (childGameObject) {
                    std::string newPrefix = prefix + "  ";
                    printGameObjectComponentsRecursive(childGameObject, current_level + 1, max_level, component_klass, newPrefix, visited);
                }
            }
        }
    }
    void analyzeLiveSceneViewInDepth(UnityResolve::UnityType::Component* liveSceneView, const std::string& prefix = "") {
        if (!liveSceneView) return;

        Log::DebugFmt("%s=== Deep Analysis of LiveSceneView Component ===", prefix.c_str());
        Log::DebugFmt("%sLiveSceneView ptr: %p", prefix.c_str(), liveSceneView);

        // 1. 基本信息
        auto gameObject = liveSceneView->GetGameObject();
        auto transform = liveSceneView->GetTransform();
        Log::DebugFmt("%sGameObject: %s (ptr: %p)", prefix.c_str(),
                      gameObject ? gameObject->GetName().c_str() : "NULL", gameObject);
        Log::DebugFmt("%sTransform: %p", prefix.c_str(), transform);

        // 2. 尝试获取LiveSceneView的类定义和字段
        static auto LiveSceneView_klass = Il2cppUtils::GetClass("Core.dll", "Inspix", "LiveSceneView");
        if (LiveSceneView_klass) {
            Log::DebugFmt("%sLiveSceneView class found: %p", prefix.c_str(), LiveSceneView_klass);

            // 尝试获取一些可能的字段
            std::vector<std::string> possibleFields = {
                    "_view", "view", "sceneView", "mainView",
                    "_transform", "_gameObject", "_renderer",
                    "_camera", "camera", "_canvas", "canvas",
                    "_components", "components", "_children", "children"
            };

            for (const auto& fieldName : possibleFields) {
                auto field = LiveSceneView_klass->Get<UnityResolve::Field>(fieldName);
                if (field) {
                    Log::DebugFmt("%s  Field found: %s (offset: 0x%X, static: %s)", prefix.c_str(),
                                  fieldName.c_str(), field->offset, field->static_field ? "yes" : "no");

                    if (!field->static_field && field->offset > 0) {
                        // 尝试读取字段值
                        try {
                            void* fieldValue = *reinterpret_cast<void**>(
                                    reinterpret_cast<uintptr_t>(liveSceneView) + field->offset
                            );
                            Log::DebugFmt("%s    └─ Value: %p", prefix.c_str(), fieldValue);

                            // 如果字段值看起来像是GameObject或Component
                            if (fieldValue) {
                                auto possibleGameObject = reinterpret_cast<UnityResolve::UnityType::GameObject*>(fieldValue);
                                try {
                                    auto objName = possibleGameObject->GetName();
                                    if (!objName.empty()) {
                                        Log::DebugFmt("%s      └─ GameObject name: %s", prefix.c_str(), objName.c_str());
                                    }
                                } catch (...) {
                                    // 可能不是GameObject，尝试Component
                                    try {
                                        auto possibleComponent = reinterpret_cast<UnityResolve::UnityType::Component*>(fieldValue);
                                        auto compGameObj = possibleComponent->GetGameObject();
                                        if (compGameObj) {
                                            auto compObjName = compGameObj->GetName();
                                            Log::DebugFmt("%s      └─ Component on GameObject: %s", prefix.c_str(), compObjName.c_str());
                                        }
                                    } catch (...) {
                                        // 不是Component
                                    }
                                }
                            }
                        } catch (...) {
                            Log::DebugFmt("%s    └─ Failed to read field value", prefix.c_str());
                        }
                    }
                }
            }
        }

        // 3. 分析LiveSceneView能访问的所有组件
        Log::DebugFmt("%s--- Components accessible from LiveSceneView ---", prefix.c_str());
        auto allComponents = liveSceneView->GetComponentsInChildren<UnityResolve::UnityType::Component*>();
        Log::DebugFmt("%sTotal accessible components: %d", prefix.c_str(), allComponents.size());

        std::map<std::string, int> componentTypeCount;
        std::map<std::string, std::vector<UnityResolve::UnityType::Component*>> componentsByType;

        for (size_t i = 0; i < allComponents.size() && i < 50; i++) { // 限制数量
            auto comp = allComponents[i];
            if (comp) {
                std::string kompName = "Unknown";
                if (comp->Il2CppClass.klass) {
                    kompName = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", comp->Il2CppClass.klass);
                }
                componentTypeCount[kompName]++;
                componentsByType[kompName].push_back(comp);
            }
        }

        // 按类型分组显示组件
        for (const auto& pair : componentTypeCount) {
            const auto& typeName = pair.first;
            const auto& count = pair.second;
            Log::DebugFmt("%s  %s: %d instances", prefix.c_str(), typeName.c_str(), count);

            // 显示每个实例所在的GameObject
            for (size_t i = 0; i < componentsByType[typeName].size() && i < 3; i++) {
                auto comp = componentsByType[typeName][i];
                auto compGameObj = comp->GetGameObject();
                auto gameObjName = compGameObj ? compGameObj->GetName() : "NULL";
                Log::DebugFmt("%s    ├─ [%d] on GameObject: %s", prefix.c_str(), i, gameObjName.c_str());
            }
            if (componentsByType[typeName].size() > 3) {
                Log::DebugFmt("%s    └─ ... and %d more", prefix.c_str(),
                              componentsByType[typeName].size() - 3);
            }
        }

        // 4. 分析Transform层级
        if (transform) {
            Log::DebugFmt("%s--- Transform Hierarchy from LiveSceneView ---", prefix.c_str());
            const auto childCount = transform->GetChildCount();
            auto parent = transform->GetParent();

            Log::DebugFmt("%sTransform parent: %s", prefix.c_str(),
                          parent ? (parent->GetGameObject() ? parent->GetGameObject()->GetName().c_str() : "NULL") : "ROOT");
            Log::DebugFmt("%sTransform children: %d", prefix.c_str(), childCount);

            for (int i = 0; i < childCount && i < 10; i++) {
                auto childTransform = transform->GetChild(i);
                if (childTransform) {
                    auto childGameObj = childTransform->GetGameObject();
                    auto childName = childGameObj ? childGameObj->GetName() : "NULL";
                    Log::DebugFmt("%s  Child[%d]: %s", prefix.c_str(), i, childName.c_str());

                    // 显示子对象的组件
                    if (childGameObj) {
                        static auto Component_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
                        if (Component_klass) {
                            auto childComponents = childGameObj->GetComponents<UnityResolve::UnityType::Component*>(Component_klass);
                            Log::DebugFmt("%s    └─ Components: %d", prefix.c_str(), childComponents.size());
                            for (size_t j = 0; j < childComponents.size() && j < 3; j++) {
                                auto childComp = childComponents[j];
                                std::string childCompType = "Unknown";
                                if (childComp && childComp->Il2CppClass.klass) {
                                    childCompType = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", childComp->Il2CppClass.klass);
                                }
                                Log::DebugFmt("%s      ├─ [%d] %s", prefix.c_str(), j, childCompType.c_str());
                            }
                        }
                    }
                }
            }
        }

        Log::DebugFmt("%s=== End Deep Analysis of LiveSceneView ===", prefix.c_str());
    }

    void exploreComponentsInDetail(UnityResolve::UnityType::GameObject* gameObject, const std::string& prefix = "") {
        if (!gameObject) return;
        
        auto objName = gameObject->GetName();
        Log::DebugFmt("%s=== Detailed Component Analysis for: %s ===", prefix.c_str(), objName.c_str());
        
        // 获取所有组件
        static auto Component_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
        if (Component_klass) {
            auto components = gameObject->GetComponents<UnityResolve::UnityType::Component*>(Component_klass);
            Log::DebugFmt("%sTotal components: %d", prefix.c_str(), components.size());
            
            // 检查是否有重复的指针
            std::set<void*> componentPtrs;
            int duplicateCount = 0;
            for (size_t i = 0; i < components.size(); i++) {
                if (componentPtrs.find(components[i]) != componentPtrs.end()) {
                    duplicateCount++;
                } else {
                    componentPtrs.insert(components[i]);
                }
            }
            Log::DebugFmt("%sDuplicate components: %d, Unique: %d", prefix.c_str(), duplicateCount, componentPtrs.size());
            
            for (size_t i = 0; i < components.size(); i++) {
                auto component = components[i];
                if (component) {
                    auto componentName = component->GetName();
                    
                    // 尝试多种方式获取组件的实际类型信息
                    auto componentType = component->GetType();
                    std::string typeName = "Unknown";
                    std::string className = "Unknown";
                    
                    if (componentType) {
                        typeName = componentType->GetFullName();
                        className = componentType->FormatTypeName();
                    }
                    
                    // 尝试从Il2Cpp对象直接获取类型信息
                    void* klass = nullptr;
                    std::string klassName = "Unknown";
                    if (component->Il2CppClass.klass) {
                        klass = component->Il2CppClass.klass;
                        klassName = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass);
                    }
                    
                    // 检查这个组件是否属于不同的GameObject
                    auto compGameObject = component->GetGameObject();
                    auto compGameObjectName = compGameObject ? compGameObject->GetName() : "NULL";
                    bool isDifferentGameObject = (compGameObject != gameObject);
                    
                    Log::DebugFmt("%s  Component[%d]: %s (ptr: %p)", prefix.c_str(), i, componentName.c_str(), component);
                    Log::DebugFmt("%s    ├─ FullName: %s", prefix.c_str(), typeName.c_str());
                    Log::DebugFmt("%s    ├─ ClassName: %s", prefix.c_str(), className.c_str());
                    Log::DebugFmt("%s    ├─ Il2CppClass: %s (ptr: %p)", prefix.c_str(), klassName.c_str(), klass);
                    Log::DebugFmt("%s    ├─ GameObject: %s %s", prefix.c_str(), compGameObjectName.c_str(), 
                                 isDifferentGameObject ? "(DIFFERENT!)" : "(same)");
                    Log::DebugFmt("%s    ├─ Component==GameObject: %s", prefix.c_str(), 
                                 ((void*)component == gameObject) ? "YES!" : "no");
                    
                    // 如果是LiveSceneView，进行深度分析
                    if (klassName == "LiveSceneView") {
                        Log::DebugFmt("%s    ├─ *** DETECTED LiveSceneView - Starting Deep Analysis ***", prefix.c_str());
                        analyzeLiveSceneViewInDepth(component, prefix + "    ");
                    }
                    
                    // 尝试获取Transform组件信息
                    auto transform = component->GetTransform();
                    if (transform && transform != component) {
                        auto transformGameObj = transform->GetGameObject();
                        auto transformGameObjName = transformGameObj ? transformGameObj->GetName() : "NULL";
                        Log::DebugFmt("%s    ├─ Transform GameObject: %s", prefix.c_str(), transformGameObjName.c_str());
                    }
                    
                    // 尝试获取这个组件的子组件
                    Log::DebugFmt("%s    ├─ Checking for child components...", prefix.c_str());
                    auto childComponents = component->GetComponentsInChildren<UnityResolve::UnityType::Component*>();
                    if (childComponents.size() > 1) { // 大于1因为包含自己
                        Log::DebugFmt("%s    ├─ Child components from %s: %d", prefix.c_str(), klassName.c_str(), childComponents.size() - 1);
                        for (size_t j = 1; j < childComponents.size() && j <= 5; j++) { // 限制显示数量，跳过自己
                            auto childComp = childComponents[j];
                            if (childComp && childComp != component) {
                                auto childCompName = childComp->GetName();
                                auto childCompGameObject = childComp->GetGameObject();
                                auto childCompGameObjectName = childCompGameObject ? childCompGameObject->GetName() : "NULL";
                                
                                // 获取子组件的类型
                                std::string childKlassName = "Unknown";
                                if (childComp->Il2CppClass.klass) {
                                    childKlassName = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", childComp->Il2CppClass.klass);
                                }
                                
                                Log::DebugFmt("%s      ├─ [%d] %s (%s) on GameObject: %s", prefix.c_str(), 
                                            j-1, childKlassName.c_str(), childCompName.c_str(), childCompGameObjectName.c_str());
                            }
                        }
                    } else {
                        Log::DebugFmt("%s    ├─ No child components found", prefix.c_str());
                    }
                    
                    // 如果是Transform组件，显示层级信息
                    if (klassName == "Transform") {
                        auto transformComp = reinterpret_cast<UnityResolve::UnityType::Transform*>(component);
                        if (transformComp) {
                            const auto childCount = transformComp->GetChildCount();
                            auto parent = transformComp->GetParent();
                            auto parentName = parent ? (parent->GetGameObject() ? parent->GetGameObject()->GetName() : "NULL") : "ROOT";
                            
                            Log::DebugFmt("%s    ├─ Transform Parent: %s", prefix.c_str(), parentName.c_str());
                            Log::DebugFmt("%s    └─ Transform Children: %d", prefix.c_str(), childCount);
                            
                            for (int k = 0; k < childCount && k < 5; k++) {
                                auto childTransform = transformComp->GetChild(k);
                                if (childTransform) {
                                    auto childGameObj = childTransform->GetGameObject();
                                    auto childObjName = childGameObj ? childGameObj->GetName() : "NULL";
                                    Log::DebugFmt("%s      ├─ Transform Child[%d]: %s", prefix.c_str(), k, childObjName.c_str());
                                }
                            }
                        }
                    }
                    
                    Log::DebugFmt("%s", prefix.c_str()); // 空行分隔
                }
            }
        }
        
        // 尝试获取所有子GameObject
        auto transform = gameObject->GetTransform();
        if (transform) {
            const auto childCount = transform->GetChildCount();
            if (childCount > 0) {
                Log::DebugFmt("%s=== Direct Child GameObjects: %d ===", prefix.c_str(), childCount);
                for (int i = 0; i < childCount && i < 5; i++) {
                    auto childTransform = transform->GetChild(i);
                    if (childTransform) {
                        auto childGameObject = childTransform->GetGameObject();
                        if (childGameObject) {
                            auto childName = childGameObject->GetName();
                            Log::DebugFmt("%s  Child GameObject[%d]: %s", prefix.c_str(), i, childName.c_str());
                            
                            // 递归分析第一级子对象的组件
                            if (i < 2) { // 只分析前两个子对象
                                exploreComponentsInDetail(childGameObject, prefix + "    ");
                            }
                        }
                    }
                }
            }
        }
        
        Log::DebugFmt("%s=== End Detailed Analysis ===", prefix.c_str());
    }

    // 异步执行分析的函数
    void performAsyncAnalysis() {
        if (viewObjectCache) {
            auto gameObject = viewObjectCache;
            auto gameObjectName = gameObject->GetName();
            Log::DebugFmt("Live scene view game object is %s", gameObjectName.c_str());

            // 打印所有组件的递归结构
            Log::DebugFmt("=== Recursive GameObject Components Structure (max 3 levels) ===");
            printGameObjectComponentsRecursive(gameObject, 0, 6);
            Log::DebugFmt("=== End Recursive Structure ===");

            // 详细分析每个组件
            Log::DebugFmt("=== Detailed Component Analysis ===");
            exploreComponentsInDetail(gameObject, "  ");
            Log::DebugFmt("=== End Detailed Component Analysis ===");

            // 可选：打印特定类型的组件
            static auto Transform_klass = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Transform");
            if (Transform_klass) {
                Log::DebugFmt("=== Recursive Transform Components Only ===");
                printGameObjectComponentsRecursive(gameObject, 0, 6, Transform_klass);
                Log::DebugFmt("=== End Transform Structure ===");
            }
        }
        analysisScheduled = false;
        analysisRunning = false;
    }

    // 启动异步分析的包装函数
    void startAsyncAnalysis() {
        if (analysisRunning.exchange(true)) {
            return; // 已经在运行中
        }

        try {
            // 使用 std::async 创建异步任务
            analysisFuture = std::async(std::launch::async, []() {
                // 延迟5秒
                std::this_thread::sleep_for(std::chrono::seconds(20));

                // 检查Unity线程状态
                UnityResolve::ThreadAttach();

                Log::DebugFmt("Starting delayed async analysis");
                performAsyncAnalysis();

                UnityResolve::ThreadDetach();
            });
        } catch (const std::exception& e) {
            Log::ErrorFmt("Failed to start async analysis: %s", e.what());
            analysisRunning = false;
        }
    }

    static int hooked_count = 0;
    DEFINE_HOOK(void, LiveSceneController_InitializeSceneAsync_MoveNext, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("LiveSceneController_InitializeSceneAsync_MoveNext HOOKED");
        LiveSceneController_InitializeSceneAsync_MoveNext_Orig(self, method);
        Log::DebugFmt("LiveSceneController_InitializeSceneAsync_MoveNext HOOKED Finished");
//        hooked_count = hooked_count+1;
//        if (hooked_count <= 1) return;
//        // 启动异步非阻塞分析（5秒后执行）
//        if (!analysisScheduled.exchange(true)) {
//            Log::DebugFmt("Starting async analysis in 5 seconds...");
//            startAsyncAnalysis();
//        }
    }

    DEFINE_HOOK(bool, LiveSceneControllerLogic_Initialize_MoveNext, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("LiveSceneControllerLogic_Initialize_MoveNext HOOKED");
        auto result = LiveSceneControllerLogic_Initialize_MoveNext_Orig(self, method);
        Log::DebugFmt("LiveSceneControllerLogic_Initialize_MoveNext HOOKED Finished");
        return result;
    }

    DEFINE_HOOK(void*, Scene_GetRootGameObjects_Injected, (Il2cppUtils::Il2CppObject* self, void* method)) {
        Log::DebugFmt("Scene_GetRootGameObjects_Injected HOOKED");
        auto result = Scene_GetRootGameObjects_Injected_Orig(self, method);
        Log::DebugFmt("Scene_GetRootGameObjects_Injected HOOKED Finished");
        return result;
    }

#pragma endregion

    void Install(HookInstaller* hookInstaller) {
        ADD_HOOK(Internal_LogException, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_LogException(System.Exception,UnityEngine.Object)"));
        ADD_HOOK(Internal_Log, Il2cppUtils::il2cpp_resolve_icall(
                "UnityEngine.DebugLogHandler::Internal_Log(UnityEngine.LogType,UnityEngine.LogOption,System.String,UnityEngine.Object)"));
        
        // 👀
        ADD_HOOK(CoverImageCommandReceiver_Awake, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "CoverImageCommandReceiver", "Awake"));
        ADD_HOOK(CharacterVisibleReceiver_SetupExistCharacter, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix.Character", "CharacterVisibleReceiver", "SetupExistCharacter"));

        // 👀 old
        ADD_HOOK(MRS_AppsCoverScreen_SetActiveCoverImage, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Inspix.LiveMain", "AppsCoverScreen", "SetActiveCoverImage"));
        ADD_HOOK(CharacterVisibleReceiver_UpdateAvatarVisibility, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Inspix.Character", "CharacterVisibleReceiver", "UpdateAvatarVisibility"));
        ADD_HOOK(Hailstorm_AssetDownloadJob_get_UrlBase, Il2cppUtils::GetMethodPointer("Core.dll", "Hailstorm", "AssetDownloadJob", "get_UrlBase"));

        ADD_HOOK(FootShadowManipulator_OnInstantiate, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix.Character.FootShadow", "FootShadowManipulator", "OnInstantiate"));
        ADD_HOOK(ItemManipulator_OnInstantiate, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix.Character.Item", "ItemManipulator", "OnInstantiate"));
        ADD_HOOK(ScenePropManipulator_OnInstantiate, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix.Operator", "ScenePropManipulator", "OnInstantiate"));
        ADD_HOOK(SceneManager_GetSceneByName, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement", "SceneManager", "GetSceneByName"));
        auto sceneChangerMethod = Il2cppUtils::GetMethodPointer(
                "Assembly-CSharp.dll", "", "SceneChanger", "AddSceneAsync");
        if (!sceneChangerMethod) {
            sceneChangerMethod = Il2cppUtils::GetMethodPointer(
                    "Assembly-CSharp.dll", "Tecotec", "SceneChanger", "AddSceneAsync");
        }
        ADD_HOOK(SceneChanger_AddSceneAsync, sceneChangerMethod);

        auto sceneChangerClass = Il2cppUtils::GetClassIl2cpp(
                "Assembly-CSharp.dll", "", "SceneChanger");
        if (!sceneChangerClass) {
            sceneChangerClass = Il2cppUtils::GetClassIl2cpp(
                    "Assembly-CSharp.dll", "Tecotec", "SceneChanger");
        }
        if (sceneChangerClass) {
            auto changeScene = Il2cppUtils::GetMethodIl2cpp(sceneChangerClass, "ChangeScene", 6);
            sceneChangerChangeSceneMethod = changeScene;
            ADD_HOOK(SceneChanger_ChangeScene,
                     changeScene ? reinterpret_cast<void*>(changeScene->methodPointer) : nullptr);
            auto setIsInTransition = Il2cppUtils::GetMethodIl2cpp(
                    sceneChangerClass, "set_IsInTransition", 1);
            sceneChangerSetIsInTransitionMethod = setIsInTransition;
            ADD_HOOK(SceneChanger_set_IsInTransition,
                     setIsInTransition ? reinterpret_cast<void*>(setIsInTransition->methodPointer) : nullptr);
            auto changeSceneStateMachine = Il2cppUtils::find_nested_class(
                    sceneChangerClass, [](void* nestedClass) {
                        const auto name = static_cast<Il2cppUtils::Il2CppClassHead*>(nestedClass)->name;
                        return name && std::string_view(name).starts_with(
                                "<ChangeSceneSequenceAsync>d__");
                    });
            auto changeSceneMoveNext = Il2cppUtils::GetMethodIl2cpp(
                    changeSceneStateMachine, "MoveNext", 0);
            sceneChangerStateField = changeSceneStateMachine
                    ? Il2cppUtils::il2cpp_class_get_field_from_name(
                            changeSceneStateMachine, "<>1__state")
                    : nullptr;
            ADD_HOOK(SceneChanger_ChangeSceneSequenceAsync_MoveNext,
                     changeSceneMoveNext
                             ? reinterpret_cast<void*>(changeSceneMoveNext->methodPointer)
                             : nullptr);
            auto addSceneView = Il2cppUtils::GetMethodIl2cpp(sceneChangerClass, "AddSceneViewAsync", 4);
            ADD_HOOK(SceneChanger_AddSceneViewAsync,
                     addSceneView ? reinterpret_cast<void*>(addSceneView->methodPointer) : nullptr);
        } else {
            Log::Error("[LoginFlow] unable to resolve SceneChanger class");
        }

        auto titleSceneControllerClass = Il2cppUtils::GetClassIl2cpp(
                "Assembly-CSharp.dll", "Tecotec", "TitleSceneController");
        if (titleSceneControllerClass) {
            auto initializeStateMachine = Il2cppUtils::find_nested_class(
                    titleSceneControllerClass, [](void* nestedClass) {
                        const auto name = static_cast<Il2cppUtils::Il2CppClassHead*>(nestedClass)->name;
                        return name && std::string_view(name).starts_with("<InitializeSceneAsync>d__");
                    });
            auto initializeMoveNext = Il2cppUtils::GetMethodIl2cpp(
                    initializeStateMachine, "MoveNext", 0);
            if (initializeMoveNext) {
                ADD_HOOK(TitleSceneController_InitializeSceneAsync_MoveNext,
                         initializeMoveNext->methodPointer);
            } else {
                Log::Warn("[LoginFlow] unable to resolve TitleSceneController.InitializeSceneAsync.MoveNext");
            }
        }

        auto homeSceneControllerClass = Il2cppUtils::GetClassIl2cpp(
                "Assembly-CSharp.dll", "Tecotec", "HomeSceneController");
        if (homeSceneControllerClass) {
            const auto findHomeAsyncStateMachine = [&](std::string_view methodName) {
                return Il2cppUtils::find_nested_class(
                        homeSceneControllerClass, [methodName](void* nestedClass) {
                            const auto name = static_cast<Il2cppUtils::Il2CppClassHead*>(nestedClass)->name;
                            return name && std::string_view(name).starts_with(
                                    std::string("<") + std::string(methodName) + ">d__");
                        });
            };
            auto homeInitializeStateMachine = findHomeAsyncStateMachine("InitializeSceneAsync");
            auto homeInitializeMoveNext = Il2cppUtils::GetMethodIl2cpp(
                    homeInitializeStateMachine, "MoveNext", 0);
            homeInitializeStateField = homeInitializeStateMachine
                    ? Il2cppUtils::il2cpp_class_get_field_from_name(
                            homeInitializeStateMachine, "<>1__state") : nullptr;
            ADD_HOOK(HomeSceneController_InitializeSceneAsync_MoveNext,
                     homeInitializeMoveNext ? homeInitializeMoveNext->methodPointer : 0);
            if (!homeInitializeMoveNext || !homeInitializeStateField) {
                Log::Warn("[HomeFlow] unable to bind Home.InitializeSceneAsync state machine");
            }

            auto homePrepareStateMachine = findHomeAsyncStateMachine("PrepareChangeSceneAsync");
            auto homePrepareMoveNext = Il2cppUtils::GetMethodIl2cpp(homePrepareStateMachine, "MoveNext", 0);
            homePrepareStateField = homePrepareStateMachine
                    ? Il2cppUtils::il2cpp_class_get_field_from_name(
                            homePrepareStateMachine, "<>1__state") : nullptr;
            ADD_HOOK(HomeSceneController_PrepareChangeSceneAsync_MoveNext,
                     homePrepareMoveNext ? homePrepareMoveNext->methodPointer : 0);
            if (!homePrepareMoveNext || !homePrepareStateField) {
                Log::Warn("[HomeFlow] unable to bind Home.PrepareChangeSceneAsync state machine");
            }
        } else {
            Log::Warn("[HomeFlow] HomeSceneController class is unavailable");
        }

        if (Config::enableOfflineApiMock) {
            auto* gacha = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Tecotec", "GachaSceneController");
            if (gacha) {
                auto* phase = Il2cppUtils::GetMethodIl2cpp(gacha, "SetPhase", 4);
                ADD_HOOK(GachaSceneController_SetPhase, phase ? phase->methodPointer : 0);
                const auto findAsync = [&](std::string_view methodName) {
                    auto* nested = Il2cppUtils::find_nested_class(gacha, [methodName](void* nestedClass) {
                        const auto* name = static_cast<Il2cppUtils::Il2CppClassHead*>(nestedClass)->name;
                        return name && std::string_view(name).starts_with(std::string("<") + std::string(methodName) + ">d__");
                    });
                    return nested ? Il2cppUtils::GetMethodIl2cpp(nested, "MoveNext", 0) : nullptr;
                };
                auto* onGacha = findAsync("OnGachaAsync");
                auto* executeNested = Il2cppUtils::find_nested_class(gacha, [](void* nestedClass) {
                    const auto* name = static_cast<Il2cppUtils::Il2CppClassHead*>(nestedClass)->name;
                    return name && std::string_view(name).starts_with("<OnGachaExecute>d__");
                });
                auto* execute = executeNested ? Il2cppUtils::GetMethodIl2cpp(executeNested, "MoveNext", 0) : nullptr;
                gachaExecuteStateField = executeNested ? Il2cppUtils::il2cpp_class_get_field_from_name(executeNested, "<>1__state") : nullptr;
                if (execute && execute->methodPointer) {
                    // 5.1.0: OnGachaExecute.MoveNext tests this shared API-result
                    // getter. Verify its four instructions before binding.
                    const uintptr_t getter = execute->methodPointer + 0x5e4e394 - 0x4bd02f8;
                    const uint32_t expected[] = {0xf9400c08, 0xf100011f, 0x1a9f17e0, 0xd65f03c0};
                    if (std::memcmp(reinterpret_cast<void*>(getter), expected, sizeof(expected)) == 0) {
                        ADD_HOOK(GachaApiResult_get_Success, getter);
                        gachaIl2cppBase = execute->methodPointer - 0x4bd02f8;
                        const uintptr_t aotSetException = gachaIl2cppBase + 0x3fe9138;
                        if (*reinterpret_cast<const uint32_t*>(aotSetException) == 0xa9be57fe
                                && *reinterpret_cast<const uint32_t*>(aotSetException + 4) == 0xa9014ff4) {
                            ADD_HOOK(GachaAotBuilder_SetException, aotSetException);
                        }
                        const uintptr_t raiseNull = gachaIl2cppBase + 0x3de3308;
                        const uint32_t expectedRaise[] = {0xf81f0ffe, 0x9400f110};
                        if (std::memcmp(reinterpret_cast<void*>(raiseNull), expectedRaise, sizeof(expectedRaise)) == 0) {
                            ADD_HOOK(GachaNullReferenceRaise, raiseNull);
                        }
                    } else {
                        Log::Warn("[GachaFlow] API result getter did not match 5.1.0; diagnostic disabled");
                    }
                }
                if (executeNested) {
                    auto* metadata = Il2cppUtils::ToUnityResolveClass(executeNested);
                    for (auto* field : metadata->fields) if (field && field->type) {
                        Log::InfoFmt("[GachaFlow] ExecuteState field=%s type=%s offset=%d", field->name.c_str(), field->type->name.c_str(), field->offset);
                    }
                }
                auto* startNested = Il2cppUtils::find_nested_class(gacha, [](void* nestedClass) {
                    const auto* name = static_cast<Il2cppUtils::Il2CppClassHead*>(nestedClass)->name;
                    return name && std::string_view(name).starts_with("<StartGachaAnimationAsync>d__");
                });
                auto* start = startNested ? Il2cppUtils::GetMethodIl2cpp(startNested, "MoveNext", 0) : nullptr;
                gachaStartAnimationStateField = startNested
                        ? Il2cppUtils::il2cpp_class_get_field_from_name(startNested, "<>1__state") : nullptr;
                if (startNested) {
                    auto* metadata = Il2cppUtils::ToUnityResolveClass(startNested);
                    for (auto* field : metadata->fields) if (field && field->type) {
                        Log::InfoFmt("[GachaFlow] StartAnimationState field=%s type=%s offset=%d",
                                     field->name.c_str(), field->type->name.c_str(), field->offset);
                    }
                }
                auto* end = findAsync("EndGachaAnimationAsync");
                ADD_HOOK(GachaSceneController_OnGachaAsync_MoveNext, onGacha ? onGacha->methodPointer : 0);
                ADD_HOOK(GachaSceneController_OnGachaExecute_MoveNext, execute ? execute->methodPointer : 0);
                ADD_HOOK(GachaSceneController_StartGachaAnimationAsync_MoveNext, start ? start->methodPointer : 0);
                ADD_HOOK(GachaSceneController_EndGachaAnimationAsync_MoveNext, end ? end->methodPointer : 0);
            }
            auto* resultUI = Il2cppUtils::GetClassIl2cpp("Assembly-CSharp.dll", "Tecotec", "GachaResultNewUI");
            auto* init = resultUI ? Il2cppUtils::GetMethodIl2cpp(resultUI, "InitUI", 3) : nullptr;
            ADD_HOOK(GachaResultNewUI_InitUI, init ? init->methodPointer : 0);
            auto* uniTask = UnityResolve::Get("UniTask.dll");
            auto* voidBuilder = uniTask ? uniTask->Get("AsyncUniTaskVoidMethodBuilder", "Cysharp.Threading.Tasks.CompilerServices") : nullptr;
            if (voidBuilder) for (auto* candidate : voidBuilder->methods) {
                if (candidate && !candidate->static_function && candidate->name == "SetException"
                        && candidate->args.size() == 1 && candidate->args[0] && candidate->args[0]->pType
                        && candidate->args[0]->pType->name == "System.Exception") {
                    ADD_HOOK(UniTaskVoidBuilder_SetException, candidate->function);
                    break;
                }
            }
            auto* corlib = UnityResolve::Get("mscorlib.dll");
            auto* asyncVoid = corlib ? corlib->Get("AsyncVoidMethodBuilder", "System.Runtime.CompilerServices") : nullptr;
            if (asyncVoid) for (auto* candidate : asyncVoid->methods) {
                if (candidate && !candidate->static_function && candidate->name == "SetException"
                        && candidate->args.size() == 1 && candidate->args[0] && candidate->args[0]->pType
                        && candidate->args[0]->pType->name == "System.Exception") {
                    ADD_HOOK(AsyncVoidBuilder_SetException, candidate->function);
                    break;
                }
            }
        }

        ADD_HOOK(TitleSceneController_OnClickFirstScreen, Il2cppUtils::GetMethodPointer(
                "Assembly-CSharp.dll", "Tecotec", "TitleSceneController", "OnClickFirstScreen"));
        ADD_HOOK(TitleSceneController_OnClickMenuButton, Il2cppUtils::GetMethodPointer(
                "Assembly-CSharp.dll", "Tecotec", "TitleSceneController", "OnClickMenuButton"));
        ADD_HOOK(TitleSceneController_OnClickStartButton, Il2cppUtils::GetMethodPointer(
                "Assembly-CSharp.dll", "Tecotec", "TitleSceneController", "OnClickStartButton"));

        const auto dumpRelevantMethods = [](UnityResolve::Class* klass, const char* label) {
            if (!klass) return;
            for (auto* method : klass->methods) {
                if (!method) continue;
                const auto& name = method->name;
                const bool relevant = name.find("Login") != std::string::npos
                        || name.find("Home") != std::string::npos
                        || name.find("Scene") != std::string::npos
                        || name.find("Start") != std::string::npos
                        || name.find("Enter") != std::string::npos
                        || name.find("Load") != std::string::npos
                        || name.find("Click") != std::string::npos;
                if (!relevant) continue;
                std::string args = "(";
                for (size_t index = 0; index < method->args.size(); ++index) {
                    if (index) args += ",";
                    const auto* argument = method->args[index];
                    args += argument && argument->pType ? argument->pType->name : "?";
                }
                args += ")";
                const auto returnType = method->return_type ? method->return_type->name : "?";
                Log::InfoFmt("[LoginFlow] %s method=%s args=%s return=%s ptr=%p",
                             label, name.c_str(), args.c_str(), returnType.c_str(), method->function);
            }
        };
        auto assemblyCSharp = UnityResolve::Get("Assembly-CSharp.dll");
        if (assemblyCSharp) {
            dumpRelevantMethods(assemblyCSharp->Get("TitleSceneController", "Tecotec"),
                                "TitleSceneController");
            dumpRelevantMethods(assemblyCSharp->Get("SceneChanger", ""), "SceneChanger");
            dumpRelevantMethods(assemblyCSharp->Get("SceneChanger", "Tecotec"), "SceneChanger");
        }

        // Read the real client's contract before binding additional hooks.
        // Only type metadata is logged: no player values or account credentials.
        if (Config::enableOfflineApiMock && assemblyCSharp) {
            // lazyInit populates only explicitly requested classes. Iterating
            // Assembly.classes here silently misses the menu and DTO classes.
            const std::pair<const char*, const char*> contractTypes[] = {
                    {"SceneChanger", ""}, {"Plan", "Org.OpenAPITools.Model"},
                    {"HomeGetHomeResponse", "Org.OpenAPITools.Model"},
                    {"CommonFooter", "Tecotec"}, {"HomeMenu", "Tecotec"},
                    {"HomeMenuCommonIcons", "Tecotec"},
                    {"HomeSceneController", "Tecotec"},
                    {"HomeCalendar", "Tecotec"}, {"CalendarData", "Tecotec"},
                    {"GachaSceneController", "*"}, {"GachaSceneView", "*"},
                    {"GachaScenePresenter", "*"}, {"GachaAnimation", "*"},
                    {"GachaResultNewUI", "*"}, {"GachaResult", "Org.OpenAPITools.Model"},
                    {"GachaSetPurchaseResponse", "Org.OpenAPITools.Model"},
                    {"GachaGetGuaranteePointListRequest", "Org.OpenAPITools.Model"},
                    {"GachaGetGuaranteePointListResponse", "Org.OpenAPITools.Model"},
                    {"GachaGuaranteePointExchangeItem", "Org.OpenAPITools.Model"},
                    {"GachaReceiptItem", "Org.OpenAPITools.Model"},
                    {"GachaSetGuaranteePointExchangeRequest", "Org.OpenAPITools.Model"},
                    {"GachaSetGuaranteePointExchangeResponse", "Org.OpenAPITools.Model"}};
            for (const auto& [name, namespaze] : contractTypes) {
                auto* klass = assemblyCSharp->Get(name, namespaze);
                if (!klass) {
                    Log::InfoFmt("[ClientContract] unavailable=%s.%s", namespaze, name);
                    continue;
                }
                Log::InfoFmt("[ClientContract] class=%s.%s", klass->namespaze.c_str(), klass->name.c_str());
                for (auto* field : klass->fields) {
                    if (!field) continue;
                    Log::InfoFmt("[ClientContract] %s field=%s type=%s offset=%d static=%d",
                                 klass->name.c_str(), field->name.c_str(),
                                 field->type ? field->type->name.c_str() : "?",
                                 field->offset, field->static_field);
                }
                for (auto* method : klass->methods) {
                    if (!method) continue;
                    std::string args;
                    for (auto* arg : method->args) {
                        if (!args.empty()) args += ",";
                        args += arg && arg->pType ? arg->pType->name : "?";
                        args += " ";
                        args += arg ? arg->name : "?";
                    }
                    Log::InfoFmt("[ClientContract] %s method=%s args=(%s) return=%s",
                                 klass->name.c_str(), method->name.c_str(), args.c_str(),
                                 method->return_type ? method->return_type->name.c_str() : "?");
                }
            }
            // Signatures confirmed from the 5.1.0 ClientContract trace. Do not
            // use Class.Get<Method>'s name-only fallback for these ABI bindings.
            const auto exactVoidMethod = [&](const char* className, const char* methodName,
                                              const char* argumentType) -> void* {
                auto* klass = assemblyCSharp->Get(className, "Tecotec");
                if (!klass) return nullptr;
                for (auto* candidate : klass->methods) {
                    if (candidate && !candidate->static_function && candidate->name == methodName
                            && candidate->return_type && candidate->return_type->name == "System.Void"
                            && candidate->args.size() == 1 && candidate->args[0]
                            && candidate->args[0]->pType
                            && candidate->args[0]->pType->name == argumentType) return candidate->function;
                }
                return nullptr;
            };
            if (auto* pointer = exactVoidMethod("CommonFooter", "OnClickMenuButtonMoveSceneId", "SceneDefine.SceneId")) {
                ADD_HOOK(CommonFooter_OnClickMenuButtonMoveSceneId, pointer);
            }
            if (auto* pointer = exactVoidMethod("HomeCalendar", "SetData", "System.Collections.Generic.List<Org.OpenAPITools.Model.Plan>")) {
                ADD_HOOK(HomeCalendar_SetData, pointer);
            }
            auto* uniTaskAssembly = UnityResolve::Get("UniTask.dll");
            auto* builder = uniTaskAssembly ? uniTaskAssembly->Get(
                    "AsyncUniTaskMethodBuilder", "Cysharp.Threading.Tasks.CompilerServices") : nullptr;
            if (builder) {
                for (auto* candidate : builder->methods) {
                    if (candidate && !candidate->static_function && candidate->name == "SetException"
                            && candidate->return_type && candidate->return_type->name == "System.Void"
                            && candidate->args.size() == 1 && candidate->args[0]
                            && candidate->args[0]->pType
                            && candidate->args[0]->pType->name == "System.Exception") {
                        ADD_HOOK(UniTaskBuilder_SetException, candidate->function);
                        break;
                    }
                }
            } else {
                Log::Info("[ClientContract] UniTask builder unavailable; fault diagnostic not bound");
            }
        }

        auto sceneManagerAssembly = UnityResolve::Get("UnityEngine.CoreModule.dll");
        auto sceneManagerClass = sceneManagerAssembly
                ? sceneManagerAssembly->Get("SceneManager", "UnityEngine.SceneManagement")
                : nullptr;
        if (sceneManagerClass) {
            const auto findSceneMethod = [&](const char* methodName,
                                             std::initializer_list<const char*> expectedTypes)
                    -> UnityResolve::Method* {
                for (auto* candidate : sceneManagerClass->methods) {
                    if (!candidate || candidate->name != methodName ||
                            candidate->args.size() != expectedTypes.size()) {
                        continue;
                    }
                    auto expected = expectedTypes.begin();
                    bool matches = true;
                    for (auto* argument : candidate->args) {
                        if (!argument || !argument->pType ||
                                argument->pType->name != *expected++) {
                            matches = false;
                            break;
                        }
                    }
                    if (matches) return candidate;
                }
                return nullptr;
            };

            auto loadSceneAsyncNameMode = findSceneMethod(
                    "LoadSceneAsync", {"System.String", "UnityEngine.SceneManagement.LoadSceneMode"});
            ADD_HOOK(SceneManager_LoadSceneAsync_NameMode,
                     loadSceneAsyncNameMode ? loadSceneAsyncNameMode->function : nullptr);

            auto loadSceneName = findSceneMethod("LoadScene", {"System.String"});
            ADD_HOOK(SceneManager_LoadScene_Name,
                     loadSceneName ? loadSceneName->function : nullptr);

            auto loadSceneNameMode = findSceneMethod(
                    "LoadScene", {"System.String", "UnityEngine.SceneManagement.LoadSceneMode"});
            ADD_HOOK(SceneManager_LoadScene_NameMode,
                     loadSceneNameMode ? loadSceneNameMode->function : nullptr);

            auto loadSceneIndexMode = findSceneMethod(
                    "LoadScene", {"System.Int32", "UnityEngine.SceneManagement.LoadSceneMode"});
            ADD_HOOK(SceneManager_LoadScene_IndexMode,
                     loadSceneIndexMode ? loadSceneIndexMode->function : nullptr);
        } else {
            Log::Error("[LoginFlow] unable to resolve UnityEngine.SceneManagement.SceneManager");
        }

        ADD_HOOK(TimelineCommandReceiver_Awake, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "TimelineCommandReceiver", "Awake"));

        ADD_HOOK(ManagerParams_get_SeatsCount, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix.Audience.IndirectRendering", "ManagerParams", "get_SeatsCount"));
#pragma region draft_hook
//        ADD_HOOK(LiveSceneController_InitializeSceneAsync, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "LiveSceneController", "InitializeSceneAsync"));
//        ADD_HOOK(SceneControllerBase_GetSceneView, Il2cppUtils::GetMethodPointer("Core.dll", "", "SceneControllerBase`1", "SetSceneParam"));
//        ADD_HOOK(LiveSceneControllerLogic_FindAssetPaths, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "LiveSceneControllerLogic", "FindAssetPaths"));
//        ADD_HOOK(LiveSceneControllerLogic_LoadLocationAssets, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "LiveSceneControllerLogic", "LoadLocationAssets"));
//        ADD_HOOK(LiveSceneControllerLogic_ctor, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "LiveSceneControllerLogic", ".ctor"));
        //        ADD_HOOK(LiveSceneController_PrepareChangeSceneAsync, Il2cppUtils::GetMethodPointer("Core.dll", "Inspix", "LiveSceneController", "PrepareChangeSceneAsync"));

//        Il2cppUtils::MethodInfo* method = nullptr;
//        auto LiveSceneController_klass = Il2cppUtils::GetClassIl2cpp("Core.dll", "Inspix", "LiveSceneController");
//        if (LiveSceneController_klass) {
//            auto InitializeSceneAsync_klass = Il2cppUtils::find_nested_class_from_name(
//                    LiveSceneController_klass, "<InitializeSceneAsync>d__3");
//            method = Il2cppUtils::GetMethodIl2cpp(InitializeSceneAsync_klass, "MoveNext", 0);
//            if (method) {
//                ADD_HOOK(LiveSceneController_InitializeSceneAsync_MoveNext, method->methodPointer);
//            }
//        }
//        auto LiveSceneControllerLogic_klass = Il2cppUtils::GetClassIl2cpp("Core.dll", "Inspix", "LiveSceneControllerLogic");
//        if (LiveSceneControllerLogic_klass) {
//            auto Initialize_klass = Il2cppUtils::find_nested_class_from_name(
//                    LiveSceneControllerLogic_klass, "<Initialize>d__12");
//            method = Il2cppUtils::GetMethodIl2cpp(Initialize_klass, "MoveNext", 0);
//            if (method) {
//                ADD_HOOK(LiveSceneControllerLogic_Initialize_MoveNext, method->methodPointer);
//            }
//        }
#pragma endregion

    }
}
