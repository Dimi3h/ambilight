#!/bin/sh
# Boot hook for the ambilight service. Homebrew Channel runs it through run-parts as
# /var/lib/webosbrew/init.d/70-ambilight (a symlink made by tools/install.sh).
#
# Emergency brakes - any one of them leaves the TV booting exactly as without ambilight:
#   1. package missing                -> do nothing (run-parts skips a dangling symlink)
#   2. $STATE/disable exists           -> do nothing (manual off switch; 3 and 4 write it too)
#   3. boot-loop guard: 3 boots in a row without the service reaching a stable 20 s
#      -> write $STATE/disable and stop starting it
#   4. crash-loop guard (supervise.sh): 5 exits within 60 s each -> $STATE/disable
# Homebrew Channel's own failsafe still applies: a reboot during its startup window skips
# all hooks on the next boot. This hook returns at once; the work runs detached and only
# starts after that window.
PATH=/usr/sbin:/usr/bin:/sbin:/bin${PATH:+:$PATH} # run-parts gives none (see supervise.sh)
export PATH
SVC_DIR=/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service
STATE=/media/developer/ambilight

[ -x "$SVC_DIR/ambilight" ] && [ -x "$SVC_DIR/supervise.sh" ] || exit 0
mkdir -p "$STATE" || exit 0
[ -e "$STATE/disable" ] && exit 0

n=$(cat "$STATE/boot-attempts" 2>/dev/null)
case "$n" in '' | *[!0-9]*) n=0 ;; esac
if [ "$n" -ge 3 ]; then
    echo "$(date '+%F %T') autostart disabled: $n boots in a row without a stable run (boot-loop guard)" >> "$STATE/disable"
    sync
    exit 0
fi
echo $((n + 1)) > "$STATE/boot-attempts"
sync

setsid "$SVC_DIR/supervise.sh" boot < /dev/null > /dev/null 2>&1 &
exit 0
