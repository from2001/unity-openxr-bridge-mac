#!/usr/bin/env bash
set -euo pipefail

# Probe the shared protocol with an in-process host/client loopback.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
probe="$repo_root/Runtime/MetalXRProtocol/build/metalxr_protocol_loopback"

if [[ ! -x "$probe" ]]; then
  "$repo_root/Scripts/build-metalxr-protocol.sh"
fi

output="$("$probe")"
printf '%s\n' "$output"

grep -q "server received hello" <<<"$output"
grep -q "client received hello ack" <<<"$output"
grep -q "server received heartbeat" <<<"$output"
grep -q "client received heartbeat" <<<"$output"
grep -q "VERSION_MISMATCH" <<<"$output"
grep -q "MetalXR protocol loopback probe passed" <<<"$output"
