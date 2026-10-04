#!/usr/bin/env bash
# Compatibility entry point. Transaction semantics live in deploy.py.
set -euo pipefail

TOOLS_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
DEPLOY=${DEPLOY:-"$TOOLS_DIR/deploy.py"}
PYTHON=${PYTHON:-python3}

exec "$PYTHON" "$DEPLOY" deploy "$@"
