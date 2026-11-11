#ifndef LINKURA_LOCALIFY_LOCAL_H
#define LINKURA_LOCALIFY_LOCAL_H

#include <filesystem>

namespace LinkuraLocal::Local {
    // Module data directory inside the game (<filesDir>/linkura-local). The
    // offline database lives next to it.
    std::filesystem::path GetBasePath();
}

#endif //LINKURA_LOCALIFY_LOCAL_H
