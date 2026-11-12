// Optional browser smoke: NODE_PATH must include Playwright's installed package.
const {chromium, firefox} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {pathToFileURL} = require('node:url');

(async () => {
  const project = path.resolve(process.argv[2]);
  const server = process.argv[3];
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'mosaicraft-browser-'));
  try {
    fs.cpSync(project, temporary, {recursive:true});
    const entry = path.join(temporary, 'index.html');
    const html = fs.readFileSync(entry, 'utf8');
    const pattern = /(<script id="mosaicraft-snapshot" type="application\/json">)([\s\S]*?)(<\/script>)/;
    const snapshot = JSON.parse(html.match(pattern)[2]);
    for (const [name, engine] of [['chromium', chromium], ['firefox', firefox]]) {
      const browser = await engine.launch({headless:true});
      try {
        const page = await browser.newPage({viewport:{width:1000,height:760}, offline:true});
        const errors = [], requests = [];
        page.on('pageerror', error => errors.push(error.message));
        page.on('request', request => requests.push(request.url()));
        await page.goto(pathToFileURL(entry).href);
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('缓存'));
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('0.0 / 64') === false);
        await page.locator('#in').click();
        await page.locator('#reference').click();
        await page.locator('#view').dblclick();
        await page.waitForFunction(() => document.querySelector('#material').open);
        await page.locator('#close').click();
        await page.locator('#reference').click();
        await page.locator('#fit').click();
        assert.equal(await page.locator('input,select,[contenteditable=true]').count(), 0);
        assert(requests.every(url => url.startsWith('file:') || url.startsWith('data:')));
        assert.deepEqual(errors, []);
        await page.screenshot({path:path.join(project, '..', `portable-${name}.png`)});
        const big = {...snapshot, grid:{...snapshot.grid, columns:1000, rows:1000}, layout:Array(1000000).fill(0)};
        fs.writeFileSync(path.join(temporary, 'large.html'), html.replace(pattern,
          (_, a, b, c) => a + JSON.stringify(big) + c));
        requests.length = 0;
        await page.goto(pathToFileURL(path.join(temporary, 'large.html')).href);
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('总览'));
        assert(requests.length < 8, 'overview must not request all source assets');
        for (let i = 0; i < 14; ++i) await page.locator('#in').click();
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('视口浏览'));
        assert(requests.length < 20, 'zoom must load visible resources only');
        const budget = await page.evaluate(() => {
          const cache = new MosaicraftViewport.BitmapCache();
          for (let i = 0; i < 24; ++i) cache.add(String(i), {naturalWidth:1024,naturalHeight:1024,src:'loaded'});
          return {bytes:cache.bytes, old:cache.get('0')};
        });
        assert(budget.bytes <= 64 * 1024 * 1024 && budget.old === null);
        assert.deepEqual(errors, []);
        if (server) {
          await page.context().setOffline(false);
          await page.goto(server);
          await page.waitForFunction(() => document.querySelector('#run-status').textContent.includes('连接'));
          await page.locator('[data-cmd="preview"]').click();
          const surface = page.locator('#live-preview-surface');
          const before = await surface.boundingBox();
          await page.locator('#live-preview-resize').press('ArrowDown');
          assert((await surface.boundingBox()).height > before.height);
          await page.locator('#live-preview-reset-size').click();
          await page.locator('[data-cmd="build"]').click();
          await page.locator('#b-input').fill(path.join(project, '..', 'photos'));
          await page.locator('#b-output').fill(path.join(project, '..', 'library'));
          await page.locator('#b-mode').selectOption('add');
          assert(await page.locator('#b-normsize').isDisabled());
          assert((await page.evaluate(() => commandFromPayload('build', buildBuildPayload()))).includes('library add'));
          await page.locator('#b-mode').selectOption('normalize');
          assert(await page.locator('#b-backend').isDisabled());
          assert((await page.evaluate(() => commandFromPayload('build', buildBuildPayload()))).includes('library normalize'));
          await page.locator('[data-cmd="project"]').click();
          await page.locator('#r-project').fill(project);
          await page.locator('#r-info').click();
          await page.waitForFunction(() => document.querySelector('#r-info-result').textContent.includes('assetCount'));
          await page.locator('#r-output').fill(path.join(project, '..', 'browser-render.png'));
          await page.locator('#r-jitter').fill('0.04');
          await page.locator('#r-mode').selectOption('html');
          assert(await page.locator('#r-format').isDisabled());
          await page.locator('#r-mode').selectOption('image');
          await page.locator('#r-overwrite').check();
          await page.locator('#btn-run').click();
          await page.waitForFunction(() => taskStore.detail?.state === 'succeeded' && taskStore.detail?.type === 'render', null, {timeout:60000});
          assert(fs.existsSync(path.join(project, '..', 'browser-render.png')));
          assert.deepEqual(errors, []);
        }
        console.log(`${name}: offline viewer, million-cell fallback, viewport loading, cache eviction${server ? ', Web UI project form' : ''} passed`);
      } finally { await browser.close(); }
    }
  } finally { fs.rmSync(temporary, {recursive:true,force:true}); }
})().catch(error => { console.error(error); process.exitCode = 1; });
