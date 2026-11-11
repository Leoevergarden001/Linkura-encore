#!/bin/bash
# Sign an UNSIGNED offline IPA (from `ios-offline-stage`) with your own identity.
#
#   tools/ios_sign_ipa.sh <unsigned.ipa> <profile.mobileprovision> "<codesign identity>" <signed.ipa>
#
# - profile: a provisioning profile for the staged bundle id (default
#   org.linkura.preservation.client501). Xcode creates one for a Personal
#   Team when you build any app with that bundle id; it lives in
#   ~/Library/Developer/Xcode/UserData/Provisioning Profiles/.
# - identity: from `security find-identity -v -p codesigning`, e.g.
#   "Apple Development: you@example.com (ABCDE12345)".
# Free (Personal Team) signatures expire after 7 days; re-sign and reinstall.
# Install: Xcode > Devices and Simulators > drag the IPA, or
#   xcrun devicectl device install app --device <udid> <signed.ipa>
set -euo pipefail
[ $# -eq 4 ] || { sed -n 2,14p "$0"; exit 2; }
IPA="$1"; PROFILE="$2"; IDENTITY="$3"
OUT="$(cd "$(dirname "$4")" && pwd)/$(basename "$4")"
[ -e "$OUT" ] && { echo "refusing to overwrite $OUT" >&2; exit 1; }
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
unzip -q "$IPA" -d "$WORK"
APP="$(ls -d "$WORK"/Payload/*.app)"
security cms -D -i "$PROFILE" > "$WORK/profile.plist"
/usr/libexec/PlistBuddy -x -c "Print :Entitlements" "$WORK/profile.plist" > "$WORK/entitlements.plist"
PROFILE_APP_ID="$(/usr/libexec/PlistBuddy -c "Print :Entitlements:application-identifier" "$WORK/profile.plist")"
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$APP/Info.plist")"
case "$PROFILE_APP_ID" in
  *."$BUNDLE_ID"|*.\*) ;;
  *) echo "profile is for $PROFILE_APP_ID, app is $BUNDLE_ID" >&2; exit 1 ;;
esac
cp "$PROFILE" "$APP/embedded.mobileprovision"
# Inner code first: dylibs and frameworks, then the app with entitlements.
find "$APP/Frameworks" -maxdepth 1 \( -name "*.dylib" -o -name "*.framework" \) -print0 2>/dev/null |
    while IFS= read -r -d '' item; do
        codesign --force --sign "$IDENTITY" --timestamp=none "$item"
    done
codesign --force --sign "$IDENTITY" --timestamp=none --entitlements "$WORK/entitlements.plist" "$APP"
codesign --verify --deep --strict "$APP"
(cd "$WORK" && zip -qr -y "$OUT" Payload)
echo "signed: $OUT ($BUNDLE_ID)"
