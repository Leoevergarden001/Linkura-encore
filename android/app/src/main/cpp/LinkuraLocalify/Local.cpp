#include "Local.h"
#include "Plugin.h"

namespace LinkuraLocal::Local {
    std::filesystem::path GetBasePath() {
        return Plugin::GetInstance().GetHookInstaller()->localizationFilesDir;
    }
}
