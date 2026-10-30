#pragma once

// The web page; it reads /status every 2 s (and /log while Details is open) and sends requests to
// /set and /schedule. It asks for the password once, keeps it in the browser's local storage and
// sends it as basic auth with every call.
static const char PAGE[] = R"html(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<meta name=color-scheme content="light dark">
<title>Spa</title>
<style>
body{font-family:system-ui,sans-serif;max-width:28em;margin:0 auto;padding:1em}
h1{font-size:1.4em;margin-bottom:.2em}
.dim,small,th{color:GrayText;font-weight:normal}
.warn{color:#d33}
table{border-collapse:collapse;width:100%;table-layout:fixed}
th,td{text-align:left;padding:.45em .5em .45em 0;border-bottom:1px solid #8883}
th:first-child{width:4.5em}
th:last-child{width:9em}
select,button{font:inherit;padding:.3em .5em}
details{margin:1em 0}
summary{cursor:pointer}
textarea{width:100%;box-sizing:border-box;font:.9em ui-monospace,monospace}
pre{white-space:pre-wrap;font-size:.8em}
</style></head><body>
<h1>Spa</h1>
<form id=login hidden onsubmit="return signin()">
<input name=username autocomplete=username value=spa hidden>
<input id=pw type=password autocomplete=current-password placeholder=Password> <button>Log in</button>
<p id=loginmsg class=warn></p>
</form>
<div id=main hidden>
<p id=info class=dim></p>
<p id=warn class=warn></p>
<table>
<tr><th><th>Now<th>
<tr><td>Water<td id=water><td>
<tr><td>Power<td id=power><td><button id=b-power onclick="toggle('power')"></button>
<tr><td>Heater<td id=heater><td><button id=b-heater onclick="toggle('heater')"></button>
<tr><td>Bubbles<td id=bubbles><td><button id=b-bubbles onclick="toggle('bubbles')"></button>
<tr><td>Target<td id=target><td><select id=temp></select> <button onclick="send('temp=' + temp.value)">Set</button>
<tr><td>Salt<td id=salt><td><select id=hours><option value=3>3 h<option value=5>5 h<option value=8>8 h<option value=off>Off</select>
<button onclick="send('salt=' + hours.value)">Set</button>
<tr><td>Filter<td id=filter><td>
</table>
<p>Last command: <span id=msg>none</span></p>
<details><summary>Schedule</summary>
<textarea id=sched rows=8></textarea>
<p><button onclick=save()>Save</button> <span id=smsg></span></p>
<p><small>One entry per line: days, time (24-hour), then settings. Days are daily, weekdays, weekends,
or a list such as mon,wed,fri. # starts a comment.<br>
<code>fri 15:00 power=on heater=on temp=102</code><br><code>daily 02:00 salt=3</code></small></p>
</details>
<details id=more><summary>Details</summary>
<p id=disp class=dim></p>
<p>Press once: <select id=btn><option>bubbles<option>filter<option>heater<option>power<option>up<option>down<option>salt<option>unit</select>
<button onclick="send('press=' + btn.value)">Press</button></p>
<pre id=lg></pre>
</details>
</div>
<script>
let s = {};
let shown;
let pass = localStorage.getItem('pass');
async function api(url, opts = {}) {
  const r = await fetch(url, {...opts, headers: {Authorization: 'Basic ' + btoa('spa:' + pass)}});
  if (r.status == 401) {
    if (pass)
      loginmsg.textContent = 'Wrong password.';
    localStorage.removeItem('pass');
    pass = null;
    main.hidden = true;
    login.hidden = false;
    throw new Error('password required');
  }
  return r;
}
function signin() {
  loginmsg.textContent = '';
  pass = pw.value;
  localStorage.setItem('pass', pass);
  start();
  return false;
}
function start() {
  login.hidden = true;
  main.hidden = false;
  api('/schedule').then(r => r.text()).then(t => sched.value = t);
  refresh();
}
async function send(q) {
  msg.textContent = (await (await api('/set?' + q, {method: 'POST'})).text()).trim();
  refresh();
}
function toggle(k) { send(k + '=' + (s[k] ? 'off' : 'on')); }
async function save() {
  smsg.textContent = (await (await api('/schedule', {method: 'POST', body: sched.value})).text()).trim();
}
async function refresh() {
  if (!pass)
    return;
  s = await (await api('/status')).json();
  const deg = v => v == null ? 'not known yet' : v + '°' + s.unit;
  const on = b => b ? 'on' : 'off';
  const left = s.hours_left == null ? '' : ', ' + s.hours_left + ' h left';
  info.textContent = s.time + ' · WiFi ' + s.rssi + ' dBm · up ' + (s.uptime / 3600).toFixed(1) + ' h · ' +
    s.dropped + ' frames dropped';
  warn.textContent = (s.bus ? '' : 'No data from the spa. ') + (s.error ? 'Panel shows ' + s.error + '.' : '');
  water.textContent = deg(s.water);
  power.textContent = on(s.power);
  heater.textContent = !s.heater ? 'off' : s.heater_suspended ? 'on, suspended while salt runs' : s.heating ? 'heating' : 'on, at temperature';
  bubbles.textContent = on(s.bubbles);
  for (const k of ['power', 'heater', 'bubbles'])
    document.getElementById('b-' + k).textContent = s[k] ? 'Turn off' : 'Turn on';
  target.textContent = deg(s.target);
  salt.textContent = on(s.salt) + (s.salt ? left : '');
  filter.textContent = on(s.filter) + (s.filter && !s.salt ? left : '');
  if (s.unit && !temp.length) {
    for (let t = s.unit == 'F' ? 104 : 40; t >= (s.unit == 'F' ? 50 : 10); t--)
      temp.add(new Option(t + '°' + s.unit, t));
    temp.value = s.target ?? '';
  }
  if (s.job)
    msg.textContent = s.job + ', working' + (s.waiting ? ' (' + s.waiting + ' more waiting)' : '') + '…';
  else if (s.results != shown && s.result)
    msg.textContent = s.result;
  shown = s.results;
  if (more.open) {
    disp.textContent = 'Display "' + s.display + '"';
    lg.textContent = await (await api('/log')).text();
  }
}
if (pass)
  start();
else
  login.hidden = false;
setInterval(refresh, 2000);
</script>
</body></html>
)html";
