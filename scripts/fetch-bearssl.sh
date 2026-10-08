#!/bin/sh
set -eu
DEST=${1:-.third_party/bearssl}
if [ -d "$DEST/.git" ]; then
    echo "BearSSL already present at $DEST"
    exit 0
fi
mkdir -p "$(dirname "$DEST")"
git clone --depth 1 https://www.bearssl.org/git/BearSSL "$DEST"
echo "BearSSL fetched to $DEST"
