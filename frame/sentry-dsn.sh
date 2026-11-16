# Sourced by frame/release.sh: the Sentry DSN crash reports are sent to, when the player chooses Send
# Report. From KARTFRAME_SENTRY_DSN, else the file KARTFRAME_SENTRY_DSN_FILE (default
# ~/.config/kartframe-dev/sentry-dsn, one line) on the build machine. Never commit it: without one the
# release has no Send Report, and the game only says how to report the crash by hand. Prints whether
# one was found, never the DSN.
if [ -z "${KARTFRAME_SENTRY_DSN:-}" ]; then
    _dsn_file=${KARTFRAME_SENTRY_DSN_FILE:-$HOME/.config/kartframe-dev/sentry-dsn}
    if [ -r "$_dsn_file" ]; then
        KARTFRAME_SENTRY_DSN=$(head -n 1 "$_dsn_file" | tr -d '[:space:]')
    fi
    unset _dsn_file
fi
export KARTFRAME_SENTRY_DSN=${KARTFRAME_SENTRY_DSN:-}
if [ -n "$KARTFRAME_SENTRY_DSN" ]; then
    echo "crash reports: Send Report goes to the Sentry DSN given (KARTFRAME_SENTRY_DSN)"
else
    echo "crash reports: no Sentry DSN (KARTFRAME_SENTRY_DSN or ~/.config/kartframe-dev/sentry-dsn): no Send Report"
fi
