#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="$ROOT_DIR/server"
HOST="127.0.0.1"
PORT="8888"

TOTAL="${1:-500}"
CONCURRENCY="${2:-50}"
PATH_URI="${3:-/hello.html}"

TMP_DIR="$(mktemp -d)"
SERVER_LOG="$TMP_DIR/server.log"
CODES_FILE="$TMP_DIR/codes.txt"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

echo "==> build"
make -C "$ROOT_DIR" -j4 >/dev/null || {
  echo "build failed"
  exit 1
}

echo "==> start server"
(cd "$ROOT_DIR" && "$SERVER_BIN" >"$SERVER_LOG" 2>&1) &
SERVER_PID=$!
sleep 0.4

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "server failed to start"
  cat "$SERVER_LOG"
  exit 1
fi

URL="http://$HOST:$PORT$PATH_URI"
echo "==> load test total=$TOTAL concurrency=$CONCURRENCY url=$URL"

START_MS="$(date +%s%3N)"
seq 1 "$TOTAL" | xargs -P "$CONCURRENCY" -I{} sh -c \
  'curl -sS -o /dev/null -w "%{http_code}\n" "'"$URL"'" || echo ERR' \
  >"$CODES_FILE"
END_MS="$(date +%s%3N)"

OK="$(grep -c '^200$' "$CODES_FILE" || true)"
ERR="$(grep -c '^ERR$' "$CODES_FILE" || true)"
ALL="$(wc -l <"$CODES_FILE" | tr -d '[:space:]')"
OTHER=$((ALL - OK - ERR))
ELAPSED_MS=$((END_MS - START_MS))

RPS="$(awk -v t="$ALL" -v ms="$ELAPSED_MS" 'BEGIN {
  if (ms <= 0) print "inf";
  else printf "%.2f", (t * 1000.0) / ms;
}')"

echo "==> summary total=$ALL ok=$OK err=$ERR other=$OTHER elapsed_ms=$ELAPSED_MS rps=$RPS"
echo "==> status distribution"
sort "$CODES_FILE" | uniq -c | sort -nr | head -n 10
