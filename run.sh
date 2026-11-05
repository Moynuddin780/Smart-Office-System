#!/usr/bin/env bash
# Start the stack and wait until it is genuinely ready.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$SCRIPT_DIR"

GRN=$'\033[32m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'

[ -f .env ] || { echo "no .env — run ./setup.sh first"; exit 1; }
# shellcheck disable=SC1091
set -a; . ./.env; set +a
: "${HTTP_PORT:=8090}"

docker compose up -d || exit 1

printf "waiting for the application to report healthy "
url="http://localhost:${HTTP_PORT}/healthz"
for i in $(seq 1 90); do
    body="$(curl -fsS "$url" 2>/dev/null)" && {
        echo
        printf "${GRN}ready${OFF}\n\n"
        echo "$body" | python3 -m json.tool 2>/dev/null || echo "$body"
        echo
        echo "  Open      http://localhost:${HTTP_PORT}"
        [ -n "${SEED_ADMIN_EMAIL:-}" ] && echo "  Sign in   ${SEED_ADMIN_EMAIL}"
        echo
        printf "${DIM}  logs      docker compose logs -f app${OFF}\n"
        printf "${DIM}  stop      docker compose down${OFF}\n"
        exit 0
    }
    printf "."
    sleep 2
done

echo
printf "${RED}it did not become healthy within 180s${OFF}\n\n"
docker compose ps
echo
echo "last 40 lines from the application:"
docker compose logs --tail 40 app
exit 1
