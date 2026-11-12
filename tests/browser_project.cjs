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
          window.loadedImages = []; window.imageErrors = []; window.requestedImages = []; window.decodedImages = [];
          const decode = window.createImageBitmap;
          if (decode) window.createImageBitmap = async (...args) => {
            const url = args[0].src, bitmap = await decode(...args);
            window.decodedImages.push({url, bitmap});
            return bitmap;
          };
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
              window.lastTileHeight = h;
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
        assert.equal(await page.evaluate(() => MosaicraftViewport.initialTileHeight), 256);
        assert.equal(await page.evaluate(() => MosaicraftViewport.maxTileHeight), 16384);
        const visibleLimit = await page.evaluate(() => {
          const canvas = document.querySelector('#view');
          const data = JSON.parse(document.getElementById('mosaicraft-snapshot').textContent);
          const min = Math.min(canvas.clientWidth / (data.grid.columns * data.outputTile[0] / data.outputTile[1]),
            canvas.clientHeight / data.grid.rows);
          const scale = MosaicraftViewport.clamp(MosaicraftViewport.initialTileHeight, min,
            Math.max(MosaicraftViewport.maxTileHeight, min));
          return (Math.ceil(canvas.clientWidth / (scale * data.outputTile[0] / data.outputTile[1])) + 2) *
            (Math.ceil(canvas.clientHeight / scale) + 2);
        });
        assert(await page.evaluate(() => window.loadedImages.filter(url => url.includes('/assets/')).length) <= visibleLimit,
          'initial zoom must only load viewport assets');
        assert(!(await page.locator('#status').textContent()).includes('/ 64'));
        if (process.env.MOSAICRAFT_BROWSER_VIEWPORT_ONLY) {
          await page.waitForFunction(() => document.querySelector('#status').textContent.includes('窗口浏览') &&
            !document.querySelector('#status').textContent.includes('已加载 0'));
          const initialStatus = await page.locator('#status').textContent();
          const initialRequests = await page.evaluate(() => window.requestedImages.length);
          assert(initialRequests <= visibleLimit + 1, 'initial requests must remain within the viewport');
          const initialOrder = await page.evaluate(() => {
            const canvas = document.querySelector('#view'), clip = MosaicraftViewport.clip(canvas);
            const data = JSON.parse(document.getElementById('mosaicraft-snapshot').textContent);
            const {columns, rows} = data.grid, aspect = data.outputTile[0] / data.outputTile[1];
            const min = Math.min(canvas.clientWidth / (columns * aspect), canvas.clientHeight / rows);
            const scale = MosaicraftViewport.clamp(MosaicraftViewport.initialTileHeight, min,
              Math.max(MosaicraftViewport.maxTileHeight, min));
            const g = MosaicraftViewport.geometry(canvas.clientWidth, canvas.clientHeight, columns, rows, aspect, scale, columns / 2, rows / 2);
            const v = MosaicraftViewport.visible(g, clip, columns, rows);
            const cx = (clip.x0 + clip.x1) / 2, cy = (clip.y0 + clip.y1) / 2, distances = new Map();
            for (let row = v.firstRow; row < v.lastRow; ++row) for (let col = v.firstCol; col < v.lastCol; ++col) {
              const id = data.layout[row * columns + col];
              if (id < 0) continue;
              const key = data.assets[id].path.split('/').map(encodeURIComponent).join('/');
              const distance = (g.x + (col + .5) * g.tw - cx) ** 2 + (g.y + (row + .5) * g.th - cy) ** 2;
              distances.set(key, Math.min(distances.get(key) ?? Infinity, distance));
            }
            return window.requestedImages.filter(key => key.startsWith('assets/')).slice(0, 8).map(key => distances.get(key));
          });
          assert(initialOrder.every((distance, i) => Number.isFinite(distance) && (!i || distance >= initialOrder[i - 1])),
            'initial requests must expand outwards from the window center');
          await page.screenshot({path:path.join(project, '..', `viewport-${name}-initial.png`)});
          const before = await page.locator('#view').screenshot();
          const box = await page.locator('#view').boundingBox();
          await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
          await page.mouse.down();
          await page.mouse.move(box.x + box.width / 6, box.y + box.height * .7);
          await page.mouse.up();
          await page.waitForTimeout(200);
          assert(!before.equals(await page.locator('#view').screenshot()), 'panning must change visible content');
          const releasedAfterPan = await page.evaluate(() => window.decodedImages.filter(item => item.bitmap.width === 0).length);
          assert(releasedAfterPan > 0, 'leaving the window must close decoded bitmaps');
          await page.evaluate(() => { document.querySelector('#view').style.transform = 'translateY(10000px)'; });
          await page.waitForTimeout(100);
          const hiddenRequests = await page.evaluate(() => window.requestedImages.length);
          await page.waitForTimeout(200);
          assert.equal(await page.evaluate(() => window.requestedImages.length), hiddenRequests, 'offscreen canvas must pause loading');
          assert.equal(await page.evaluate(() => window.decodedImages.filter(item => item.bitmap.width > 0).length), 0,
            'offscreen canvas must release all decoded bitmaps');
          await page.evaluate(() => { document.querySelector('#view').style.transform = ''; });
          await page.waitForTimeout(100);
          assert.deepEqual(errors, []);
          assert.deepEqual(await page.evaluate(() => window.imageErrors), []);
          assert.equal(await page.evaluate(() => window.outsideDraws), 0, 'offscreen tiles must never be drawn');
          const report = {browser:name,version:browser.version(),initialStatus,initialRequests,visibleLimit,
            initialDistances:initialOrder,releasedAfterPan,hiddenResidentBitmaps:0,
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
        assert.equal(await page.evaluate(() => window.lastTileHeight), 256);
        for (let i = 0; i < 14; ++i) await page.locator('#in').click();
        await page.waitForFunction(() => document.querySelector('#status').textContent.includes('最大缩放') &&
          window.lastTileHeight === 16384);
        await page.locator('#in').click();
        assert.equal(await page.evaluate(() => window.lastTileHeight), 16384);
        await page.locator('#out').click();
        await page.waitForFunction(() => window.lastTileHeight === 10240);
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
