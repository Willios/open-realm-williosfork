#!/usr/bin/env bash
# Build the Warcraft Survivors browser page into build/web/survivor.
# Run from engine/ in Git Bash. Needs the Emscripten SDK (C:/emsdk, 4.0.21).
set -e
EMSDK="${EMSDK:-/c/emsdk}"
export PATH="$EMSDK/upstream/emscripten:$EMSDK:$PATH"
for node in "$EMSDK"/node/*/bin; do PATH="$node:$PATH"; done
# Emscripten downloads its SDL2/zlib ports with urllib, which lacks the
# Windows certificate store: hand it certifi's bundle when available.
if [ -z "$SSL_CERT_FILE" ]; then
    SSL_CERT_FILE="$(python -c 'import certifi; print(certifi.where())' 2>/dev/null || true)"
    [ -n "$SSL_CERT_FILE" ] && export SSL_CERT_FILE
fi
python port/web/build-engine.py --survivor ../src "$@"
