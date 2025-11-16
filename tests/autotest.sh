#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/ferretptimize"
TEST_PNG="$ROOT_DIR/tests/assets/test.png"
PORT="${FERRET_AUTOTEST_PORT:-9090}"
HOST="127.0.0.1"
export HOST PORT TEST_PNG

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
RESPONSE_TUNED_JSON="$TMP_DIR/response_tuned.json"
GENERATED_PNG="$TMP_DIR/autotest.png"
export GENERATED_PNG

python3 - <<'PY'
import struct, zlib, os, random
path = os.environ.get("GENERATED_PNG")
w, h = 192, 192
rows = []
for y in range(h):
    row = bytearray()
    for x in range(w):
        r = random.randrange(0, 256)
        g = random.randrange(0, 256)
        b = random.randrange(0, 256)
        a = 255
        row.extend([r, g, b, a])
    rows.append(b'\x00' + bytes(row))  # filter type 0
raw = b''.join(rows)
def chunk(tag, payload):
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff)
hdr = chunk(b'IHDR', struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
idat = chunk(b'IDAT', zlib.compress(raw, level=5))
png = b'\x89PNG\r\n\x1a\n' + hdr + idat + chunk(b'IEND', b'')
with open(path, "wb") as f:
    f.write(png)
PY

TEST_PNG="$GENERATED_PNG"
export HOST PORT TEST_PNG GENERATED_PNG

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

python3 - "$RESPONSE_JSON" "$RESPONSE_TUNED_JSON" <<'PY'
import json, sys, os, subprocess

def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def assert_base_response(data, expected_count=None):
    if data.get("status") != "ok":
        raise SystemExit("autotest: server returned error status: %s" % data.get("message"))
    results = data.get("results") or []
    if expected_count is not None and len(results) != expected_count:
        raise SystemExit(f"autotest: expected {expected_count} outputs, got {len(results)}")
    if expected_count is None and len(results) == 0:
        raise SystemExit("autotest: expected at least one output")
    return results

def print_summary(label, data):
    job_id = data.get("jobId")
    duration = data.get("durationMs", 0.0)
    input_bytes = data.get("inputBytes", 0) or 0
    filename = data.get("filename") or "unknown"
    print("")
    print(f"🧪 [{label}] Job #{job_id} – {duration:.2f} ms")
    print(f"   • Input: {filename} ({input_bytes} bytes / {input_bytes/1024:.2f} KB)")
    total_output = 0
    for idx, res in enumerate(data.get("results") or [], 1):
        size = res.get("bytes", 0) or 0
        total_output += size
        pct = (1 - size / input_bytes) * 100 if input_bytes else 0
        tuning = res.get("tuning")
        tuning_note = f" [{tuning}]" if tuning else ""
        print(f"   • Output {idx}: {res.get('format','').upper()} {res.get('label','')} – {size} bytes ({size/1024:.2f} KB, {pct:.1f}% smaller){tuning_note}")
    print(f"   • Combined output: {total_output} bytes ({total_output/1024:.2f} KB)")

base = load_json(sys.argv[1])
base_results = assert_base_response(base, expected_count=4)
print_summary("autotest", base)
size_index = {(r.get("format"), r.get("label")): r.get("bytes") for r in base_results}

# Submit tuned variants to validate tuning metadata
def run_tune(format_name, label, intent, outfile):
    curl_cmd = [
        "curl", "--fail", "--silent", "--show-error",
        "-H", "Content-Type: application/octet-stream",
        "-H", "X-Filename: autotest.png",
        "-H", f"X-Tune-Format: {format_name}",
        "-H", f"X-Tune-Label: {label}",
        "-H", f"X-Tune-Intent: {intent}",
        "--data-binary", f"@{os.environ['TEST_PNG']}",
        f"http://{os.environ['HOST']}:{os.environ['PORT']}/api/compress"
    ]
    with open(outfile, "w", encoding="utf-8") as outf:
        subprocess.run(curl_cmd, check=True, stdout=outf)
    tuned = load_json(outfile)
    tuned_results = assert_base_response(tuned, expected_count=1)
    target = next((r for r in tuned_results if r.get("format") == format_name and r.get("label") == label), None)
    if not target:
        raise SystemExit(f"autotest: tuned response missing {format_name.upper()} {label} result")
    if target.get("tuning") != intent:
        raise SystemExit(f"autotest: expected tuned {format_name} to carry tuning='{intent}', got {target.get('tuning')}")
    before = size_index.get((format_name, label))
    after = target.get("bytes", 0)
    if before:
        if format_name == "png" and label == "lossless":
            target_ratio = 1.0
        else:
            target_ratio = 0.70
        if intent == "more" and not (after <= before * target_ratio):
            raise SystemExit(f"autotest: expected {format_name} {label} tuning 'more' to shrink or hold ({after} !<= {before}*{target_ratio})")
        if intent == "less" and not (after >= before):
            raise SystemExit(f"autotest: expected {format_name} {label} tuning 'less' to keep or grow bytes ({after} !>= {before})")
    size_index[(format_name, label)] = after
    print_summary(f"autotest-tuned-{format_name}-{intent}", tuned)

run_tune("png", "lossless", "more", sys.argv[2])
run_tune("png", "lossless", "less", sys.argv[2])
run_tune("webp", "high", "more", sys.argv[2])

print("✅ [autotest] All variants validated")
print("✅ [autotest] Tuning paths validated")
PY
