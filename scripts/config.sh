# Sourced by the repo's scripts: sets REPO and loads config.env.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [ ! -f "$REPO/config.env" ]; then echo "missing $REPO/config.env (copy config.example.env and edit it)" >&2; exit 1; fi
# shellcheck disable=SC1091
. "$REPO/config.env"
