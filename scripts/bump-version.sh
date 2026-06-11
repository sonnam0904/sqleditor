#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:?Usage: bump-version.sh <version>}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
METADATA_FILE="${ROOT_DIR}/APP_METADATA"

if grep -q '^APP_VERSION=' "${METADATA_FILE}"; then
    sed -i "s/^APP_VERSION=.*/APP_VERSION=${VERSION}/" "${METADATA_FILE}"
else
    echo "APP_VERSION=${VERSION}" >> "${METADATA_FILE}"
fi

echo "Updated APP_VERSION to ${VERSION} in APP_METADATA"
