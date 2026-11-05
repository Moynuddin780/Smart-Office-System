#!/usr/bin/env bash
# End-to-end proof against the RUNNING stack.
#
# Provisions two client companies, does real work in one, and then tries to
# reach it from the other. Tenant isolation is the property a multi-tenant
# product either has or does not have, and the only way to know is to try.
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

GRN=$'\033[32m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'
pass=0; fail=0
ok()  { printf "  ${GRN}ok${OFF}    %s\n" "$1"; pass=$((pass+1)); }
bad() { printf "  ${RED}FAIL${OFF}  %s\n" "$1"; fail=$((fail+1)); }
note(){ printf "        ${DIM}%s${OFF}\n" "$1"; }

set -a; . ./.env; set +a
BASE="http://localhost:${HTTP_PORT%%[[:space:]]*}"
J='Content-Type: application/json'

api() { # method path token [body]
  local m=$1 p=$2 t=${3:-} b=${4:-}
  if [ -n "$b" ]; then
    curl -s -X "$m" -H "$J" ${t:+-H "Authorization: Bearer $t"} -d "$b" "$BASE$p"
  else
    curl -s -X "$m" ${t:+-H "Authorization: Bearer $t"} "$BASE$p"
  fi
}
code() { # method path token
  local m=$1 p=$2 t=${3:-}
  curl -s -o /dev/null -w '%{http_code}' -X "$m" ${t:+-H "Authorization: Bearer $t"} "$BASE$p"
}
jget() { python3 -c "import sys,json;d=json.load(sys.stdin);print(d$1)" 2>/dev/null; }

echo "Smart Office — end-to-end"
echo "  $BASE"
echo

# ── sign in as the vendor ───────────────────────────────────────────────────
echo "Vendor"
SU=$(api POST /api/auth/login "" "{\"email\":\"$SEED_ADMIN_EMAIL\",\"password\":\"$SEED_ADMIN_PASSWORD\"}" | jget "['token']")
[ -n "$SU" ] && ok "superadmin signed in" || { bad "superadmin could not sign in"; exit 1; }
[ "$(code GET /api/cameras "$SU")" = "403" ] \
  && ok "a superadmin is refused inside a tenant" \
  || bad "a superadmin reached tenant data"
note "the vendor provisions companies; they do not read a client's faces"

# ── provision two companies ─────────────────────────────────────────────────
echo
echo "Provisioning"
mk() {
  api POST /api/admin/tenants "$SU" \
    "{\"name\":\"$1\",\"admin_email\":\"$2\",\"admin_name\":\"Admin\",\"admin_password\":\"$3\"}"
}
A_JSON=$(mk "Acme Ltd $RANDOM" "acme-$RANDOM@x.test" "acmepass123")
B_JSON=$(mk "Borealis $RANDOM" "bor-$RANDOM@x.test" "borpass123")
A_ID=$(echo "$A_JSON" | jget "['id']"); A_MAIL=$(echo "$A_JSON" | jget "['admin_created']")
B_ID=$(echo "$B_JSON" | jget "['id']"); B_MAIL=$(echo "$B_JSON" | jget "['admin_created']")
[ -n "$A_ID" ] && [ -n "$B_ID" ] && ok "two companies created (#$A_ID, #$B_ID)" || bad "could not create companies"

A=$(api POST /api/auth/login "" "{\"email\":\"$A_MAIL\",\"password\":\"acmepass123\"}" | jget "['token']")
B=$(api POST /api/auth/login "" "{\"email\":\"$B_MAIL\",\"password\":\"borpass123\"}" | jget "['token']")
[ -n "$A" ] && [ -n "$B" ] && ok "both company admins signed in" || bad "a company admin could not sign in"

# ── real work inside company A ──────────────────────────────────────────────
echo
echo "Company A does real work"
CAM=$(api POST /api/cameras "$A" \
  '{"name":"Front door","rtsp_sub":"rtsp://127.0.0.1:8554/sub","rtsp_main":"rtsp://127.0.0.1:8554/main","role":"area","detect_hz":10}' | jget "['id']")
[ -n "$CAM" ] && ok "camera created (#$CAM)" || bad "camera not created"

EMP=$(api POST /api/employees "$A" '{"emp_code":"E001","name":"Aslam Hossain","department":"Engineering"}' | jget "['id']")
[ -n "$EMP" ] && ok "employee created (#$EMP)" || bad "employee not created"

