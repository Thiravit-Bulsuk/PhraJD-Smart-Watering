import express from 'express';
import { FieldValue, Firestore, Timestamp } from '@google-cloud/firestore';
import mqtt from 'mqtt';
import crypto from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const port = Number(process.env.PORT || 4173);
const mqttUrl = process.env.MQTT_URL;
const prefix = process.env.TOPIC_PREFIX || 'phrajd/watering';
const controlPassword = process.env.CONTROL_PASSWORD || '';
const logToken = process.env.LOG_TOKEN || '';

const VALVE_COUNT = 7;
const ROUND_COUNT = 4;
const MAX_DURATION_MIN = 240;
const MAX_FAILED_LOGINS = 10;
const FAILED_LOGIN_WINDOW_MS = 10 * 60 * 1000;
const PUBLISH_TIMEOUT_MS = 15000;

// Logs: one Firestore document per Bangkok day, deleted by a TTL policy on expireAt.
// Reads are cached so page views cannot exhaust the Firestore free quota.
const LOG_COLLECTION = 'wateringLogs';
const LOG_KEEP_DAYS = 3;
const LOG_BATCH_MAX = 50;
const LOG_TEXT_MAX = 200;
const LOG_CACHE_MS = 30000;
const DAY_MS = 24 * 60 * 60 * 1000;
const BANGKOK_OFFSET_MS = 7 * 60 * 60 * 1000;

// The browser reads live status straight from EMQX over WebSocket with a subscribe-only
// account, so this server holds no MQTT connection and Cloud Run can scale to zero.
const view = {
  wsUrl: process.env.MQTT_WS_URL || defaultWsUrl(mqttUrl),
  username: process.env.MQTT_VIEW_USERNAME || '',
  password: process.env.MQTT_VIEW_PASSWORD || ''
};

if (!mqttUrl) {
  console.error('MQTT_URL is required');
  process.exit(1);
}

function defaultWsUrl(url) {
  try {
    const u = new URL(url);
    return `wss://${u.hostname}:8084/mqtt`;
  } catch {
    return '';
  }
}

// Connects only for the duration of one command; an idle instance keeps nothing open.
function publishCommand(command) {
  return new Promise((resolve, reject) => {
    const client = mqtt.connect(mqttUrl, {
      username: process.env.MQTT_USERNAME,
      password: process.env.MQTT_PASSWORD,
      clientId: `phrajd-web-${crypto.randomUUID().slice(0, 8)}`,
      connectTimeout: PUBLISH_TIMEOUT_MS,
      reconnectPeriod: 0
    });
    const timer = setTimeout(() => finish(new Error('timeout')), PUBLISH_TIMEOUT_MS);
    let done = false;
    function finish(error) {
      if (done) return;
      done = true;
      clearTimeout(timer);
      client.end(true);
      if (error) reject(error); else resolve();
    }
    client.once('error', finish);
    client.once('close', () => finish(new Error('closed')));
    client.once('connect', () => {
      client.publish(`${prefix}/cmd`, JSON.stringify(command), { qos: 1 }, (error) => finish(error));
    });
  });
}

const failedLogins = new Map();

function isLockedOut(ip) {
  const entry = failedLogins.get(ip);
  if (!entry) return false;
  if (Date.now() - entry.first > FAILED_LOGIN_WINDOW_MS) {
    failedLogins.delete(ip);
    return false;
  }
  return entry.count >= MAX_FAILED_LOGINS;
}

function recordFailedLogin(ip) {
  const entry = failedLogins.get(ip) || { count: 0, first: Date.now() };
  entry.count += 1;
  failedLogins.set(ip, entry);
}

function secretMatches(given, expected) {
  const a = crypto.createHash('sha256').update(String(given || '')).digest();
  const b = crypto.createHash('sha256').update(expected).digest();
  return crypto.timingSafeEqual(a, b);
}

let firestore;
const db = () => (firestore ??= new Firestore());
const logDoc = (key) => db().collection(LOG_COLLECTION).doc(key);
const dayKey = (ms) => new Date(ms + BANGKOK_OFFSET_MS).toISOString().slice(0, 10);
const dayStartMs = (key) => Date.parse(`${key}T00:00:00+07:00`);

async function saveLogs(items) {
  const now = Date.now();
  const byDay = new Map();
  for (const item of items) {
    const text = typeof item?.m === 'string' ? item.m.slice(0, LOG_TEXT_MAX) : '';
    if (!text) continue;
    const deviceMs = Number.isInteger(item.t) ? item.t * 1000 : 0;
    // Device time 0 means its clock was not synced yet.
    const t = deviceMs > 0 && Math.abs(deviceMs - now) < DAY_MS ? deviceMs : now;
    const s = Number.isInteger(item.s) ? item.s : 0;
    const key = dayKey(t);
    if (!byDay.has(key)) byDay.set(key, []);
    byDay.get(key).push({ t, s, m: text });
  }
  if (!byDay.size) return;
  const batch = db().batch();
  for (const [key, entries] of byDay) {
    batch.set(logDoc(key), {
      expireAt: Timestamp.fromMillis(dayStartMs(key) + (LOG_KEEP_DAYS + 1) * DAY_MS),
      // arrayUnion ignores entries already stored, so a retried upload is not duplicated.
      logs: FieldValue.arrayUnion(...entries)
    }, { merge: true });
  }
  await batch.commit();
  logCache = null;
}

let logCache = null;

