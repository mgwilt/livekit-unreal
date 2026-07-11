#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
OUTPUT=${1:-${TMPDIR:-/tmp}/livekit-unreal-release}
VERSION=$(awk -F'"' '/"VersionName"/ { print $4; exit }' "$ROOT/LiveKitBridge.uplugin")

if [ "${LIVEKIT_BINARY_RELEASE_APPROVED:-0}" != "1" ]; then
  printf 'Binary packaging is blocked by the third-party release gate.\n' >&2
  printf 'See THIRD_PARTY_NOTICES.md and THIRD_PARTY_COMPONENTS.json.\n' >&2
  exit 1
fi

"$ROOT/Scripts/verify-release-compliance.sh" binary

if [ -z "$VERSION" ]; then
  printf 'Unable to read VersionName from LiveKitBridge.uplugin\n' >&2
  exit 1
fi

mkdir -p "$OUTPUT"
OUTPUT=$(CDPATH='' cd -- "$OUTPUT" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/livekit-release-build.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

PACKAGE="$WORK/LiveKitBridge"
ARTIFACT_NAME="LiveKitForUnreal-$VERSION-UE5.8-Apple.zip"
ARTIFACT="$OUTPUT/$ARTIFACT_NAME"

"$ROOT/Scripts/build-plugin.sh" "$PACKAGE"
"$ROOT/Scripts/smoke-blueprint-package.sh" "$PACKAGE"

rm -f "$ARTIFACT" "$ARTIFACT.sha256"
find "$PACKAGE" -name '.DS_Store' -delete
COPYFILE_DISABLE=1 ditto -c -k --norsrc --keepParent "$PACKAGE" "$ARTIFACT"

if unzip -Z1 "$ARTIFACT" | grep -Eq '(^|/)(__MACOSX|\.DS_Store|\._[^/]+)(/|$)'; then
  printf 'Release archive contains macOS metadata.\n' >&2
  exit 1
fi
(
  cd "$OUTPUT"
  shasum -a 256 "$ARTIFACT_NAME" > "$ARTIFACT_NAME.sha256"
)

printf 'Release package: %s\n' "$ARTIFACT"
printf 'Checksum: %s.sha256\n' "$ARTIFACT"
