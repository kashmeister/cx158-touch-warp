#!/bin/bash
# Compile bin/cx158-touch-warp and sign it ad hoc.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
clang -O2 -Wall -Wextra \
    -framework CoreFoundation -framework CoreGraphics -framework IOKit -framework ApplicationServices \
    -o bin/cx158-touch-warp cx158-touch-warp.c
# An identifier-based designated requirement (instead of the default per-build cdhash) keeps the
# Input Monitoring and Accessibility grants valid across rebuilds.
codesign --force --sign - --identifier local.cx158-touch-warp \
    --requirements '=designated => identifier "local.cx158-touch-warp"' bin/cx158-touch-warp
echo "built bin/cx158-touch-warp"