ZONE=$(api POST "/api/cameras/$CAM/zones" "$A" \
  "{\"type\":\"desk\",\"name\":\"Desk 1\",\"polygon\":[[10,10],[300,10],[300,400],[10,400]],\"employee_id\":$EMP}" | jget "['id']")
[ -n "$ZONE" ] && ok "desk zone bound to the employee (#$ZONE)" || bad "desk zone not created"

LINE=$(api POST "/api/cameras/$CAM/gate-lines" "$A" '{"name":"Entrance","ax":0,"ay":200,"bx":640,"by":200}' | jget "['id']")
[ -n "$LINE" ] && ok "gate line created (#$LINE)" || bad "gate line not created"

# a desk zone with no employee must be refused
DZ=$(code POST "/api/cameras/$CAM/zones" "$A")
echo '{"type":"desk","name":"Nobody","polygon":[[0,0],[1,0],[1,1]]}' > /tmp/dz.json
RC=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "$J" -H "Authorization: Bearer $A" \
     -d @/tmp/dz.json "$BASE/api/cameras/$CAM/zones")
[ "$RC" = "400" ] && ok "a desk zone with no employee is refused" || bad "an unassigned desk zone was accepted ($RC)"

# ── the isolation test ──────────────────────────────────────────────────────
echo
echo "Isolation — company B tries to reach company A's data"
declare -a LEAK=()
for p in "/api/cameras/$CAM" "/api/cameras/$CAM/zones" "/api/cameras/$CAM/gate-lines"; do
  c=$(code GET "$p" "$B")
  [ "$c" = "404" ] || [ "$c" = "403" ] || LEAK+=("GET $p -> $c")
done
c=$(code DELETE "/api/cameras/$CAM" "$B"); [ "$c" = "404" ] || [ "$c" = "403" ] || LEAK+=("DELETE camera -> $c")
c=$(code DELETE "/api/zones/$ZONE" "$B")
[ "$(api GET "/api/cameras/$CAM/zones" "$A" | python3 -c 'import sys,json;print(len(json.load(sys.stdin)))')" = "1" ] \
  || LEAK+=("company B deleted company A's zone")
c=$(code GET /api/admin/tenants "$B"); [ "$c" = "403" ] || LEAK+=("company admin reached the vendor console -> $c")

if [ ${#LEAK[@]} -eq 0 ]; then
  ok "company B could not read, delete or escalate into company A"
else
  for l in "${LEAK[@]}"; do bad "$l"; done
fi

# B sees only its own, empty, world
n=$(api GET /api/cameras "$B" | python3 -c 'import sys,json;print(len(json.load(sys.stdin)))')
[ "$n" = "0" ] && ok "company B's camera list is its own (empty)" || bad "company B sees $n cameras"

# ── reporting ───────────────────────────────────────────────────────────────
echo
echo "Reporting"
TODAY=$(date +%F)
S=$(api GET "/api/attendance/daily?date=$TODAY" "$A" | jget "['summary']['total']")
[ "$S" = "1" ] && ok "the attendance report lists the employee (absent so far)" || bad "attendance report wrong (total=$S)"
D=$(api GET "/api/desk/daily?date=$TODAY" "$A" | jget "['rows'][0]['zone_name']")
[ "$D" = "Desk 1" ] && ok "the desk report lists the desk" || bad "desk report wrong ($D)"
CSV=$(curl -s -H "Authorization: Bearer $A" "$BASE/api/reports/attendance.csv?date=$TODAY" | head -1)
[[ "$CSV" == date,employee_code,* ]] && ok "the CSV export has a header row" || bad "CSV export wrong"

# ── the engine picked the camera up ─────────────────────────────────────────
echo
echo "Engine"
R=$(api GET /healthz "" | jget "['cameras_running']")
[ "${R:-0}" -ge 1 ] && ok "the engine started a worker for the new camera ($R running)" \
                     || bad "the engine did not pick up the camera"
E=$(api GET /healthz "" | jget "['engines']")
[ "$E" = "9" ] && ok "9 NPU engines live (3 models x 3 cores)" || bad "engines=$E"

echo
if [ "$fail" -eq 0 ]; then
  printf "${GRN}RESULT: PASS${OFF} — %d checks\n" "$pass"; exit 0
fi
printf "${RED}RESULT: FAIL${OFF} — %d of %d failed\n" "$fail" "$((pass+fail))"; exit 1
