#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/ferretptimize"
TEST_PNG="$ROOT_DIR/tests/assets/test.png"
PORT="${FERRET_AUTOTEST_PORT:-9090}"
HOST="127.0.0.1"
export HOST PORT TEST_PNG

: "${FP_EXPERT_API_KEY:=autotest-expert-key}"
export FP_EXPERT_API_KEY

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
RESPONSE_EXPERT_JSON="$TMP_DIR/response_expert.json"
GENERATED_PNG="$TMP_DIR/autotest.png"
export RESPONSE_EXPERT_JSON
export GENERATED_PNG

python3 - <<'PY'
import struct, zlib, os, random
path = os.environ.get("GENERATED_PNG")
w, h = 192, 192
rows = []
for y in range(h):
    row = bytearray()
    for x in range(w):
        # Leave a transparent 8px border for trim/crop tests
        border = (x < 8 or y < 8 or x >= w - 8 or y >= h - 8)
        r = random.randrange(0, 256)
        g = random.randrange(0, 256)
        b = random.randrange(0, 256)
        a = 0 if border else 255
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

# Expert mode: enforce auth gate then run authorized request
EXPERT_URL = f"http://{os.environ['HOST']}:{os.environ['PORT']}/api/expert/compress"
multipart = [
    "-F", f"files[]=@{os.environ['TEST_PNG']};filename=expert1.png",
    "-F", f"files[]=@{os.environ['TEST_PNG']};filename=expert2.png",
    "-F", 'metadata={"crop":{"enabled":true,"x":12,"y":12,"width":64,"height":48},"trimEnabled":true,"trimTolerance":0.0,"webpQuality":82,"pngLevel":6,"pngQuantColors":88,"avifQuality":29};type=application/json',
    "-F", 'metadata0={"crop":{"enabled":true,"x":8,"y":8,"width":40,"height":32},"pngLevel":5,"pngQuantColors":96,"webpQuality":70,"avifQuality":30,"trimEnabled":true,"trimTolerance":0.01};type=application/json',
    "-F", 'metadata1={"crop":{"enabled":true,"x":16,"y":12,"width":24,"height":20},"pngLevel":7,"pngQuantColors":128,"webpQuality":85,"avifQuality":26,"trimEnabled":true,"trimTolerance":0.02};type=application/json',
]

unauth = subprocess.run(
    ["curl", "--silent", "--output", "/dev/null", "--write-out", "%{http_code}", *multipart, EXPERT_URL],
    check=False,
    text=True,
    capture_output=True,
)
if unauth.returncode != 0 or unauth.stdout.strip() != "401":
    raise SystemExit(f"expert autotest: expected 401 without Authorization, got rc={unauth.returncode} body='{unauth.stdout.strip()}' stderr='{unauth.stderr.strip()}'")

curl_cmd_expert = [
    "curl", "--fail", "--silent", "--show-error",
    "-H", f"Authorization: ApiKey {os.environ['FP_EXPERT_API_KEY']}",
    *multipart,
    EXPERT_URL,
]
with open(os.environ["RESPONSE_EXPERT_JSON"], "w", encoding="utf-8") as outf:
    subprocess.run(curl_cmd_expert, check=True, stdout=outf)

expert = load_json(os.environ["RESPONSE_EXPERT_JSON"])
files = expert.get("files") or []
if expert.get("status") != "ok":
    raise SystemExit(f"expert autotest: server returned error {expert.get('message')}")
if len(files) != 2:
    raise SystemExit(f"expert autotest: expected 2 file results, got {len(files)}")
for key in ("bytes_saved", "total_input_bytes", "total_output_bytes", "elapsed_ms"):
    if key not in expert:
        raise SystemExit(f"expert autotest: expected aggregate field '{key}' in response")
expected_opts = [
    {"crop": {"width": 40, "height": 32}, "png_level": 5, "png_colors": 96, "webp_quality": 70, "avif_quality": 30},
    {"crop": {"width": 24, "height": 20}, "png_level": 7, "png_colors": 128, "webp_quality": 85, "avif_quality": 26},
]
for idx, item in enumerate(files, 1):
    geom = item.get("geometry") or {}
    expected = expected_opts[idx - 1]
    if geom.get("outputWidth") != expected["crop"]["width"] or geom.get("outputHeight") != expected["crop"]["height"]:
        raise SystemExit(f"expert autotest: file {idx} geometry mismatch {geom} expected {expected['crop']}")
    if not item.get("cropApplied"):
        raise SystemExit(f"expert autotest: expected cropApplied=true for file {idx}")
    trim_flag = item.get("trimApplied", item.get("trims_applied"))
    if trim_flag is None:
        raise SystemExit(f"expert autotest: expected trim marker on file {idx}")
    if "bytes_saved" not in item:
        raise SystemExit(f"expert autotest: expected bytes_saved per file {idx}")
    outputs = item.get("results") or []
    if len(outputs) != 4:
        raise SystemExit(f"expert autotest: expected 4 outputs per file, got {len(outputs)}")
    for out in outputs:
        params = out.get("params_used") or {}
        if not params:
            raise SystemExit(f"expert autotest: missing params_used for output {out}")
        fmt = out.get("format")
        if fmt == "png" and params.get("level") != expected["png_level"]:
            raise SystemExit(f"expert autotest: expected png level {expected['png_level']} for file {idx}, got {params}")
        if fmt == "pngquant" and params.get("colors") != expected["png_colors"]:
            raise SystemExit(f"expert autotest: expected pngquant colors {expected['png_colors']} for file {idx}, got {params}")
        if fmt == "webp" and params.get("quality") != expected["webp_quality"]:
            raise SystemExit(f"expert autotest: expected webp quality {expected['webp_quality']} for file {idx}, got {params}")
        if fmt == "avif" and params.get("quality") != expected["avif_quality"]:
            raise SystemExit(f"expert autotest: expected avif quality {expected['avif_quality']} for file {idx}, got {params}")
print_summary("autotest-expert", {"jobId": files[0].get("jobId"), "durationMs": files[0].get("durationMs", 0), "inputBytes": files[0].get("inputBytes", 0), "filename": files[0].get("filename"), "results": files[0].get("results")})
print("✅ [autotest] Expert multipart path validated (auth + crop)")
PY
