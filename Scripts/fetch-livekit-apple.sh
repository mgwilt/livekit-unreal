#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
DEST="$ROOT/Source/ThirdParty/Apple"
LOCK="$DEST/dependencies.lock"
FACADE_SOURCE="$ROOT/Facade/LiveKitUnrealFacade.swift"

# shellcheck disable=SC1090
. "$LOCK"

mkdir -p "$DEST"

fetch() {
  name=$1
  version=$2
  url=$3
  expected=$4
  output="$DEST/$name.xcframework.zip"
  temporary="$output.download"

  if [ -f "$output" ] && printf '%s  %s\n' "$expected" "$output" | shasum -a 256 -c - >/dev/null 2>&1; then
    printf '%s %s already verified\n' "$name" "$version"
    return
  fi

  rm -f "$temporary"
  curl --fail --location --retry 3 --output "$temporary" "$url"
  printf '%s  %s\n' "$expected" "$temporary" | shasum -a 256 -c -
  mv "$temporary" "$output"
  printf '%s %s installed\n' "$name" "$version"
}

fetch \
  LiveKit \
  "$LIVEKIT_VERSION" \
  "https://github.com/livekit/client-sdk-swift-xcframework/releases/download/$LIVEKIT_VERSION/LiveKit.xcframework.zip" \
  "$LIVEKIT_SHA256"

fetch \
  LiveKitWebRTC \
  "$LIVEKIT_WEBRTC_VERSION" \
  "https://github.com/livekit/webrtc-xcframework/releases/download/$LIVEKIT_WEBRTC_VERSION/LiveKitWebRTC.xcframework.zip" \
  "$LIVEKIT_WEBRTC_SHA256"

fetch \
  RustLiveKitUniFFI \
  "$RUST_LIVEKIT_UNIFFI_VERSION" \
  "https://github.com/livekit/livekit-uniffi-xcframework/releases/download/$RUST_LIVEKIT_UNIFFI_VERSION/RustLiveKitUniFFI.xcframework.zip" \
  "$RUST_LIVEKIT_UNIFFI_SHA256"

mkdir -p "$DEST/Headers"
unzip -p \
  "$DEST/LiveKit.xcframework.zip" \
  'LiveKit.xcframework/macos-arm64_x86_64/LiveKit.framework/Versions/A/Headers/LiveKit-Swift.h' \
  > "$DEST/Headers/LiveKit-Swift-macOS.h"
unzip -p \
  "$DEST/LiveKit.xcframework.zip" \
  'LiveKit.xcframework/ios-arm64/LiveKit.framework/Headers/LiveKit-Swift.h' \
  > "$DEST/Headers/LiveKit-Swift-iOS.h"

extract_mac_framework() {
  name=$1
  archive="$DEST/$name.xcframework.zip"
  source="$DEST/.extract/$name.xcframework/macos-arm64_x86_64/$name.framework"

  rm -rf "$DEST/.extract"
  unzip -q -o "$archive" "$name.xcframework/macos-arm64_x86_64/$name.framework/*" -d "$DEST/.extract"
  rm -rf "$DEST/Mac/$name.framework"
  mkdir -p "$DEST/Mac"
  mv "$source" "$DEST/Mac/$name.framework"
  rm -rf "$DEST/.extract"
}

extract_mac_framework LiveKit
extract_mac_framework LiveKitWebRTC
extract_mac_framework RustLiveKitUniFFI

build_facade_object() {
  sdk_name=$1
  target=$2
  framework_root=$3
  output_root=$4
  header_path=$5

  sdk_path=$(xcrun --sdk "$sdk_name" --show-sdk-path)
  mkdir -p "$output_root/cache"
  xcrun --sdk "$sdk_name" swiftc "$FACADE_SOURCE" \
    -swift-version 5 \
    -parse-as-library \
    -emit-object \
    -emit-module \
    -emit-module-path "$output_root/LiveKitUnrealFacade.swiftmodule" \
    -emit-objc-header \
    -emit-objc-header-path "$header_path" \
    -module-name LiveKitUnrealFacade \
    -target "$target" \
    -sdk "$sdk_path" \
    -module-cache-path "$output_root/cache" \
    -F "$framework_root" \
    -o "$output_root/LiveKitUnrealFacade.o"
  /usr/bin/libtool -static \
    -o "$output_root/libLiveKitUnrealFacade.a" \
    "$output_root/LiveKitUnrealFacade.o"
}

FACADE_BUILD="$DEST/.facade-build"
rm -rf "$FACADE_BUILD"
mkdir -p "$FACADE_BUILD"

build_facade_object \
  macosx \
  arm64-apple-macos14.0 \
  "$DEST/Mac" \
  "$FACADE_BUILD/macos-arm64" \
  "$DEST/Headers/LiveKitUnrealFacade-Swift-macOS.h"
build_facade_object \
  macosx \
  x86_64-apple-macos14.0 \
  "$DEST/Mac" \
  "$FACADE_BUILD/macos-x86_64" \
  "$FACADE_BUILD/LiveKitUnrealFacade-Swift-macOS-x86_64.h"

mkdir -p "$DEST/Facade/Mac"
lipo -create \
  "$FACADE_BUILD/macos-arm64/libLiveKitUnrealFacade.a" \
  "$FACADE_BUILD/macos-x86_64/libLiveKitUnrealFacade.a" \
  -output "$DEST/Facade/Mac/libLiveKitUnrealFacade.a"

mkdir -p "$FACADE_BUILD/ios-frameworks"
unzip -q -o "$DEST/LiveKit.xcframework.zip" \
  'LiveKit.xcframework/ios-arm64/LiveKit.framework/*' \
  -d "$FACADE_BUILD/ios-frameworks"
build_facade_object \
  iphoneos \
  arm64-apple-ios15.0 \
  "$FACADE_BUILD/ios-frameworks/LiveKit.xcframework/ios-arm64" \
  "$FACADE_BUILD/ios-arm64" \
  "$DEST/Headers/LiveKitUnrealFacade-Swift-iOS.h"

mkdir -p "$DEST/Facade/IOS"
cp "$FACADE_BUILD/ios-arm64/libLiveKitUnrealFacade.a" \
  "$DEST/Facade/IOS/libLiveKitUnrealFacade.a"

rm -rf "$FACADE_BUILD"
printf 'LiveKitUnrealFacade built for macOS arm64/x86_64 and iOS arm64\n'
