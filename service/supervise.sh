#!/bin/sh
# Keeps the ambilight service running. Started detached by autostart.sh ("boot": let the
# system settle first) or by ambilightctl ("now"). Exits when $STATE/disable appears.
SVC_DIR=/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service
STATE=/media/developer/ambilight
BIN=$SVC_DIR/ambilight

mkdir -p "$STATE"
exec 9> "$STATE/supervise.lock"
flock -n 9 || exit 0 # only one supervisor

log() {
    if [ -f "$STATE/supervise.log" ] && [ "$(wc -c < "$STATE/supervise.log")" -gt 65536 ]; then
        tail -n 200 "$STATE/supervise.log" > "$STATE/supervise.log.tmp" && mv "$STATE/supervise.log.tmp" "$STATE/supervise.log"
    fi
    echo "$(date '+%F %T') $*" >> "$STATE/supervise.log"
}

# after Homebrew Channel's startup failsafe window, with Luna and media services up
[ "$1" = boot ] && sleep 30

fast=0
while [ ! -e "$STATE/disable" ]; do
    started=$(date +%s)
    args=$(cat "$STATE/args" 2>/dev/null)
    log "starting service $args"
    # shellcheck disable=SC2086 # args is a deliberate word list
    "$BIN" --log "$STATE/ambilight.log" $args < /dev/null > /dev/null 2>&1 9>&- &
    pid=$!
    echo "$pid" > "$STATE/service.pid"
    # running for 60 s counts as a stable boot: reset the boot-loop guard
    (sleep 60 && kill -0 "$pid" 2> /dev/null && echo 0 > "$STATE/boot-attempts" && sync) 9>&- &
    wait "$pid"
    rc=$?
    if [ -e "$STATE/disable" ]; then
        log "service stopped (rc=$rc); disabled, not restarting"
        break
    fi
    ran=$(($(date +%s) - started))
    if [ "$ran" -lt 60 ]; then fast=$((fast + 1)); else fast=0; fi
    log "service exited rc=$rc after ${ran}s (quick exits in a row: $fast)"
    if [ "$fast" -ge 5 ]; then
        echo "$(date '+%F %T') autostart disabled: service exited $fast times within 60 s (crash-loop guard, last rc=$rc)" >> "$STATE/disable"
        sync
        log "crash-loop guard tripped"
        break
    fi
    sleep $((2 + fast * 5))
done
rm -f "$STATE/service.pid"
