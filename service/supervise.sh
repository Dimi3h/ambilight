#!/bin/sh
# Keeps the ambilight service running. Started detached by autostart.sh ("boot": let the
# system settle first) or by ambilightctl ("now"). Exits when $STATE/disable appears.
# When the service exits with 75 its executable was replaced on disk: after an update the
# new version is elevated and this script hands over to the new supervise.sh; after a
# removal through Homebrew Channel it removes the boot hook and stops.
SVC_DIR=/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service
STATE=/media/developer/ambilight
BIN=$SVC_DIR/ambilight
EXIT_REPLACED=75

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
    log "starting service ($(cat "$STATE/args" 2> /dev/null))"
    # settings come from $STATE/args; ambilightctl set rewrites it and sends SIGUSR1 to reload
    "$BIN" --log "$STATE/ambilight.log" --args-file "$STATE/args" --status-file "$STATE/status.json" \
        --pause-file "$STATE/paused" --maintain < /dev/null > /dev/null 2>&1 9>&- &
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
    if [ "$rc" = "$EXIT_REPLACED" ]; then
        # an installer may remove the old files before it unpacks the new ones: give the
        # new version up to 60 s to appear before treating this as a removal
        i=0
        until [ -x "$BIN" ] && [ -x "$SVC_DIR/supervise.sh" ] && [ -x "$SVC_DIR/ambilightctl" ] || [ $i -ge 30 ]; do
            sleep 2; i=$((i + 1))
        done
        if [ -x "$BIN" ] && [ -x "$SVC_DIR/supervise.sh" ] && [ -x "$SVC_DIR/ambilightctl" ]; then
            sleep 3 # let the installer finish writing
            log "package updated to $(cat "$SVC_DIR/VERSION" 2> /dev/null): elevating, starting the new version"
            "$SVC_DIR/ambilightctl" elevate > /dev/null 2>&1 || log "elevation failed"
            if [ $(($(date +%s) - started)) -ge 30 ]; then
                rm -f "$STATE/service.pid"
                exec 9>&- # release the lock for the new supervisor
                exec sh "$SVC_DIR/supervise.sh" now
            fi
            # replaced again within 30 s of a start: restart here and let the crash-loop
            # guard below count it, so a faulty detection can never loop forever
        else
            log "package removed: removing the boot hook, stopping"
            rm -f /var/lib/webosbrew/init.d/70-ambilight
            break
        fi
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
