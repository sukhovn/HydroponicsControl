const char DASHBOARD_HTML[] PROGMEM = R"==(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Hydro Tower</title>
<link href="https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Exo+2:wght@300;400;600&display=swap" rel="stylesheet">
<style>
  :root {
    --bg:       #0a0f0a;
    --panel:    #0f1a0f;
    --border:   #1a3a1a;
    --green:    #00e676;
    --green-dim:#004d25;
    --amber:    #ffab00;
    --amber-dim:#3d2800;
    --red:      #ff1744;
    --red-dim:  #3d0010;
    --text:     #c8e6c9;
    --muted:    #4a6a4a;
    --mono:     'Share Tech Mono', monospace;
    --sans:     'Exo 2', sans-serif;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text); font-family: var(--sans);
    min-height: 100vh; display: flex; flex-direction: column; align-items: center;
    padding: 24px 16px;
    background-image:
      repeating-linear-gradient(0deg, transparent, transparent 39px, var(--border) 39px, var(--border) 40px),
      repeating-linear-gradient(90deg, transparent, transparent 39px, var(--border) 39px, var(--border) 40px);
    background-size: 40px 40px;
  }
  header {
    width: 100%; max-width: 480px; display: flex; align-items: center; gap: 12px; margin-bottom: 32px;
  }
  .logo {
    width: 36px; height: 36px; border: 2px solid var(--green); border-radius: 50%;
    display: flex; align-items: center; justify-content: center;
    font-size: 18px; box-shadow: 0 0 12px var(--green); flex-shrink: 0;
  }
  h1 { font-family: var(--mono); font-size: 1.1rem; letter-spacing: 0.15em; color: var(--green); text-shadow: 0 0 8px var(--green); }
  .status-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); margin-left: auto; transition: background 0.3s, box-shadow 0.3s; }
  .status-dot.online { background: var(--green); box-shadow: 0 0 8px var(--green); animation: pulse 2s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.4; } }
  .card {
    width: 100%; max-width: 480px; background: var(--panel); border: 1px solid var(--border);
    border-radius: 4px; padding: 20px; margin-bottom: 16px; position: relative; overflow: hidden;
    animation: fadeIn 0.4s ease both;
  }
  .card::before { content: ''; position: absolute; top: 0; left: 0; width: 3px; height: 100%; background: var(--green); box-shadow: 0 0 8px var(--green); }
  @keyframes fadeIn { from { opacity: 0; transform: translateY(8px); } to { opacity: 1; transform: translateY(0); } }
  .card:nth-child(2) { animation-delay: 0.05s; }
  .card:nth-child(3) { animation-delay: 0.10s; }
  .card:nth-child(4) { animation-delay: 0.15s; }
  .card-label { font-family: var(--mono); font-size: 0.65rem; letter-spacing: 0.2em; color: var(--muted); text-transform: uppercase; margin-bottom: 14px; }
  .input-row { display: flex; gap: 8px; }
  input[type="password"] {
    flex: 1; background: var(--bg); border: 1px solid var(--border); border-radius: 2px;
    color: var(--green); font-family: var(--mono); font-size: 0.85rem; padding: 10px 12px;
    outline: none; transition: border-color 0.2s;
  }
  input:focus { border-color: var(--green); }
  input::placeholder { color: var(--muted); }
  .btn {
    font-family: var(--mono); font-size: 0.75rem; letter-spacing: 0.1em; border: 1px solid;
    border-radius: 2px; padding: 10px 16px; cursor: pointer; background: transparent;
    transition: background 0.15s, box-shadow 0.15s, transform 0.1s; white-space: nowrap;
  }
  .btn:active { transform: scale(0.97); }
  .btn-primary { color: var(--green); border-color: var(--green); }
  .btn-primary:hover { background: var(--green-dim); box-shadow: 0 0 12px var(--green-dim); }
  .btn-on { color: var(--green); border-color: var(--green); }
  .btn-on:hover { background: var(--green-dim); }
  .btn-on.active { background: var(--green-dim); box-shadow: 0 0 10px var(--green); }
  .btn-off { color: var(--red); border-color: var(--red); }
  .btn-off:hover { background: var(--red-dim); }
  .btn-off.active { background: var(--red-dim); box-shadow: 0 0 10px var(--red); }
  .btn-amber { color: var(--amber); border-color: var(--amber); }
  .btn-amber:hover { background: var(--amber-dim); }
  .btn-amber.active { background: var(--amber-dim); box-shadow: 0 0 10px var(--amber); }
  .control-row { display: flex; align-items: center; gap: 10px; margin-bottom: 10px; }
  .control-row:last-child { margin-bottom: 0; }
  .control-label { font-family: var(--mono); font-size: 0.8rem; color: var(--text); width: 48px; flex-shrink: 0; }
  .btn-pair { display: flex; gap: 8px; flex: 1; }
  .btn-pair .btn { flex: 1; }
  .save-row { display: flex; align-items: center; gap: 8px; margin-top: 10px; }
  .save-row label { font-family: var(--mono); font-size: 0.7rem; color: var(--muted); cursor: pointer; user-select: none; }
  input[type="checkbox"] { accent-color: var(--green); width: 14px; height: 14px; cursor: pointer; }
  #toast {
    position: fixed; bottom: 24px; left: 50%;
    transform: translateX(-50%) translateY(60px);
    background: var(--panel); border: 1px solid var(--green); color: var(--green);
    font-family: var(--mono); font-size: 0.75rem; letter-spacing: 0.1em;
    padding: 10px 20px; border-radius: 2px; transition: transform 0.3s ease;
    pointer-events: none; white-space: nowrap; box-shadow: 0 0 16px var(--green-dim); z-index: 100;
  }
  #toast.show { transform: translateX(-50%) translateY(0); }
  #toast.error { border-color: var(--red); color: var(--red); box-shadow: 0 0 16px var(--red-dim); }
  .hidden { display: none !important; }
