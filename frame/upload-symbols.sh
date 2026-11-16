#!/bin/bash
# KartFrame crash reports: uploads the game's symbols to Sentry, so the stacks of sent crash reports
# show function names. frame/release.sh runs it when SENTRY_AUTH_TOKEN is set; it can be run by hand:
#
#   SENTRY_AUTH_TOKEN=... SENTRY_ORG=<org> SENTRY_PROJECT=<project> frame/upload-symbols.sh ~/.local/share/KartFrame/bin/kartframe
#
# The game is linked on the Frame from the kit and the player's disc (frame/setup.sh), so the file to
# upload is the one setup.sh built here against the kit being released. Sentry matches it to reports by
# its GNU build id; a player's build with another id (another kit, or other folders) shows addresses only.
# The token, the organisation and the project come from the environment (or the CLI's own login and
# defaults); none of them is ever written into the repository. The CLI is sentry-cli, or Sentry's newer
# `sentry` CLI (SENTRY_CLI=path picks one).
set -e
FILE=${1:?usage: upload-symbols.sh KARTFRAME_BINARY}
[ -f "$FILE" ] || { echo "upload-symbols: no file $FILE" >&2; exit 1; }
CLI=${SENTRY_CLI:-}
[ -n "$CLI" ] || CLI=$(command -v sentry-cli || command -v sentry || true)
[ -n "$CLI" ] || { echo "upload-symbols: neither sentry-cli nor sentry is installed (SENTRY_CLI=path)" >&2; exit 1; }
BUILD_ID=$(readelf -n "$FILE" 2> /dev/null | awk '/Build ID/ {print $3; exit}')
[ -n "$BUILD_ID" ] || echo "upload-symbols: warning: $(basename "$FILE") has no GNU build id (built before -Wl,--build-id?)" >&2
echo "upload-symbols: $(basename "$FILE"), build id ${BUILD_ID:-none}, with $(basename "$CLI")"
"$CLI" debug-files upload --type elf --wait "$FILE"
