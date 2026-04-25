#!/usr/bin/env bash
# Build and deploy the hms-cpap Angular UI.
#
# The hms-cpap binary serves static files directly from
#   frontend/dist/frontend/browser/
# (path is pinned in config.json -> "static_dir"), so `ng build` IS the deploy.
# No file copy is required — the running service picks up the new bundle on
# the next browser request (hard-refresh to bypass cache).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRONTEND_DIR="$(cd "${SCRIPT_DIR}/../frontend" && pwd)"
STATIC_DIR="${FRONTEND_DIR}/dist/frontend/browser"

cd "${FRONTEND_DIR}"

if [[ ! -d node_modules ]]; then
  echo "[build_frontend] installing npm deps..."
  npm ci
fi

echo "[build_frontend] building Angular bundle..."
npm run build

if [[ ! -d "${STATIC_DIR}" ]]; then
  echo "[build_frontend] ERROR: expected output missing: ${STATIC_DIR}" >&2
  exit 1
fi

BUNDLE_COUNT=$(find "${STATIC_DIR}" -maxdepth 1 -type f -name '*.js' | wc -l)
echo "[build_frontend] done — ${BUNDLE_COUNT} JS bundles in ${STATIC_DIR}"
echo "[build_frontend] hard-refresh the browser (Ctrl+Shift+R) to bypass cache"
