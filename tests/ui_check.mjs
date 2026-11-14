// UI 质量检查：对比度、触控、横向溢出、版本热区、SW。
// 用法：BASE=http://127.0.0.1:8111 USER=admin PASS=xxx [DARK=1] node tests/ui_check.mjs
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
let chromium;
try { ({ chromium } = require('playwright')); }
catch (e) { ({ chromium } = require(process.env.PLAYWRIGHT_PATH || '/root/.npm/_npx/f0a362733743bae2/node_modules/playwright')); }

const BASE = process.env.BASE || 'http://127.0.0.1:8111';
const USER = process.env.USER || 'admin';
const PASS = process.env.PASS || 'lingos-ui-1';
const args = ['--no-sandbox', '--disable-dev-shm-usage', '--disable-gpu', '--use-gl=swiftshader', '--disable-software-rasterizer'];

function contrast(a, b) {
  const lum = (c) => { const [r, g, bl] = c.map((x) => { x /= 255; return x <= 0.03928 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); }); return 0.2126 * r + 0.7152 * g + 0.0722 * bl; };
  return (Math.max(lum(a), lum(b)) + 0.05) / (Math.min(lum(a), lum(b)) + 0.05);
}
const results = [];
const b = await chromium.launch({ args });
const p = await b.newPage({ viewport: { width: 390, height: 844 } });
if (process.env.DARK) await p.addInitScript(() => { try { localStorage.setItem('lo-theme', 'night'); } catch (e) {} });
await p.goto(BASE + '/'); await p.waitForTimeout(800);
if (await p.evaluate(() => !document.getElementById('auth').hidden)) {
  await p.fill('#authUser', USER); await p.fill('#authPass', PASS); await p.click('#authSubmit'); await p.waitForTimeout(1600);
}

async function measure(name) {
  const r = await p.evaluate(() => {
    const parse = (s) => { const m = /rgba?\(([^)]+)\)/.exec(s || ''); if (!m) return null; const a = m[1].split(',').map(Number); return { rgb: a.slice(0, 3), alpha: a.length > 3 ? a[3] : 1 }; };
    const bgOf = (el) => { let e = el; while (e) { const c = parse(getComputedStyle(e).backgroundColor); if (c && c.alpha > 0.95) return c.rgb; e = e.parentElement; } return (parse(getComputedStyle(document.documentElement).backgroundColor) || { rgb: [255, 255, 255] }).rgb; };
    const sels = ['h1', 'h2', 'p', '.muted', '.btn', '.badge', '.tabbar button', '.kv-row b', '.tl-empty', '.seg-btn', '.cmd'];
    const rows = [];
    for (const sel of sels) for (const el of document.querySelectorAll(sel)) {
      const rect = el.getBoundingClientRect(); if (rect.width < 2 || rect.height < 2) continue;
      const cs = getComputedStyle(el); if (cs.visibility === 'hidden' || cs.display === 'none') continue;
      const fg = parse(cs.color); if (!fg || fg.alpha < 0.9) continue;
      rows.push({ sel, color: cs.color, bg: 'rgb(' + bgOf(el).join(',') + ')', size: parseFloat(cs.fontSize) });
    }
    const small = [];
    for (const el of document.querySelectorAll('button, a[role="button"], .chip, .seg-btn, .cmd')) {
      const rect = el.getBoundingClientRect(); if (rect.width < 2 || rect.height < 2) continue;
      if (rect.height < 44 || rect.width < 44) small.push({ text: (el.textContent || '').trim().slice(0, 14), w: Math.round(rect.width), h: Math.round(rect.height) });
    }
    const vBtn = document.querySelector('[data-act="version-tap"]');
    const vr = vBtn ? vBtn.getBoundingClientRect() : null;
    return { rows: rows.slice(0, 120), small: small.slice(0, 20), overflow: document.documentElement.scrollWidth > innerWidth + 1, versionHit: vr ? { w: Math.round(vr.width), h: Math.round(vr.height) } : null };
  });
  const issues = [], seen = new Set();
  for (const row of r.rows) {
    const m1 = /rgba?\(([^)]+)\)/.exec(row.color), m2 = /rgba?\(([^)]+)\)/.exec(row.bg); if (!m1 || !m2) continue;
    const cr = contrast(m1[1].split(',').map(Number).slice(0, 3), m2[1].split(',').map(Number).slice(0, 3));
    const need = row.size >= 24 ? 3 : 4.5;
    if (cr + 0.05 < need) { const key = row.sel + row.color; if (seen.has(key)) continue; seen.add(key); issues.push({ sel: row.sel, ratio: Math.round(cr * 100) / 100, need }); }
  }
  if (r.versionHit && r.versionHit.w > 0 && (r.versionHit.h < 44 || r.versionHit.w < 44)) r.small.push({ text: 'version-hit', w: r.versionHit.w, h: r.versionHit.h });
  results.push({ view: name, overflow: r.overflow, small: r.small, contrast: issues, versionHit: r.versionHit });
}

for (const v of ['home', 'devices', 'files', 'security', 'ai', 'system', 'settings']) {
  await p.evaluate((x) => { location.hash = '#/' + x; }, v); await p.waitForTimeout(700);
  await measure(v);
}
for (const sec of ['appearance', 'plugins', 'about']) {
  await p.evaluate((x) => { location.hash = '#/settings'; const b = document.querySelector('[data-set="' + x + '"]'); if (b) b.click(); }, sec);
  await p.waitForTimeout(700);
  await measure('settings/' + sec);
  await p.evaluate(() => { const b = document.querySelector('.set-back'); if (b) b.click(); });
  await p.waitForTimeout(300);
}
// about 打开测试工作台（连点 3 次），再测终端
await p.evaluate(() => { location.hash = '#/settings'; });
await p.waitForTimeout(300);
await p.evaluate(() => { const b = document.querySelector('[data-set="about"]'); if (b) b.click(); });
await p.waitForTimeout(400);
for (let i = 0; i < 3; i++) { await p.evaluate(() => { const b = document.querySelector('[data-act="version-tap"]'); if (b) b.click(); }); await p.waitForTimeout(100); }
await p.waitForTimeout(500);
await p.evaluate(() => { const b = document.querySelector('[data-act="open-term"]'); if (b) b.click(); });
await p.waitForTimeout(500); await measure('terminal');
await p.evaluate(() => { const b = document.querySelector('[data-act="close-term"]'); if (b) b.click(); });
await p.evaluate(() => { location.hash = '#/devices'; }); await p.waitForTimeout(400);
await p.evaluate(() => { const b = document.querySelector('[data-act="add-device"]'); if (b) b.click(); }); await p.waitForTimeout(400);
await measure('confirm');
const regs = await p.evaluate(() => navigator.serviceWorker ? navigator.serviceWorker.getRegistrations().then((r) => r.length) : -1);
await b.close();
const bad = results.filter((x) => x.overflow || x.small.length || x.contrast.length);
console.log(JSON.stringify({ base: BASE, dark: !!process.env.DARK, sw: regs, badCount: bad.length, results }, null, 1));
process.exit(bad.length ? 1 : 0);
