# Shared by server/build-host.sh and ios/build.sh (sourced, not executed).
# Compiles the offline server (server/src) with LINKURA_MOCK_STANDALONE for a
# toolchain given in CC/CXX arrays and leaves the objects in SERVER_OBJECTS.
#
# Inputs:  CC, CXX (arrays), OUT (output dir)
# Outputs: SERVER_OBJECTS (array), SERVER_INCLUDES, SERVER_CXXFLAGS

SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SERVER_DIR")"
THIRD_PARTY="$REPO_DIR/third_party"
GEN="$OUT/generated"
OBJ="$OUT/obj"
mkdir -p "$GEN" "$OBJ"

CMAKE="${CMAKE:-$(command -v cmake || ls -d "$HOME"/Library/Android/sdk/cmake/*/bin/cmake 2>/dev/null | tail -1)}"
[ -x "$CMAKE" ] || { echo "cmake not found (set CMAKE=...)" >&2; exit 1; }
# Built-in responses (server/data/builtin) and SQL (server/db) are embedded as headers.
"$CMAKE" -DINPUT_DIR="$SERVER_DIR/data/builtin" -DOUTPUT_HEADER="$GEN/offline_api_mock_builtin.hpp" \
    -P "$SERVER_DIR/cmake/generate_builtin_json_header.cmake" >/dev/null
"$CMAKE" -DINPUT_DIR="$SERVER_DIR/db" -DOUTPUT_HEADER="$GEN/http_mock_backend_builtin_sql.hpp" \
    -P "$SERVER_DIR/cmake/generate_builtin_sql_header.cmake" >/dev/null

SERVER_INCLUDES=(-I"$SERVER_DIR/src" -I"$SERVER_DIR/src/backend" -I"$THIRD_PARTY" -I"$THIRD_PARTY/sqlite" -I"$GEN")
SERVER_CXXFLAGS=(-std=c++2b -O2 -DLINKURA_MOCK_STANDALONE=1 -fvisibility=hidden -Wall -Wno-unused-function)
SQLITE_FLAGS=(-O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -fvisibility=hidden -w)

"${CC[@]}" "${SQLITE_FLAGS[@]}" -c "$THIRD_PARTY/sqlite/sqlite3.c" -o "$OBJ/sqlite3.o"
SERVER_OBJECTS=("$OBJ/sqlite3.o")
for src in \
    "$SERVER_DIR/src/RouteRegistry.cpp" \
    "$SERVER_DIR/src/backend/HttpMockBackend.cpp" \
    "$SERVER_DIR/src/standalone/MockPlatformStandalone.cpp" \
    "$SERVER_DIR/src/standalone/OfflineHttpServer.cpp"; do
    obj="$OBJ/$(basename "${src%.*}").o"
    "${CXX[@]}" "${SERVER_CXXFLAGS[@]}" "${SERVER_INCLUDES[@]}" -c "$src" -o "$obj"
    SERVER_OBJECTS+=("$obj")
done
