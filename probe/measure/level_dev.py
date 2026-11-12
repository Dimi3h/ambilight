#!/usr/bin/env python3
"""Development copy of the level app's page (osd/index.html), for measurements on the TV.

It adds two launch parameters to the page:
  {"fill":"#rrggbb"}        paint the whole screen in the top UI layer; {"fill":null} ends it
  {"subs":"yellow"|"white"} show two lines of subtitle-like text; {"subs":null} ends it
Both end by themselves after 20 s. Usage, from the repository root:

  python3 probe/measure/level_dev.py > level-dev.html
  ssh root@TV 'cat > /media/developer/apps/usr/palm/applications/io.github.dimi3h.ambilight.osd/index.html' < level-dev.html

Reinstalling the package (or the level app) puts the released page back.
"""
import os
import sys

src = open(os.path.join(os.path.dirname(__file__), '..', '..', 'osd', 'index.html'), encoding='utf-8').read()

ELEMENTS = '''<div id="fillscreen" style="position:absolute;left:0;top:0;width:100%;height:100%;display:none"></div>
<div id="subs" style="position:absolute;left:0;right:0;bottom:44px;text-align:center;display:none;font-family:'LG Smart UI',sans-serif;font-weight:600;font-size:46px;line-height:60px"></div>
<div id="box">'''

HOOKS = '''    if (p && ('fill' in p || 'subs' in p)) {
      var fs = document.getElementById('fillscreen'), sb = document.getElementById('subs');
      clearTimeout(timer);
      fs.style.display = sb.style.display = 'none';
      if (p.fill) { fs.style.background = p.fill; fs.style.display = 'block'; }
      if (p.subs) {
        sb.innerHTML = 'Elke geboorte is<br>van groot belang voor de groep.';
        if (p.subs === 'yellow') {
          sb.style.color = '#ffff00'; sb.style.fontWeight = 600;
          sb.style.textShadow = '-3px 0 #000, 3px 0 #000, 0 -3px #000, 0 3px #000, 2px 2px #000, -2px -2px #000, 2px -2px #000, -2px 2px #000';
        } else {
          sb.style.color = '#f0f0f0'; sb.style.fontWeight = 400; sb.style.textShadow = '0 0 4px rgba(0,0,0,0.6)';
        }
        sb.style.display = 'block';
      }
      if (p.fill || p.subs) {
        if (!counting) { counting = true; frames = 0; requestAnimationFrame(count); }
        timer = setTimeout(function () { fs.style.display = sb.style.display = 'none'; timer = setTimeout(leave, FADE_MS); }, 20000);
      } else {
        timer = setTimeout(leave, FADE_MS);
      }
      return;
    }
'''

ANCHOR_ELEMENT = '<div id="box">'
ANCHOR_SHOW = "    var level = p && typeof p.pct === 'number'"
if src.count(ANCHOR_ELEMENT) != 1 or src.count(ANCHOR_SHOW) != 1:
    sys.exit('osd/index.html changed: the places where the test hooks go were not found')
src = src.replace(ANCHOR_ELEMENT, ELEMENTS)
src = src.replace(ANCHOR_SHOW, HOOKS + ANCHOR_SHOW)
sys.stdout.write(src)
