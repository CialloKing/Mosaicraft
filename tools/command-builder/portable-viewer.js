(() => {
  'use strict';
  const data = JSON.parse(document.getElementById('mosaicraft-snapshot').textContent);
  const {columns, rows} = data.grid;
  const aspect = data.outputTile[0] / data.outputTile[1];
  const canvas = document.getElementById('view'), context = canvas.getContext('2d', {alpha: false});
  const status = document.getElementById('status'), material = document.getElementById('material');
  const cache = new MosaicraftViewport.BitmapCache();
  const loading = new Map(), failed = new Set();
  let wanted = new Set(), queue = [], frame = 0, fitted = true, scale = 1;
  let centerX = columns / 2, centerY = rows / 2, drag = null, current = null;
  let reference = false;
  const schedule = () => { if (!frame) frame = requestAnimationFrame(paint); };
  function pump() {
    while (loading.size < MosaicraftViewport.parallelLoads && queue.length) {
      const key = queue.shift();
      if (!wanted.has(key) || cache.get(key) || loading.has(key) || failed.has(key)) continue;
      const image = new Image();
      loading.set(key, image);
      image.onload = () => { loading.delete(key); cache.add(key, image); schedule(); pump(); };
      image.onerror = () => { loading.delete(key); failed.add(key); schedule(); pump(); };
      image.src = key;
    }
  }
  function minimum() { return Math.min(canvas.clientWidth / (columns * aspect), canvas.clientHeight / rows); }
  function paint() {
    frame = 0;
    const width = canvas.clientWidth, height = canvas.clientHeight;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    if (canvas.width !== Math.ceil(width * dpr) || canvas.height !== Math.ceil(height * dpr)) {
      canvas.width = Math.ceil(width * dpr); canvas.height = Math.ceil(height * dpr);
    }
    if (fitted) { scale = minimum(); centerX = columns / 2; centerY = rows / 2; }
    scale = MosaicraftViewport.clamp(scale, minimum(), Math.max(1024, minimum()));
    const g = MosaicraftViewport.geometry(width, height, columns, rows, aspect, scale, centerX, centerY);
    current = g; centerX = (width / 2 - g.x) / g.tw; centerY = (height / 2 - g.y) / g.th;
    const v = MosaicraftViewport.visible(g, {x0: 0, y0: 0, x1: width, y1: height}, columns, rows);
    const count = (v.lastCol - v.firstCol) * (v.lastRow - v.firstRow);
    const assets = new Map();
    const level = Math.max(g.tw, g.th) * dpr <= 32 ? 32 : Math.max(g.tw, g.th) * dpr <= 128 ? 128 : 0;
    let estimated = 0;
    if (count <= 16000) {
      for (let y = v.firstRow; y < v.lastRow; ++y) for (let x = v.firstCol; x < v.lastCol; ++x) {
        const id = data.layout[y * columns + x];
        if (id < 0 || assets.has(id)) continue;
        const asset = data.assets[id];
        const fullFits = asset.width * asset.height * 4 < MosaicraftViewport.cacheBudget / 2;
        const chosen = level || !fullFits ? level || 128 : 0;
        assets.set(id, chosen ? asset['thumbnail' + chosen] : asset.display);
        estimated += chosen ? chosen * chosen * 4 : asset.width * asset.height * 4;
      }
    }
    const overviewOnly = count > 16000 || estimated > MosaicraftViewport.cacheBudget - 16 * 1024 * 1024;
    wanted = new Set([data.overview]);
    if (!overviewOnly) for (const path of assets.values()) wanted.add(path);
    if (reference || data.appearance.targetBlend > 0) wanted.add(data.targetDisplay);
    queue = Array.from(wanted);
    context.setTransform(dpr, 0, 0, dpr, 0, 0);
    context.globalAlpha = 1; context.fillStyle = '#222'; context.fillRect(0, 0, width, height);
    const overview = cache.get(data.overview);
    if (overview) context.drawImage(overview, g.x, g.y, g.tw * columns, g.th * rows);
    if (!overviewOnly) {
      for (let y = v.firstRow; y < v.lastRow; ++y) for (let x = v.firstCol; x < v.lastCol; ++x) {
        const id = data.layout[y * columns + x];
        const bitmap = id >= 0 && cache.get(assets.get(id));
        if (bitmap) {
          context.drawImage(bitmap, g.x + x * g.tw, g.y + y * g.th, g.tw + .1, g.th + .1);
          const target = cache.get(data.targetDisplay);
          if (target && data.appearance.targetBlend > 0) {
            context.globalAlpha = data.appearance.targetBlend;
            context.drawImage(target, x / columns * target.width, y / rows * target.height,
              target.width / columns, target.height / rows, g.x + x * g.tw, g.y + y * g.th, g.tw + .1, g.th + .1);
            context.globalAlpha = 1;
          }
        }
      }
    }
    const target = reference && cache.get(data.targetDisplay);
    if (target) context.drawImage(target, g.x, g.y, g.tw * columns, g.th * rows);
    status.textContent = `${columns} × ${rows} · ${data.assets.length} 素材 · ${overviewOnly ? '总览，继续放大查看素材' : '视口浏览'} · 缓存 ${(cache.bytes / 1048576).toFixed(1)} / 64 MiB${failed.size ? ' · 部分资源读取失败' : ''}`;
    pump();
  }
  function zoom(factor, x = canvas.clientWidth / 2, y = canvas.clientHeight / 2) {
    if (!current) return;
    const worldX = (x - current.x) / current.tw, worldY = (y - current.y) / current.th;
    scale = MosaicraftViewport.clamp(scale * factor, minimum(), Math.max(1024, minimum())); fitted = false;
    centerX = worldX + (canvas.clientWidth / 2 - x) / (scale * aspect);
    centerY = worldY + (canvas.clientHeight / 2 - y) / scale; schedule();
  }
  canvas.addEventListener('wheel', e => { e.preventDefault(); const r = canvas.getBoundingClientRect(); zoom(Math.exp(-e.deltaY * .001), e.clientX - r.left, e.clientY - r.top); }, {passive: false});
  canvas.addEventListener('pointerdown', e => { if (e.button !== 0) return; drag = {id: e.pointerId, x: e.clientX, y: e.clientY, cx: centerX, cy: centerY}; canvas.setPointerCapture(e.pointerId); });
  canvas.addEventListener('pointermove', e => { if (drag?.id !== e.pointerId) return; fitted = false; centerX = drag.cx - (e.clientX - drag.x) / (scale * aspect); centerY = drag.cy - (e.clientY - drag.y) / scale; schedule(); });
  canvas.addEventListener('lostpointercapture', () => { drag = null; });
  canvas.addEventListener('dblclick', e => {
    if (!current) return;
    const r = canvas.getBoundingClientRect(), x = Math.floor((e.clientX - r.left - current.x) / current.tw), y = Math.floor((e.clientY - r.top - current.y) / current.th);
    if (x < 0 || x >= columns || y < 0 || y >= rows) return;
    const id = data.layout[y * columns + x];
    if (id < 0) { status.textContent = '此格在匹配时无候选素材。'; return; }
    const asset = data.assets[id];
    const full = asset.width * asset.height * 4 <= 8 * 1024 * 1024;
    document.getElementById('material-image').src = full ? asset.display : asset.thumbnail128;
    document.getElementById('material-label').textContent = `素材 ${id + 1} · ${asset.width} × ${asset.height} · 格子 ${x}, ${y}${full ? '' : ' · 大素材使用缩略显示'}`;
    material.showModal();
  });
  document.getElementById('close').onclick = () => { material.close(); document.getElementById('material-image').src = ''; };
  document.getElementById('fit').onclick = () => { fitted = true; schedule(); };
  document.getElementById('in').onclick = () => zoom(1.6);
  document.getElementById('out').onclick = () => zoom(1 / 1.6);
  document.getElementById('reference').onclick = e => { reference = !reference; e.target.setAttribute('aria-pressed', String(reference)); schedule(); };
  document.getElementById('fullscreen').onclick = async () => { try { if (document.fullscreenElement) await document.exitFullscreen(); else await document.documentElement.requestFullscreen(); } catch { status.textContent = '浏览器不允许全屏。'; } };
  new ResizeObserver(schedule).observe(canvas);
  schedule();
})();
