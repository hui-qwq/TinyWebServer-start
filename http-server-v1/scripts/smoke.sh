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

  # 400: conflicting Content-Length headers
  cl_conflict_resp="$TMP_DIR/cl_conflict.out"
  printf 'POST /echo HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nContent-Length: 3\r\nContent-Length: 5\r\n\r\nabcde' \
    | nc "$HOST" "$PORT" >"$cl_conflict_resp" || true
  first_cl_conflict="$(head -n 1 "$cl_conflict_resp" || true)"
  if [[ "$first_cl_conflict" == *"400 Bad Request"* ]]; then
    record_pass "content_length_conflict -> 400"
  else
    record_fail "content_length_conflict expected 400"
  fi

  # 400: non-digit Content-Length
  cl_nondigit_resp="$TMP_DIR/cl_nondigit.out"
  printf 'POST /echo HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nContent-Length: abc\r\n\r\nhello' \
    | nc "$HOST" "$PORT" >"$cl_nondigit_resp" || true
  first_cl_nondigit="$(head -n 1 "$cl_nondigit_resp" || true)"
  if [[ "$first_cl_nondigit" == *"400 Bad Request"* ]]; then
    record_pass "content_length_nondigit -> 400"
  else
    record_fail "content_length_nondigit expected 400"
  fi

  # 431: headers larger than limit
  long_h="$TMP_DIR/long_header.txt"
  head -c 9000 /dev/zero | tr '\0' 'a' >"$long_h"
  hdr431_out="$TMP_DIR/h431.out"
  {
    printf 'GET / HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nX-Long: '
    cat "$long_h"
    printf '\r\n\r\n'
  } | nc "$HOST" "$PORT" >"$hdr431_out" || true
  first_431="$(head -n 1 "$hdr431_out" || true)"
  if [[ "$first_431" == *"431"* ]]; then
    record_pass "header_too_large -> 431"
  else
    record_fail "header_too_large expected 431"
  fi

  # HTTP/1.1 + Connection: close should close after first response
  h11_close_resp="$TMP_DIR/h11_close.out"
  printf 'GET /hello.html HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nConnection: close\r\n\r\nGET /time HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nConnection: close\r\n\r\n' \
    | nc "$HOST" "$PORT" >"$h11_close_resp" || true
  h11_close_cnt="$(grep -aoE 'HTTP/1\.1 [0-9]{3} ' "$h11_close_resp" | wc -l | tr -d '[:space:]')"
  if [[ "$h11_close_cnt" -eq 1 ]]; then
    record_pass "http11_connection_close_single_response"
  else
    record_fail "http11_connection_close expected=1 got=$h11_close_cnt"
  fi

  # HTTP/1.0 + Connection: keep-alive should allow two responses
  h10_keepalive_resp="$TMP_DIR/h10_keepalive.out"
  printf 'GET /hello.html HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET /time HTTP/1.0\r\nConnection: close\r\n\r\n' \
    | nc "$HOST" "$PORT" >"$h10_keepalive_resp" || true
  h10_keepalive_cnt="$(grep -aoE 'HTTP/1\.1 [0-9]{3} ' "$h10_keepalive_resp" | wc -l | tr -d '[:space:]')"
  if [[ "$h10_keepalive_cnt" -ge 2 ]]; then
    record_pass "http10_keepalive_two_requests"
  else
    record_fail "http10_keepalive_two_requests expected>=2 got=$h10_keepalive_cnt"
  fi

  # keep-alive: two requests in one connection should yield two responses
  ka_resp="$TMP_DIR/keepalive.out"
  printf 'GET /hello.html HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nConnection: keep-alive\r\n\r\nGET /time HTTP/1.1\r\nHost: 127.0.0.1:8888\r\nConnection: close\r\n\r\n' \
    | nc "$HOST" "$PORT" >"$ka_resp" || true
  # 统计原始流中 HTTP 状态行出现次数。第二个响应可能不在“行首”，
  # 用 -o 提取匹配片段比 '^HTTP/1.1 ' 更稳健。
  http_cnt="$(grep -aoE 'HTTP/1\.1 [0-9]{3} ' "$ka_resp" | wc -l | tr -d '[:space:]')"
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
