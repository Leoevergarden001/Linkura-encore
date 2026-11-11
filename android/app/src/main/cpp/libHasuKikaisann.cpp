#include "LinkuraLocalify/Plugin.h"
#include "LinkuraLocalify/Log.h"
#include "LinkuraLocalify/Local.h"
#include "LinkuraLocalify/Hook.h"

#include <jni.h>
#include <android/log.h>
#include "string"
#include "shadowhook.h"
#include "xdl.h"
#include "LinkuraLocalify/config/Config.hpp"
#include "backend/HttpMockBackend.hpp"

#include <filesystem>
#include <fstream>

JavaVM* g_javaVM = nullptr;
jclass g_linkuraHookMainClass = nullptr;
jmethodID showToastMethodId = nullptr;

bool UnityResolveProgress::startInit = false;
UnityResolveProgress::Progress UnityResolveProgress::assembliesProgress{};
UnityResolveProgress::Progress UnityResolveProgress::classProgress{};

namespace
{
    class AndroidHookInstaller : public LinkuraLocal::HookInstaller
    {
    public:
        explicit AndroidHookInstaller(const std::string& il2cppLibraryPath, const std::string& localizationFilesDir)
                : m_Il2CppLibrary(xdl_open(il2cppLibraryPath.c_str(), RTLD_LAZY))
        {
            this->m_il2cppLibraryPath = il2cppLibraryPath;
            this->localizationFilesDir = localizationFilesDir;
        }

        ~AndroidHookInstaller() override {
            xdl_close(m_Il2CppLibrary);
        }

        void* InstallHook(void* addr, void* hook, void** orig) override
        {
            return shadowhook_hook_func_addr(addr, hook, orig);
        }

        LinkuraLocal::OpaqueFunctionPointer LookupSymbol(const char* name) override
        {
            return reinterpret_cast<LinkuraLocal::OpaqueFunctionPointer>(xdl_sym(m_Il2CppLibrary, name, NULL));
        }

    private:
        void* m_Il2CppLibrary;
    };
}

extern "C"
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_javaVM = vm;
    return JNI_VERSION_1_6;
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_initHook(JNIEnv *env, jclass clazz, jstring targetLibraryPath,
                                                                 jstring localizationFilesDir) {
    // Store global reference to the class
    g_linkuraHookMainClass = (jclass)env->NewGlobalRef(clazz);
    showToastMethodId = env->GetStaticMethodID(clazz, "showToast", "(Ljava/lang/String;)V");

    const auto targetLibraryPathChars = env->GetStringUTFChars(targetLibraryPath, nullptr);
    const std::string targetLibraryPathStr = targetLibraryPathChars;

    const auto localizationFilesDirChars = env->GetStringUTFChars(localizationFilesDir, nullptr);
    const std::string localizationFilesDirCharsStr = localizationFilesDirChars;

    auto& plugin = LinkuraLocal::Plugin::GetInstance();
    plugin.InstallHook(std::make_unique<AndroidHookInstaller>(targetLibraryPathStr, localizationFilesDirCharsStr));
}



