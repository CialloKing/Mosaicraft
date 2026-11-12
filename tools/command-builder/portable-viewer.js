(() => {
  'use strict';
  const data = JSON.parse(document.getElementById('mosaicraft-snapshot').textContent);
  const {columns, rows} = data.grid;
  const aspect = data.outputTile[0] / data.outputTile[1];
  const canvas = document.getElementById('view'), context = canvas.getContext('2d', {alpha: false});
  const status = document.getElementById('status'), material = document.getElementById('material');
  const cache = new MosaicraftViewport.BitmapCache();
  const loading = new Map(), failed = new Set(), dirty = new Set();
  const targetPath = data.target;
  // 原文件名可能含 #、% 或空格，按路径段编码后才能安全交给浏览器。
  const resourceUrl = path => path.split('/').map(encodeURIComponent).join('/');
  let wanted = new Set(), groups = new Map(), queue = [], next = 0, frame = 0, repaint = true;
  let fitted = false, scale = MosaicraftViewport.maxTileHeight;
  let centerX = columns / 2, centerY = rows / 2, drag = null, current = null, visibleCells = 0;
  let reference = false;
  function schedule(full = true) {
    repaint ||= full;
    if (!frame) frame = requestAnimationFrame(paint);
  }
  function pump() {
    // 可视窗口尚未更新时，不从上一帧的可见队列继续加载。
    if (repaint || !MosaicraftViewport.clip(canvas)) return;
    while (loading.size < MosaicraftViewport.parallelLoads && next < queue.length) {
      const key = queue[next++];
      if (!wanted.has(key) || cache.get(key) || loading.has(key) || failed.has(key)) continue;
      const image = new Image();
      loading.set(key, image);
      image.onload = async () => {
        if (loading.get(key) !== image) return;
        let bitmap;
        try {
          // file:// 无需 fetch；位图可在离开窗口时 close，避免只移除 Image 引用却保留解码资源。
          bitmap = typeof createImageBitmap === 'function' ? await createImageBitmap(image) : image;
          if (loading.get(key) !== image || !wanted.has(key) || !MosaicraftViewport.clip(canvas)) {
            MosaicraftViewport.releaseBitmap(bitmap);
            return;
          }
          cache.add(key, bitmap); dirty.add(key);
        } catch {
          if (loading.get(key) === image && wanted.has(key)) failed.add(key);
        } finally {
          if (bitmap !== image) image.removeAttribute('src');
          if (loading.get(key) === image) loading.delete(key);
          schedule(key === targetPath); pump();
        }
      };
      image.onerror = () => {
        if (loading.get(key) !== image) return;
        loading.delete(key); failed.add(key); schedule(false); pump();
      };
      image.src = resourceUrl(key);
    }
  }
  function cancelOutside() {
    for (const [key, image] of loading) {
      if (wanted.has(key)) continue;
      image.onload = image.onerror = null;
      image.removeAttribute('src');
      loading.delete(key);
    }
    cache.retain(wanted);
  }
  function visibilityChanged() {
    if (!MosaicraftViewport.clip(canvas)) {
      wanted.clear(); groups.clear(); queue = []; next = 0;
      cancelOutside(); dirty.clear(); repaint = true;
    }
    schedule();
  }
  function minimum() { return Math.min(canvas.clientWidth / (columns * aspect), canvas.clientHeight / rows); }
  function maximum() { return Math.max(MosaicraftViewport.maxTileHeight, minimum()); }
  function drawGroup(key) {
    const bitmap = cache.get(key), cells = groups.get(key);
    if (!bitmap || !cells) return;
    const g = current, target = cache.get(targetPath), blend = data.appearance.targetBlend;
    context.save();
    context.beginPath();
    context.rect(g.clip.x0, g.clip.y0, g.clip.x1 - g.clip.x0, g.clip.y1 - g.clip.y0);
    context.clip();
    for (const {col: x, row: y, x: dx, y: dy, w: dw, h: dh} of cells) {
      context.fillRect(dx, dy, dw, dh);
      context.drawImage(bitmap, dx, dy, dw, dh);
      if (target && blend > 0) {
        context.globalAlpha = blend;
        context.drawImage(target, x / columns * target.width, y / rows * target.height,
          target.width / columns, target.height / rows, dx, dy, dw, dh);
        context.globalAlpha = 1;
      }
    }
    context.restore();
  }
  function paint() {
    frame = 0;
    const clip = MosaicraftViewport.clip(canvas);
    if (!clip) {
      wanted.clear(); groups.clear(); queue = []; next = 0;
      cancelOutside(); dirty.clear(); repaint = true;
      return;
    }
    const width = canvas.clientWidth, height = canvas.clientHeight;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    if (canvas.width !== Math.ceil(width * dpr) || canvas.height !== Math.ceil(height * dpr)) {
      canvas.width = Math.ceil(width * dpr); canvas.height = Math.ceil(height * dpr); repaint = true;
    }
    if (repaint) {
      if (fitted) { scale = minimum(); centerX = columns / 2; centerY = rows / 2; }
      scale = MosaicraftViewport.clamp(scale, minimum(), maximum());
      const g = MosaicraftViewport.geometry(width, height, columns, rows, aspect, scale, centerX, centerY);
      current = {...g, dpr, clip}; centerX = (width / 2 - g.x) / g.tw; centerY = (height / 2 - g.y) / g.th;
      const v = MosaicraftViewport.visible(g, clip, columns, rows);
      const visibleColumns = MosaicraftViewport.pixelBands(v.firstCol, v.lastCol, g.x, g.tw, dpr);
      const visibleRows = MosaicraftViewport.pixelBands(v.firstRow, v.lastRow, g.y, g.th, dpr);
      visibleCells = visibleColumns.length * visibleRows.length;
      groups = new Map();
      if (!reference) {
        for (const {index: y, start: y0, end: y1} of visibleRows) for (const {index: x, start: x0, end: x1} of visibleColumns) {
          const id = data.layout[y * columns + x];
          if (id < 0) continue;
          const key = data.assets[id].path;
          if (!groups.has(key)) groups.set(key, []);
          groups.get(key).push({col: x, row: y, x: x0 / dpr, y: y0 / dpr, w: (x1 - x0) / dpr, h: (y1 - y0) / dpr});
        }
      }
      wanted = new Set(groups.keys());
      if (reference || data.appearance.targetBlend > 0) wanted.add(targetPath);
      cancelOutside();
      // 与实时预览共用按格子中心到窗口中心的距离排序，同一素材只排一次。
      queue = MosaicraftViewport.centerFirst(wanted, groups, clip, reference ? targetPath : '');
      next = 0;
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.globalAlpha = 1; context.fillStyle = '#222'; context.fillRect(0, 0, width, height);
      const target = reference && cache.get(targetPath);
      if (target) context.drawImage(target, g.x, g.y, g.tw * columns, g.th * rows);
      else for (const key of groups.keys()) drawGroup(key);
      repaint = false;
    } else if (!reference) {
      // 图片到达时只绘制使用它的可见格，避免每加载一个素材都重画整幅大网格。
      for (const key of dirty) drawGroup(key);
    }
    dirty.clear();
    status.textContent = `${columns} × ${rows} · ${data.assets.length} 素材 · 窗口浏览${scale === maximum() ? ' · 最大缩放' : ''} · 可见 ${visibleCells} 格 · 已加载 ${cache.entries.size} · 缓存 ${(cache.bytes / 1048576).toFixed(1)} MiB${failed.size ? ` · ${failed.size} 个资源读取失败（浏览器可能不支持原格式）` : ''}`;
    pump();
  }
  function zoom(factor, x = canvas.clientWidth / 2, y = canvas.clientHeight / 2) {
    if (!current) return;
    const worldX = (x - current.x) / current.tw, worldY = (y - current.y) / current.th;
    const nextScale = MosaicraftViewport.clamp(scale * factor, minimum(), maximum());
    if (nextScale === scale) return;
    scale = nextScale; fitted = false;
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
    document.getElementById('material-image').src = resourceUrl(asset.path);
    document.getElementById('material-label').textContent = `${asset.path.split('/').pop()} · ${asset.width} × ${asset.height} · 格子 ${x}, ${y}`;
    material.showModal();
  });
  document.getElementById('close').onclick = () => material.close();
  material.addEventListener('close', () => document.getElementById('material-image').removeAttribute('src'));
  document.getElementById('fit').onclick = () => { fitted = true; schedule(); };
  document.getElementById('in').onclick = () => zoom(1.6);
  document.getElementById('out').onclick = () => zoom(1 / 1.6);
  document.getElementById('reference').onclick = e => { reference = !reference; e.target.setAttribute('aria-pressed', String(reference)); schedule(); };
  document.getElementById('fullscreen').onclick = async () => { try { if (document.fullscreenElement) await document.exitFullscreen(); else await document.documentElement.requestFullscreen(); } catch { status.textContent = '浏览器不允许全屏。'; } };
  new ResizeObserver(visibilityChanged).observe(canvas);
  new IntersectionObserver(visibilityChanged).observe(canvas);
  window.addEventListener('scroll', visibilityChanged, {passive: true, capture: true});
  window.visualViewport?.addEventListener('scroll', visibilityChanged, {passive: true});
  window.visualViewport?.addEventListener('resize', visibilityChanged, {passive: true});
  document.addEventListener('visibilitychange', visibilityChanged);
  window.addEventListener('pagehide', () => { wanted.clear(); cancelOutside(); });
  schedule();
})();
