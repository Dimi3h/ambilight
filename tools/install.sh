#!/bin/sh
# Install or upgrade ambilight on a rooted LG webOS TV and enable autostart.
#
# usage: tools/install.sh <tv-host> [--hyperhdr <host>[:<port>]] [--disable-piccap]
#   --hyperhdr        HyperHDR JSON-API address the service sends to (default port 19444);
#                     stored in /media/developer/ambilight/args on the TV
#   --disable-piccap  turn PicCap's own autostart off (PicCap stays installed; its config
#                     is backed up and tools/uninstall.sh turns it back on)
# env:  AMBI_SSH_KEY  private key for root@<tv-host> (optional; else your ssh config)
#
# Needs: zig and python3 (for the build), ssh/scp, a TV with Homebrew Channel and root.
set -e
cd "$(dirname "$0")/.."

TV=$1
[ -n "$TV" ] || { sed -n '2,13p' "$0"; exit 2; }
shift
HH=""
PICCAP_OFF=0
while [ $# -gt 0 ]; do
    case "$1" in
        --hyperhdr) HH=$2; shift 2 ;;
        --disable-piccap) PICCAP_OFF=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

APP=io.github.dimi3h.ambilight
SVC=$APP.service
SVC_DIR=/media/developer/apps/usr/palm/services/$SVC
STATE=/media/developer/ambilight
HB=/media/developer/apps/usr/palm/services/org.webosbrew.hbchannel.service
KEY=${AMBI_SSH_KEY:+-i $AMBI_SSH_KEY}
# shellcheck disable=SC2086
ssh_tv() { ssh -o BatchMode=yes $KEY "root@$TV" "$@"; }
# luna-send only prints with a TTY
# shellcheck disable=SC2086
luna_tv() { ssh -tt -o BatchMode=yes $KEY "root@$TV" "$@; exit" 2> /dev/null | tr -d '\r'; }

echo "== build"
sh build.sh
VER=$(cat VERSION)
IPK=build/${APP}_${VER}_arm.ipk
LOCAL_MD5=$(md5 -q build/ambilight 2> /dev/null || md5sum build/ambilight | cut -d' ' -f1)

echo "== stop a running instance (upgrade)"
ssh_tv "[ -x $SVC_DIR/ambilightctl ] && $SVC_DIR/ambilightctl stop || true; mkdir -p /media/developer/tmp $STATE"

echo "== install $IPK"
# shellcheck disable=SC2086
scp -q -O $KEY "$IPK" "root@$TV:/media/developer/tmp/ambilight.ipk"
luna_tv "luna-send -n 6 -w 30000 luna://com.webos.appInstallService/dev/install '{\"id\":\"$APP\",\"ipkUrl\":\"/media/developer/tmp/ambilight.ipk\",\"subscribe\":true}' > /dev/null"
i=0
until [ "$(ssh_tv "md5sum $SVC_DIR/ambilight 2>/dev/null | cut -d' ' -f1")" = "$LOCAL_MD5" ]; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "install did not complete" >&2; exit 1; }
    sleep 1
done
ssh_tv "rm -f /media/developer/tmp/ambilight.ipk"

echo "== elevate service (Homebrew Channel)"
luna_tv "luna-send -n 1 -w 30000 luna://org.webosbrew.hbchannel.service/exec '{\"command\":\"$HB/elevate-service $SVC\"}'" | grep -q '"returnValue":true' \
    || { echo "elevation failed" >&2; exit 1; }

if [ -n "$HH" ]; then
    host=${HH%%:*}; port=${HH#*:}; [ "$port" = "$HH" ] && port=19444
    ssh_tv "echo '--host $host --port $port' > $STATE/args"
fi

if [ $PICCAP_OFF = 1 ]; then
    echo "== PicCap: back up config, turn its autostart off"
    if ssh_tv "[ -f /media/developer/apps/usr/palm/services/org.webosbrew.piccap.service/config.json ]"; then
        mkdir -p "backups/$(date +%F)"
        ssh_tv "cat /media/developer/apps/usr/palm/services/org.webosbrew.piccap.service/config.json" > "backups/$(date +%F)/piccap-config-before-install.json"
        ssh_tv "cp /media/developer/apps/usr/palm/services/org.webosbrew.piccap.service/config.json $STATE/piccap-config.backup.json"
        was=$(ssh_tv "grep -o '\"autostart\":[a-z]*' /media/developer/apps/usr/palm/services/org.webosbrew.piccap.service/config.json | cut -d: -f2")
        # remember the state from before the first install only (a reinstall must not overwrite it)
        ssh_tv "[ -f $STATE/piccap-autostart ] || echo '$was' > $STATE/piccap-autostart"
        luna_tv "luna-send -n 1 -w 8000 luna://org.webosbrew.piccap.service/setSettings '{\"autostart\":false}' > /dev/null; sleep 1; luna-send -n 1 -w 8000 luna://org.webosbrew.piccap.service/stop '{}' > /dev/null"
        echo "   PicCap autostart was '$was', now off (PicCap stays installed)"
    else
        echo "   PicCap not installed; nothing to do"
    fi
elif ssh_tv "[ -L /var/lib/webosbrew/init.d/piccapautostart ]"; then
    echo "!! PicCap's autostart is on: PicCap and ambilight would compete for the capture resource."
    echo "   Re-run with --disable-piccap, or stop PicCap yourself."
fi

echo "== boot hook + start"
ssh_tv "mkdir -p /var/lib/webosbrew/init.d && ln -sfn $SVC_DIR/autostart.sh /var/lib/webosbrew/init.d/70-ambilight"
ssh_tv "$SVC_DIR/ambilightctl start"
