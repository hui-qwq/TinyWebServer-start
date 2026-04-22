#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="$ROOT_DIR/server"
HOST="127.0.0.1"
PORT="8888"

PASS=0
FAIL=0

TMP_DIR="$(mktemp -d)"
SERVER_LOG="$TMP_DIR/server.log"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

record_pass() {
  echo "[PASS] $1"
  PASS=$((PASS + 1))
}

record_fail() {
  echo "[FAIL] $1"
  FAIL=$((FAIL + 1))
}

check_status() {
  local name="$1"
  local method="$2"
  local url="$3"
  local expect="$4"
  local data="${5:-}"

  local hdr="$TMP_DIR/${name}.hdr"
  local body="$TMP_DIR/${name}.body"
  local code

  if [[ -n "$data" ]]; then
    code="$(curl -sS -o "$body" -D "$hdr" -X "$method" "$url" --data-binary "$data" -w "%{http_code}")" || {
      record_fail "$name (curl failed)"
      return
    }
  else
    code="$(curl -sS -o "$body" -D "$hdr" -X "$method" "$url" -w "%{http_code}")" || {
      record_fail "$name (curl failed)"
      return
    }
  fi

  if [[ "$code" == "$expect" ]]; then
    record_pass "$name -> $code"
  else
    record_fail "$name expected=$expect got=$code"
  fi
}

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

BASE_URL="http://$HOST:$PORT"

echo "==> run smoke cases"
check_status "get_root" "GET" "$BASE_URL/" "200"
check_status "get_hello" "GET" "$BASE_URL/hello.html" "200"
check_status "get_not_found" "GET" "$BASE_URL/notfound" "404"
check_status "put_not_allowed" "PUT" "$BASE_URL/hello.html" "405"
check_status "post_echo" "POST" "$BASE_URL/echo" "200" "a=1&b=2"

# 413: body larger than 1MB
big_body="$TMP_DIR/big_body.txt"
head -c 1100000 /dev/zero | tr '\0' 'a' >"$big_body"
code_413="$(curl -sS -o "$TMP_DIR/too_large.body" -D "$TMP_DIR/too_large.hdr" \
  -X POST "$BASE_URL/echo" --data-binary @"$big_body" -w "%{http_code}")"
if [[ "$code_413" == "413" ]]; then
  record_pass "post_too_large -> 413"
else
  record_fail "post_too_large expected=413 got=$code_413"
fi

# 400: malformed request line via nc
if command -v nc >/dev/null 2>&1; then
  bad_resp="$(printf 'GET /broken HTTP/1.1\r\nHost 127.0.0.1\r\n\r\n' | nc "$HOST" "$PORT" | head -n 1 || true)"
  if [[ "$bad_resp" == *"400 Bad Request"* ]]; then
    record_pass "bad_request_malformed_header -> 400"
  else
    record_fail "bad_request_malformed_header expected 400"
  fi

  # keep-alive: two requests in one connection should yield two responses
  ka_resp="$TMP_DIR/keepalive.out"
  printf 'GET /hello.html HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nConnection: keep-alive\r\n\r\nGET /time HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nConnection: close\r\n\r\n' \
    | nc "$HOST" "$PORT" >"$ka_resp" || true
  http_cnt="$(grep -c '^HTTP/1.1 ' "$ka_resp" || true)"
  if [[ "$http_cnt" -ge 2 ]]; then
    record_pass "keep_alive_two_requests"
  else
    record_fail "keep_alive_two_requests expected>=2 responses got=$http_cnt"
  fi
else
  echo "[SKIP] nc not found, skip malformed/keepalive raw checks"
fi

echo
echo "==> summary: pass=$PASS fail=$FAIL"
if [[ "$FAIL" -ne 0 ]]; then
  echo "==> server log tail"
  tail -n 40 "$SERVER_LOG" || true
  exit 1
fi

exit 0
