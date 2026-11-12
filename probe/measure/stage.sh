#!/bin/sh
# usage: stage.sh <ipk>
# Test of the automatic update: makes the installed version look older and stages <ipk> as
# the downloaded, checked update (as ambilightctl's fetch would leave it). Dev tool.
D=/media/developer/ambilight
SVC=/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service
IPK=$1
real=$(cat $SVC/VERSION)
sum=$(sha256sum "$IPK" | cut -d' ' -f1)
cp "$IPK" $D/update.ipk
echo 0.2.4 > $SVC/VERSION
printf '{"checked":%s,"current":"0.2.4","latest":"%s","newer":true,"ipkUrl":"https://github.com/Dimi3h/ambilight/releases/download/v%s/test.ipk","sha256":"%s","size":%s}\n' \
    "$(date +%s)" "$real" "$real" "$sum" "$(wc -c < "$IPK")" > $D/update.json
echo "$real" > $D/update-ready
rm -f $D/update-tried
echo "staged $real over a pretended 0.2.4; sha256 $(echo $sum | cut -c1-16)"
