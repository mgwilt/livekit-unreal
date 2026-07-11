#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
ENGINE=${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}
EDITOR="$ENGINE/Engine/Binaries/Mac/UnrealEditor-Cmd"
PACKAGE=${1:-}

if [ -z "$PACKAGE" ] || [ ! -s "$PACKAGE/LiveKitBridge.uplugin" ]; then
  printf 'Usage: %s /absolute/path/to/packaged/LiveKitBridge\n' "$0" >&2
  exit 1
fi

if [ ! -x "$EDITOR" ]; then
  printf 'UnrealEditor-Cmd was not found at %s\n' "$EDITOR" >&2
  printf 'Set UE_ROOT to an Unreal Engine 5.8 installation.\n' >&2
  exit 1
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/livekit-blueprint-smoke.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

PROJECT="$WORK/BlueprintSmoke"
mkdir -p "$PROJECT/Config" "$PROJECT/Plugins"
cp "$ROOT/Tests/BlueprintSmoke/BlueprintSmoke.uproject" "$PROJECT/BlueprintSmoke.uproject"
cp "$ROOT/Tests/BlueprintSmoke/Config/DefaultEngine.ini" "$PROJECT/Config/DefaultEngine.ini"
ditto "$PACKAGE" "$PROJECT/Plugins/LiveKitBridge"

REPORT="$WORK/Automation"
"$EDITOR" "$PROJECT/BlueprintSmoke.uproject" \
  -unattended -nop4 -nosplash -nullrhi -NoSound \
  -ExecCmds="Automation RunTests LiveKitBridge.;Quit" \
  -TestExit="Automation Test Queue Empty" \
  -ReportOutputPath="$REPORT"

test -s "$REPORT/index.json"
printf 'Blueprint-only package smoke test passed\n'
