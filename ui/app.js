// Ambilight settings app (webOS web app, 1920x1080, remote + pointer).
//
// Talks to the service only through `ambilightctl` on the TV, run via Homebrew
// Channel's public `exec` method. All values are validated here and again by
// ambilightctl; only fixed sub-commands with safe characters are ever sent.
// Updates are installed through Homebrew Channel's own `install` method (it downloads
// the IPK and checks its SHA-256); the service then re-applies its permissions and
// restarts itself.
(function () {
  'use strict';

  var APP_ID = 'io.github.dimi3h.ambilight';
  var CTL = '/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service/ambilightctl';
  var HB = 'luna://org.webosbrew.hbchannel.service/';
  var RELEASES = 'https://github.com/Dimi3h/ambilight/releases/download/';
  var POLL_MS = 2000;
  var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACK: 461, ESC: 27 };
  var onTv = typeof PalmServiceBridge !== 'undefined';

  // ---------------------------------------------------------------- backend
  var pending = []; // keep bridges alive until they are done
  function lunaSub(uri, params, onMsg) {
    var bridge = new PalmServiceBridge();
    pending.push(bridge);
    var done = function () { var i = pending.indexOf(bridge); if (i >= 0) pending.splice(i, 1); };
    bridge.onservicecallback = function (msg) {
      var r;
      try { r = JSON.parse(msg); } catch (e) { done(); onMsg(null, e); return; }
      if (onMsg(r) === true) done();
    };
    bridge.call(uri, JSON.stringify(params || {}));
  }
  function luna(uri, params) {
    return new Promise(function (resolve, reject) {
      lunaSub(uri, params, function (r, err) { if (err) reject(err); else resolve(r); return true; });
    });
  }
  // Homebrew Channel reports a non-zero exit as returnValue:false with "Command failed: …";
  // ambilightctl then still explains itself as JSON on stdout, which is what we want to show.
  function exec(cmd) {
    return luna(HB + 'exec', { command: cmd }).then(function (r) {
      var out = r.stdoutString || '';
      if (!r.returnValue && !/^\s*\{/.test(out)) throw new Error((r.stderrString || '').trim() || 'opdracht mislukt');
      return out;
    });
  }
  var ERRORS = { // ambilightctl's messages, for the viewer
    'feed not reachable': 'GitHub is niet bereikbaar',
    'package not in feed': 'de updatelijst is onvolledig',
    'invalid host': 'ongeldig adres', 'invalid port': 'ongeldige poort',
    'invalid priority': 'ongeldige prioriteit', 'invalid gain': 'ongeldige helderheid'
  };
  var ctl = onTv ? function (args) { return exec(CTL + ' ' + args); } : mockCtl();

  // Install an IPK through Homebrew Channel; onStatus gets "Downloaden… 40%" etc.
  function hbInstall(url, sha256, onStatus) {
    if (!onTv) return mockInstall(onStatus);
    return new Promise(function (resolve, reject) {
      lunaSub(HB + 'install', { ipkUrl: url, ipkHash: sha256, id: APP_ID, subscribe: true }, function (r, err) {
        if (err || !r) { reject(err || new Error('geen antwoord')); return true; }
        if (r.returnValue === false) { reject(new Error(r.errorText || 'installatie mislukt')); return true; }
        if (r.statusText) onStatus(dutchStatus(r.statusText) + (r.progress ? ' ' + Math.round(r.progress) + '%' : ''));
        if (r.finished) { resolve(); return true; }
        return false;
      });
    });
  }
  function dutchStatus(s) {
    return ({ 'Downloading…': 'Downloaden…', 'Verifying…': 'Controleren…', 'Installing…': 'Installeren…',
      'Finished.': 'Klaar' })[s] || s;
  }

  // Desktop preview without a TV: an in-memory stand-in for ambilightctl.
  function mockCtl() {
    var s = { running: true, paused: false, autostart: true, disabled: null, boot_count: 0, letterbox: 5,
      version: '0.2.0',
      update: { checked: Date.now() / 1000 | 0, current: '0.2.0', latest: '0.2.1', newer: true,
        ipkUrl: RELEASES + 'v0.2.1/io.github.dimi3h.ambilight_0.2.1_arm.ipk', sha256: new Array(65).join('a') },
      settings: { host: '192.168.129.249', port: 19444, priority: 150, gain: 1.0, letterbox: true, ui_blend: true, bluekey: false } };
    return function (args) {
      var a = args.split(' ');
      var ok = function (x) { return Promise.resolve(JSON.stringify(x || { ok: true }) + '\n'); };
      if (a[0] === 'stop') { s.running = false; s.disabled = 'stopped by ambilightctl'; return ok(); }
      if (a[0] === 'start' || a[0] === 'setup') { s.running = true; s.paused = false; s.disabled = null; s.autostart = true; return ok(); }
      if (a[0] === 'pause') { s.paused = true; return ok(); }
      if (a[0] === 'resume') { s.paused = false; return ok(); }
      if (a[0] === 'check-update') { s.update.checked = Date.now() / 1000 | 0; return ok(s.update); }
      if (a[0] === 'set') {
        a.slice(1).forEach(function (kv) {
          var k = kv.split('=')[0], v = kv.split('=')[1];
          if (k === 'letterbox' || k === 'uiblend' || k === 'bluekey') s.settings[k === 'uiblend' ? 'ui_blend' : k] = v === 'on';
          else s.settings[k] = k === 'host' ? v : Number(v);
        });
        return ok();
      }
      var st = s.running ? { t: Date.now() / 1000 | 0, version: s.version, hist: 'open', fps: s.paused ? 0 : 20, reopens: 0,
        letterbox: s.settings.letterbox ? s.letterbox : 0, hyperhdr: true, paused: s.paused } : null;
      return Promise.resolve(JSON.stringify({ running: s.running, supervisor: s.running, autostart: s.autostart,
        paused: s.paused, disabled: s.disabled, boot_count: s.boot_count, version: s.version, settings: s.settings,
        update: s.update, status: st,
        log: ['[85.578] histogram opened', '[145.584] frames=1199 holds=0 reopens=0 hist=open'] }));
    };
  }
  function mockInstall(onStatus) {
    return new Promise(function (resolve) {
      var p = 0;
      var t = setInterval(function () {
        p += 25;
        onStatus(p < 100 ? 'Downloaden… ' + p + '%' : 'Installeren…');
        if (p >= 125) { clearInterval(t); resolve(); }
      }, 400);
    });
  }

  // ---------------------------------------------------------------- validation (mirrors ambilightctl)
  var valid = {
    host: function (v) { return /^[A-Za-z0-9.-]{1,63}$/.test(v); },
    port: function (v) { return /^\d+$/.test(v) && +v >= 1 && +v <= 65535; },
    priority: function (v) { return /^\d+$/.test(v) && +v >= 1 && +v <= 253; }
  };
  function cmpVersion(a, b) {
    var pa = String(a).split('.'), pb = String(b).split('.');
    for (var i = 0; i < 3; i++) if ((+pa[i] || 0) !== (+pb[i] || 0)) return (+pa[i] || 0) - (+pb[i] || 0);
    return 0;
  }
  // an update newer than what is installed right now (update.json may predate an update)
  function validUpdate(u) {
    return !!u && /^\d+\.\d+\.\d+$/.test(u.latest) && /^[0-9a-f]{64}$/.test(u.sha256) &&
      typeof u.ipkUrl === 'string' && u.ipkUrl.indexOf(RELEASES) === 0 &&
      cmpVersion(u.latest, (data && data.version) || u.current || '0.0.0') > 0;
  }

  // ---------------------------------------------------------------- state + rendering
  var $ = function (id) { return document.getElementById(id); };
  var data = null, busy = false, editing = null, gainPct = null, gainTimer = null;
  var installing = null, setupTried = false;

  function refresh() {
    if (busy || document.hidden) return Promise.resolve();
    return ctl('json-status').then(function (out) {
      data = JSON.parse(out);
      render();
      firstRunSetup();
    }).catch(function (e) { toast('Status niet beschikbaar: ' + e.message, true); });
  }

  // Installed through Homebrew Channel: nothing has run yet. Finish the setup once.
  function firstRunSetup() {
    if (setupTried || !data || data.autostart || data.disabled) return;
    setupTried = true;
    toast('Ambilight wordt ingesteld…');
    run('setup', 'Ambilight is ingesteld en gestart');
  }

  function render() {
    if (!data) return;
    var st = data.status, s = data.settings, guard = data.disabled && !/stopped by ambilightctl/.test(data.disabled);
    var on = data.running && !data.paused;
    var dot = $('dot'), text;
    if (!data.running) { dot.className = 'dot off'; text = 'Uit'; }
    else if (data.paused) { dot.className = 'dot off'; text = 'Uit (leds uit)'; }
    else if (!st) { dot.className = 'dot wait'; text = 'Start…'; }
    else if (st.hist === 'open') { dot.className = 'dot on'; text = 'Actief'; }
    else { dot.className = 'dot wait'; text = 'Wacht op beeld'; }
    $('stateText').textContent = text;
    $('sSource').textContent = !data.running ? '–' : !st ? 'Opstarten' :
      st.hist === 'open' ? 'Histogram (16 zones)' : 'Wacht op onbeveiligd beeld';
    $('sHyper').textContent = s.host + ':' + s.port + (st && on ? (st.hyperhdr ? ' · verbonden' : ' · niet bereikbaar') : '');
    $('sBars').textContent = !s.letterbox ? 'Herkenning uit' : !st || !on ? '–' :
      st.letterbox ? st.letterbox + ' rijen (≈' + Math.round(st.letterbox * 100 / 36) + '% boven en onder)' : 'Geen';
    $('sFps').textContent = st && on ? Math.round(st.fps) : '–';
    $('sAuto').textContent = data.autostart ? 'Aan' + (data.boot_count ? ' · opstartteller ' + data.boot_count + '/3' : '') : 'Niet ingesteld';
    $('sVersion').textContent = data.version || '–';
    var reason = $('reason');
    reason.style.display = guard ? 'block' : 'none';
    if (guard) reason.textContent = 'Uitgeschakeld door een noodrem: ' + data.disabled + '. Zet Ambilight aan om opnieuw te proberen.';
    $('ver').textContent = data.version ? 'v' + data.version : '';
    $('log').textContent = (data.log || []).join('\n');

    setToggle('vPower', on);
    $('powerHint').textContent = s.bluekey ? 'Of met de blauwe knop · blijft zo na herstart' : 'Uit blijft uit, ook na herstart';
    setToggle('vBluekey', s.bluekey);
    setToggle('vLetterbox', s.letterbox);
    setToggle('vUiblend', s.ui_blend);
    renderUpdate();
    if (editing !== 'host') $('vHost').value = s.host;
    if (editing !== 'port') $('vPort').value = s.port;
    if (editing !== 'priority') $('vPriority').value = s.priority;
    if (gainPct === null || !gainTimer) gainPct = Math.round(s.gain * 100);
    $('vGain').textContent = gainPct + '%';
  }
  function renderUpdate() {
    var u = data && data.update, hint = $('updateHint'), btn = $('vUpdate');
    if (installing) { hint.textContent = installing; btn.textContent = 'Bezig…'; btn.className = 'action'; return; }
    if (validUpdate(u)) {
      hint.textContent = 'Versie ' + u.latest + ' is beschikbaar (nu ' + (data.version || u.current) + ')';
      btn.textContent = 'Installeren'; btn.className = 'action new';
    } else {
      var when = u && u.checked ? new Date(u.checked * 1000) : null;
      hint.textContent = when ? 'Up-to-date · gecontroleerd ' + when.toLocaleDateString('nl-BE') + ' ' +
        when.toLocaleTimeString('nl-BE', { hour: '2-digit', minute: '2-digit' }) : 'Nog niet gecontroleerd (gebeurt dagelijks)';
      btn.textContent = 'Controleren'; btn.className = 'action';
    }
  }
  function setToggle(id, on) {
    var el = $(id);
    el.textContent = on ? 'Aan' : 'Uit';
    el.className = 'toggle' + (on ? ' on' : '');
  }

  var toastTimer = null;
  function toast(msg, err) {
    var t = $('toast');
    t.textContent = msg;
    t.className = 'toast show' + (err ? ' err' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = 'toast'; }, 3000);
  }

  // ---------------------------------------------------------------- actions
  function run(args, okMsg) {
    busy = true;
    return ctl(args).then(function (out) {
      var r = null;
      if (/^\s*\{/.test(out)) {
        r = JSON.parse(out);
        if (r.ok === false) throw new Error(r.error || 'mislukt');
      }
      if (okMsg) toast(typeof okMsg === 'function' ? okMsg(r) : okMsg);
      return r;
    }).catch(function (e) { toast('Fout: ' + (ERRORS[e.message] || e.message), true); })
      .then(function (r) { busy = false; return refresh().then(function () { return r; }); });
  }
  function setValue(key, value, msg) { return run('set ' + key + '=' + value, msg || 'Opgeslagen'); }

  function activate(key) {
    if (!data) return;
    var s = data.settings;
    if (key === 'power') {
      if (!data.running) run('start', 'Ambilight aan');
      else if (data.paused) run('resume', 'Ambilight aan');
      else run('pause', 'Ambilight uit');
    } else if (key === 'bluekey') setValue('bluekey', s.bluekey ? 'off' : 'on');
    else if (key === 'letterbox') setValue('letterbox', s.letterbox ? 'off' : 'on');
    else if (key === 'uiblend') setValue('uiblend', s.ui_blend ? 'off' : 'on');
    else if (key === 'update') update();
    else if (key === 'host' || key === 'port' || key === 'priority') startEdit(key);
    else if (key === 'gain') { /* left/right adjusts */ }
  }

  function update() {
    if (installing) return;
    var u = data.update;
    if (!validUpdate(u)) {
      run('check-update', function (r) {
        return !r ? 'Controle mislukt' : r.newer ? 'Versie ' + r.latest + ' is beschikbaar' : 'Je hebt de nieuwste versie';
      });
      return;
    }
    installing = 'Downloaden…';
    renderUpdate();
    hbInstall(u.ipkUrl, u.sha256, function (status) { installing = status; renderUpdate(); }).then(function () {
      installing = null;
      toast('Versie ' + u.latest + ' geïnstalleerd · Ambilight herstart vanzelf');
      // this page is still the old version: load the new one once the service is back
      setTimeout(function () { window.location.reload(); }, 9000);
    }).catch(function (e) {
      installing = null;
      toast('Update mislukt: ' + e.message, true);
      renderUpdate();
    });
  }

  function stepGain(dir) {
    if (gainPct === null) return;
    gainPct = Math.max(10, Math.min(300, gainPct + dir * 10));
    $('vGain').textContent = gainPct + '%';
    clearTimeout(gainTimer);
    gainTimer = setTimeout(function () {
      gainTimer = null;
      setValue('gain', (gainPct / 100).toFixed(2));
    }, 400);
  }

  var inputs = { host: 'vHost', port: 'vPort', priority: 'vPriority' };
  function startEdit(key) {
    editing = key;
    var el = $(inputs[key]);
    el.focus();
    el.select && el.select();
  }
  function endEdit(commit) {
    var key = editing;
    if (!key) return;
    var el = $(inputs[key]), v = String(el.value).trim();
    editing = null;
    el.blur();
    if (!commit || String(data.settings[key]) === v) { render(); return; }
    if (!valid[key](v)) { toast('Ongeldige waarde', true); render(); return; }
    setValue(key, v);
  }

  // ---------------------------------------------------------------- navigation
  var rows = Array.prototype.slice.call(document.querySelectorAll('.row'));
  var focus = 0;
  function setFocus(i) {
    focus = Math.max(0, Math.min(rows.length - 1, i));
    rows.forEach(function (r, j) { r.classList.toggle('focus', j === focus); });
  }
  function key() { return rows[focus].getAttribute('data-key'); }

  document.addEventListener('keydown', function (e) {
    var k = e.keyCode;
    if (editing) {
      if (k === KEY.ENTER) { e.preventDefault(); endEdit(true); }
      else if (k === KEY.BACK || k === KEY.ESC) { e.preventDefault(); endEdit(false); }
      else if (k === KEY.UP || k === KEY.DOWN) { e.preventDefault(); endEdit(true); setFocus(focus + (k === KEY.UP ? -1 : 1)); }
      return;
    }
    if (k === KEY.UP) setFocus(focus - 1);
    else if (k === KEY.DOWN) setFocus(focus + 1);
    else if (k === KEY.ENTER) activate(key());
    else if (k === KEY.LEFT || k === KEY.RIGHT) {
      if (key() === 'gain') stepGain(k === KEY.LEFT ? -1 : 1);
      else if (/^(power|bluekey|letterbox|uiblend)$/.test(key())) activate(key());
    } else if (k === KEY.BACK || k === KEY.ESC) {
      if (window.PalmSystem && window.PalmSystem.platformBack) window.PalmSystem.platformBack();
      else window.close();
    } else return;
    e.preventDefault();
  });
  rows.forEach(function (r, i) {
    r.addEventListener('mouseenter', function () { if (!editing) setFocus(i); });
    r.addEventListener('click', function () { setFocus(i); activate(key()); });
  });
  Object.keys(inputs).forEach(function (k) {
    $(inputs[k]).addEventListener('blur', function () { if (editing === k) endEdit(true); });
  });

  document.addEventListener('visibilitychange', function () { if (!document.hidden) refresh(); });
  setFocus(0);
  refresh();
  setInterval(refresh, POLL_MS);
})();
