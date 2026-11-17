// Cloudflare Worker + D1 version of server/server.js (same API, the mod needs no change).
const PAGE = 20;
const MAX_BODY = 64 * 1024;
const POSTS_PER_HOUR = 5;
const MAX_PER_USER = 20;

// Limits of the settings a config may contain (must match ConfigIO.hpp).
const BOOLS = ['enabled', 'showTraj', 'logOn', 'dualOn', 'bothSides', 'previewBoth', 'showPath', 'orbDetect',
  'dropFatal', 'twoPlayerOn', 'moveDetect', 'swapTwoP', 'waveAnglesOn', 'autoplay', 'autoLearn', 'autoKeep', 'subTick'];
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
const userOk = (u) => /^a\d{1,10}$/.test(u);

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
const sha = async (s) => hex(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(s)));
const newToken = () => hex(crypto.getRandomValues(new Uint8Array(16)));
// Owner = whoever holds the secret token that was returned when the config was posted.
async function isOwner(db, id, req) {
  const tok = req.headers.get('x-token') || '';
  if (!tok || tok.length > 64) return false;
  const r = await db.prepare('SELECT tok FROM configs WHERE id = ?').bind(id).first();
  return !!r && !!r.tok && r.tok === (await sha(tok));
}

const json = (code, obj) => new Response(JSON.stringify(obj), {
  status: code, headers: { 'Content-Type': 'application/json' },
});

async function readBody(req) {
  const raw = await req.text();
  if (raw.length > MAX_BODY) throw new Error('too large');
  try { return JSON.parse(raw || '{}'); } catch { throw new Error('bad json'); }
}

const LIKES = '(SELECT COUNT(*) FROM likes l WHERE l.config_id = c.id)';
const brief = (r) => ({ id: r.id, name: r.name, author: r.author, likes: r.likes, downloads: r.downloads, created: r.created });

