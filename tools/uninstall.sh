#!/bin/sh
# Full rollback of ambilight on the TV: stop it, remove its boot hook, the package and its
# state, and turn PicCap's autostart back on if tools/install.sh turned it off (and PicCap is
# still installed).
#
# usage: tools/uninstall.sh <tv-host> [--keep-state]
#   --keep-state  keep /media/developer/ambilight (logs, settings) on the TV
# env:  AMBI_SSH_KEY  private key for root@<tv-host> (optional)
# The service's logs are copied to backups/<date>-uninstall/ first.
set -e
cd "$(dirname "$0")/.."

TV=$1
[ -n "$TV" ] || { sed -n '2,9p' "$0"; exit 2; }
KEEP=0
[ "$2" = "--keep-state" ] && KEEP=1

APP=io.github.dimi3h.ambilight
SVC_DIR=/media/developer/apps/usr/palm/services/$APP.service
PICCAP_DIR=/media/developer/apps/usr/palm/services/org.webosbrew.piccap.service
STATE=/media/developer/ambilight
KEY=${AMBI_SSH_KEY:+-i $AMBI_SSH_KEY}
# shellcheck disable=SC2086
ssh_tv() { ssh -o BatchMode=yes $KEY "root@$TV" "$@"; }
# shellcheck disable=SC2086
luna_tv() { ssh -tt -o BatchMode=yes $KEY "root@$TV" "$@; exit" 2> /dev/null | tr -d '\r'; }

echo "== stop service, remove boot hook"
ssh_tv "rm -f /var/lib/webosbrew/init.d/70-ambilight; [ -x $SVC_DIR/ambilightctl ] && $SVC_DIR/ambilightctl stop || true; pkill -f $SVC_DIR/ 2>/dev/null || true"

echo "== save logs"
dir="backups/$(date +%F)-uninstall"
mkdir -p "$dir"
# shellcheck disable=SC2086
scp -q -O $KEY "root@$TV:$STATE/*.log*" "$dir/" 2> /dev/null || true
was=$(ssh_tv "cat $STATE/piccap-autostart 2>/dev/null || true")
piccap=$(ssh_tv "[ -d $PICCAP_DIR ] && echo yes || echo no") # it may have been removed since the install

echo "== remove the on-screen level and the package"
luna_tv "luna-send -n 3 -w 20000 luna://com.webos.appInstallService/dev/remove '{\"id\":\"$APP.osd\",\"subscribe\":true}' > /dev/null"
luna_tv "luna-send -n 4 -w 20000 luna://com.webos.appInstallService/dev/remove '{\"id\":\"$APP\",\"subscribe\":true}' > /dev/null"
i=0
while ssh_tv "[ -e $SVC_DIR ]"; do
    i=$((i + 1)); [ $i -gt 20 ] && { echo "package removal did not complete" >&2; break; }
    sleep 1
done

if [ "$was" = "true" ] && [ "$piccap" != yes ]; then
    echo "== PicCap: its autostart was on before the install, but PicCap is no longer installed: nothing to turn back on"
elif [ "$was" = "true" ]; then
    echo "== PicCap: autostart back on"
    luna_tv "luna-send -n 1 -w 8000 luna://org.webosbrew.piccap.service/setSettings '{\"autostart\":true}' > /dev/null; sleep 2; luna-send -n 1 -w 8000 luna://org.webosbrew.piccap.service/start '{}' > /dev/null"
fi

if [ $KEEP = 0 ]; then
    ssh_tv "rm -rf $STATE"
fi

echo "== check"
ssh_tv "ls /var/lib/webosbrew/init.d/; [ -e $SVC_DIR ] && echo 'package still present' || echo 'package removed'; pidof ambilight >/dev/null && echo 'service still running' || echo 'service not running'; [ -e $STATE ] && echo 'state kept' || echo 'state removed'"
if [ "$piccap" = yes ]; then
    luna_tv "luna-send -n 1 -w 5000 luna://org.webosbrew.piccap.service/status '{}' | grep -o -E '\"(isRunning|connected)\":[a-z]*' | tr '\n' ' '; echo"
else
    echo "PicCap not installed"
fi
