#!/bin/bash
# Build the iOS offline runtime.
#   ios/build.sh ios    -> build/ios/LinkuraOffline.dylib   (arm64, iOS 15+, unsigned)
#   ios/build.sh host   -> build/host/linkura-offline-server (macOS, for tests)
# The backend sources are shared with the Android module
# (app/src/main/cpp/LinkuraLocalify/http-mock) and compiled with
# LINKURA_MOCK_STANDALONE, which swaps the hook framework for a small shim.
set -euo pipefail
TARGET="${1:-ios}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
CPP="$ROOT/app/src/main/cpp"
MOCK="$CPP/LinkuraLocalify/http-mock"
OUT="$HERE/build/$TARGET"
GEN="$HERE/build/generated"
mkdir -p "$OUT" "$GEN"

CMAKE="${CMAKE:-$(command -v cmake || ls -d "$HOME"/Library/Android/sdk/cmake/*/bin/cmake 2>/dev/null | tail -1)}"
[ -x "$CMAKE" ] || { echo "cmake not found (set CMAKE=...)" >&2; exit 1; }
"$CMAKE" -DINPUT_DIR="$MOCK/builtin" -DOUTPUT_HEADER="$GEN/offline_api_mock_builtin.hpp" \
    -P "$CPP/cmake/generate_builtin_json_header.cmake"
"$CMAKE" -DINPUT_DIR="$MOCK/backend" -DOUTPUT_HEADER="$GEN/http_mock_backend_builtin_sql.hpp" \
    -P "$CPP/cmake/generate_builtin_sql_header.cmake"

CXX_SOURCES=(
    "$MOCK/RouteRegistry.cpp"
    "$MOCK/backend/HttpMockBackend.cpp"
    "$MOCK/standalone/MockPlatformStandalone.cpp"
    "$MOCK/standalone/OfflineHttpServer.cpp"
)
INCLUDES=(-I"$CPP/LinkuraLocalify" -I"$CPP/deps" -I"$CPP/deps/sqlite" -I"$GEN" -I"$MOCK" -I"$MOCK/backend")
CXXFLAGS=(-std=c++2b -O2 -DLINKURA_MOCK_STANDALONE=1 -fvisibility=hidden -Wall -Wno-unused-function)
SQLITE_FLAGS=(-O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -fvisibility=hidden -w)

case "$TARGET" in
  ios)
    SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
    CC=(xcrun --sdk iphoneos clang -target arm64-apple-ios15.0 -isysroot "$SDK")
    CXX=(xcrun --sdk iphoneos clang++ -target arm64-apple-ios15.0 -isysroot "$SDK")
    ;;
  host)
    CC=(xcrun clang)
    CXX=(xcrun clang++)
    ;;
  *) echo "usage: $0 ios|host" >&2; exit 2 ;;
esac

OBJ="$OUT/obj"; mkdir -p "$OBJ"
"${CC[@]}" "${SQLITE_FLAGS[@]}" -c "$CPP/deps/sqlite/sqlite3.c" -o "$OBJ/sqlite3.o"
OBJECTS=("$OBJ/sqlite3.o")
for src in "${CXX_SOURCES[@]}"; do
    obj="$OBJ/$(basename "${src%.*}").o"
    "${CXX[@]}" "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$src" -o "$obj"
    OBJECTS+=("$obj")
done

if [ "$TARGET" = ios ]; then
    "${CXX[@]}" "${CXXFLAGS[@]}" -fobjc-arc "${INCLUDES[@]}" -c "$HERE/LinkuraOffline.mm" -o "$OBJ/LinkuraOffline.o"
    "${CXX[@]}" -dynamiclib -install_name @executable_path/Frameworks/LinkuraOffline.dylib \
        -framework Foundation "${OBJECTS[@]}" "$OBJ/LinkuraOffline.o" -o "$OUT/LinkuraOffline.dylib"
    echo "built $OUT/LinkuraOffline.dylib"
else
    "${CXX[@]}" "${CXXFLAGS[@]}" "${INCLUDES[@]}" -c "$HERE/host/main.cpp" -o "$OBJ/main.o"
    "${CXX[@]}" "${OBJECTS[@]}" "$OBJ/main.o" -o "$OUT/linkura-offline-server"
    echo "built $OUT/linkura-offline-server"
fi
