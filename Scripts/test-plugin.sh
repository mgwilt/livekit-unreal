#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/livekit-plugin-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

PACKAGE="$WORK/LiveKitBridge"
"$ROOT/Scripts/build-plugin.sh" "$PACKAGE"
"$ROOT/Scripts/smoke-blueprint-package.sh" "$PACKAGE"