export default {
  async fetch(req, env) {
    try {
      const db = env.DB;
      const url = new URL(req.url);
      const parts = url.pathname.split('/').filter(Boolean);
      if (parts[0] !== 'configs') return json(404, { error: 'Not found' });
      const id = parts[1] !== undefined ? Number(parts[1]) : null;
      if (parts.length > 1 && !Number.isInteger(id)) return json(404, { error: 'Config not found' });

      // GET /configs?q=&page=&sort=top|new
      if (req.method === 'GET' && parts.length === 1) {
        const q = text(url.searchParams.get('q'), 32).replace(/[\\%_]/g, '\\$&');
        const order = url.searchParams.get('sort') === 'new' ? 'c.created DESC' : 'likes DESC, c.created DESC';
        const like = `%${q}%`;
        const total = (await db.prepare("SELECT COUNT(*) AS n FROM configs WHERE name LIKE ? ESCAPE '\\'").bind(like).first()).n;
        const pages = Math.max(1, Math.ceil(total / PAGE));
        const page = Math.min(pages, Math.max(1, Number(url.searchParams.get('page')) || 1));
        const { results } = await db.prepare(
          `SELECT c.id, c.name, c.author, c.downloads, c.created, ${LIKES} AS likes FROM configs c
           WHERE c.name LIKE ? ESCAPE '\\' ORDER BY ${order} LIMIT ? OFFSET ?`
        ).bind(like, PAGE, (page - 1) * PAGE).all();
        return json(200, { total, page, pages, items: results.map(brief) });
      }

      // GET /configs/:id?user=
      if (req.method === 'GET' && parts.length === 2) {
        const r = await db.prepare(
          `SELECT c.id, c.name, c.author, c.descr, c.vals, c.downloads, c.created, ${LIKES} AS likes FROM configs c WHERE c.id = ?`
        ).bind(id).first();
        if (!r) return json(404, { error: 'Config not found' });
        const user = text(url.searchParams.get('user'), 24);
        const liked = userOk(user) && !!(await db.prepare('SELECT 1 AS x FROM likes WHERE config_id = ? AND user = ?').bind(id, user).first());
        const mine = await isOwner(db, id, req);
        return json(200, { ...brief(r), desc: r.descr, liked, mine, values: JSON.parse(r.vals) });
      }

      // GET /configs/:id/download
      if (req.method === 'GET' && parts.length === 3 && parts[2] === 'download') {
        const r = await db.prepare('UPDATE configs SET downloads = downloads + 1 WHERE id = ? RETURNING name, vals').bind(id).first();
        if (!r) return json(404, { error: 'Config not found' });
        return json(200, { name: r.name, values: JSON.parse(r.vals) });
      }

      // POST /configs { name, desc, author, user, values }
      if (req.method === 'POST' && parts.length === 1) {
        const b = await readBody(req);
        const name = text(b.name, 32), desc = text(b.desc, 200), author = text(b.author, 24), user = text(b.user, 24);
        const values = cleanValues(b.values);
        if (name.length < 3) return json(400, { error: 'Name must be at least 3 characters' });
        if (!userOk(user) || !author) return json(400, { error: 'Log in to your GD account' });
        if (!values) return json(400, { error: 'Invalid config' });

        const ip = req.headers.get('CF-Connecting-IP') || '?';
        const now = Date.now();
        await db.prepare('DELETE FROM post_log WHERE t < ?').bind(now - 3600e3).run();
        const recent = (await db.prepare('SELECT COUNT(*) AS n FROM post_log WHERE ip = ?').bind(ip).first()).n;
        if (recent >= POSTS_PER_HOUR) return json(429, { error: 'Post limit reached, try later' });

        if (await db.prepare('SELECT 1 AS x FROM configs WHERE user = ? AND name = ? COLLATE NOCASE').bind(user, name).first()) {
          return json(409, { error: 'You already posted a config with this name' });
        }
        if ((await db.prepare('SELECT COUNT(*) AS n FROM configs WHERE user = ?').bind(user).first()).n >= MAX_PER_USER) {
          return json(400, { error: `Max ${MAX_PER_USER} configs per account` });
        }
        await db.prepare('INSERT INTO post_log (ip, t) VALUES (?, ?)').bind(ip, now).run();
        const token = newToken();
        const r = await db.prepare(
          'INSERT INTO configs (name, descr, author, user, vals, tok, created) VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id'
        ).bind(name, desc, author, user, JSON.stringify(values), await sha(token), now).first();
        return json(201, { id: r.id, token });
      }

      // PUT /configs/:id { name, desc, values? }   (header x-token = owner secret)
      if (req.method === 'PUT' && parts.length === 2) {
        if (!(await isOwner(db, id, req))) return json(403, { error: 'You can only edit your own configs' });
        const b = await readBody(req);
        const name = text(b.name, 32), desc = text(b.desc, 200);
        if (name.length < 3) return json(400, { error: 'Name must be at least 3 characters' });
        const cur = await db.prepare('SELECT user FROM configs WHERE id = ?').bind(id).first();
        if (await db.prepare('SELECT 1 AS x FROM configs WHERE user = ? AND name = ? COLLATE NOCASE AND id != ?').bind(cur.user, name, id).first()) {
          return json(409, { error: 'You already posted a config with this name' });
        }
        if (b.values !== undefined) {
          const values = cleanValues(b.values);
          if (!values) return json(400, { error: 'Invalid config' });
          // new settings = new config: old likes no longer apply
          await db.batch([
            db.prepare('UPDATE configs SET name = ?, descr = ?, vals = ? WHERE id = ?').bind(name, desc, JSON.stringify(values), id),
            db.prepare('DELETE FROM likes WHERE config_id = ?').bind(id),
          ]);
        } else {
          await db.prepare('UPDATE configs SET name = ?, descr = ? WHERE id = ?').bind(name, desc, id).run();
        }
        return json(200, { ok: true });
      }

      // POST /configs/:id/like { user }  (toggle)
      if (req.method === 'POST' && parts.length === 3 && parts[2] === 'like') {
        const user = text((await readBody(req)).user, 24);
        if (!userOk(user)) return json(400, { error: 'Log in to your GD account' });
        if (!(await db.prepare('SELECT 1 AS x FROM configs WHERE id = ?').bind(id).first())) {
          return json(404, { error: 'Config not found' });
        }
        const del = await db.prepare('DELETE FROM likes WHERE config_id = ? AND user = ?').bind(id, user).run();
        const liked = del.meta.changes === 0;
        if (liked) await db.prepare('INSERT INTO likes (config_id, user) VALUES (?, ?)').bind(id, user).run();
        const likes = (await db.prepare('SELECT COUNT(*) AS n FROM likes WHERE config_id = ?').bind(id).first()).n;
        return json(200, { likes, liked });
      }

      // DELETE /configs/:id  (header x-token = owner secret, or x-admin-token = secret ADMIN_TOKEN)
      if (req.method === 'DELETE' && parts.length === 2) {
        const admin = !!env.ADMIN_TOKEN && req.headers.get('x-admin-token') === env.ADMIN_TOKEN;
        if (!admin && !(await isOwner(db, id, req))) return json(403, { error: 'You can only delete your own configs' });
        const r = await db.batch([
          db.prepare('DELETE FROM likes WHERE config_id = ?').bind(id),
          db.prepare('DELETE FROM configs WHERE id = ?').bind(id),
        ]);
        const ok = r[1].meta.changes > 0;
        return json(ok ? 200 : 404, { ok });
      }

      return json(405, { error: 'Method not allowed' });
    } catch (e) {
      return e.message === 'too large' ? json(413, { error: 'Body too large' })
        : e.message === 'bad json' ? json(400, { error: 'Bad request' })
        : json(500, { error: 'Server error' });
    }
  },
};
