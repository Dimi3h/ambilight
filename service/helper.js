// helper.js - JSON tasks for ambilightctl, run with the TV's own node.
//
//   node helper.js feed <repo.json> <installed-version>
//       Read the Homebrew Channel feed (the release's repo.json), validate it and print
//       the update info as JSON.
'use strict';
const fs = require('fs');

const APP_ID = 'io.github.dimi3h.ambilight';
const RELEASES = 'https://github.com/Dimi3h/ambilight/releases/download/';

function cmpVersion(a, b) {
  const pa = a.split('.').map(Number), pb = b.split('.').map(Number);
  for (let i = 0; i < 3; i++) if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) - (pb[i] || 0);
  return 0;
}

function feed(file, current) {
  const j = JSON.parse(fs.readFileSync(file, 'utf8'));
  const p = (j.packages || []).find((x) => x && x.id === APP_ID);
  if (!p || !p.manifest) throw new Error('package not in feed');
  const mf = p.manifest;
  const latest = String(mf.version || '');
  const ipkUrl = String(mf.ipkUrl || '');
  const sha256 = String((mf.ipkHash || {}).sha256 || '');
  if (!/^\d+\.\d+\.\d+$/.test(latest)) throw new Error('bad version in feed');
  if (!ipkUrl.startsWith(RELEASES) || !/^[\w./-]+$/.test(ipkUrl.slice(8))) throw new Error('unexpected ipkUrl in feed');
  if (!/^[0-9a-f]{64}$/.test(sha256)) throw new Error('bad sha256 in feed');
  const cur = /^\d+\.\d+\.\d+$/.test(current) ? current : '0.0.0';
  return {
    checked: Math.floor(Date.now() / 1000), current: cur, latest,
    newer: cmpVersion(latest, cur) > 0, ipkUrl, sha256, size: Number(mf.ipkSize) || 0
  };
}

try {
  const [cmd, a, b] = process.argv.slice(2);
  let out;
  if (cmd === 'feed' && a) out = feed(a, b || '0.0.0');
  else throw new Error('usage: helper.js feed <repo.json> <installed-version>');
  process.stdout.write(JSON.stringify(out) + '\n');
} catch (err) {
  process.stdout.write(JSON.stringify({ ok: false, error: String(err.message || err) }) + '\n');
  process.exit(1);
}
