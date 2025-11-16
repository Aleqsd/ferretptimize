#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/ferretptimize"
TEST_PNG="$ROOT_DIR/tests/assets/test.png"
PORT="${FERRET_AUTOTEST_PORT:-9090}"
HOST="127.0.0.1"

if [[ ! -x "$BIN" ]]; then
  echo "Binary not found: $BIN" >&2
  exit 1
fi

if [[ ! -f "$TEST_PNG" ]]; then
  echo "Test PNG missing at $TEST_PNG" >&2
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "curl is required for autotest" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
SERVER_LOG="$TMP_DIR/server.log"
RESPONSE_JSON="$TMP_DIR/response.json"

cleanup() {
  local status=$?
  if [[ -n "${SERVER_PID:-}" ]]; then
    if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
      kill "$SERVER_PID" >/dev/null 2>&1 || true
      wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
  fi
  if [[ $status -ne 0 && -f "$SERVER_LOG" ]]; then
    echo "[autotest] server log:" >&2
    cat "$SERVER_LOG" >&2 || true
  fi
  rm -rf "$TMP_DIR"
  trap - EXIT
  exit $status
}
trap 'cleanup' EXIT

FERRET_HOST="$HOST" FERRET_PORT="$PORT" "$BIN" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

wait_for_server() {
  local attempts=100
  while (( attempts > 0 )); do
    if python3 - "$HOST" "$PORT" <<'PY'; then
import socket, sys
host = sys.argv[1]
port = int(sys.argv[2])
s = socket.socket()
s.settimeout(0.1)
try:
    s.connect((host, port))
except OSError:
    sys.exit(1)
else:
    s.close()
    sys.exit(0)
PY
      return 0
    fi
    sleep 0.1
    attempts=$((attempts - 1))
  done
  return 1
}

if ! wait_for_server; then
  echo "Server failed to start; log follows:" >&2
  cat "$SERVER_LOG" >&2
  exit 1
fi

curl --fail --silent --show-error \
  -H "Content-Type: application/octet-stream" \
  -H "X-Filename: autotest.png" \
  --data-binary @"$TEST_PNG" \
  "http://$HOST:$PORT/api/compress" >"$RESPONSE_JSON"

python3 - "$RESPONSE_JSON" <<'PY'
import json, sys
from textwrap import indent

path = sys.argv[1]
data = json.load(open(path))
if data.get("status") != "ok":
    raise SystemExit("autotest: server returned error status: %s" % data.get("message"))
results = data.get("results") or []
if len(results) != 4:
    raise SystemExit("autotest: expected 4 outputs, got %d" % len(results))

job_id = data.get("jobId")
duration = data.get("durationMs", 0.0)
input_bytes = data.get("inputBytes", 0) or 0
filename = data.get("filename") or "unknown"

print("")
print(f"🧪 [autotest] Job #{job_id} – {duration:.2f} ms")
print(f"   • Input: {filename} ({input_bytes} bytes / {input_bytes/1024:.2f} KB)")

total_output = 0
for idx, res in enumerate(results, 1):
    size = res.get("bytes", 0) or 0
    total_output += size
    pct = (1 - size / input_bytes) * 100 if input_bytes else 0
    print(f"   • Output {idx}: {res.get('format','').upper()} {res.get('label','')} – {size} bytes ({size/1024:.2f} KB, {pct:.1f}% smaller)")

print(f"   • Combined output: {total_output} bytes ({total_output/1024:.2f} KB)")
print("✅ [autotest] All variants validated")
PY
