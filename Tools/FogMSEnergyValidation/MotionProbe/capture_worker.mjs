// Native UE_MCP_Bridge editor capture only. Run externally before motion_probe.py.
// node capture_worker.mjs <motion_config.json>; no HighResShot or SceneCapture.
import fs from 'node:fs';
import path from 'node:path';
const config = JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''));
if (!/^[A-Za-z0-9_-]+$/.test(config.id) || !path.isAbsolute(config.evidence_root)) throw Error('Unsafe config');
const root = path.join(config.evidence_root, config.id);
const ws = new WebSocket('ws://127.0.0.1:9877');
let open = false, pending = null, serial = 0, stopped = false;
const started = Date.now();
function write(file, value) { fs.writeFileSync(file + '.tmp', JSON.stringify(value, null, 2)); fs.renameSync(file + '.tmp', file); }
function stop() { if (stopped) return; stopped = true; clearInterval(timer); ws.close(); }
ws.addEventListener('open', () => { open = true; });
ws.addEventListener('error', event => { console.error(String(event.error || event.message)); process.exitCode = 2; stop(); });
ws.addEventListener('message', event => {
  const result = JSON.parse(String(event.data));
  if (!pending || result.id !== pending.rpcId) return;
    write(path.join(root, 'capture_acks', String(pending.sequence).padStart(6, '0') + '.json'), { sequence: pending.sequence, received_utc: new Date().toISOString(), result });
  pending = null;
});
const timer = setInterval(() => {
  try {
    if (Date.now() - started > (config.timeout_seconds + 45) * 1000) throw Error('Capture worker timeout');
    if (!open || !fs.existsSync(root)) return;
    const heartbeat = path.join(root, 'capture_worker.json');
    if (!fs.existsSync(heartbeat)) write(heartbeat, { ready: true, capture_method: 'UE_MCP_Bridge.capture_screenshot', target: 'editor', started_utc: new Date().toISOString() });
    // Only immutable files cross processes: Windows readers can deny replacement.
    if (fs.existsSync(path.join(root, 'finished.json'))) { stop(); return; }
    const requestPath = path.join(root, 'capture_requests', String(serial + 1).padStart(6, '0') + '.json');
    if (pending || !fs.existsSync(requestPath)) return;
    const request = JSON.parse(fs.readFileSync(requestPath, 'utf8'));
    if (request.sequence <= serial) return;
    const destination = path.resolve(request.filename);
    if (path.dirname(destination).toLowerCase() !== path.resolve(root).toLowerCase() || !destination.endsWith('.png')) throw Error('Screenshot outside run directory');
    serial = request.sequence;
    pending = { sequence: serial, rpcId: serial };
    ws.send(JSON.stringify({ jsonrpc: '2.0', id: serial, method: 'capture_screenshot', params: { filename: destination, target: 'editor' } }));
  } catch (error) { console.error(error.stack); process.exitCode = 2; stop(); }
}, 5);
