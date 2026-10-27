#!/usr/bin/env bash
# Runs sti_selftest.exe against tests/mock_usbd.py in a throwaway Wine prefix.
# Usage: scripts/run-sti-selftest.sh BUILD_DIR
set -euo pipefail

build=$(realpath "${1:-build}")
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k 2>/dev/null || true; rm -rf "$work"' EXIT

export WINEPREFIX="$work/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export SFL_USBD_SOCKET="$work/usbd.sock"

wineboot -i >/dev/null 2>&1
python3 "$root/tests/mock_usbd.py" "$SFL_USBD_SOCKET" &
mock=$!
for _ in $(seq 50); do [ -S "$SFL_USBD_SOCKET" ] && break; sleep 0.1; done

wine "$build/sti_selftest.exe" "$(winepath -w "$build/sti.dll")"
wait "$mock" || true
