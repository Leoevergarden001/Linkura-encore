#pragma once

// The offline API backend (RouteRegistry + HttpMockBackend) only needs
// logging, JSON, the module's base path and the current archive id. On
// Android these come from the hook framework. The standalone build (iOS
// in-app server, host tests) defines LINKURA_MOCK_STANDALONE and provides
// the same names without IL2CPP/JNI.
#if defined(LINKURA_MOCK_STANDALONE)
#include "standalone/MockPlatformStandalone.hpp"
#else
#include "../HookMain.h"
#include "../Local.h"
#endif
