#!/usr/bin/env bash
# End-to-end check against a running pricer-api. Every assertion here is a
# published value or a financial identity, not a snapshot of previous output.
set -euo pipefail

BASE="${1:-http://localhost:8080}"
pass=0

check() {
  local label="$1" actual="$2" expected="$3" tol="${4:-1e-6}"
  python3 - "$label" "$actual" "$expected" "$tol" <<'PY'
import sys
label, actual, expected, tol = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
if abs(actual - expected) > tol:
    print(f"FAIL  {label}: got {actual}, expected {expected} (tolerance {tol})")
    sys.exit(1)
print(f"ok    {label}: {actual}")
PY
  pass=$((pass + 1))
}

price() {
  curl -fsS -X POST "$BASE/price" -H 'Content-Type: application/json' -d "$1" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["price"])'
}

echo "== health =="
curl -fsS "$BASE/health" | python3 -m json.tool | head -6

echo
echo "== closed form against published values =="
# Hull's worked example.
check "Hull call (S=42,K=40,r=10%,v=20%,T=0.5)" \
  "$(price '{"instrument":{"option":"call","strike":40,"expiry":0.5},
             "market":{"spot":42,"rate":0.10,"vol":0.2}}')" 4.759422392871542 1e-9
check "Hull put" \
  "$(price '{"instrument":{"option":"put","strike":40,"expiry":0.5},
             "market":{"spot":42,"rate":0.10,"vol":0.2}}')" 0.8085993729000922 1e-9

echo
echo "== put-call parity =="
C=$(price '{"instrument":{"option":"call","strike":100,"expiry":1},
            "market":{"spot":100,"rate":0.05,"dividend":0.02,"vol":0.25}}')
P=$(price '{"instrument":{"option":"put","strike":100,"expiry":1},
            "market":{"spot":100,"rate":0.05,"dividend":0.02,"vol":0.25}}')
python3 - "$C" "$P" <<'PY'
import math, sys
c, p = float(sys.argv[1]), float(sys.argv[2])
rhs = 100 * math.exp(-0.02) - 100 * math.exp(-0.05)
assert abs((c - p) - rhs) < 1e-9, f"parity broken: {c-p} vs {rhs}"
print(f"ok    call - put = {c - p:.10f}, forward identity = {rhs:.10f}")
PY

echo
echo "== American exercise =="
check "American put, 2000-step tree" \
  "$(price '{"engine":"binomial","instrument":{"option":"put","strike":100,"expiry":1,
             "exercise":"american"},"market":{"spot":100,"rate":0.05,"vol":0.2},
             "params":{"steps":2000,"greeks":false}}')" 6.0903 2e-3

echo
echo "== path-dependent payoffs =="
ASIAN=$(price '{"instrument":{"type":"asian","option":"call","strike":100,"expiry":1,
                "averaging_steps":252},"market":{"spot":100,"rate":0.05,"vol":0.3},
                "params":{"paths":200000,"greeks":false}}')
PLAIN=$(price '{"instrument":{"option":"call","strike":100,"expiry":1},
                "market":{"spot":100,"rate":0.05,"vol":0.3}}')
python3 - "$ASIAN" "$PLAIN" <<'PY'
import sys
asian, plain = float(sys.argv[1]), float(sys.argv[2])
assert 0 < asian < plain, f"Asian {asian} should sit below vanilla {plain}"
print(f"ok    Asian {asian:.4f} < vanilla {plain:.4f}, as averaging requires")
PY

echo
echo "== error handling =="
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/price" \
  -H 'Content-Type: application/json' -d '{"instrument":{}}')
[ "$code" = "400" ] && echo "ok    malformed request returns 400" || { echo "FAIL  got $code"; exit 1; }

code=$(curl -s -o /dev/null -w '%{http_code}' "$BASE/market/quote/..%2Fetc")
[ "$code" = "404" ] || [ "$code" = "400" ] && echo "ok    path traversal rejected ($code)" \
  || { echo "FAIL  traversal got $code"; exit 1; }

echo
echo "all checks passed"
