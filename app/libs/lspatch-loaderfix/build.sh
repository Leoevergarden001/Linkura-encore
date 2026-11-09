#!/bin/bash
# Rebuild lspatch_loaderfix.jar from lspatch.jar (LSPatch 0.7).
#
# Why: XResources extends xposed.dummy.XResourcesSuperClass, which LSPosed
# only generates at runtime. On the first launch after a clean install there
# is no vdex for LSPatch's in-memory loader dex yet, so ART verifies
# LSPLoader eagerly, tries to load XResources before that superclass exists,
# and marks it erroneous. LSPLoader.initModules then throws
# NoClassDefFoundError and the app aborts (CheckJNI) or crashes. Launches
# after the first one find the vdex and work. Shipping static
# xposed.dummy.* stubs (same constructors as the generated classes) inside
# loader.dex makes the early resolution succeed.
#
# Needs: JDK 17 (javac), Android build-tools d8, platforms/android-34.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIBS="$(dirname "$HERE")"
SDK="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
BT="${BUILD_TOOLS:-$SDK/build-tools/34.0.0}"
JAVA_HOME="${JAVA_HOME:-/path/to/jdk-17}"
export JAVA_HOME
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/stubs" "$WORK/cls" "$WORK/dex" "$WORK/jar/assets/lspatch"
"$JAVA_HOME/bin/javac" --release 8 -nowarn -d "$WORK/stubs" "$HERE"/stubs/android/content/res/*.java
"$JAVA_HOME/bin/javac" --release 8 -nowarn -cp "$WORK/stubs" -d "$WORK/cls" "$HERE"/src/xposed/dummy/*.java
unzip -q -o "$LIBS/lspatch.jar" assets/lspatch/loader.dex -d "$WORK/orig"
"$BT/d8" --release --min-api 28 --lib "$SDK/platforms/android-34/android.jar" \
    --output "$WORK/dex" "$WORK/orig/assets/lspatch/loader.dex" "$WORK"/cls/xposed/dummy/*.class
cp "$WORK/dex/classes.dex" "$WORK/jar/assets/lspatch/loader.dex"
cp "$LIBS/lspatch.jar" "$WORK/out.jar"
(cd "$WORK/jar" && zip -q "$WORK/out.jar" assets/lspatch/loader.dex)
mv "$WORK/out.jar" "$LIBS/lspatch_loaderfix.jar"
echo "wrote $LIBS/lspatch_loaderfix.jar"
