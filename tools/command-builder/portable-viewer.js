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
  let fitted = true, scale = 1, centerX = columns / 2, centerY = rows / 2, drag = null, current = null;
  let reference = false;
  function schedule(full = true) {
    repaint ||= full;
    if (!frame) frame = requestAnimationFrame(paint);
  }
  function pump() {
    while (loading.size < MosaicraftViewport.parallelLoads && next < queue.length) {
      const key = queue[next++];
      if (!wanted.has(key) || cache.get(key) || loading.has(key) || failed.has(key)) continue;
      const image = new Image();
      loading.set(key, image);
      image.onload = () => {
        loading.delete(key); cache.add(key, image); dirty.add(key);
        schedule(key === targetPath); pump();
      };
      image.onerror = () => { loading.delete(key); failed.add(key); schedule(false); pump(); };
      image.src = resourceUrl(key);
    }
  }
  function minimum() { return Math.min(canvas.clientWidth / (columns * aspect), canvas.clientHeight / rows); }
  function drawGroup(key) {
    const bitmap = cache.get(key), cells = groups.get(key);
    if (!bitmap || !cells) return;
    const g = current, target = cache.get(targetPath), blend = data.appearance.targetBlend;
    for (const cell of cells) {
      const x = cell % columns, y = Math.floor(cell / columns);
      // 相邻格共用物理像素边界，防止缩小后的半透明边缘露出背景，也使增量绘制与全绘制一致。
      const dx = Math.round((g.x + x * g.tw) * g.dpr) / g.dpr;
      const dy = Math.round((g.y + y * g.th) * g.dpr) / g.dpr;
      const dw = Math.round((g.x + (x + 1) * g.tw) * g.dpr) / g.dpr - dx;
      const dh = Math.round((g.y + (y + 1) * g.th) * g.dpr) / g.dpr - dy;
      if (dw <= 0 || dh <= 0) continue;
      context.fillRect(dx, dy, dw, dh);
      context.drawImage(bitmap, dx, dy, dw, dh);
      if (target && blend > 0) {
        context.globalAlpha = blend;
        context.drawImage(target, x / columns * target.width, y / rows * target.height,
          target.width / columns, target.height / rows, dx, dy, dw, dh);
        context.globalAlpha = 1;
      }
    }
  }
  function paint() {
    frame = 0;
    const width = canvas.clientWidth, height = canvas.clientHeight;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    if (canvas.width !== Math.ceil(width * dpr) || canvas.height !== Math.ceil(height * dpr)) {
      canvas.width = Math.ceil(width * dpr); canvas.height = Math.ceil(height * dpr); repaint = true;
    }
    if (repaint) {
      if (fitted) { scale = minimum(); centerX = columns / 2; centerY = rows / 2; }
      scale = MosaicraftViewport.clamp(scale, minimum(), Math.max(1024, minimum()));
      const g = MosaicraftViewport.geometry(width, height, columns, rows, aspect, scale, centerX, centerY);
      current = {...g, dpr}; centerX = (width / 2 - g.x) / g.tw; centerY = (height / 2 - g.y) / g.th;
      const v = MosaicraftViewport.visible(g, {x0: 0, y0: 0, x1: width, y1: height}, columns, rows);
      groups = new Map();
      if (!reference) {
        for (let y = v.firstRow; y < v.lastRow; ++y) for (let x = v.firstCol; x < v.lastCol; ++x) {
          const cell = y * columns + x, id = data.layout[cell];
          if (id < 0) continue;
          const key = data.assets[id].path;
          if (!groups.has(key)) groups.set(key, []);
          groups.get(key).push(cell);
        }
      }
      wanted = new Set(groups.keys());
      if (reference || data.appearance.targetBlend > 0) wanted.add(targetPath);
      // 参考图优先，剩余资源只按可见区域排队；缓存不因大小或格子数而淘汰。
      queue = wanted.has(targetPath) ? [targetPath, ...groups.keys()] : [...wanted];
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
    status.textContent = `${columns} × ${rows} · ${data.assets.length} 素材 · 视口浏览 · 已加载 ${cache.entries.size} · 缓存 ${(cache.bytes / 1048576).toFixed(1)} MiB${failed.size ? ` · ${failed.size} 个资源读取失败（浏览器可能不支持原格式）` : ''}`;
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
    document.getElementById('material-image').src = resourceUrl(asset.path);
    document.getElementById('material-label').textContent = `${asset.path.split('/').pop()} · ${asset.width} × ${asset.height} · 格子 ${x}, ${y}`;
    material.showModal();
  });
  document.getElementById('close').onclick = () => { material.close(); document.getElementById('material-image').removeAttribute('src'); };
  document.getElementById('fit').onclick = () => { fitted = true; schedule(); };
  document.getElementById('in').onclick = () => zoom(1.6);
  document.getElementById('out').onclick = () => zoom(1 / 1.6);
  document.getElementById('reference').onclick = e => { reference = !reference; e.target.setAttribute('aria-pressed', String(reference)); schedule(); };
  document.getElementById('fullscreen').onclick = async () => { try { if (document.fullscreenElement) await document.exitFullscreen(); else await document.documentElement.requestFullscreen(); } catch { status.textContent = '浏览器不允许全屏。'; } };
  new ResizeObserver(() => schedule()).observe(canvas);
  schedule();
})();
