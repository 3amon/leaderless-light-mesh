#!/usr/bin/env bash
# Complete local equivalent of the opt-in GitLab host and firmware jobs.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZEPHYR_WORKSPACE=${ZEPHYR_WORKSPACE:-"$(dirname "$REPO_ROOT")/zephyr-ws"}
DEV_TOOLS_VENV=${DEV_TOOLS_VENV:-"$REPO_ROOT/.cache/dev-tools"}
export ZEPHYR_WORKSPACE
export PATH="$DEV_TOOLS_VENV/bin:$ZEPHYR_WORKSPACE/.venv/bin:$PATH"
cd "$REPO_ROOT"

if [ ! -x "$ZEPHYR_WORKSPACE/.venv/bin/west" ]; then
    echo "west is missing from $ZEPHYR_WORKSPACE/.venv" >&2
    echo "create the pinned build environment from requirements-build.txt first" >&2
    exit 2
fi

if [ ! -x "$DEV_TOOLS_VENV/bin/python" ]; then
    python3 -m venv "$DEV_TOOLS_VENV"
fi
"$DEV_TOOLS_VENV/bin/python" -m pip install \
    --disable-pip-version-check --requirement requirements-dev.txt
"$DEV_TOOLS_VENV/bin/pre-commit" run --all-files --show-diff-on-failure
scripts/bootstrap_workspace.sh
PRISTINE=${PRISTINE:-always} scripts/ci.sh
