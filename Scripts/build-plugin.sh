#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
ENGINE=${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}
RUN_UAT="$ENGINE/Engine/Build/BatchFiles/RunUAT.sh"
OUTPUT=${1:-${TMPDIR:-/tmp}/LiveKitBridge-Package}

if [ ! -x "$RUN_UAT" ]; then
  printf 'Unreal Automation Tool was not found at %s\n' "$RUN_UAT" >&2
  printf 'Set UE_ROOT to an Unreal Engine 5.8 installation.\n' >&2
  exit 1
fi

mkdir -p "$(dirname -- "$OUTPUT")"
OUTPUT_PARENT=$(CDPATH='' cd -- "$(dirname -- "$OUTPUT")" && pwd)
OUTPUT="$OUTPUT_PARENT/$(basename -- "$OUTPUT")"

case "$OUTPUT/" in
  "$ROOT/"*)
    printf 'BuildPlugin output must be outside the plugin source tree: %s\n' "$OUTPUT" >&2
    exit 1
    ;;
esac

if [ "${LIVEKIT_SKIP_FETCH:-0}" != "1" ]; then
  "$ROOT/Scripts/fetch-livekit-apple.sh"
fi
"$ROOT/Scripts/verify-livekit-apple.sh"

"$RUN_UAT" BuildPlugin \
  -Plugin="$ROOT/LiveKitBridge.uplugin" \
  -Package="$OUTPUT" \
  -HostPlatforms=Mac \
  -TargetPlatforms=Mac+IOS \
  -StrictIncludes

test -s "$OUTPUT/LiveKitBridge.uplugin"
printf 'Packaged LiveKitBridge at %s\n' "$OUTPUT"
