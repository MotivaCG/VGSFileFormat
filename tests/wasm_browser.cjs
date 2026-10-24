// Compare real decoded frames across WASM build configurations in installed Edge.
// node tests/wasm_browser.cjs PLAYWRIGHT_PACKAGES FIXTURE.vgs REPORT.json MODULE.js...
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const { createRequire } = require('node:module');

(async () => {
  const [packages, fixture, report, ...inputs] = process.argv.slice(2);
  // @manifest.json optionally supplies {module,fixture} pairs for cross-version parity.
  const modules=inputs.flatMap(input=>input.startsWith('@')
    ? JSON.parse(fs.readFileSync(input.slice(1),'utf8')) : [{module:input,fixture}]);
  if (!modules.length) throw Error('Supply at least one WASM module');
  const { chromium } = createRequire(path.resolve(packages, 'package.json'))('playwright-core');
  const root = path.resolve(__dirname, '..');
  const routes = new Map([['/capture.vgs', path.resolve(fixture)]]);
  modules.forEach((item, i) => {
    routes.set(`/${i}/vgsdecoder.js`, path.resolve(item.module));
    routes.set(`/${i}/capture.vgs`,path.resolve(item.fixture||fixture));
    for (const name of ['vgs.mjs', 'vgssource.mjs'])
      routes.set(`/${i}/${name}`, path.join(root, 'decoder/wasm/js', name));
  });
  const server = http.createServer((req, res) => {
    res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    if (req.url === '/') { res.end('<!doctype html><title>VGS build check</title>'); return; }
    const file = routes.get(req.url);
    if (!file) { res.writeHead(404); res.end(); return; }
    res.setHeader('Content-Type', file.endsWith('.vgs') ? 'application/octet-stream' : 'text/javascript');
    fs.createReadStream(file).pipe(res);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  const reports = [];
  try {
    browser = await chromium.launch({ channel: 'msedge', headless: true });
    for (let i = 0; i < modules.length; ++i) {
      const page = await browser.newPage();
      await page.goto(`http://127.0.0.1:${server.address().port}/`);
      const result = await page.evaluate(async i => {
        const { VgsCapture, VgsError } = await import(`/${i}/vgs.mjs`);
        const data = new Uint8Array(await (await fetch(`/${i}/capture.vgs`)).arrayBuffer());
        const capture = await VgsCapture.open(data, { readAhead: false });
        const result = { duration: capture.duration, frameCount: capture.frameCount, shDegree: capture.shDegree, frames: [] };
        try {
          for (const seconds of [0, capture.duration / 2, Math.max(0, capture.duration - 1 / 30)]) {
            const frame = await capture.setTime(seconds, { copy: true });
            if (!frame.splatCount) throw Error('Expected a nonempty frame');
            const hashes = {};
            for (const key of ['positions', 'rotations', 'scales', 'opacities', 'colors', 'sphericalHarmonics', 'active']) {
              const array = frame[key];
              if (array) {
                const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
                hashes[key] = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), b => b.toString(16).padStart(2, '0')).join('');
              }
            }
            result.frames.push({ seconds, splatCount: frame.splatCount, hashes });
          }
        } finally { capture.close(); }
        let rejected = false;
        try { const bad = await VgsCapture.open(new Uint8Array(256), { readAhead: false }); bad.close(); }
        catch (error) { rejected = error instanceof VgsError; }
        if (!rejected) throw Error('Malformed capture did not produce the expected error');
        return result;
      }, i);
      if (reports.length && JSON.stringify(result) !== JSON.stringify(reports[0].result))
        throw Error('Decoded frames differ between build configurations');
      reports.push({ module: path.resolve(modules[i].module),fixture:path.resolve(modules[i].fixture||fixture), result });
      console.log(`${modules[i].module}: three exact matching frames, malformed input rejected`);
      await page.close();
    }
    fs.mkdirSync(path.dirname(path.resolve(report)), { recursive: true });
    fs.writeFileSync(report, JSON.stringify({ browser: browser.version(), reports }, null, 2));
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
