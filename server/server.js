// Config sharing server for the Input Buffer mod. Node >= 18, no dependencies.
//   node server.js            (PORT=8787 DATA=./data.json ADMIN_TOKEN=secret TRUST_PROXY=1)
// Put it behind HTTPS (Caddy / nginx / Cloudflare tunnel) for real use.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const PORT = +process.env.PORT || 8787;
const DATA = process.env.DATA || path.join(__dirname, 'data.json');
const ADMIN = process.env.ADMIN_TOKEN || '';
const TRUST_PROXY = process.env.TRUST_PROXY === '1';
const MAX_BODY = 64 * 1024;
const PAGE = 20;

// ---- limits of the settings a config may contain (must match ConfigIO.hpp) ----
const BOOLS = ['enabled', 'showTraj', 'logOn', 'dualOn', 'bothSides', 'previewBoth', 'showPath', 'orbDetect',
  'dropFatal', 'twoPlayerOn', 'moveDetect', 'swapTwoP', 'waveAnglesOn', 'autoplay', 'autoLearn', 'autoKeep'];
const INTS = {
  previewLen: [10, 400], previewEvery: [1, 60], keepTicks: [0, 2000], lineWidth: [2, 40], pathHorizon: [20, 300],
  pathEvery: [1, 60], pathBudget: [200, 20000], trajOpacity: [10, 100], settle: [1, 30], airHorizon: [10, 300],
  robotHold: [1, 60], gainClick: [1, 60], minBudget: [50, 5000], noclipTicks: [0, 5000], failOpen: [1, 60],
  waveUp: [3000, 8000], waveDown: [3000, 8000], miniUp: [3000, 8500], miniDown: [3000, 8500],
  trajectory: [20, 400], shipImminent: [2, 120], waveImminent: [2, 120], holdGain: [1, 30],
  autoHorizon: [20, 300], autoEvery: [1, 30], autoPaths: [1, 12], autoStride: [1, 12], autoBudget: [200, 20000],
  autoCell: [2, 60], autoWindow: [5, 400], autoPrune: [1, 20], autoForget: [20, 2000], tps: [0, 10000],
  fps: [0, 1000], tapTicks: [1, 40], budget: [200, 40000], shipLook: [4, 300], waveLook: [4, 300],
};
for (let i = 0; i < 8; i++) INTS['maxDelay' + i] = [0, 64];

function cleanValues(v) {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return null;
  const out = {};
  let n = 0;
  for (const k of BOOLS) if (typeof v[k] === 'boolean') { out[k] = v[k]; n++; }
  for (const [k, [lo, hi]] of Object.entries(INTS)) {
    if (typeof v[k] === 'number' && Number.isFinite(v[k])) {
      out[k] = Math.min(hi, Math.max(lo, Math.round(v[k]))); n++;
    }
  }
  return n ? out : null;
}
const text = (s, max) => String(s ?? '').replace(/[\u0000-\u001f\u007f]/g, ' ').trim().slice(0, max);

// ---- storage: one JSON file, written atomically ----
let db = { nextId: 1, configs: [] };
try { db = JSON.parse(fs.readFileSync(DATA, 'utf8')); } catch { /* first run */ }
let dirty = false;
function save() { dirty = true; }
setInterval(() => {
  if (!dirty) return;
  dirty = false;
  const tmp = DATA + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(db));
  fs.renameSync(tmp, DATA);
}, 2000).unref();
process.on('SIGINT', () => { if (dirty) fs.writeFileSync(DATA, JSON.stringify(db)); process.exit(0); });

// ---- rate limit (per IP, per minute) ----
const hits = new Map();
function limited(ip, bucket, max, windowMs) {
  const key = bucket + ip, now = Date.now();
  const arr = (hits.get(key) || []).filter(t => now - t < windowMs);
  arr.push(now);
  hits.set(key, arr);
  return arr.length > max;
}
setInterval(() => { const now = Date.now(); for (const [k, a] of hits) if (!a.some(t => now - t < 3600e3)) hits.delete(k); }, 60e3).unref();

const send = (res, code, obj) => {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(body) });
  res.end(body);
};
const readBody = (req) => new Promise((resolve, reject) => {
  let size = 0; const chunks = [];
  req.on('data', c => { size += c.length; if (size > MAX_BODY) { reject(new Error('too large')); req.destroy(); } else chunks.push(c); });
  req.on('end', () => { try { resolve(JSON.parse(Buffer.concat(chunks).toString() || '{}')); } catch { reject(new Error('bad json')); } });
  req.on('error', reject);
});
const brief = c => ({ id: c.id, name: c.name, author: c.author, likes: c.likes.length, downloads: c.downloads, created: c.created });

