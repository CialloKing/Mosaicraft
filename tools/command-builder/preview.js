function setupPreviewSizing(surface, handle, resetButton) {
  const key = 'mosaicraft-preview-size-v1';
  const saved = storedJson(key, {});
  const size = {
    width: Number.isFinite(saved.width) && saved.width > 0 ? saved.width : null,
    height: Number.isFinite(saved.height) && saved.height > 0 ? saved.height : null
  };
  let drag = null;

  function apply() {
    for (const axis of ['width', 'height']) {
      const property = '--preview-' + axis;
      if (size[axis] === null) surface.style.removeProperty(property);
      else surface.style.setProperty(property, size[axis] + 'px');
    }
    resetButton.disabled = size.width === null && size.height === null;
  }

  function save(widthChanged, heightChanged) {
    // 记住实际尺寸，避免拖出边界时保存一个不可见的尺寸。
    if (widthChanged) size.width = surface.offsetWidth;
    if (heightChanged) size.height = surface.offsetHeight;
    apply();
    saveJson(key, size);
  }

  handle.addEventListener('pointerdown', event => {
    if (event.button !== 0 || drag) return;
    event.preventDefault();
    handle.focus({preventScroll: true});
    handle.setPointerCapture(event.pointerId);
    drag = {id: event.pointerId, x: event.clientX, y: event.clientY,
      width: surface.offsetWidth, height: surface.offsetHeight, before: {...size}};
  });
  handle.addEventListener('pointermove', event => {
    if (drag?.id !== event.pointerId) return;
    const dx = event.clientX - drag.x;
    const dy = event.clientY - drag.y;
    size.width = Math.abs(dx) >= 2 ? Math.max(1, Math.round(drag.width + dx)) : drag.before.width;
    size.height = Math.abs(dy) >= 2 ? Math.max(1, Math.round(drag.height + dy)) : drag.before.height;
    apply();
  });
  handle.addEventListener('lostpointercapture', event => {
    if (drag?.id !== event.pointerId) return;
    const widthChanged = size.width !== drag.before.width;
    const heightChanged = size.height !== drag.before.height;
    drag = null;
    if (widthChanged || heightChanged) save(widthChanged, heightChanged);
  });
  handle.addEventListener('keydown', event => {
    const step = event.shiftKey ? 64 : 24;
    const offsets = {ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step]};
    const offset = offsets[event.key];
    if (!Array.isArray(offset)) return;
    event.preventDefault();
    const [dx, dy] = offset;
    if (dx) size.width = Math.max(1, surface.offsetWidth + dx);
    if (dy) size.height = Math.max(1, surface.offsetHeight + dy);
    apply();
    save(dx !== 0, dy !== 0);
  });
  resetButton.addEventListener('click', () => {
    size.width = size.height = null;
    save(false, false);
  });
  apply();
}

