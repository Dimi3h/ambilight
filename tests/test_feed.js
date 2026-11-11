// Unit test for service/helper.js (the update feed check): node tests/test_feed.js
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const helper = path.join(__dirname, '..', 'service', 'helper.js');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ambifeed-'));
const base = 'https://github.com/Dimi3h/ambilight/releases/download/v9.9.9/';
const sha = 'a'.repeat(64);

function run(manifest, current, pkgId) {
  const feed = { packages: [{ id: pkgId || 'io.github.dimi3h.ambilight', manifest }] };
  const file = path.join(dir, 'repo.json');
  fs.writeFileSync(file, JSON.stringify(feed));
  const r = spawnSync(process.execPath, [helper, 'feed', file, current], { encoding: 'utf8' });
  return { code: r.status, out: JSON.parse(r.stdout) };
}
const good = { version: '9.9.9', ipkUrl: base + 'io.github.dimi3h.ambilight_9.9.9_arm.ipk', ipkHash: { sha256: sha }, ipkSize: 5 };
let fails = 0;
function check(name, cond) {
  console.log((cond ? 'ok   ' : 'FAIL ') + 'feed: ' + name);
  if (!cond) fails++;
}

let r = run(good, '0.2.0');
check('newer version is offered', r.code === 0 && r.out.newer === true && r.out.latest === '9.9.9' && r.out.sha256 === sha);
r = run(good, '9.9.9');
check('same version is not newer', r.code === 0 && r.out.newer === false);
r = run(good, '10.0.0');
check('older feed is not newer (numeric compare)', r.code === 0 && r.out.newer === false);
r = run(Object.assign({}, good, { ipkUrl: 'https://example.com/x.ipk' }), '0.2.0');
check('IPK from another site is rejected', r.code !== 0 && r.out.ok === false);
r = run(Object.assign({}, good, { ipkUrl: base + 'x.ipk?a=$(reboot)' }), '0.2.0');
check('IPK URL with shell characters is rejected', r.code !== 0 && r.out.ok === false);
r = run(Object.assign({}, good, { ipkHash: { sha256: 'xyz' } }), '0.2.0');
check('bad checksum is rejected', r.code !== 0 && r.out.ok === false);
r = run(Object.assign({}, good, { version: '9.9.9"; rm -rf /' }), '0.2.0');
check('bad version string is rejected', r.code !== 0 && r.out.ok === false);
r = run(good, '0.2.0', 'some.other.app');
check('feed without our package is rejected', r.code !== 0 && r.out.ok === false);

fs.rmSync(dir, { recursive: true, force: true });
process.exit(fails ? 1 : 0);
