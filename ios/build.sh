#!/bin/bash
# Build the iOS offline runtime:
#   ios/build.sh   -> ios/build/LinkuraOffline.dylib (arm64, iOS 15+, unsigned)
# The dylib embeds the shared offline server (server/src, compiled with
# LINKURA_MOCK_STANDALONE) and starts it on 127.0.0.1 inside the game.
# Set LINKURA_REMOTE=0 to leave out the development remote (tap/screenshot).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC=(xcrun --sdk iphoneos clang -target arm64-apple-ios15.0 -isysroot "$SDK")
CXX=(xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -isysroot "$SDK")
source "$HERE/../server/build-common.sh"

SOURCES=("$HERE/LinkuraOffline.mm" "$HERE/LinkuraPush.mm")
LINK_FLAGS=()
# The development build also reads per-path response overrides from
# Documents/LinkuraOffline/mock_api; the release build (LINKURA_REMOTE=0) ignores them.
DEV_FLAGS=()
if [ "${LINKURA_REMOTE:-1}" = 0 ]; then LINK_FLAGS=(-Wl,-U,_LinkuraRemoteStart); else SOURCES+=("$HERE/LinkuraRemote.mm"); DEV_FLAGS=(-DLINKURA_DEV_BUILD=1); fi
OBJECTS=("${SERVER_OBJECTS[@]}")
for src in "${SOURCES[@]}"; do
    obj="$OBJ/$(basename "${src%.*}").o"
    "${CXX[@]}" "${SERVER_CXXFLAGS[@]}" ${DEV_FLAGS[@]+"${DEV_FLAGS[@]}"} -fobjc-arc "${SERVER_INCLUDES[@]}" -c "$src" -o "$obj"
    OBJECTS+=("$obj")
done
"${CXX[@]}" -dynamiclib -install_name @executable_path/Frameworks/LinkuraOffline.dylib \
    -framework Foundation -framework UIKit -framework CoreGraphics ${LINK_FLAGS[@]+"${LINK_FLAGS[@]}"} "${OBJECTS[@]}" -o "$OUT/LinkuraOffline.dylib"
echo "built $OUT/LinkuraOffline.dylib"