const server = http.createServer(async (req, res) => {
  const ip = (TRUST_PROXY && req.headers['x-forwarded-for']?.split(',')[0].trim()) || req.socket.remoteAddress || '?';
  try {
    if (limited(ip, 'all', 120, 60e3)) return send(res, 429, { error: 'Too many requests' });
    const url = new URL(req.url, 'http://x');
    const parts = url.pathname.split('/').filter(Boolean);
    if (parts[0] !== 'configs') return send(res, 404, { error: 'Not found' });
    const find = id => db.configs.find(c => c.id === +id);

    // GET /configs?q=&page=&sort=top|new
    if (req.method === 'GET' && parts.length === 1) {
      const q = text(url.searchParams.get('q'), 32).toLowerCase();
      const sort = url.searchParams.get('sort') === 'new' ? 'new' : 'top';
      let list = db.configs.filter(c => !q || c.name.toLowerCase().includes(q));
      list.sort(sort === 'new' ? (a, b) => b.created - a.created : (a, b) => b.likes.length - a.likes.length || b.created - a.created);
      const pages = Math.max(1, Math.ceil(list.length / PAGE));
      const page = Math.min(pages, Math.max(1, +url.searchParams.get('page') || 1));
      return send(res, 200, { total: list.length, page, pages, items: list.slice((page - 1) * PAGE, page * PAGE).map(brief) });
    }
    // GET /configs/:id?user=
    if (req.method === 'GET' && parts.length === 2) {
      const c = find(parts[1]);
      if (!c) return send(res, 404, { error: 'Config not found' });
      const user = text(url.searchParams.get('user'), 24);
      return send(res, 200, { ...brief(c), desc: c.desc, liked: !!user && c.likes.includes(user) });
    }
    // GET /configs/:id/download
    if (req.method === 'GET' && parts.length === 3 && parts[2] === 'download') {
      const c = find(parts[1]);
      if (!c) return send(res, 404, { error: 'Config not found' });
      c.downloads++; save();
      return send(res, 200, { name: c.name, values: c.values });
    }
    // POST /configs  { name, desc, author, user, values }
    if (req.method === 'POST' && parts.length === 1) {
      if (limited(ip, 'post', 5, 3600e3)) return send(res, 429, { error: 'Post limit reached, try later' });
      const b = await readBody(req);
      const name = text(b.name, 32), desc = text(b.desc, 200), author = text(b.author, 24), user = text(b.user, 24);
      const values = cleanValues(b.values);
      if (name.length < 3) return send(res, 400, { error: 'Name must be at least 3 characters' });
      if (!/^a\d{1,10}$/.test(user) || !author) return send(res, 400, { error: 'Log in to your GD account' });
      if (!values) return send(res, 400, { error: 'Invalid config' });
      if (db.configs.some(c => c.user === user && c.name.toLowerCase() === name.toLowerCase()))
        return send(res, 409, { error: 'You already posted a config with this name' });
      if (db.configs.filter(c => c.user === user).length >= 20) return send(res, 400, { error: 'Max 20 configs per account' });
      const c = { id: db.nextId++, name, desc, author, user, values, likes: [], downloads: 0, created: Date.now() };
      db.configs.push(c); save();
      return send(res, 201, { id: c.id });
    }
    // POST /configs/:id/like  { user }  (toggle)
    if (req.method === 'POST' && parts.length === 3 && parts[2] === 'like') {
      const c = find(parts[1]);
      if (!c) return send(res, 404, { error: 'Config not found' });
      const user = text((await readBody(req)).user, 24);
      if (!/^a\d{1,10}$/.test(user)) return send(res, 400, { error: 'Log in to your GD account' });
      const i = c.likes.indexOf(user);
      if (i >= 0) c.likes.splice(i, 1); else c.likes.push(user);
      save();
      return send(res, 200, { likes: c.likes.length, liked: i < 0 });
    }
    // DELETE /configs/:id   (header: x-admin-token)
    if (req.method === 'DELETE' && parts.length === 2) {
      if (!ADMIN || req.headers['x-admin-token'] !== ADMIN) return send(res, 403, { error: 'Forbidden' });
      const n = db.configs.length;
      db.configs = db.configs.filter(c => c.id !== +parts[1]); save();
      return send(res, n === db.configs.length ? 404 : 200, { ok: n !== db.configs.length });
    }
    send(res, 405, { error: 'Method not allowed' });
  } catch (e) {
    send(res, e.message === 'too large' ? 413 : 400, { error: e.message === 'too large' ? 'Body too large' : 'Bad request' });
  }
});
server.listen(PORT, () => console.log('config server on :' + PORT));
