#!/usr/bin/env bash
set -euo pipefail
plugin_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dep_root="$plugin_root/Source/ThirdParty/Linux"
source "$dep_root/dependencies.lock"
if [[ "$(uname -m)" != x86_64 ]]; then
    echo 'The Linux dependency lock requires x86_64.' >&2
    exit 1
fi
archive="$dep_root/$LIVEKIT_CPP_ARCHIVE.download"
if [[ ! -f "$dep_root/SDK/.upstream.sha256" ]]; then
    curl -fL --retry 2 "$LIVEKIT_CPP_URL" -o "$archive"
    printf '%s  %s\n' "$LIVEKIT_CPP_SHA256" "$archive" | sha256sum --check --status
    stage="$(mktemp -d "$dep_root/.extract.XXXXXX")"
    trap 'rm -rf -- "$stage"' EXIT
    tar -xzf "$archive" -C "$stage" --strip-components=1
    (
        cd "$stage"
        find include lib -type f -print0 | sort -z | xargs -0 sha256sum > .upstream.sha256
        printf '%s\n' "$LIVEKIT_CPP_SHA256" > .archive.sha256
    )
    # Replace only the generated SDK directory after archive verification.
    rm -rf -- "$dep_root/SDK"
    mv -- "$stage" "$dep_root/SDK"
    rm -- "$archive"
    trap - EXIT
fi
[[ "$(cat "$dep_root/SDK/.archive.sha256")" == "$LIVEKIT_CPP_SHA256" ]]
(cd "$dep_root/SDK" && sha256sum --check --status .upstream.sha256)
adapter_root="$plugin_root/Source/WindowsAdapter"
# Only the C interface crosses the standard-library boundary.
g++ -std=c++20 -shared -fPIC -pthread -O2 -fvisibility=hidden \
    -I"$adapter_root/include" -I"$dep_root/SDK/include" \
    "$adapter_root"/src/*.cpp \
    -L"$dep_root/SDK/lib" -llivekit \
    -Wl,-rpath,'$ORIGIN' -Wl,-soname,libLiveKitUnrealAdapter.so \
    -Wl,--version-script="$adapter_root/exports.map" \
    -o "$dep_root/SDK/lib/libLiveKitUnrealAdapter.so"
(
    cd "$dep_root/SDK"
    find include lib -type f -print0 | sort -z | xargs -0 sha256sum > .verified.sha256
)
(
    cd "$plugin_root"
    find Source/WindowsAdapter -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.map' \) -print0 |
        sort -z | xargs -0 sha256sum
    sha256sum Source/ThirdParty/Linux/dependencies.lock Scripts/fetch-livekit-linux.sh
) > "$dep_root/SDK/.adapter-source.sha256"
echo 'LiveKit Linux SDK and C adapter verified.'
