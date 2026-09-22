#!/usr/bin/env bash
#
# Move this App Lab app between the repo and the UNO Q, and drive it there.
#
#   push [-n]   repo -> board, keeping the board-side build cache
#   pull [-n]   board -> repo, so App Lab edits become commits
#   diff        list the files that differ, touching nothing
#   run         push, then start, then follow the MPU log
#   start | stop | logs | monitor
#
#   -n          show the diff and stop
#
# UNOQ_SERIAL   adb serial, required only when several devices are attached
# UNOQ_APPS_DIR app directory on the board (default /home/arduino/ArduinoApps)
set -euo pipefail

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
APP_NAME=$(basename "$APP_DIR")
BOARD_APPS=${UNOQ_APPS_DIR:-/home/arduino/ArduinoApps}
BOARD_APP="$BOARD_APPS/$APP_NAME"
BOARD_STAGE="/tmp/unoq-sync"

# scripts/ is excluded in both directions: adb pull drops the exec bit, so a round trip
# would come back and overwrite this file with an unrunnable copy of itself.
EXCLUDES=(--exclude=.cache --exclude=__pycache__ --exclude='*.pyc' --exclude=.venv --exclude=scripts)

die() { echo "unoq: $*" >&2; exit 1; }

adb_() {
    if [[ -n ${UNOQ_SERIAL:-} ]]; then
        adb -s "$UNOQ_SERIAL" "$@"
    else
        adb "$@"
    fi
}

sh_() { adb_ shell "$@"; }

cli() { sh_ "arduino-app-cli $*"; }

require_board_app() {
    sh_ "test -f '$BOARD_APP/app.yaml'" \
        || die "no app.yaml at $BOARD_APP - run 'push' first"
}

# Board copy, cache stripped, landed in a local temp dir whose path is echoed.
fetch_board_copy() {
    local dest=$1
    sh_ "rm -rf $BOARD_STAGE && mkdir -p $BOARD_STAGE && cp -a '$BOARD_APP' $BOARD_STAGE/ \
         && rm -rf $BOARD_STAGE/$APP_NAME/.cache $BOARD_STAGE/$APP_NAME/python/__pycache__"
    adb_ pull "$BOARD_STAGE/$APP_NAME" "$dest" >/dev/null 2>&1 || die "adb pull failed"
}

show_diff() {
    local board=$1 label=$2
    if diff -rq "${EXCLUDES[@]}" "$board" "$APP_DIR" >/tmp/unoq-diff.$$ 2>&1; then
        echo "board and repo agree"
        rm -f /tmp/unoq-diff.$$
        return 1
    fi
    echo "$label"
    sed -e "s#$board#board#g" -e "s#$APP_DIR#repo#g" /tmp/unoq-diff.$$
    rm -f /tmp/unoq-diff.$$
    return 0
}

cmd_diff() {
    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN
    require_board_app
    fetch_board_copy "$tmp"
    show_diff "$tmp/$APP_NAME" "differences:" || true
}

cmd_push() {
    local dry=${1:-}
    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN

    if sh_ "test -f '$BOARD_APP/app.yaml'" 2>/dev/null; then
        fetch_board_copy "$tmp"
        show_diff "$tmp/$APP_NAME" "overwriting on the board:" || true
    else
        echo "first push: creating $BOARD_APP"
    fi
    [[ $dry == -n ]] && return 0

    local stage="$tmp/out/$APP_NAME"
    mkdir -p "$stage"
    rsync -a "${EXCLUDES[@]}" "$APP_DIR"/ "$stage"/
    sh_ "rm -rf $BOARD_STAGE && mkdir -p $BOARD_STAGE"
    adb_ push "$stage" "$BOARD_STAGE/" >/dev/null 2>&1 || die "adb push failed"
    # Everything but .cache is replaced: dropping the cache costs a full sketch rebuild.
    sh_ "mkdir -p '$BOARD_APP' \
         && find '$BOARD_APP' -mindepth 1 -maxdepth 1 ! -name .cache -exec rm -rf {} + \
         && cp -a $BOARD_STAGE/$APP_NAME/. '$BOARD_APP'/ \
         && rm -rf $BOARD_STAGE"
    echo "pushed to $BOARD_APP"
}

cmd_pull() {
    local dry=${1:-}
    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN

    require_board_app
    fetch_board_copy "$tmp"
    show_diff "$tmp/$APP_NAME" "overwriting in the repo:" || true
    [[ $dry == -n ]] && return 0

    rsync -a --delete "${EXCLUDES[@]}" "$tmp/$APP_NAME"/ "$APP_DIR"/
    echo "pulled into $APP_DIR - review with git diff before committing"
}

case ${1:-} in
    push)    cmd_push "${2:-}" ;;
    pull)    cmd_pull "${2:-}" ;;
    diff)    cmd_diff ;;
    start)   require_board_app; cli app start "$BOARD_APP" ;;
    stop)    require_board_app; cli app stop "$BOARD_APP" ;;
    logs)    require_board_app; cli app logs "$BOARD_APP" --follow ;;
    monitor) cli monitor ;;
    run)     cmd_push; cli app start "$BOARD_APP"; cli app logs "$BOARD_APP" --follow ;;
    *)       sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit 1 ;;
esac
