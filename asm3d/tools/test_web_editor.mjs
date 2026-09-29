// Tests the browser editor (build/web) in headless Chromium with Playwright.
//
//   node tools/test_web_editor.mjs [build/web] [--selftest | --interactive] [--shot out.png] [--args "..."] [--frames N] [--query sample=1]
//
// Serves the folder over HTTP, opens index.html (storage not persisted),
// and either runs the editor self test inside the page (--selftest: the same
// end-to-end checks as `asm3d_editor --selftest`, minus the desktop build) or
// runs the editor for N frames and saves a screenshot. --interactive clicks
// and types like a user (create a project, play, reload: storage persists).
import http from 'node:http';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join, extname, resolve } from 'node:path';
import { createRequire } from 'node:module';
import { execSync } from 'node:child_process';

const argv = process.argv.slice(2);
const root = resolve(argv[0] && !argv[0].startsWith('--') ? argv[0] : 'build/web');
const opt = (k, d) => { const i = argv.indexOf(k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const selftest = argv.includes('--selftest');
const interactive = argv.includes('--interactive');
const shot = opt('--shot', null);
const frames = parseInt(opt('--frames', '120'), 10);
const extra = opt('--args', '');
const query = opt('--query', '');

let playwright;
try { playwright = createRequire(import.meta.url)('playwright'); }
catch { playwright = createRequire(import.meta.url)(join(execSync('npm root -g').toString().trim(), 'playwright')); }

const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json' };
const server = http.createServer((req, res) => {
  let p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (p.endsWith('/')) p += 'index.html';
  const f = join(root, p);
  if (!f.startsWith(root) || !existsSync(f) || statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, { 'Content-Type': types[extname(f)] || 'application/octet-stream' });
  res.end(readFileSync(f));
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const browser = await playwright.chromium.launch({
  headless: true,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist'],
});
const page = await browser.newPage({ viewport: { width: 1600, height: 940 } });
const logs = [];
page.on('console', (m) => logs.push(m.text()));
page.on('pageerror', (e) => logs.push('PAGE ERROR: ' + e.message));
const args = (selftest ? '--selftest ' : '') + extra;
const persist = interactive ? '1' : '0';
const url = `http://127.0.0.1:${port}/index.html?persist=${persist}&args=${encodeURIComponent(args.trim())}${query ? '&' + query : ''}`;
let failed = false;
try {
  await page.goto(url);
  await page.waitForFunction(() => window.asm3d || window.asm3dError, null, { timeout: 600000 });
  const err = await page.evaluate(() => window.asm3dError || null);
  if (err) throw new Error(err);
  if (interactive) {
    // like a user: pick the Platformer card, create the project, press F5, reload
    await page.waitForFunction(() => window.asm3d.frames > 10);
    const check = (ok, what) => { if (!ok) { failed = true; console.log('FAIL: ' + what); } else console.log('ok: ' + what); };
    await page.mouse.click(1215, 40 + 170); await page.waitForTimeout(400);
    await page.mouse.click(430, 40 + 512); await page.waitForTimeout(1500);
    check(logs.some((l) => l.includes("created project 'My Game' from template 'Platformer'")), 'mouse: create a project from a template');
    await page.keyboard.press('F5'); await page.waitForTimeout(1500);
    check(logs.some((l) => l.includes('play mode started')), 'keyboard: F5 starts play mode');
    await page.keyboard.press('F5'); await page.waitForTimeout(800);
    await page.waitForTimeout(800);   // let the write-behind reach IndexedDB
    await page.reload();
    await page.waitForFunction(() => window.asm3d && window.asm3d.frames > 5, null, { timeout: 600000 });
    const names = await page.evaluate(() => [...document.querySelectorAll('#projects option')].map((o) => o.textContent));
    check(names.includes('My Game'), 'the project is still there after a reload (IndexedDB)');
    if (shot) await page.screenshot({ path: shot });
  } else if (selftest) {
    const rc = await page.evaluate(() => window.asm3d.exitCode);
    const passed = logs.find((l) => l.includes('self test passed'));
    console.log(passed || '(no pass message)');
    if (rc !== 0) { failed = true; console.log('browser editor self test FAILED (exit code ' + rc + ')'); }
  } else {
    await page.waitForFunction((n) => window.asm3d.frames >= n || !window.asm3d.running, frames, { timeout: 600000 });
    if (shot) { await page.screenshot({ path: shot }); console.log('screenshot: ' + shot); }
  }
} catch (e) {
  failed = true;
  console.log('ERROR: ' + e.message);
}
const bad = logs.filter((l) => /error|failed|PAGE ERROR/i.test(l) && !/Bad\.a3script|broken|does_not_exist|expected/i.test(l));
if (bad.length) console.log('--- log lines with errors ---\n' + bad.slice(0, 40).join('\n'));
if (argv.includes('--log')) console.log('--- log ---\n' + logs.join('\n'));
await browser.close();
server.close();
process.exit(failed ? 1 : 0);
