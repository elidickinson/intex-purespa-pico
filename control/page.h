#pragma once

// The web page; it reads /status and /log every 2 s and sends requests to /set and /schedule.
static const char PAGE[] = R"html(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Spa</title>
<style>
body{font-family:system-ui,sans-serif;max-width:34em;margin:1em auto;padding:0 1em}
td{padding:.15em 1em .15em 0}
button,select,input{font-size:1em;margin:.2em 0}
textarea{width:100%;font-family:monospace;font-size:.9em}
pre{white-space:pre-wrap;font-size:.85em}
.warn{color:#b00}
small{color:#555}
</style></head><body>
<h1>Spa</h1>
<p id=warn class=warn></p>
<table id=st></table>
<h2>Controls</h2>
<p><button id=b-power onclick="toggle('power')">Power</button>
<button id=b-heater onclick="toggle('heater')">Heater</button>
<button id=b-bubbles onclick="toggle('bubbles')">Bubbles</button></p>
<p><input id=temp type=number min=10 max=104 style="width:5em"> <button onclick="set('temp='+temp.value)">Set temperature</button></p>
<p><select id=salt><option value=off>Off<option>3<option>5<option>8</select> <button onclick="set('salt='+salt.value)">Set salt hours</button></p>
<p>Test: <select id=btn><option>bubbles<option>filter<option>heater<option>power<option>up<option>down<option>salt<option>unit</select>
<button onclick="set('press='+btn.value)">Press once</button></p>
<p id=msg></p>
<h2>Schedule</h2>
<textarea id=sched rows=8></textarea>
<p><button onclick=save()>Save schedule</button> <span id=smsg></span></p>
<p><small>One entry per line: days, time (24-hour, New York time), then settings.
Days are daily, weekdays, weekends, or a list such as mon,wed,fri. # starts a comment. Example:<br>
<code>fri 15:00 power=on heater=on temp=102</code><br><code>daily 02:00 salt=3</code></small></p>
<h2>Log</h2>
<pre id=lg></pre>
<script>
let s = {};
const on = b => b ? 'on' : 'off';
async function post(url, body) {
  const r = await fetch(url, {method: 'POST', body});
  return await r.text();
}
async function set(q) {
  msg.textContent = await post('/set?' + q);
  refresh();
}
function toggle(k) { set(k + '=' + (s[k] ? 'off' : 'on')); }
async function save() { smsg.textContent = await post('/schedule', sched.value); }
async function refresh() {
  s = await (await fetch('/status')).json();
  const deg = v => v == null ? '?' : v + '\u00b0' + s.unit;
  const left = s.hours_left == null ? '' : ', ' + s.hours_left + ' h left';
  const rows = [
    ['Time', s.time],
    ['Water', deg(s.water)],
    ['Target', s.target == null ? 'not shown yet' : deg(s.target)],
    ['Power', on(s.power)],
    ['Heater', s.heater ? (s.heating ? 'heating' : 'on, at temperature') : 'off'],
    ['Bubbles', on(s.bubbles)],
    ['Filter', on(s.filter) + (s.filter && !s.salt ? left : '')],
    ['Salt', on(s.salt) + (s.salt ? left : '')],
    ['Error', s.error || 'none'],
    ['Display', '<code>' + s.display.replace(/ /g, '&nbsp;') + '</code>'],
    ['Doing', s.job ? s.job + (s.waiting ? ' (' + s.waiting + ' more waiting)' : '') : 'nothing'],
  ];
  st.innerHTML = rows.map(([k, v]) => '<tr><td>' + k + '<td>' + v).join('');
  warn.textContent = (s.bus ? '' : 'No data from the spa bus. ') +
    (s.read_only ? 'Read-only build: presses are logged, not sent to the spa.' : '');
  for (const k of ['power', 'heater', 'bubbles'])
    document.getElementById('b-' + k).textContent = k[0].toUpperCase() + k.slice(1) + ': turn ' + (s[k] ? 'off' : 'on');
  lg.textContent = await (await fetch('/log')).text();
}
fetch('/schedule').then(r => r.text()).then(t => sched.value = t);
refresh();
setInterval(refresh, 2000);
</script>
</body></html>
)html";
