#!/usr/bin/env bash
# Runs sti_selftest.exe against tests/mock_usbd.py in a throwaway Wine prefix,
# once over the Unix socket (Wine Staging) and once over loopback TCP (the
# fallback used by mainline Wine, which has no AF_UNIX support).
# Usage: scripts/run-sti-selftest.sh BUILD_DIR
set -euo pipefail

build=$(realpath "${1:-build}")
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k 2>/dev/null || true; rm -rf "$work"' EXIT

export WINEPREFIX="$work/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export SFL_USBD_TOKEN=selftest-token
wineboot -i >/dev/null 2>&1
dll=$(winepath -w "$build/sti.dll")

run() {
    local mode=$1
    python3 "$root/tests/mock_usbd.py" "$mode" "$work/endpoint" &
    local mock=$!
    for _ in $(seq 50); do [ -e "$work/endpoint" ] && break; sleep 0.1; done
    if [ "$mode" = unix ]; then
        export SFL_USBD_SOCKET=$work/endpoint; unset SFL_USBD_TCP
    else
        export SFL_USBD_SOCKET=$work/no-such-socket SFL_USBD_TCP; SFL_USBD_TCP=$(cat "$work/endpoint")
    fi
    echo "== $mode transport"
    wine "$build/sti_selftest.exe" "$dll"
    wait "$mock" || true
}

run tcp
# AF_UNIX exists only in Wine Staging; elsewhere sti.dll would skip straight to
# TCP, which this mock (listening on a Unix socket) does not serve.
if wine --version | grep -qi staging; then run unix; else echo "== unix transport skipped (Wine without AF_UNIX)"; fi
