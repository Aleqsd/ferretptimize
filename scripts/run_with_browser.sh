#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/ferretptimize"
if [[ ! -x "$BIN" ]]; then
  echo "Binary not found at $BIN" >&2
  exit 1
fi

windows_can_resolve() {
  local host=$1
  if command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoLogo -NonInteractive -Command "try { [System.Net.Dns]::GetHostAddresses('${host}') | Out-Null; exit 0 } catch { exit 1 }" >/dev/null 2>&1 && return 0
  fi
  return 1
}

detect_wsl_ip() {
  local ip
  ip=$(ip route get 1 2>/dev/null | awk '{for (i = 1; i <= NF; ++i) if ($i == "src") { print $(i + 1); exit }}')
  if [[ -n "${ip:-}" ]]; then
    echo "$ip"
    return 0
  fi
  ip=$(hostname -I 2>/dev/null | awk '{print $1}')
  if [[ -n "${ip:-}" ]]; then
    echo "$ip"
    return 0
  fi
  return 1
}

HOST="${FERRET_HOST:-0.0.0.0}"
PORT="${FERRET_PORT:-4317}"
BIND_HOST="$HOST"
if [[ -z "$BIND_HOST" ]]; then
  BIND_HOST="0.0.0.0"
fi

PROBE_HOST="$BIND_HOST"
if [[ -z "$PROBE_HOST" || "$PROBE_HOST" == "0.0.0.0" || "$PROBE_HOST" == "::" ]]; then
  PROBE_HOST="127.0.0.1"
fi

OPEN_HOST="${FERRET_OPEN_HOST:-}"
if [[ -z "$OPEN_HOST" ]]; then
  if [[ "$BIND_HOST" != "0.0.0.0" && "$BIND_HOST" != "127.0.0.1" ]]; then
    OPEN_HOST="$BIND_HOST"
  else
    OPEN_HOST="wsl.localhost"
    if ! windows_can_resolve "$OPEN_HOST"; then
      if WSL_IP=$(detect_wsl_ip); then
        echo "ℹ️  Using WSL IP ${WSL_IP} for browser URL" >&2
        OPEN_HOST="$WSL_IP"
      else
        OPEN_HOST="127.0.0.1"
      fi
    fi
  fi
fi
URL="http://${OPEN_HOST}:${PORT}/"

SERVER_PID=""
cleanup() {
  local status=$?
  if [[ -n "$SERVER_PID" ]]; then
    if kill -0 "$SERVER_PID" >/dev/null 2>&1; then
      kill "$SERVER_PID" >/dev/null 2>&1 || true
      wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
  fi
  trap - EXIT INT TERM
  exit $status
}
trap cleanup EXIT INT TERM

wait_for_server() {
  local host=$1
  local port=$2
  local attempts=100
  while (( attempts > 0 )); do
    if python3 - "$host" "$port" <<'PY'; then
import socket, sys
host = sys.argv[1]
port = int(sys.argv[2])
s = socket.socket()
s.settimeout(0.2)
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
    sleep 0.2
    attempts=$((attempts - 1))
  done
  return 1
}

launch_browser() {
  if command -v powershell.exe >/dev/null 2>&1; then
    powershell.exe -NoLogo -NonInteractive -Command "Start-Process 'chrome.exe' '${URL}'" >/dev/null 2>&1 && return 0
  fi
  if command -v cmd.exe >/dev/null 2>&1; then
    cmd.exe /C start "" "${URL}" >/dev/null 2>&1 && return 0
  fi
  if command -v xdg-open >/dev/null 2>&1; then
    xdg-open "${URL}" >/dev/null 2>&1 && return 0
  fi
  return 1
}

"$BIN" &
SERVER_PID=$!

echo "⏳ Waiting for ferretptimize on ${BIND_HOST}:${PORT}..." >&2
if wait_for_server "$PROBE_HOST" "$PORT"; then
  echo "🌐 Launching browser at ${URL}" >&2
  launch_browser || echo "⚠️  Unable to auto-open browser; open ${URL} manually" >&2
else
  echo "⚠️  Server did not become ready; leaving browser closed" >&2
fi

wait "$SERVER_PID"
