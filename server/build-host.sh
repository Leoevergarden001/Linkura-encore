#!/bin/bash
# Build the offline server as a desktop program, for development and tests:
#   server/build-host.sh   -> server/build/host/linkura-offline-server
# Run it with a data directory and point HTTP requests at it, or use
# server/tests/smoke_test.py, which starts it on a temporary directory.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build/host"
if [ "$(uname)" = Darwin ]; then CC=(xcrun clang); CXX=(xcrun clang++); else CC=(cc); CXX=(c++); fi
source "$HERE/build-common.sh"
"${CXX[@]}" "${SERVER_CXXFLAGS[@]}" "${SERVER_INCLUDES[@]}" -c "$HERE/host/main.cpp" -o "$OBJ/main.o"
LIBS=(); [ "$(uname)" = Darwin ] || LIBS=(-lpthread -ldl)
"${CXX[@]}" "${SERVER_OBJECTS[@]}" "$OBJ/main.o" ${LIBS[@]+"${LIBS[@]}"} -o "$OUT/linkura-offline-server"
echo "built $OUT/linkura-offline-server"
