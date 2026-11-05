#!/usr/bin/env bash
# Smart Office — prepare this machine to run the stack.
#
# Idempotent: safe to re-run. It checks rather than assumes, and says what to do
# about anything it finds wrong.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$SCRIPT_DIR"

RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; DIM=$'\033[2m'; OFF=$'\033[0m'
fail=0
ok()   { printf "  ${GRN}ok${OFF}    %s\n" "$1"; }
warn() { printf "  ${YEL}warn${OFF}  %s\n" "$1"; }
bad()  { printf "  ${RED}FAIL${OFF}  %s\n" "$1"; fail=1; }
note() { printf "        ${DIM}%s${OFF}\n" "$1"; }

echo "Smart Office — setup"
echo

# ── 1. the NPU ──────────────────────────────────────────────────────────────
# Judged on OUTPUT, not on an exit code: piping a check through tail or head
# replaces its exit status with the pipe's, which is always 0.
echo "NPU"
if lspci -d 1ff4: 2>/dev/null | grep -q .; then
    ok "a DEEPX device is present on the PCIe bus"
else
    bad "no DEEPX device found (lspci -d 1ff4: is empty)"
    note "the board may need a cold power cycle, not just a reboot"
fi

if command -v dxrt-cli >/dev/null 2>&1; then
    status="$(dxrt-cli -s 2>&1)"
    if grep -q "NPU 0:" <<<"$status"; then
        ok "dxrt-cli reports live cores"
        grep -E "^NPU [0-9]:" <<<"$status" | sed 's/^/        /'
    else
        bad "dxrt-cli did not report any NPU core"
        note "$(head -1 <<<"$status")"
    fi
else
    bad "dxrt-cli is not on PATH — the DEEPX runtime is not installed"
fi

if systemctl is-active --quiet dxrt.service 2>/dev/null; then
    ok "dxrt.service is running"
else
    bad "dxrt.service is not running — the container brokers inference through it"
    note "sudo systemctl start dxrt.service"
fi

for p in /dev/dxrt0 /tmp/dxrt_dynamic_ipc.sock /dev/shm/dxrt_monitor_v2; do
    if [ -e "$p" ]; then ok "$p"
    else bad "$p is missing — the container cannot reach the NPU without it"; fi
done

# ── 2. models ───────────────────────────────────────────────────────────────
echo
echo "Models"
MODEL_SOURCE_DIR="${MODEL_SOURCE_DIR:-}"
for m in yolo26s_pose.dxnn scrfd_10g.dxnn ArcFace_MobileFaceNet.dxnn; do
    if [ -f "models/$m" ]; then
        ok "models/$m  ($(du -h "models/$m" | cut -f1))"
    elif [ -n "$MODEL_SOURCE_DIR" ] && [ -f "$MODEL_SOURCE_DIR/$m" ]; then
        cp "$MODEL_SOURCE_DIR/$m" models/ && ok "copied $m from \$MODEL_SOURCE_DIR"
    else
        bad "models/$m is missing"
        note "copy it in, or re-run with MODEL_SOURCE_DIR=/path/to/models"
    fi
done

# ── 3. the vendored DXRT, and whether it still matches the host ─────────────
echo
echo "DXRT runtime (vendored for the image)"
if [ ! -f third_party/dxrt/lib/libdxrt.so ]; then
    if [ -f /usr/local/lib/libdxrt.so ]; then
        mkdir -p third_party/dxrt/lib third_party/dxrt/include
        cp -rP /usr/local/include/dxrt third_party/dxrt/include/ 2>/dev/null
        cp -P /usr/local/lib/libdxrt.so* /usr/local/lib/libonnxruntime.so* \
              third_party/dxrt/lib/ 2>/dev/null
        ok "copied the host's DXRT into third_party/dxrt"
    else
        bad "no DXRT at /usr/local and none vendored"
    fi
else
    host_ver="$(dxrt-cli -v 2>/dev/null | head -1)"
    ok "vendored DXRT present  ${DIM}(host: ${host_ver:-unknown})${OFF}"
    note "a DXRT upgrade on the host means re-copying these — delete third_party/dxrt/lib and re-run"
fi

# ── 4. docker ───────────────────────────────────────────────────────────────
echo
echo "Docker"
if docker info >/dev/null 2>&1; then
    ok "the daemon is reachable as $(whoami)"
else
    bad "cannot talk to the Docker daemon"
    note "sudo usermod -aG docker $(whoami), then log out and back in"
fi
docker compose version >/dev/null 2>&1 && ok "compose v2" || bad "docker compose v2 is not available"

# ── 5. configuration ────────────────────────────────────────────────────────
echo
echo "Configuration"
if [ ! -f .env ]; then
    cp .env.example .env
    warn "created .env from the example — it is NOT usable yet"
fi
need_secret=0
grep -qE '^DB_PASSWORD=.+' .env || { bad ".env: DB_PASSWORD is empty"; need_secret=1; }
grep -qE '^JWT_SECRET=.{16,}' .env || { bad ".env: JWT_SECRET is empty or too short"; need_secret=1; }
grep -qE '^SEED_ADMIN_PASSWORD=.{10,}' .env || warn ".env: SEED_ADMIN_PASSWORD is unset — nobody will be able to log in"
if [ "$need_secret" = 1 ]; then
    note "generate them with:"
    note "  sed -i \"s|^DB_PASSWORD=.*|DB_PASSWORD=\$(openssl rand -hex 16)|\" .env"
    note "  sed -i \"s|^JWT_SECRET=.*|JWT_SECRET=\$(openssl rand -base64 48 | tr -d '\\n')|\" .env"
    note "They have no defaults on purpose: a shipped secret means every tenant's"
    note "tokens are forgeable and the database password is in a public repository."
fi

# ── 6. build ────────────────────────────────────────────────────────────────
echo
if [ "$fail" = 0 ]; then
    echo "Building the image (the first run compiles Drogon and takes a while)…"
    if docker compose build; then ok "image built"; else bad "the image build failed"; fi
else
    warn "skipping the build — fix the failures above first"
fi

echo
if [ "$fail" = 0 ]; then
    printf "${GRN}setup complete${OFF} — start it with ./run.sh\n"
else
    printf "${RED}setup incomplete${OFF} — see the FAIL lines above\n"
fi
exit "$fail"