extern "C"
JNIEXPORT void JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_loadConfig(JNIEnv *env, jclass clazz,
                                                                   jstring config_json_str) {
    const auto configJsonStrChars = env->GetStringUTFChars(config_json_str, nullptr);
    const std::string configJson = configJsonStrChars;

    const auto dbCmdPath = LinkuraLocal::Local::GetBasePath().parent_path() / "db_cmd";
    std::error_code ec;
    if (std::filesystem::exists(dbCmdPath, ec)) {
        std::ifstream cmdFile(dbCmdPath);
        std::string cmd;
        std::getline(cmdFile, cmd);
        cmdFile.close();
        while (!cmd.empty() && std::isspace(static_cast<unsigned char>(cmd.back()))) cmd.pop_back();

        LinkuraLocal::Log::InfoFmt("[loadConfig] db_cmd: %s", cmd.c_str());
        auto& backend = LinkuraLocal::HttpMock::HttpMockBackend::Get();
        if (cmd == "reset") {
            backend.Reset();
        } else if (cmd == "rebuild") {
            backend.Rebuild();
        }
        std::filesystem::remove(dbCmdPath, ec);
    }

    LinkuraLocal::Config::LoadConfig(configJson);
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_loadArchiveConfig(JNIEnv *env, jclass clazz,
                                                                          jstring config_json_str) {
    try {
        const auto configJsonStrChars = env->GetStringUTFChars(config_json_str, nullptr);
        const std::string configJson = configJsonStrChars;
        
        LinkuraLocal::Log::InfoFmt("Loading archive config");
        LinkuraLocal::Config::LoadArchiveConfig(configJson);
        
        env->ReleaseStringUTFChars(config_json_str, configJsonStrChars);
        LinkuraLocal::Log::Info("Archive config loaded successfully");
        
    } catch (const std::exception& e) {
        LinkuraLocal::Log::ErrorFmt("Error loading archive config: %s", e.what());
    } catch (...) {
        LinkuraLocal::Log::Error("Unknown error loading archive config");
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_loadClientResVersion(JNIEnv *env, jclass clazz,
                                                                             jstring currentClient, jstring currentRes,
                                                                             jstring latestClient, jstring latestRes) {
    try {
        const auto currentClientChars = env->GetStringUTFChars(currentClient, nullptr);
        const auto currentResChars = env->GetStringUTFChars(currentRes, nullptr);
        const auto latestClientChars = env->GetStringUTFChars(latestClient, nullptr);
        const auto latestResChars = env->GetStringUTFChars(latestRes, nullptr);
        
        const std::string currentClientVersion = currentClientChars;
        const std::string currentResVersion = currentResChars;
        std::string latestClientVersion = latestClientChars;
        std::string latestResVersion = latestResChars;

        LinkuraLocal::Log::InfoFmt("Loading current client: %s, current res: %s", currentClientVersion.c_str(), currentResVersion.c_str());
        LinkuraLocal::Log::InfoFmt("Loading latest client: %s, latest res: %s", latestClientVersion.c_str(), latestResVersion.c_str());
        if (latestClientVersion.empty()) latestClientVersion = currentClientVersion;
        if (latestResVersion.empty()) latestResVersion = currentResVersion;
        // Store versions in Config
        LinkuraLocal::Config::currentClientVersion = VersionCompatibility::Version(currentClientVersion);
        LinkuraLocal::Config::currentResVersion = currentResVersion;
        LinkuraLocal::Config::latestClientVersion = VersionCompatibility::Version(latestClientVersion);
        LinkuraLocal::Config::latestResVersion = latestResVersion;
        
        env->ReleaseStringUTFChars(currentClient, currentClientChars);
        env->ReleaseStringUTFChars(currentRes, currentResChars);
        env->ReleaseStringUTFChars(latestClient, latestClientChars);
        env->ReleaseStringUTFChars(latestRes, latestResChars);
        
        LinkuraLocal::Log::Info("Client and res versions (current and latest) loaded successfully");
        
    } catch (const std::exception& e) {
        LinkuraLocal::Log::ErrorFmt("Error loading client res version: %s", e.what());
    } catch (...) {
        LinkuraLocal::Log::Error("Unknown error loading client res version");
    }
}

extern "C"
JNIEXPORT jint JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_pluginCallbackLooper(JNIEnv *env,
                                                                             jclass clazz) {
    LinkuraLocal::Log::ToastLoop(env, clazz);

    if (UnityResolveProgress::startInit) {
        return 9;
    }
    return 0;
}









extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_resetMockDatabase(JNIEnv *env, jclass clazz, jstring filesDir) {
    const auto chars = env->GetStringUTFChars(filesDir, nullptr);
    const std::string dir(chars);
    env->ReleaseStringUTFChars(filesDir, chars);
    return LinkuraLocal::HttpMock::HttpMockBackend::ResetAtFilesDir(dir) ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_io_github_chocolzs_linkura_localify_LinkuraHookMain_rebuildMockDatabase(JNIEnv *env, jclass clazz, jstring filesDir) {
    const auto chars = env->GetStringUTFChars(filesDir, nullptr);
    const std::string dir(chars);
    env->ReleaseStringUTFChars(filesDir, chars);
    return LinkuraLocal::HttpMock::HttpMockBackend::RebuildAtFilesDir(dir) ? JNI_TRUE : JNI_FALSE;
}