</style>
</head>
<body>

<header>
  <div class="logo">🌿</div>
  <h1>HYDRO_TOWER</h1>
  <div class="status-dot" id="status-dot"></div>
</header>

<!-- Login -->
<div class="card" id="login-card">
  <div class="card-label">// authentication</div>
  <div class="input-row">
    <input type="password" id="api-key-input" placeholder="enter api key" autocomplete="off" />
    <button class="btn btn-primary" onclick="login()">CONNECT</button>
  </div>
  <div class="save-row">
    <input type="checkbox" id="save-key" />
    <label for="save-key">remember api key on this device</label>
  </div>
</div>

<!-- Controls -->
<div id="controls" class="hidden">

  <div class="card">
    <div class="card-label">// pump</div>
    <div class="control-row">
      <span class="control-label">PUMP</span>
      <div class="btn-pair">
        <button class="btn btn-on"  id="btn-pump-on"  onclick="sendManual({manual:true, pump:true})">ON</button>
        <button class="btn btn-off" id="btn-pump-off" onclick="sendManual({manual:true, pump:false})">OFF</button>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-label">// light</div>
    <div class="control-row">
      <span class="control-label">LIGHT</span>
      <div class="btn-pair">
        <button class="btn btn-on"  id="btn-light-on"  onclick="sendManual({manual:true, light:true})">ON</button>
        <button class="btn btn-off" id="btn-light-off" onclick="sendManual({manual:true, light:false})">OFF</button>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="card-label">// schedule</div>
    <div class="control-row">
      <span class="control-label" style="width:auto;flex:1;font-size:0.72rem;color:var(--muted);">return to automatic cycle</span>
      <button class="btn btn-amber" id="btn-resume" onclick="sendManual({manual:false})">RESUME</button>
    </div>
  </div>

</div>

<div id="toast"></div>

<script>
  let apiKey = '';

  // ── Restore saved key ──────────────────────────────────────
  const savedKey = localStorage.getItem('hydro_api_key');
  if (savedKey) {
    document.getElementById('api-key-input').value = savedKey;
    document.getElementById('save-key').checked = true;
  }

  document.getElementById('api-key-input').addEventListener('keydown', e => {
    if (e.key === 'Enter') login();
  });

  // ── Login ──────────────────────────────────────────────────
  async function login() {
    const input   = document.getElementById('api-key-input');
    const saveBox = document.getElementById('save-key');
    const key     = input.value.trim();
    if (!key) { toast('enter api key', true); return; }

    try {
      const res = await fetch('/api/control', { headers: { 'X-API-Key': key } });
      if (res.status === 401) { toast('invalid api key', true); return; }
      if (!res.ok)            { toast('connection error', true); return; }

      apiKey = key;

      if (saveBox.checked) {
        localStorage.setItem('hydro_api_key', key);
      } else {
        localStorage.removeItem('hydro_api_key');
      }

      input.value = '';
      document.getElementById('login-card').classList.add('hidden');
      document.getElementById('controls').classList.remove('hidden');
      document.getElementById('status-dot').classList.add('online');
      toast('connected');
    } catch(e) {
      toast('could not reach controller', true);
    }
  }

  // ── Single handler for all manual commands ─────────────────
  async function sendManual(body) {
    try {
      const res = await fetch('/api/manual', {
        method:  'POST',
        headers: { 'X-API-Key': apiKey, 'Content-Type': 'application/json' },
        body:    JSON.stringify(body)
      });

      if (res.status === 401) { toast('unauthorized', true);  return; }
      if (!res.ok)            { toast('command failed', true); return; }

      if (!body.manual) {
        // Resume schedule — clear all active states, briefly highlight button
        ['btn-pump-on','btn-pump-off','btn-light-on','btn-light-off'].forEach(clearActive);
        setActive('btn-resume', true);
        setTimeout(() => clearActive('btn-resume'), 800);
        toast('schedule resumed');
        return;
      }

      if ('pump' in body) {
        setActive('btn-pump-on',  body.pump);
        setActive('btn-pump-off', !body.pump);
        toast('pump ' + (body.pump ? 'on' : 'off'));
      }
      if ('light' in body) {
        setActive('btn-light-on',  body.light);
        setActive('btn-light-off', !body.light);
        toast('light ' + (body.light ? 'on' : 'off'));
      }

    } catch(e) {
      toast('could not reach controller', true);
    }
  }

  // ── UI helpers ─────────────────────────────────────────────
  function setActive(id, active) { document.getElementById(id)?.classList.toggle('active', active); }
  function clearActive(id)       { document.getElementById(id)?.classList.remove('active'); }

  let toastTimer;
  function toast(msg, isError = false) {
    const el = document.getElementById('toast');
    el.textContent = '> ' + msg;
    el.className   = 'show' + (isError ? ' error' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.className = ''; }, 2500);
  }
</script>
</body>
</html>
)==";