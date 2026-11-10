// Ambilight settings app (webOS web app, 1920x1080, remote + pointer).
//
// Talks to the service only through `ambilightctl` on the TV, run via Homebrew
// Channel's public `exec` method. All values are validated here and again by
// ambilightctl; only fixed sub-commands with safe characters are ever sent.
(function () {
  'use strict';

  var CTL = '/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service/ambilightctl';
  var POLL_MS = 2000;
  var KEY = { LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, ENTER: 13, BACK: 461, ESC: 27 };

  // ---------------------------------------------------------------- backend
  var pending = []; // keep bridges alive until they answer
  function luna(uri, params) {
    return new Promise(function (resolve, reject) {
      var bridge = new PalmServiceBridge();
      pending.push(bridge);
      bridge.onservicecallback = function (msg) {
        pending.splice(pending.indexOf(bridge), 1);
        try { resolve(JSON.parse(msg)); } catch (e) { reject(e); }
      };
      bridge.call(uri, JSON.stringify(params || {}));
    });
  }
  function exec(cmd) {
    return luna('luna://org.webosbrew.hbchannel.service/exec', { command: cmd }).then(function (r) {
      if (!r.returnValue) throw new Error(r.errorText || r.stderrString || 'exec failed');
      return r.stdoutString || '';
    });
  }
  var ctl = typeof PalmServiceBridge === 'undefined' ? mockCtl() : function (args) { return exec(CTL + ' ' + args); };

  // Desktop preview without a TV: an in-memory stand-in for ambilightctl.
  function mockCtl() {
    var s = { running: true, autostart: true, disabled: null, boot_count: 0, letterbox: 5,
      settings: { host: '192.168.129.249', port: 19444, priority: 150, gain: 1.0, letterbox: true, ui_blend: true } };
    return function (args) {
      var a = args.split(' ');
      if (a[0] === 'stop') { s.running = false; s.disabled = 'stopped by ambilightctl'; }
      if (a[0] === 'start') { s.running = true; s.disabled = null; }
      if (a[0] === 'set') {
        a.slice(1).forEach(function (kv) {
          var k = kv.split('=')[0], v = kv.split('=')[1];
          if (k === 'letterbox' || k === 'uiblend') s.settings[k === 'uiblend' ? 'ui_blend' : k] = v === 'on';
          else s.settings[k] = k === 'host' ? v : Number(v);
        });
        return Promise.resolve('{"ok":true}\n');
      }
      if (a[0] === 'start' || a[0] === 'stop') return Promise.resolve(a[0] === 'stop' ? 'stopped\n' : 'service: running\n');
      var st = s.running ? { t: Date.now() / 1000 | 0, version: 'preview', hist: 'open', fps: 20, reopens: 0,
        letterbox: s.settings.letterbox ? s.letterbox : 0, hyperhdr: true } : null;
      return Promise.resolve(JSON.stringify({ running: s.running, supervisor: s.running, autostart: s.autostart,
        disabled: s.disabled, boot_count: s.boot_count, settings: s.settings, status: st,
        log: ['[85.578] histogram opened', '[145.584] frames=1199 holds=0 reopens=0 hist=open'] }));
    };
  }

  // ---------------------------------------------------------------- validation (mirrors ambilightctl)
  var valid = {
    host: function (v) { return /^[A-Za-z0-9.-]{1,63}$/.test(v); },
    port: function (v) { return /^\d+$/.test(v) && +v >= 1 && +v <= 65535; },
    priority: function (v) { return /^\d+$/.test(v) && +v >= 1 && +v <= 253; }
  };

  // ---------------------------------------------------------------- state + rendering
  var $ = function (id) { return document.getElementById(id); };
  var data = null, busy = false, editing = null, gainPct = null, gainTimer = null;

  function refresh() {
    if (busy || document.hidden) return Promise.resolve();
    return ctl('json-status').then(function (out) {
      data = JSON.parse(out);
      render();
    }).catch(function (e) { toast('Status niet beschikbaar: ' + e.message, true); });
  }

  function render() {
    if (!data) return;
    var st = data.status, s = data.settings, guard = data.disabled && !/stopped by ambilightctl/.test(data.disabled);
    var dot = $('dot'), text;
    if (!data.running) { dot.className = 'dot off'; text = 'Uit'; }
    else if (!st) { dot.className = 'dot wait'; text = 'Start…'; }
    else if (st.hist === 'open') { dot.className = 'dot on'; text = 'Actief'; }
    else { dot.className = 'dot wait'; text = 'Wacht op beeld'; }
    $('stateText').textContent = text;
    $('sSource').textContent = !data.running ? '–' : !st ? 'Opstarten' :
      st.hist === 'open' ? 'Histogram (16 zones)' : 'Wacht op onbeveiligd beeld';
    $('sHyper').textContent = s.host + ':' + s.port + (st ? (st.hyperhdr ? ' · verbonden' : ' · niet bereikbaar') : '');
    $('sBars').textContent = !s.letterbox ? 'Herkenning uit' : !st ? '–' :
      st.letterbox ? st.letterbox + ' rijen (≈' + Math.round(st.letterbox * 100 / 36) + '% boven en onder)' : 'Geen';
    $('sFps').textContent = st ? Math.round(st.fps) : '–';
    $('sAuto').textContent = data.autostart ? 'Aan' + (data.boot_count ? ' · opstartteller ' + data.boot_count + '/3' : '') : 'Niet geïnstalleerd';
    var reason = $('reason');
    reason.style.display = guard ? 'block' : 'none';
    if (guard) reason.textContent = 'Uitgeschakeld door een noodrem: ' + data.disabled + '. Zet Ambilight aan om opnieuw te proberen.';
    $('ver').textContent = st && st.version ? 'v' + st.version : '';
    $('log').textContent = (data.log || []).join('\n');

    setToggle('vPower', data.running);
    setToggle('vLetterbox', s.letterbox);
    setToggle('vUiblend', s.ui_blend);
    if (editing !== 'host') $('vHost').value = s.host;
    if (editing !== 'port') $('vPort').value = s.port;
    if (editing !== 'priority') $('vPriority').value = s.priority;
    if (gainPct === null || !gainTimer) gainPct = Math.round(s.gain * 100);
    $('vGain').textContent = gainPct + '%';
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
    toastTimer = setTimeout(function () { t.className = 'toast'; }, 2500);
  }

  // ---------------------------------------------------------------- actions
  function run(args, okMsg) {
    busy = true;
    return ctl(args).then(function (out) {
      if (/^\s*\{/.test(out)) {
        var r = JSON.parse(out);
        if (!r.ok) throw new Error(r.error || 'mislukt');
      }
      if (okMsg) toast(okMsg);
    }).catch(function (e) { toast('Fout: ' + e.message, true); })
      .then(function () { busy = false; return refresh(); });
  }
  function setValue(key, value) { return run('set ' + key + '=' + value, 'Opgeslagen'); }

  function activate(key) {
    if (!data) return;
    var s = data.settings;
    if (key === 'power') run(data.running ? 'stop' : 'start', data.running ? 'Ambilight uit' : 'Ambilight aan');
    else if (key === 'letterbox') setValue('letterbox', s.letterbox ? 'off' : 'on');
    else if (key === 'uiblend') setValue('uiblend', s.ui_blend ? 'off' : 'on');
    else if (key === 'host' || key === 'port' || key === 'priority') startEdit(key);
    else if (key === 'gain') { /* left/right adjusts */ }
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
      else if (/^(power|letterbox|uiblend)$/.test(key())) activate(key());
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
