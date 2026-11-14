// UI 质量检查：对比度、触控尺寸、横向溢出。只读。
// 用法：BASE=http://127.0.0.1:8111 USER=admin PASS=lingos-ui-1 node tests/ui_check.mjs
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const { chromium } = require('/root/.npm/_npx/f0a362733743bae2/node_modules/playwright');

const BASE = process.env.BASE || 'http://127.0.0.1:8111';
const USER = process.env.USER || 'admin';
const PASS = process.env.PASS || 'lingos-ui-1';
const args = ['--no-sandbox', '--disable-dev-shm-usage', '--disable-gpu', '--use-gl=swiftshader', '--disable-software-rasterizer'];

function contrast(rgb1, rgb2) {
  const lum = (c) => {
    const [r, g, b] = c.map((x) => { x /= 255; return x <= 0.03928 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4); });
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };
  const a = lum(rgb1), b = lum(rgb2);
  return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
}

const results = [];
const b = await chromium.launch({ args });
const p = await b.newPage({ viewport: { width: 390, height: 844 } });
if (process.env.DARK) await p.addInitScript(() => { try { localStorage.setItem('lo-theme', 'night'); } catch (e) {} });
await p.goto(BASE + '/'); await p.waitForTimeout(700);
if (await p.evaluate(() => !document.getElementById('auth').hidden)) {
  await p.fill('#authUser', USER); await p.fill('#authPass', PASS); await p.click('#authSubmit'); await p.waitForTimeout(1500);
}
const views = ['home', 'devices', 'files', 'security', 'ai', 'system', 'settings'];
for (const v of views) {
  await p.evaluate((x) => { location.hash = '#/' + x; }, v); await p.waitForTimeout(700);
  const r = await p.evaluate(() => {
    const parse = (s) => {
      const m = /rgba?\(([^)]+)\)/.exec(s || ''); if (!m) return null;
      const a = m[1].split(',').map(Number); return { rgb: a.slice(0, 3), alpha: a.length > 3 ? a[3] : 1 };
    };
    const bgOf = (el) => {
      let e = el;
      while (e) {
        const c = parse(getComputedStyle(e).backgroundColor);
        if (c && c.alpha > 0.95) return c.rgb;
        e = e.parentElement;
      }
      const hs = getComputedStyle(document.documentElement).backgroundColor;
      return (parse(hs) || { rgb: [255, 255, 255] }).rgb;
    };
    const sels = ['h1', 'h2', 'p', '.muted', '.btn', '.badge', '.tabbar button', '.nav-item', '.kv-row b', '.tl-empty', '.sub'];
    const rows = [];
    for (const sel of sels) {
      for (const el of document.querySelectorAll(sel)) {
        const rect = el.getBoundingClientRect();
        if (rect.width < 2 || rect.height < 2) continue;
        const cs = getComputedStyle(el);
        if (cs.visibility === 'hidden' || cs.display === 'none') continue;
        const fg = parse(cs.color); if (!fg || fg.alpha < 0.9) continue;
        rows.push({ sel, color: cs.color, bg: `rgb(${bgOf(el).join(',')})`, size: parseFloat(cs.fontSize) });
      }
    }
    const small = [];
    for (const el of document.querySelectorAll('button, a[role="button"], .chip, .seg-btn')) {
      const rect = el.getBoundingClientRect();
      if (rect.width < 2 || rect.height < 2) continue;
      if (rect.height < 40 || rect.width < 40) small.push({ text: (el.textContent || '').trim().slice(0, 18), w: Math.round(rect.width), h: Math.round(rect.height) });
    }
    return { rows: rows.slice(0, 60), small: small.slice(0, 20), overflow: document.documentElement.scrollWidth > innerWidth + 1, scrollW: document.documentElement.scrollWidth, innerW: innerWidth };
  });
  const issues = [];
  const seen = new Set();
  for (const row of r.rows) {
    const m1 = /rgba?\(([^)]+)\)/.exec(row.color), m2 = /rgba?\(([^)]+)\)/.exec(row.bg);
    if (!m1 || !m2) continue;
    const cr = contrast(m1[1].split(',').map(Number).slice(0, 3), m2[1].split(',').map(Number).slice(0, 3));
    const large = row.size >= 24;
    const need = large ? 3 : 4.5;
    if (cr + 0.05 < need) {
      const key = row.sel + row.color + row.bg; if (seen.has(key)) continue; seen.add(key);
      issues.push({ sel: row.sel, ratio: Math.round(cr * 100) / 100, need, fg: row.color, bg: row.bg, size: row.size });
    }
  }
  results.push({ view: v, overflow: r.overflow, small: r.small, contrast: issues });
}
await b.close();
const bad = results.filter((x) => x.overflow || x.small.length || x.contrast.length);
console.log(JSON.stringify({ base: BASE, badCount: bad.length, results }, null, 1));
process.exit(bad.length ? 1 : 0);
