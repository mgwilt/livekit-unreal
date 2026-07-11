#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DEST="$ROOT/Source/ThirdParty/Apple"

# shellcheck disable=SC1090
. "$DEST/dependencies.lock"

printf '%s  %s\n' "$LIVEKIT_SHA256" "$DEST/LiveKit.xcframework.zip" | shasum -a 256 -c -
printf '%s  %s\n' "$LIVEKIT_WEBRTC_SHA256" "$DEST/LiveKitWebRTC.xcframework.zip" | shasum -a 256 -c -
printf '%s  %s\n' "$RUST_LIVEKIT_UNIFFI_SHA256" "$DEST/RustLiveKitUniFFI.xcframework.zip" | shasum -a 256 -c -

test -s "$DEST/Headers/LiveKit-Swift-macOS.h"
test -s "$DEST/Headers/LiveKit-Swift-iOS.h"
test -s "$DEST/Headers/LiveKitUnrealFacade-Swift-macOS.h"
test -s "$DEST/Headers/LiveKitUnrealFacade-Swift-iOS.h"
test -s "$DEST/Facade/Mac/libLiveKitUnrealFacade.a"
test -s "$DEST/Facade/IOS/libLiveKitUnrealFacade.a"

lipo "$DEST/Facade/Mac/libLiveKitUnrealFacade.a" -verify_arch arm64 x86_64
lipo "$DEST/Facade/IOS/libLiveKitUnrealFacade.a" -verify_arch arm64

printf 'LiveKit Apple dependencies and Unreal facade verified\n'
