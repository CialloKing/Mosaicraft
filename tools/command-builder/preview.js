const interactivePreview = (() => {
  const endpoint = '/api/mosaic/preview';
  const maxVisible = 16000;
  const cacheBudget = 64 * 1024 * 1024;
  const parallelLoads = 8;
  const byId = name => document.getElementById('live-preview-' + name);
  const canvas = byId('canvas');
  const context = canvas.getContext('2d', {alpha: false});
  const toggle = byId('toggle');
  const refreshButton = byId('refresh');
  const status = byId('status');
  const placeholder = byId('placeholder');
  const zoomOut = byId('out');
  const zoomIn = byId('in');
  const fitButton = byId('fit');
  const selectionBox = byId('selection');
  const cache = new Map();
  const loading = new Map();
  const failed = new Map();
  let wanted = new Map();
  let pendingLoads = [];
  let nextLoad = 0;
  let slots = [];
  let slotsByAsset = new Map();
  const dirtyAssets = new Set();
  let layoutKey = '';
  let paintedOpacity = 0;
  let cacheBytes = 0;
  let enabled = false;
  let seed = 0;
  let tab = '';
  let serverSession = '';
  let serial = 0;
  let timer = 0;
  let submission = Promise.resolve();
  let frame = 0;
  let pending = false;
  let pendingId = '';
  let plan = null;
  let planKey = '';
  let request = null;
  let requestKey = '';
  let lastError = '';
  let opacity = 0;
  let tileAspect = 9 / 16;
  let scale = 0;
  let centerX = 0;
  let centerY = 0;
  let lastGeometry = '';
  let painted = new Map();
  let pointer = null;
  const edits = new Map();
  let editRevision = 0;
  const picker = createPreviewPicker(byId, {
    replace(position, image) {
      if (plan.imageIds[position] === image.id) edits.delete(position);
      else edits.set(position, {position, imageId: image.id, version: image.version});
      editsChanged();
    },
    undo(position) {
      edits.delete(position);
      editsChanged();
    },
    clear() {
      edits.clear();
      editsChanged();
    },
    current: position => ({id: edits.get(position)?.imageId ?? plan?.imageIds[position], edited: edits.has(position)}),
    count: () => edits.size,
    paint: requestPaint,
    stale() {
      planKey = '';
      lastError = '预览已过期，请刷新后继续选图。';
      showStatus();
      updateRunButton();
    }
  });

  function capable() {
    return serverAvailable && hasFeature('interactive-preview') && context &&
      typeof createImageBitmap === 'function';
  }

  function clip() {
    if (!enabled || tab !== 'preview' || document.hidden || !canvas.clientWidth || !canvas.clientHeight) return null;
    const rect = canvas.getBoundingClientRect();
    const visual = window.visualViewport;
    const left = visual?.offsetLeft ?? 0;
    const top = visual?.offsetTop ?? 0;
    const right = left + (visual?.width ?? document.documentElement.clientWidth);
    const bottom = top + (visual?.height ?? document.documentElement.clientHeight);
    const x0 = Math.max(0, left - rect.left);
    const y0 = Math.max(0, top - rect.top);
    const x1 = Math.min(rect.width, right - rect.left);
    const y1 = Math.min(rect.height, bottom - rect.top);
    return x1 > x0 && y1 > y0 ? {x0, y0, x1, y1} : null;
  }

  function cancelPending() {
    ++serial;
    clearTimeout(timer);
    pending = false;
    if (pendingId) void deleteJson(endpoint + '/' + pendingId);
    pendingId = '';
  }

  function cancelImages() {
    wanted.clear();
    for (const load of loading.values()) load.controller.abort();
    loading.clear();
    pendingLoads = [];
    nextLoad = 0;
    layoutKey = '';
  }

  function clearPictures() {
    cancelImages();
    for (const bitmap of cache.values()) bitmap.close();
    cache.clear();
    failed.clear();
    cacheBytes = 0;
    painted.clear();
    slots = [];
    slotsByAsset.clear();
    dirtyAssets.clear();
    lastGeometry = '';
    if (context) {
      context.setTransform(1, 0, 0, 1, 0, 0);
      context.clearRect(0, 0, canvas.width, canvas.height);
    }
  }

  function discardPlan() {
    if (plan) void deleteJson(endpoint + '/' + plan.id);
    plan = null;
    planKey = '';
    edits.clear();
    scale = 0;
    clearPictures();
  }

  function showStatus() {
    const hasPlan = !!plan;
    toggle.disabled = !enabled && !capable();
    toggle.textContent = enabled ? '关闭预览' : '开启预览';
    refreshButton.hidden = !enabled;
    refreshButton.disabled = pending || !request || !capable();
    zoomOut.disabled = zoomIn.disabled = fitButton.disabled = !hasPlan;
    placeholder.hidden = hasPlan;
    placeholder.textContent = enabled ? '等待预览画面。' : '在生成页填写目标图片后开启预览。';
    byId('color').hidden = !enabled || !val('m-coloradj');
    let message = '';
    if (!capable()) message = '实时预览需要支持此功能的本地 Web UI 服务。';
    else if (enabled && pending) message = '更新中…';
    else if (enabled && lastError) message = lastError;
    else if (enabled && hasPlan) {
      message = plan.columns + ' × ' + plan.rows + ' 格';
      let missing = plan.missing;
      for (const position of edits.keys()) {
        if (plan.imageIds[position] < 0) --missing;
      }
      if (missing) message += ' · ' + missing + ' 格无匹配素材';
      if (plan.columns * plan.rows > maxVisible) message += ' · 可平移查看其余区域';
      if (edits.size) message += ' · ' + edits.size + ' 格已手动选图';
      if (planKey !== requestKey) message += ' · 回到预览窗口后更新';
    }
    if (status.textContent !== message) status.textContent = message;
    picker.sync(plan, hasPlan && !pending && planKey === requestKey && capable() && hasFeature('preview-editing'),
      tab === 'preview' && !document.hidden);
    const generation = byId('generation-note');
    generation.textContent = !enabled ? '' : plan && planKey === requestKey && !pending
      ? (edits.size ? '正式生成将应用 ' + edits.size + ' 格手动选图。' : '正式生成将使用当前预览的选图。')
      : '当前参数需要在预览页刷新后才能沿用选图。';
  }

  function editsChanged() {
    ++editRevision;
    showStatus();
    requestPaint();
  }

  function schedule(delay = 200) {
    if (!enabled || !request || !capable() || !clip() || pending || planKey === requestKey || lastError) return;
    pending = true;
    const version = ++serial;
    // A later POST must reach the queue after any older POST already on the wire.
    timer = setTimeout(() => {
      submission = submission.then(() => submit(version));
    }, delay);
    showStatus();
    updateRunButton();
  }

  function fail(version, message) {
    if (version !== serial) return;
    pending = false;
    pendingId = '';
    lastError = message;
    showStatus();
    updateRunButton();
  }

  async function submit(version) {
    if (version !== serial) return;
    if (!clip()) {
      cancelPending();
      showStatus();
      updateRunButton();
      return;
    }
    const key = requestKey;
    const submitted = await postJson(endpoint, {request, seed});
    const id = submitted.preview?.id;
    if (version !== serial) {
      if (id) void deleteJson(endpoint + '/' + id);
      return;
    }
    if (!submitted.ok || !id) {
      fail(version, submitted.message || '无法提交预览。');
      return;
    }
    pendingId = id;
    timer = setTimeout(() => poll(version, key, id), 100);
  }

  async function poll(version, key, id) {
    if (version !== serial) return;
    const data = await getJson(endpoint + '/' + id);
    if (version !== serial) return;
    const next = data.preview;
    if (!data.ok || !next) {
      fail(version, data.status === 404 ? '预览已过期或被其他请求替换，请刷新。' :
        data.message || '无法读取预览。');
      return;
    }
    if (next.state === 'queued' || next.state === 'running') {
      timer = setTimeout(() => poll(version, key, id), 120);
      return;
    }
    if (next.state !== 'succeeded') {
      void deleteJson(endpoint + '/' + id);
      const error = next.result?.diagnostic;
      fail(version, error?.code === 'preview_grid_limit' ? '预览最多支持 160,000 格，请缩小匹配网格。' :
        next.result?.message || '预览已取消，请刷新。');
      return;
    }

    const previous = plan;
    pending = false;
    pendingId = '';
    plan = next;
    planKey = key;
    if (previous?.editKey !== next.editKey) edits.clear();
    failed.clear();
    updateAspect();
    if (!previous || previous.columns !== next.columns || previous.rows !== next.rows) fit();
    if (previous) void deleteJson(endpoint + '/' + previous.id);
    showStatus();
    updateRunButton();
    requestPaint();
  }

  function updateAspect() {
    if (!plan || !request) return;
    const width = request.nativeTileW ?? 180;
    const height = request.nativeTileH ?? 320;
    tileAspect = width === 180 && height === 320 && plan.featureWidth > plan.featureHeight
      ? 320 / 180 : width / height;
  }

  function sync(activeTab) {
    tab = activeTab;
    const current = serviceInfo?.runtime?.sessionId || '';
    if (serverSession !== current) {
      serverSession = current;
      cancelPending();
      discardPlan();
      lastError = '';
    }
    if (!enabled || (tab !== 'mosaic' && tab !== 'preview')) {
      visibilityChanged();
      showStatus();
      return;
    }
    try {
      request = buildMosaicPayload(true);
      const {nativeTileW, nativeTileH, ...matching} = request;
      const key = JSON.stringify(matching);
      if (key !== requestKey) {
        cancelPending();
        lastError = '';
        requestKey = key;
      }
      opacity = Math.min(1, Math.max(0, Number(val('m-overlaystr')) || 0));
      updateAspect();
      visibilityChanged();
    } catch (error) {
      cancelPending();
      request = null;
      requestKey = '';
      lastError = error.message;
    }
    showStatus();
    requestPaint();
  }

  function refresh() {
    if (!enabled) return;
    cancelPending();
    planKey = '';
    lastError = '';
    sync(tab);
    updateRunButton();
  }

  function visibilityChanged() {
    if (!clip()) {
      cancelImages();
      if (pending) {
        cancelPending();
        showStatus();
        updateRunButton();
      }
      if (document.hidden) showStatus();
      return;
    }
    schedule();
    requestPaint();
  }

  const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

  function minimumScale(width, height) {
    const fitScale = Math.min(width / (plan.columns * tileAspect), height / plan.rows);
    if (plan.columns * plan.rows <= maxVisible) return fitScale;
    // 跨过半个格子时可能多露出一列和一行，缩放下限预留这两个边界。
    const capacity = h => Math.min(plan.columns, Math.ceil(width / (h * tileAspect)) + 1) *
      Math.min(plan.rows, Math.ceil(height / h) + 1);
    if (capacity(fitScale) <= maxVisible) return fitScale;
    let low = fitScale;
    let high = Math.max(width / tileAspect, height);
    for (let i = 0; i < 24; ++i) {
      const mid = (low + high) / 2;
      if (capacity(mid) <= maxVisible) high = mid;
      else low = mid;
    }
    return high;
  }

  function geometry() {
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    const min = minimumScale(width, height);
    scale = clamp(scale || min, min, Math.max(256, min));
    const tw = scale * tileAspect;
    const th = scale;
    const worldW = tw * plan.columns;
    const worldH = th * plan.rows;
    const x = worldW <= width ? (width - worldW) / 2 : clamp(width / 2 - centerX * tw, width - worldW, 0);
    const y = worldH <= height ? (height - worldH) / 2 : clamp(height / 2 - centerY * th, height - worldH, 0);
    centerX = (width / 2 - x) / tw;
    centerY = (height / 2 - y) / th;
    return {width, height, tw, th, x, y};
  }

  function fit() {
    if (!plan) return;
    scale = 0;
    centerX = plan.columns / 2;
    centerY = plan.rows / 2;
    requestPaint();
  }

  function zoom(factor, anchorX = canvas.clientWidth / 2, anchorY = canvas.clientHeight / 2) {
    if (!plan || !clip()) return;
    const g = geometry();
    const min = minimumScale(g.width, g.height);
    scale = clamp(g.th * factor, min, Math.max(256, min));
    centerX = (anchorX - g.x) / g.tw + (g.width / 2 - anchorX) / (scale * tileAspect);
    centerY = (anchorY - g.y) / g.th + (g.height / 2 - anchorY) / scale;
    requestPaint();
  }

  function pan(dx, dy) {
    if (!plan || !clip()) return;
    const g = geometry();
    centerX -= dx / g.tw;
    centerY -= dy / g.th;
    requestPaint();
  }

  function requestPaint() {
    if (!frame && enabled) frame = requestAnimationFrame(paint);
  }

  function touch(key) {
    const bitmap = cache.get(key);
    if (!bitmap) return;
    cache.delete(key);
    cache.set(key, bitmap);
  }

  function addBitmap(key, bitmap) {
    cache.set(key, bitmap);
    cacheBytes += bitmap.width * bitmap.height * 4;
    for (const [oldKey, old] of cache) {
      if (cacheBytes <= cacheBudget && cache.size <= maxVisible + 1000) break;
      if (wanted.has(oldKey)) continue;
      cache.delete(oldKey);
      cacheBytes -= old.width * old.height * 4;
      old.close();
    }
  }

  function queueImages() {
    for (const [key, item] of loading) {
      if (wanted.get(key) !== item.url) {
        item.controller.abort();
        loading.delete(key);
      }
    }
    pendingLoads = [];
    nextLoad = 0;
    for (const [key, url] of wanted) {
      touch(key);
      if (!cache.has(key) && !loading.has(key) && failed.get(key) !== url) pendingLoads.push([key, url]);
    }
  }

  function loadImages() {
    while (loading.size < parallelLoads && nextLoad < pendingLoads.length) {
      const [key, url] = pendingLoads[nextLoad++];
      if (cache.has(key) || loading.has(key) || failed.get(key) === url) continue;
      const controller = new AbortController();
      const load = {controller, url};
      const previewId = plan.id;
      loading.set(key, load);
      // 同一个素材和尺寸共享一个进行中的请求和一个解码位图。
      void (async () => {
        try {
          const response = await fetch(SERVER_URL + url, {signal: controller.signal});
          if (!response.ok) {
            const error = await readJsonResponse(response);
            if (!controller.signal.aborted && plan?.id === previewId && planKey === requestKey &&
                ['preview_stale', 'preview_unavailable'].includes(error.diagnostic?.code)) {
              planKey = '';
              lastError = '预览已过期或素材已变更，请刷新。';
              showStatus();
              updateRunButton();
            }
            throw new Error(String(response.status));
          }
          const bitmap = await createImageBitmap(await response.blob());
          if (!controller.signal.aborted && wanted.get(key) === url && clip()) {
            addBitmap(key, bitmap);
            dirtyAssets.add(key);
          } else bitmap.close();
        } catch (error) {
          if (!controller.signal.aborted && wanted.get(key) === url) failed.set(key, url);
        } finally {
          if (loading.get(key) === load) loading.delete(key);
          requestPaint();
        }
      })();
    }
  }

  function selectTile(x, y) {
    if (!plan || !picker.active || !clip()) return;
    const g = geometry();
    // 与 Canvas 的整数像素边界一致，避免在密集网格的边缘选中相邻格。
    const sx = canvas.width / g.width;
    const sy = canvas.height / g.height;
    const col = Math.floor(((Math.floor(x * sx) + 0.5) / sx - g.x) / g.tw);
    const row = Math.floor(((Math.floor(y * sy) + 0.5) / sy - g.y) / g.th);
    if (col >= 0 && col < plan.columns && row >= 0 && row < plan.rows) {
      picker.select(row * plan.columns + col);
    }
  }

  function paintSelection(g, visible) {
    canvas.classList.toggle('editing', picker.active);
    const position = picker.position;
    if (position < 0) {
      selectionBox.hidden = true;
      return;
    }
    const x = g.x + (position % plan.columns) * g.tw;
    const y = g.y + Math.floor(position / plan.columns) * g.th;
    selectionBox.hidden = x + g.tw <= visible.x0 || x >= visible.x1 ||
      y + g.th <= visible.y0 || y >= visible.y1;
    if (!selectionBox.hidden) {
      selectionBox.style.left = x + 'px';
      selectionBox.style.top = y + 'px';
      selectionBox.style.width = g.tw + 'px';
      selectionBox.style.height = g.th + 'px';
    }
  }

  function thumbnailSize(edge, count) {
    for (const size of [16, 32, 64, 128]) {
      if ((edge <= size || size === 128) && count * size * size * 4 <= cacheBudget - 8 * 1024 * 1024) {
        return size;
      }
      if (count * (size * 2) ** 2 * 4 > cacheBudget - 8 * 1024 * 1024) return size;
    }
    return 16;
  }

  function paint() {
    frame = 0;
    const visible = clip();
    if (!visible || !plan || !context) {
      cancelImages();
      return;
    }
    const g = geometry();
    paintSelection(g, visible);
    const ratio = Math.min(window.devicePixelRatio || 1, 2, Math.sqrt(4000000 / (g.width * g.height)));
    const pixelW = Math.max(1, Math.round(g.width * ratio));
    const pixelH = Math.max(1, Math.round(g.height * ratio));
    const sx = pixelW / g.width;
    const sy = pixelH / g.height;
    const firstCol = clamp(Math.floor((visible.x0 - g.x) / g.tw), 0, plan.columns);
    const lastCol = clamp(Math.ceil((visible.x1 - g.x) / g.tw), 0, plan.columns);
    const firstRow = clamp(Math.floor((visible.y0 - g.y) / g.th), 0, plan.rows);
    const lastRow = clamp(Math.ceil((visible.y1 - g.y) / g.th), 0, plan.rows);
    const count = (lastRow - firstRow) * (lastCol - firstCol);
    const size = thumbnailSize(Math.max(g.tw * sx, g.th * sy), count);
    const prefix = endpoint + '/' + plan.id + '/image/';
    const referenceKey = opacity > 0 && count ? 'reference:' + plan.id : '';
    const reference = cache.get(referenceKey);
    const geometryKey = [pixelW, pixelH, g.x, g.y, g.tw, g.th, plan.columns, plan.rows,
      firstCol, lastCol, firstRow, lastRow].join(',');
    if (geometryKey !== lastGeometry) {
      if (canvas.width !== pixelW) canvas.width = pixelW;
      if (canvas.height !== pixelH) canvas.height = pixelH;
      context.setTransform(sx, 0, 0, sy, 0, 0);
      context.fillStyle = '#11131f';
      context.fillRect(0, 0, g.width, g.height);
      painted.clear();
      lastGeometry = geometryKey;
    }

    const keyForLayout = [geometryKey, plan.id, editRevision, size, referenceKey].join('/');
    const layoutChanged = keyForLayout !== layoutKey;
    if (layoutChanged) {
      layoutKey = keyForLayout;
      wanted = new Map();
      slots = [];
      slotsByAsset = new Map();
      if (referenceKey) wanted.set(referenceKey, prefix + 'reference');
      for (let row = firstRow; row < lastRow; ++row) {
        for (let col = firstCol; col < lastCol; ++col) {
          const position = row * plan.columns + col;
          const edit = edits.get(position);
          const id = edit?.imageId ?? plan.imageIds[position];
          const key = id > 0 ? id + ':' + (edit?.version ?? plan.assetVersions[id]) + ':' + size : '';
          // 相邻格子共用同一条像素边界，避免缩放时出现缝隙。
          const x = Math.round((g.x + col * g.tw) * sx) / sx;
          const y = Math.round((g.y + row * g.th) * sy) / sy;
          const w = Math.round((g.x + (col + 1) * g.tw) * sx) / sx - x;
          const h = Math.round((g.y + (row + 1) * g.th) * sy) / sy - y;
          const slot = {position, key, x, y, w, h, row, col};
          slots.push(slot);
          if (key) {
            wanted.set(key, prefix + id + '-' + size);
            if (!slotsByAsset.has(key)) slotsByAsset.set(key, []);
            slotsByAsset.get(key).push(slot);
          }
        }
      }
      queueImages();
    }

    function draw({position, key, x, y, w, h, row, col}) {
      const bitmap = cache.get(key);
      const signature = bitmap ? key + (reference ? ':' + referenceKey + ':' + opacity : '') : '';
      if ((painted.get(position) || '') === signature) return;
      if (bitmap && w > 0 && h > 0) {
        context.drawImage(bitmap, x, y, w, h);
        if (reference) {
          const rw = reference.width / plan.columns;
          const rh = reference.height / plan.rows;
          context.globalAlpha = opacity;
          context.drawImage(reference, col * rw, row * rh, rw, rh, x, y, w, h);
          context.globalAlpha = 1;
        }
      } else {
        context.fillStyle = '#11131f';
        context.fillRect(x, y, w, h);
      }
      painted.set(position, signature);
    }

    if (layoutChanged || opacity !== paintedOpacity || dirtyAssets.has(referenceKey)) {
      slots.forEach(draw);
    } else {
      // 缩略图陆续返回时，只重画这批素材对应的格子。
      for (const key of dirtyAssets) slotsByAsset.get(key)?.forEach(draw);
    }
    dirtyAssets.clear();
    paintedOpacity = opacity;
    loadImages();
  }

  toggle.addEventListener('click', () => {
    enabled = !enabled;
    lastError = '';
    if (enabled) {
      seed = crypto.getRandomValues(new Uint32Array(1))[0];
      sync(tab);
      canvas.scrollIntoView({block: 'nearest'});
      visibilityChanged();
    } else {
      cancelPending();
      discardPlan();
      request = null;
      requestKey = '';
      showStatus();
    }
    updateRunButton();
  });
  refreshButton.addEventListener('click', refresh);
  zoomOut.addEventListener('click', () => zoom(1 / 1.35));
  zoomIn.addEventListener('click', () => zoom(1.35));
  fitButton.addEventListener('click', fit);
  canvas.addEventListener('wheel', event => {
    if (!plan) return;
    event.preventDefault();
    const rect = canvas.getBoundingClientRect();
    zoom(event.deltaY < 0 ? 1.18 : 1 / 1.18, event.clientX - rect.left, event.clientY - rect.top);
  }, {passive: false});
  canvas.addEventListener('pointerdown', event => {
    if (!plan || event.button !== 0) return;
    canvas.setPointerCapture(event.pointerId);
    pointer = {id: event.pointerId, x: event.clientX, y: event.clientY, moved: false};
  });
  canvas.addEventListener('pointermove', event => {
    if (pointer?.id !== event.pointerId) return;
    const dx = event.clientX - pointer.x;
    const dy = event.clientY - pointer.y;
    if (!pointer.moved && Math.hypot(dx, dy) < 4) return;
    pointer.moved = true;
    pan(dx, dy);
    pointer.x = event.clientX;
    pointer.y = event.clientY;
  });
  canvas.addEventListener('pointerup', event => {
    if (pointer?.id !== event.pointerId || pointer.moved) return;
    const rect = canvas.getBoundingClientRect();
    selectTile(event.clientX - rect.left, event.clientY - rect.top);
  });
  canvas.addEventListener('lostpointercapture', () => { pointer = null; });
  canvas.addEventListener('keydown', event => {
    if (!plan) return;
    switch (event.key) {
      case 'ArrowLeft': pan(48, 0); break;
      case 'ArrowRight': pan(-48, 0); break;
      case 'ArrowUp': pan(0, 48); break;
      case 'ArrowDown': pan(0, -48); break;
      case '+': case '=': zoom(1.35); break;
      case '-': zoom(1 / 1.35); break;
      case '0': fit(); break;
      case 'Enter': selectTile(canvas.clientWidth / 2, canvas.clientHeight / 2); break;
      default: return;
    }
    event.preventDefault();
  });
  if (window.ResizeObserver) new ResizeObserver(visibilityChanged).observe(canvas);
  if (window.IntersectionObserver) new IntersectionObserver(visibilityChanged).observe(canvas);
  window.addEventListener('resize', visibilityChanged);
  window.addEventListener('scroll', visibilityChanged, {passive: true, capture: true});
  window.visualViewport?.addEventListener('scroll', visibilityChanged, {passive: true});
  window.visualViewport?.addEventListener('resize', visibilityChanged, {passive: true});
  document.addEventListener('visibilitychange', () => {
    visibilityChanged();
    showStatus();
  });

  return {
    sync,
    refresh,
    blocksRun: activeTab => enabled && activeTab === 'mosaic' && (!plan || planKey !== requestKey || pending),
    bind(payload) {
      if (!enabled) return;
      if (this.blocksRun('mosaic')) throw new Error('请等待当前参数的预览完成，或关闭预览后生成。');
      payload.previewId = plan.id;
      if (edits.size) payload.previewEdits = Array.from(edits.values());
    }
  };
})();
