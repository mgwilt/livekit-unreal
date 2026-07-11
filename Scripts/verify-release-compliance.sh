#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-source}

ruby -rjson -e '
  root = ARGV.fetch(0)
  mode = ARGV.fetch(1)
  manifest = JSON.parse(File.read(File.join(root, "THIRD_PARTY_COMPONENTS.json")))
  abort "unsupported third-party manifest schema" unless manifest.fetch("schema_version") == 1
  components = manifest.fetch("components")
  abort "third-party manifest is empty" if components.empty?
  components.each do |component|
    %w[name source license runtime].each { |key| component.fetch(key) }
  end
  policy = manifest.fetch("release_policy")
  abort "source publication is not approved" unless policy.fetch("source_publication") == "approved"
  if mode == "binary"
    abort "binary redistribution is blocked: #{policy.fetch("binary_blocker")}" unless policy.fetch("binary_redistribution") == "approved"
    blocked = components.select { |item| item["runtime"] && item["binary_redistribution_approved"] == false }
    abort "binary redistribution has unapproved components: #{blocked.map { |item| item.fetch("name") }.join(", ")}" unless blocked.empty?
  end
' "$ROOT" "$MODE"

for file in LICENSE NOTICE THIRD_PARTY_NOTICES.md THIRD_PARTY_COMPONENTS.json; do
  test -s "$ROOT/$file"
done

grep -q 'RustLiveKitUniFFI 0.0.6' "$ROOT/THIRD_PARTY_NOTICES.md"
grep -q 'binary release gate' "$ROOT/THIRD_PARTY_NOTICES.md"
grep -q 'Copyright 2023 LiveKit, Inc.' "$ROOT/NOTICE"

printf 'Release compliance verified for %s publication.\n' "$MODE"