async function readLogs() {
  if (logCache && Date.now() - logCache.at < LOG_CACHE_MS) return logCache.logs;
  const now = Date.now();
  const since = now - LOG_KEEP_DAYS * DAY_MS;
  const refs = [];
  for (let i = 0; i <= LOG_KEEP_DAYS; i++) refs.push(logDoc(dayKey(now - i * DAY_MS)));
  const snaps = await db().getAll(...refs);
  const logs = snaps.flatMap((snap) => snap.get('logs') || [])
    .filter((l) => l.t >= since)
    .sort((a, b) => b.t - a.t || b.s - a.s)
    .map((l) => ({ time: new Date(l.t).toISOString(), text: l.m }));
  logCache = { at: Date.now(), logs };
  return logs;
}

function buildCommand(body) {
  const action = body?.action;
  if (action === 'stop') return { action };
  if (action === 'start') {
    const round = body.round ?? 0;
    if (!Number.isInteger(round) || round < 0 || round >= ROUND_COUNT) return { error: 'รอบไม่ถูกต้อง' };
    return { action, round };
  }
  if (action === 'auto') {
    if (typeof body.enabled !== 'boolean') return { error: 'คำสั่งไม่ถูกต้อง' };
    return { action, enabled: body.enabled };
  }
  if (action === 'valve') {
    if (!Number.isInteger(body.valve) || body.valve < 0 || body.valve >= VALVE_COUNT || typeof body.open !== 'boolean') {
      return { error: 'คำสั่งวาล์วไม่ถูกต้อง' };
    }
    return { action, valve: body.valve, open: body.open };
  }
  if (action === 'pump') {
    if (typeof body.on !== 'boolean') return { error: 'คำสั่งปั๊มไม่ถูกต้อง' };
    return { action, on: body.on };
  }
  if (action === 'config') {
    if (!Array.isArray(body.rounds) || body.rounds.length !== ROUND_COUNT) return { error: 'ข้อมูลรอบไม่ถูกต้อง' };
    const rounds = [];
    for (const r of body.rounds) {
      const { enabled, startHour, startMinute, duration } = r || {};
      if (typeof enabled !== 'boolean') return { error: 'ข้อมูลรอบไม่ถูกต้อง' };
      if (!Number.isInteger(startHour) || startHour < 0 || startHour > 23 ||
          !Number.isInteger(startMinute) || startMinute < 0 || startMinute > 59) {
        return { error: 'เวลาเริ่มไม่ถูกต้อง' };
      }
      if (!Number.isInteger(duration) || duration < 1 || duration > MAX_DURATION_MIN) {
        return { error: `เวลาต่อวาล์วต้องเป็นจำนวนเต็ม 1-${MAX_DURATION_MIN} นาที` };
      }
      rounds.push({ enabled, startHour, startMinute, duration });
    }
    return { action, rounds };
  }
  return { error: 'คำสั่งไม่ถูกต้อง' };
}

const app = express();
app.disable('x-powered-by');
app.set('trust proxy', 1);
app.use(express.json({ limit: '16kb' }));
app.use(express.static(path.join(__dirname, 'public')));

app.get('/vendor/mqtt.min.js', (req, res) => {
  res.sendFile(path.join(__dirname, 'node_modules/mqtt/dist/mqtt.min.js'), { maxAge: '7d' });
});

app.get('/api/mqtt', (req, res) => {
  res.set('Cache-Control', 'no-store');
  if (!view.wsUrl || !view.username || !view.password) {
    return res.status(503).json({ error: 'ยังไม่ได้ตั้งค่า MQTT สำหรับดูสถานะ (MQTT_VIEW_USERNAME / MQTT_VIEW_PASSWORD)' });
  }
  res.json({ ...view, prefix, controlEnabled: Boolean(controlPassword) });
});

app.post('/api/command', async (req, res) => {
  if (!controlPassword) return res.status(503).json({ error: 'ยังไม่ได้ตั้งรหัสควบคุม (CONTROL_PASSWORD)' });
  if (isLockedOut(req.ip)) return res.status(429).json({ error: 'ใส่รหัสผิดหลายครั้ง กรุณารอ 10 นาที' });
  if (!secretMatches(req.get('x-control-password'), controlPassword)) {
    recordFailedLogin(req.ip);
    return res.status(401).json({ error: 'รหัสผ่านไม่ถูกต้อง' });
  }
  failedLogins.delete(req.ip);

  const command = buildCommand(req.body);
  if (command.error) return res.status(400).json({ error: command.error });
  try {
    await publishCommand(command);
    res.json({ ok: true });
  } catch (error) {
    console.error('MQTT publish failed:', error.message);
    res.status(502).json({ error: 'ส่งคำสั่งไม่สำเร็จ' });
  }
});

app.get('/api/logs', async (req, res) => {
  res.set('Cache-Control', 'no-store');
  try {
    res.json({ logs: await readLogs() });
  } catch (error) {
    console.error('Read logs failed:', error.message);
    res.status(502).json({ error: 'โหลดบันทึกเหตุการณ์ไม่สำเร็จ' });
  }
});

// Called by the master board about once a minute when it has new log lines.
app.post('/api/logs', async (req, res) => {
  if (!logToken) return res.status(503).json({ error: 'LOG_TOKEN not set' });
  if (!secretMatches(req.get('x-log-token'), logToken)) return res.status(401).json({ error: 'unauthorized' });
  const items = req.body?.logs;
  if (!Array.isArray(items) || !items.length || items.length > LOG_BATCH_MAX) {
    return res.status(400).json({ error: 'invalid logs' });
  }
  try {
    await saveLogs(items);
    res.json({ ok: true });
  } catch (error) {
    console.error('Save logs failed:', error.message);
    res.status(502).json({ error: 'save failed' });
  }
});

app.listen(port, () => console.log(`พระเจดีย์ watering dashboard on http://localhost:${port}`));
