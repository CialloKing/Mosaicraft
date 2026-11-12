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
        await page.addInitScript(() => {
          const NativeImage = window.Image;
          window.loadedImages = []; window.imageErrors = []; window.requestedImages = [];
          const source = Object.getOwnPropertyDescriptor(HTMLImageElement.prototype, 'src');
          window.Image = function(...args) {
            const bitmap = new NativeImage(...args);
            Object.defineProperty(bitmap, 'src', {
              get() { return source.get.call(this); },
              set(url) { window.requestedImages.push(url); source.set.call(this, url); }
            });
            bitmap.addEventListener('load', () => window.loadedImages.push(bitmap.src));
            bitmap.addEventListener('error', () => { if (bitmap.getAttribute('src')) window.imageErrors.push(bitmap.src); });
            return bitmap;
          };
          window.Image.prototype = NativeImage.prototype;
          const draw = CanvasRenderingContext2D.prototype.drawImage;
          window.outsideDraws = 0;
          CanvasRenderingContext2D.prototype.drawImage = function(...args) {
            if (this.canvas.id === 'view' && args.length >= 5) {
              const clip = MosaicraftViewport.clip(this.canvas), [x, y, w, h] = args.slice(-4);
              if (!clip || x + w <= clip.x0 || y + h <= clip.y0 || x >= clip.x1 || y >= clip.y1) ++window.outsideDraws;
            }
            return draw.apply(this, args);
          };
        });
        const errors = [], requests = [];
        page.on('pageerror', error => errors.push(error.message));
        page.on('request', request => requests.push(request.url()));
        await page.goto(pathToFileURL(entry).href);
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('缓存'));
        await page.waitForFunction(() => window.loadedImages.some(url => url.includes('/assets/')));
        assert((await page.locator('#status').textContent()).includes('最大缩放'));
        const visibleLimit = await page.evaluate(() => {
          const canvas = document.querySelector('#view');
          const data = JSON.parse(document.getElementById('mosaicraft-snapshot').textContent);
          const min = Math.min(canvas.clientWidth / (data.grid.columns * data.outputTile[0] / data.outputTile[1]),
            canvas.clientHeight / data.grid.rows);
          const scale = Math.max(MosaicraftViewport.maxTileHeight, min);
          return (Math.ceil(canvas.clientWidth / (scale * data.outputTile[0] / data.outputTile[1])) + 2) *
            (Math.ceil(canvas.clientHeight / scale) + 2);
        });
        assert(await page.evaluate(() => window.loadedImages.filter(url => url.includes('/assets/')).length) <= visibleLimit,
          'initial maximum zoom must only load viewport assets');
        assert(!(await page.locator('#status').textContent()).includes('/ 64'));
        if (process.env.MOSAICRAFT_BROWSER_VIEWPORT_ONLY) {
          const initialStatus = await page.locator('#status').textContent();
          const initialRequests = await page.evaluate(() => window.requestedImages.length);
          assert(initialRequests <= visibleLimit + 1, 'initial requests must remain within the viewport');
          await page.screenshot({path:path.join(project, '..', `viewport-${name}-initial.png`)});
          const before = await page.locator('#view').screenshot();
          const box = await page.locator('#view').boundingBox();
          await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
          await page.mouse.down();
          await page.mouse.move(box.x + box.width / 6, box.y + box.height * .7);
          await page.mouse.up();
          await page.waitForTimeout(200);
          assert(!before.equals(await page.locator('#view').screenshot()), 'panning must change visible content');
          await page.evaluate(() => { document.querySelector('#view').style.transform = 'translateY(10000px)'; });
          await page.waitForTimeout(100);
          const hiddenRequests = await page.evaluate(() => window.requestedImages.length);
          await page.waitForTimeout(200);
          assert.equal(await page.evaluate(() => window.requestedImages.length), hiddenRequests, 'offscreen canvas must pause loading');
          await page.evaluate(() => { document.querySelector('#view').style.transform = ''; });
          await page.waitForTimeout(100);
          assert.deepEqual(errors, []);
          assert.deepEqual(await page.evaluate(() => window.imageErrors), []);
          assert.equal(await page.evaluate(() => window.outsideDraws), 0, 'offscreen tiles must never be drawn');
          const report = {browser:name,version:browser.version(),initialStatus,initialRequests,visibleLimit,
            requestsAfterPan:await page.evaluate(() => window.requestedImages.length),hiddenRequests,errors};
          fs.writeFileSync(path.join(project, '..', `viewport-${name}.json`), JSON.stringify(report,null,2));
          console.log(JSON.stringify(report));
          continue;
        }
        if (process.env.MOSAICRAFT_BROWSER_WAIT_ALL) {
          await page.locator('#fit').click();
          await page.waitForFunction(count => window.loadedImages.filter(url => url.includes('/assets/')).length === count,
            snapshot.assets.length, {timeout:180000});
          await page.waitForFunction(count => document.querySelector('#status').textContent.includes(`已加载 ${count}`),
            snapshot.assets.length + (snapshot.appearance.targetBlend > 0 ? 1 : 0));
          console.log(`${name}: ${await page.locator('#status').textContent()}`);
          const incremental = await page.locator('#view').screenshot();
          await page.locator('#fit').click();
          assert(incremental.equals(await page.locator('#view').screenshot()), 'incremental and complete drawing must match');
        }
        await page.locator('#in').click();
        await page.locator('#reference').click();
        await page.locator('#view').dblclick();
        await page.waitForFunction(() => document.querySelector('#material').open && document.querySelector('#material-image').naturalWidth > 0);
        await page.locator('#close').click();
        await page.locator('#reference').click();
        await page.locator('#fit').click();
        assert.equal(await page.locator('input,select,[contenteditable=true]').count(), 0);
        assert(requests.every(url => url.startsWith('file:') || url.startsWith('data:')));
        assert.deepEqual(errors, []);
        assert.deepEqual(await page.evaluate(() => window.imageErrors), []);
        assert((await page.evaluate(() => window.loadedImages)).every(url => !/overview|thumbnails/.test(url)));
        await page.screenshot({path:path.join(project, '..', `portable-${name}.png`)});
        const special = path.join('assets', 'special', `素材 # % 𠮷${path.extname(snapshot.assets[0].path)}`);
        fs.mkdirSync(path.join(temporary, path.dirname(special)), {recursive:true});
        fs.copyFileSync(path.join(temporary, snapshot.assets[0].path), path.join(temporary, special));
        const assets = snapshot.assets.map((asset, i) => i ? asset : {...asset, path:special.split(path.sep).join('/')});
        const big = {...snapshot, assets, grid:{...snapshot.grid, columns:1000, rows:1000}, layout:Array(1000000).fill(0)};
        fs.writeFileSync(path.join(temporary, 'large.html'), html.replace(pattern,
          (_, a, b, c) => a + JSON.stringify(big) + c));
        requests.length = 0;
        await page.goto(pathToFileURL(path.join(temporary, 'large.html')).href);
        await page.waitForFunction(() => window.loadedImages.some(url => url.includes('/assets/')));
        assert.equal(await page.evaluate(() => window.loadedImages.filter(url => url.includes('/assets/')).length), 1);
        for (let i = 0; i < 14; ++i) await page.locator('#in').click();
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('视口浏览'));
        assert(requests.length < 20, 'zoom must load visible resources only');
        const budget = await page.evaluate(() => {
          const cache = new MosaicraftViewport.BitmapCache();
          for (let i = 0; i < 24; ++i) cache.add(String(i), {naturalWidth:1024,naturalHeight:1024,src:'loaded'});
          return {bytes:cache.bytes, old:cache.get('0')?.src};
        });
        assert(budget.bytes === 96 * 1024 * 1024 && budget.old === 'loaded');
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
          await page.locator('#task-history > summary').click();
          await page.locator('#history-type').selectOption('render');
          await page.waitForFunction(() => document.querySelector('#history-a').options.length > 1);
          await page.locator('#history-a').selectOption({index:1});
          await page.locator('#history-restore-a').click();
          assert.equal(await page.locator('#r-project').inputValue(), project);
          assert(fs.existsSync(path.join(project, '..', 'browser-render.png')));
          assert.deepEqual(errors, []);
        }
        console.log(`${name}: offline viewer, million-cell source drawing, viewport loading, unbounded cache${server ? ', Web UI project form' : ''} passed`);
      } finally { await browser.close(); }
    }
  } finally { fs.rmSync(temporary, {recursive:true,force:true}); }
})().catch(error => { console.error(error); process.exitCode = 1; });