const interactivePreview = (() => {
  const endpoint = '/api/mosaic/preview';
  const visibleTileWarning = 16000;
  const initialTileHeight = MosaicraftViewport.initialTileHeight;
  const maxTileHeight = MosaicraftViewport.maxTileHeight;
  const cacheBudget = MosaicraftViewport.cacheBudget;
  const parallelLoads = MosaicraftViewport.parallelLoads;
  const byId = name => document.getElementById('live-preview-' + name);
  setupPreviewSizing(byId('surface'), byId('resize'), byId('reset-size'));
  const canvas = byId('canvas');
  const context = canvas.getContext('2d', {alpha: false});
  const guides = byId('guides');
  const guideContext = guides.getContext('2d');
  const guideNote = byId('guide-note');
  const referenceButton = byId('reference');
  const markRepeats = byId('mark-repeats');
  const markEdits = byId('mark-edits');
  const guideStyle = getComputedStyle(guides);
  const repeatColor = guideStyle.getPropertyValue('--warning').trim();
  const editColor = guideStyle.getPropertyValue('--accent2').trim();
  const toggle = byId('toggle');
  const refreshButton = byId('refresh');
  const status = byId('status');
  const progressBox = byId('progress');
  const progressFill = byId('progress-fill');
  const progressLabel = byId('progress-label');
  const densityWarning = byId('density-warning');
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
  let guideKey = '';
  let showReference = false;
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
  let layoutPending = false;
  let pending = false;
  let pendingId = '';
  let progress = null;
  let plan = null;
  let planKey = '';
  let request = null;
  let requestKey = '';
  let lastError = '';
  let opacity = 0;
  let tileAspect = 9 / 16;
  let scale = 0;
  let fitted = true;
  let centerX = 0;
  let centerY = 0;
  let lastGeometry = '';
  let previousPaint = null;
  const painted = new Map();
  let pointer = null;
  const edits = new Map();
  let editRevision = 0;
  const insights = createPreviewInsights(byId, {
    id: imageIdAt,
    edit: position => edits.get(position),
    repaint: () => requestPaint(false),
    locate(id) {
      const position = plan.imageIds.findIndex((_, index) => imageIdAt(index) === id);
      if (position < 0) return;
      fitted = false;
      centerX = position % plan.columns + .5;
      centerY = Math.floor(position / plan.columns) + .5;
      requestPaint();
    }
  });
  const picker = createPreviewPicker(byId, {
    replace(position, image) {
      const before = imageIdAt(position);
      if (plan.imageIds[position] === image.id) edits.delete(position);
      else edits.set(position, {position, imageId: image.id, version: image.version});
      insights.changed([[position, before, imageIdAt(position)]]);
      editsChanged();
    },
    undo(position) {
      const before = imageIdAt(position);
      edits.delete(position);
      insights.changed([[position, before, imageIdAt(position)]]);
      editsChanged();
    },
    clear() {
      const changes = [...edits].map(([position, edit]) => [position, edit.imageId, plan.imageIds[position]]);
      edits.clear();
      insights.changed(changes);
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
    return enabled && tab === 'preview' ? MosaicraftViewport.clip(canvas) : null;
  }

  function cancelPending() {
    ++serial;
    clearTimeout(timer);
    pending = false;
    progress = null;
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
    clearGuides();
    densityWarning.hidden = true;
    for (const bitmap of cache.values()) bitmap.close();
    cache.clear();
    failed.clear();
    cacheBytes = 0;
    painted.clear();
    slots = [];
    slotsByAsset.clear();
    dirtyAssets.clear();
    lastGeometry = '';
    previousPaint = null;
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
    insights.setPlan(null, edits);
    scale = 0;
    fitted = true;
    clearPictures();
  }

  const previewStageLabels = {
    queued: '排队中',
    validating: '检查参数',
    reading_library: '读取图库',
    building_index: '准备索引',
    matching: '匹配画面',
    finalizing: '整理预览'
  };

  function previewStageLabel(stage) {
    return previewStageLabels[stage] || '准备预览';
  }

  function renderProgress() {
    const visible = enabled && pending;
    progressBox.hidden = !visible;
    if (!visible) {
      progressBox.classList.remove('is-indeterminate');
      progressBox.removeAttribute('aria-valuenow');
      progressBox.removeAttribute('aria-valuetext');
      progressBox.removeAttribute('title');
      progressFill.style.width = '0%';
      progressLabel.textContent = '';
      return;
    }
    const matching = (progress?.activities ?? []).find(item => item.id === 'matching');
    const total = Number(matching?.total);
    const completed = Number(matching?.completed);
    const determinate = Number.isFinite(total) && total > 0;
    if (!determinate) {
      progressBox.classList.add('is-indeterminate');
      progressBox.removeAttribute('aria-valuenow');
      progressBox.removeAttribute('aria-valuetext');
      progressBox.removeAttribute('title');
      progressFill.style.width = '';
      progressLabel.textContent = previewStageLabel(progress?.stage) + '…';
      return;
    }
    const percent = Math.max(0, Math.min(100, Math.round((Number.isFinite(completed) ? completed : 0) * 100 / total)));
    progressBox.classList.remove('is-indeterminate');
    progressBox.setAttribute('aria-valuenow', String(percent));
    progressBox.setAttribute('aria-valuetext', '正在匹配预览，' + percent + '%');
    const speed = taskMetrics.rate(pendingId || 'preview', progress);
    progressBox.title = completed + ' / ' + total + ' 格' + (Number.isFinite(speed) ? ` · ${speed.toFixed(1)} 格/秒` : '');
    progressFill.style.width = percent + '%';
    progressLabel.textContent = '匹配 ' + percent + '%' + (Number.isFinite(speed) ? ` · ${speed.toFixed(1)} 格/秒` : '');
  }

  function showStatus() {
    const hasPlan = !!plan;
    toggle.disabled = !enabled && !capable();
    toggle.textContent = enabled ? '关闭预览' : '开启预览';
    refreshButton.hidden = !enabled;
    refreshButton.disabled = pending || !request || !capable();
    zoomOut.disabled = zoomIn.disabled = fitButton.disabled = !hasPlan;
    referenceButton.disabled = markRepeats.disabled = markEdits.disabled = !hasPlan || !guideContext;
    referenceButton.setAttribute('aria-pressed', String(showReference));
    placeholder.hidden = hasPlan;
    placeholder.textContent = enabled ? '等待预览画面。' : '在生成页填写目标图片后开启预览。';
    byId('color').hidden = !enabled || !val('m-coloradj');
    let message = '';
    if (!capable()) message = '实时预览需要支持此功能的本地 Web UI 服务。';
    else if (enabled && pending) message = previewStageLabel(progress?.stage) + '…';
    else if (enabled && lastError) message = lastError;
    else if (enabled && hasPlan) {
      message = plan.columns + ' × ' + plan.rows + ' 格';
      if (plan.featureBackend && plan.scoreBackend) {
        const feature = matchingBackendName(plan.featureBackend);
        const score = matchingBackendName(plan.scoreBackend);
        message += feature === score ? ' · ' + score : ' · 特征 ' + feature + ' / 评分 ' + score;
      }
      let missing = plan.missing;
      for (const position of edits.keys()) {
        if (plan.imageIds[position] < 0) --missing;
      }
      if (missing) message += ' · ' + missing + ' 格无匹配素材';
      if (edits.size) message += ' · ' + edits.size + ' 格已手动选图';
      if (planKey !== requestKey) message += ' · 回到预览窗口后更新';
    }
    if (status.textContent !== message) status.textContent = message;
    status.title = hasPlan ? plan.fallbackReason || '' : '';
    picker.sync(plan, hasPlan && !pending && planKey === requestKey && capable() && hasFeature('preview-editing'),
      tab === 'preview' && !document.hidden);
    const generation = byId('generation-note');
    generation.textContent = !enabled ? '' : plan && planKey === requestKey && !pending
      ? (edits.size ? '正式生成将应用 ' + edits.size + ' 格手动选图。' : '正式生成将使用当前预览的选图。')
      : '当前参数需要在预览页刷新后才能沿用选图。';
    renderProgress();
    outputPlanning.updatePreview();
    previewComparison.update();
  }

  function editsChanged() {
    ++editRevision;
    showStatus();
    requestPaint();
  }

  function schedule(delay = 200) {
    if (!enabled || !request || !capable() || !clip() || pending || planKey === requestKey || lastError) return;
    pending = true;
    progress = null;
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
    progress = null;
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
    progress = {stage: 'queued'};
    showStatus();
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
    if (next.progress) progress = next.progress;
    if (next.state === 'queued' || next.state === 'running') {
      showStatus();
      timer = setTimeout(() => poll(version, key, id), 120);
      return;
    }
    if (next.state !== 'succeeded') {
      void deleteJson(endpoint + '/' + id);
      fail(version, next.result?.message || '预览已取消，请刷新。');
      return;
    }

    const previous = plan;
    pending = false;
    pendingId = '';
    progress = null;
    plan = next;
    planKey = key;
    if (previous?.editKey !== next.editKey) edits.clear();
    insights.setPlan(plan, edits);
    failed.clear();
    updateAspect();
    if (!previous || previous.columns !== next.columns || previous.rows !== next.rows) {
      scale = initialTileHeight;
      fitted = false;
      centerX = next.columns / 2;
      centerY = next.rows / 2;
    }
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

  function sync(activeTab, delay = 200) {
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
      if (clip()) schedule(delay);
      else visibilityChanged();
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
    sync(tab, 0);
    updateRunButton();
  }

  function visibilityChanged() {
    if (!clip()) {
      cancelImages();
      densityWarning.hidden = true;
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
    return Math.min(width / (plan.columns * tileAspect), height / plan.rows);
  }

  function geometry() {
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    const min = minimumScale(width, height);
    if (fitted) {
      scale = min;
      centerX = plan.columns / 2;
      centerY = plan.rows / 2;
    } else {
      scale = clamp(scale, min, Math.max(maxTileHeight, min));
    }
    const g = MosaicraftViewport.geometry(width, height, plan.columns, plan.rows, tileAspect, scale, centerX, centerY);
    const {tw, th, x, y} = g;
    centerX = (width / 2 - x) / tw;
    centerY = (height / 2 - y) / th;
    return {width, height, tw, th, x, y};
  }

  function fit() {
    if (!plan) return;
    fitted = true;
    requestPaint();
  }

  function zoom(factor, anchorX = canvas.clientWidth / 2, anchorY = canvas.clientHeight / 2) {
    if (!plan || !clip()) return;
    const g = geometry();
    const min = minimumScale(g.width, g.height);
    scale = clamp(g.th * factor, min, Math.max(maxTileHeight, min));
    if (scale === g.th) return;
    fitted = false;
    centerX = (anchorX - g.x) / g.tw + (g.width / 2 - anchorX) / (scale * tileAspect);
    centerY = (anchorY - g.y) / g.th + (g.height / 2 - anchorY) / scale;
    requestPaint();
  }

  function pan(dx, dy) {
    if (!plan || !clip()) return;
    const g = geometry();
    if ((!dx || g.tw * plan.columns <= g.width) && (!dy || g.th * plan.rows <= g.height)) return;
    fitted = false;
    centerX -= dx / g.tw;
    centerY -= dy / g.th;
    requestPaint();
  }

  function requestPaint(updateLayout = true) {
    if (updateLayout) layoutPending = true;
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
      if (cacheBytes <= cacheBudget) break;
      if (wanted.has(oldKey)) continue;
      cache.delete(oldKey);
      cacheBytes -= old.width * old.height * 4;
      old.close();
    }
  }

  function queueImages(visible) {
    for (const [key, item] of loading) {
      if (wanted.get(key) !== item.url) {
        item.controller.abort();
        loading.delete(key);
      }
    }
    pendingLoads = [];
    nextLoad = 0;
    const candidates = [];
    for (const [key, url] of wanted) {
      touch(key);
      if (cache.has(key) || loading.has(key) || failed.get(key) === url) continue;
      candidates.push(key);
    }
    pendingLoads = MosaicraftViewport.centerFirst(candidates, slotsByAsset, visible,
      showReference ? 'reference:' + plan.id : '').map(key => [key, wanted.get(key)]);
  }

  function loadImages() {
    // 几何变化到下一帧之间，上一份可见清单不能再派发请求。
    if (layoutPending || !clip()) return;
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
            if (!controller.signal.aborted && plan?.id === previewId && error.diagnostic?.code === 'preview_image_unavailable') {
              const id = Number(key.split(':', 1)[0]);
              if (id > 0) insights.unavailable(id);
            }
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
          loadImages();
          requestPaint(false);
        }
      })();
    }
  }

  function selectTile(x, y) {
    if (!plan || (!picker.active && !insights.checking()) || !clip()) return;
    const g = geometry();
    // 与 Canvas 的整数像素边界一致，避免在密集网格的边缘选中相邻格。
    const sx = canvas.width / g.width;
    const sy = canvas.height / g.height;
    const col = Math.floor(((Math.floor(x * sx) + 0.5) / sx - g.x) / g.tw);
    const row = Math.floor(((Math.floor(y * sy) + 0.5) / sy - g.y) / g.th);
    if (col >= 0 && col < plan.columns && row >= 0 && row < plan.rows) {
      const position = row * plan.columns + col;
      if (picker.active) picker.select(position);
      if (insights.checking()) void insights.inspect(position);
    }
  }

  function paintSelection(g, visible) {
    canvas.classList.toggle('editing', picker.active || insights.checking());
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

  function reusePan(previous, next, sx, sy) {
    if (!previous || previous.content !== next.content || previous.width !== next.width ||
        previous.height !== next.height || previous.tw !== next.tw || previous.th !== next.th ||
        previous.visible !== next.visible || previous.reference !== next.reference) return null;
    const rawDx = (next.x - previous.x) * sx;
    const rawDy = (next.y - previous.y) * sy;
    const dx = Math.round(rawDx);
    const dy = Math.round(rawDy);
    if ((!dx && !dy) || Math.abs(rawDx - dx) > 1e-6 || Math.abs(rawDy - dy) > 1e-6) return null;
    const {left, top, right, bottom} = next;
    const x0 = Math.max(left, left + dx);
    const y0 = Math.max(top, top + dy);
    const x1 = Math.min(right, right + dx);
    const y1 = Math.min(bottom, bottom + dy);
    if (x0 >= x1 || y0 >= y1) return null;
    context.setTransform(1, 0, 0, 1, 0, 0);
    context.drawImage(canvas, x0 - dx, y0 - dy, x1 - x0, y1 - y0, x0, y0, x1 - x0, y1 - y0);
    context.fillStyle = '#11131f';
    context.fillRect(left, top, right - left, y0 - top);
    context.fillRect(left, y1, right - left, bottom - y1);
    context.fillRect(left, y0, x0 - left, y1 - y0);
    context.fillRect(x1, y0, right - x1, y1 - y0);
    context.setTransform(sx, 0, 0, sy, 0, 0);
    return {x0, y0, x1, y1};
  }

  function clearGuides() {
    if (!guides.hidden || guides.width !== 1 || guides.height !== 1) {
      guides.hidden = true;
      guides.width = guides.height = 1;
    }
    guideNote.hidden = true;
    guideKey = '';
  }

  function imageIdAt(position) {
    return edits.get(position)?.imageId ?? plan.imageIds[position];
  }

  function repeatsNeighbor(position, row, col) {
    const id = imageIdAt(position);
    return id > 0 && ((col > 0 && imageIdAt(position - 1) === id) ||
      (col + 1 < plan.columns && imageIdAt(position + 1) === id) ||
      (row > 0 && imageIdAt(position - plan.columns) === id) ||
      (row + 1 < plan.rows && imageIdAt(position + plan.columns) === id));
  }

  function paintGuides(g, view, count, referenceKey, reference) {
    const markers = markRepeats.checked || markEdits.checked || insights.activeMarkers();
    if (!guideContext || (!showReference && !markers)) {
      clearGuides();
      return;
    }
    const dense = count > visibleTileWarning;
    const compare = showReference && reference;
    const note = [];
    if (showReference && !compare) note.push(failed.has(referenceKey) ? '目标参考图不可用。' : '正在加载目标参考图…');
    if (dense && markers) note.push('放大预览后可查看标记。');
    const message = note.join(' ');
    if (guideNote.textContent !== message) guideNote.textContent = message;
    guideNote.hidden = !message;
    if (!compare && (!markers || dense)) {
      guides.hidden = true;
      guideKey = '';
      return;
    }
    const key = [layoutKey, !!compare, markRepeats.checked, markEdits.checked, insights.revision()].join('/');
    if (guideKey === key) return;
    guideKey = key;
    guides.hidden = false;
    if (guides.width !== view.width) guides.width = view.width;
    if (guides.height !== view.height) guides.height = view.height;
    guideContext.setTransform(view.width / g.width, 0, 0, view.height / g.height, 0, 0);
    guideContext.clearRect(0, 0, g.width, g.height);
    if (compare) guideContext.drawImage(reference, g.x, g.y, g.tw * plan.columns, g.th * plan.rows);
    if (!markers || dense) return;

    // 标记使用独立图层，切换时无需清空素材位图或破坏平移复用。
    guideContext.lineWidth = 1;
    for (const slot of slots) {
      const {position, row, col, x, y, w, h} = slot;
      if (insights.activeMarkers()) insights.paintMark(guideContext, slot);
      if (markRepeats.checked && repeatsNeighbor(position, row, col)) {
        guideContext.strokeStyle = repeatColor;
        guideContext.strokeRect(x + .5, y + .5, Math.max(0, w - 1), Math.max(0, h - 1));
      }
      if (markEdits.checked && edits.has(position)) {
        guideContext.fillStyle = editColor;
        const edge = Math.min(12, w, h, Math.max(2, Math.min(w, h) / 3));
        guideContext.beginPath();
        guideContext.moveTo(x + w, y);
        guideContext.lineTo(x + w - edge, y);
        guideContext.lineTo(x + w, y + edge);
        guideContext.fill();
      }
    }
  }

  function paint() {
    frame = 0;
    layoutPending = false;
    const visible = clip();
    if (!visible || !plan || !context) {
      cancelImages();
      clearGuides();
      densityWarning.hidden = true;
      return;
    }
    const g = geometry();
    paintSelection(g, visible);
    const ratio = Math.min(window.devicePixelRatio || 1, 2, Math.sqrt(4000000 / (g.width * g.height)));
    const pixelW = Math.max(1, Math.round(g.width * ratio));
    const pixelH = Math.max(1, Math.round(g.height * ratio));
    const sx = pixelW / g.width;
    const sy = pixelH / g.height;
    const {firstCol, lastCol, firstRow, lastRow} = MosaicraftViewport.visible(g, visible, plan.columns, plan.rows);
    const count = (lastRow - firstRow) * (lastCol - firstCol);
    densityWarning.hidden = count <= visibleTileWarning;
    const size = thumbnailSize(Math.max(g.tw * sx, g.th * sy), count);
    const prefix = endpoint + '/' + plan.id + '/image/';
    const referenceKey = (showReference || opacity > 0) && count ? 'reference:' + plan.id : '';
    const referenceBitmap = cache.get(referenceKey);
    const reference = opacity > 0 ? referenceBitmap : undefined;
    const content = [plan.id, editRevision, size, opacity].join('/');
    const view = {x: g.x, y: g.y, tw: g.tw, th: g.th, width: pixelW, height: pixelH,
      left: Math.floor(visible.x0 * sx), top: Math.floor(visible.y0 * sy),
      right: Math.ceil(visible.x1 * sx), bottom: Math.ceil(visible.y1 * sy), content, reference};
    view.visible = [view.left, view.top, view.right, view.bottom].join(',');
    const geometryKey = [pixelW, pixelH, g.x, g.y, g.tw, g.th, plan.columns, plan.rows,
      firstCol, lastCol, firstRow, lastRow, view.visible].join(',');
    let overlap = null;
    if (geometryKey !== lastGeometry) {
      overlap = reusePan(previousPaint, view, sx, sy);
      if (!overlap) {
        if (canvas.width !== pixelW) canvas.width = pixelW;
        if (canvas.height !== pixelH) canvas.height = pixelH;
        context.setTransform(sx, 0, 0, sy, 0, 0);
        context.fillStyle = '#11131f';
        context.fillRect(0, 0, g.width, g.height);
        painted.clear();
      }
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
      const keys = new Map();
      const columns = MosaicraftViewport.pixelBands(firstCol, lastCol, g.x, g.tw, sx);
      const rows = MosaicraftViewport.pixelBands(firstRow, lastRow, g.y, g.th, sy);
      for (const {index: row, start: y0, end: y1} of rows) {
        for (const {index: col, start: x0, end: x1} of columns) {
          const position = row * plan.columns + col;
          const edit = edits.get(position);
          const id = edit?.imageId ?? plan.imageIds[position];
          let key = '';
          if (id > 0) {
            key = edit ? id + ':' + edit.version + ':' + size : keys.get(id);
            if (!key) {
              key = id + ':' + plan.assetVersions[id] + ':' + size;
              keys.set(id, key);
            }
          }
          if (overlap && (x0 < overlap.x0 || x1 > overlap.x1 || y0 < overlap.y0 || y1 > overlap.y1)) {
            painted.delete(position);
          }
          const slot = {position, key, x: x0 / sx, y: y0 / sy, w: (x1 - x0) / sx, h: (y1 - y0) / sy, row, col};
          slots.push(slot);
          if (key) {
            if (!wanted.has(key)) {
              wanted.set(key, prefix + id + '-' + size);
              slotsByAsset.set(key, []);
            }
            slotsByAsset.get(key).push(slot);
          }
        }
      }
      // 上一帧离开的格子不可保留绘制标记，否则再次进入时会跳过绘制。
      if (overlap && painted.size > rows.length * columns.length) {
        for (const position of painted.keys()) {
          const row = Math.floor(position / plan.columns);
          const col = position % plan.columns;
          if (row < firstRow || row >= lastRow || col < firstCol || col >= lastCol) painted.delete(position);
        }
      }
      queueImages(visible);
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
    void insights.loadScores(slots, geometryKey, count > visibleTileWarning);
    paintGuides(g, view, count, referenceKey, referenceBitmap);
    paintedOpacity = opacity;
    previousPaint = view;
    loadImages();
  }

  toggle.addEventListener('click', () => {
    enabled = !enabled;
    lastError = '';
    if (enabled) {
      seed = previewComparison.seed() ?? crypto.getRandomValues(new Uint32Array(1))[0];
      canvas.scrollIntoView({block: 'nearest'});
      sync(tab, 0);
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
  referenceButton.addEventListener('click', () => {
    showReference = !showReference;
    referenceButton.setAttribute('aria-pressed', String(showReference));
    requestPaint();
  });
  markRepeats.addEventListener('change', () => requestPaint());
  markEdits.addEventListener('change', () => requestPaint());
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
    outputSampleKey() {
      return enabled && plan && !pending && planKey === requestKey ? `${plan.id}:${editRevision}` : '';
    },
    comparisonInput() {
      if (!enabled || !plan || pending || planKey !== requestKey) return null;
      return {previewId: plan.id, previewEdits: Array.from(edits.values()), editKey: plan.editKey, seed: plan.seed,
        columns: plan.columns, rows: plan.rows, featureBackend: plan.featureBackend, scoreBackend: plan.scoreBackend,
        metrics: plan.metrics || {}};
    },
    blocksRun: activeTab => enabled && activeTab === 'mosaic' && (!plan || planKey !== requestKey || pending),
    bind(payload) {
      if (!enabled) return;
      if (this.blocksRun('mosaic')) throw new Error('请等待当前参数的预览完成，或关闭预览后生成。');
      payload.previewId = plan.id;
      if (edits.size) payload.previewEdits = Array.from(edits.values());
    }
  };
})();
