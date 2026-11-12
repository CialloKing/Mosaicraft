const libraryVisuals = (() => {
  const rendered = new WeakSet();
  const coverageButton = document.getElementById('m-coverage-run');
  const coverageResult = document.getElementById('m-coverage-result');

  function colorCharts(colors, title = '图库') {
    if (!colors?.total) return `<p class="hint">${title}暂无色彩样本。</p>`;
    const peak = Math.max(1, ...colors.hue);
    const bars = colors.hue.map((count, index) => `<span title="${index * 15}°–${index * 15 + 14}° · ${count}" style="height:${100 * count / peak}%;background:hsl(${index * 15},55%,62%)"></span>`).join('');
    return `<section class="db-result"><h4>${title} · ${colors.total.toLocaleString()} 个颜色样本</h4>
      <figure><figcaption>平均 LAB a/b 密度 · a 向右，b 向上</figcaption><canvas class="color-map" width="32" height="32" role="img" aria-label="平均 LAB a/b 密度" data-colors="${escapeAttr(JSON.stringify(colors.ab))}"></canvas></figure>
      <figure><figcaption>平均 LAB 色相 · 灰度 ${colors.gray}</figcaption><div class="color-hue-bars" aria-hidden="true">${bars}</div>
      <details><summary>各色相分桶与亮度数量</summary><p>${colors.hue.map((count, i) => `${i * 15}° ${count}`).join(' · ')}</p>
      <p>LAB L（每 32 一档） · ${colors.lightness.join(' / ')}</p></details></figure></section>`;
  }

  function canvasPixels(canvas, values, color) {
    const context = canvas.getContext('2d');
    if (!context) return;
    const pixels = context.createImageData(canvas.width, canvas.height);
    values.forEach((value, index) => pixels.data.set(color(value), index * 4));
    context.putImageData(pixels, 0, 0);
  }

  function paint(root) {
    root.querySelectorAll('canvas[data-colors]').forEach(canvas => {
      if (rendered.has(canvas)) return;
      rendered.add(canvas);
      const values = JSON.parse(canvas.dataset.colors);
      const peak = Math.max(1, ...values);
      const flipped = values.slice();
      for (let row = 0; row < 32; ++row) {
        flipped.splice(row * 32, 32, ...values.slice((31 - row) * 32, (32 - row) * 32));
      }
      canvasPixels(canvas, flipped, value => [78, 164, 239, Math.round(255 * Math.sqrt(value / peak))]);
    });
  }

  function buildSummary(build) {
    if (!build) return '';
    return `<section class="db-result"><p>扫描 ${build.scanned} 张 · 归一化处理 ${build.normalized} 张 · 去重 ${build.duplicates} 张 · 入库 ${build.inserted} 张</p>
      ${build.after ? `<div class="color-charts">${colorCharts(build.before, '建库前')}${colorCharts(build.after, '建库后')}</div>` : ''}</section>`;
  }

  function mountCatalog(host, dbPath, defaultSort = 'color') {
    if (!hasFeature('library-visuals')) return;
    const section = document.createElement('details');
    section.className = 'db-result';
    section.innerHTML = `<summary>浏览素材缩略图</summary><div class="preview-toolbar">
      <label>排序 <select><option value="color">颜色</option><option value="usage">累计使用格数</option><option value="similarity">相似度</option></select></label>
      <label>参考素材 ID <input type="number" min="1" step="1"></label>
      <button type="button" class="btn btn-secondary" data-load>查看</button>
      <button type="button" class="btn btn-secondary" data-prev disabled>上一页</button>
      <button type="button" class="btn btn-secondary" data-next disabled>下一页</button></div>
      <p class="hint" role="status"></p><div class="catalog-grid"></div>`;
    const select = section.querySelector('select');
    const anchor = section.querySelector('input');
    const status = section.querySelector('[role=status]');
    const list = section.querySelector('.catalog-grid');
    const previous = section.querySelector('[data-prev]');
    const next = section.querySelector('[data-next]');
    select.value = defaultSort;
    let current = null;
    let revision = 0;

    async function load(offset = null) {
      const version = ++revision;
      status.textContent = '正在读取素材…';
      previous.disabled = next.disabled = true;
      const payload = {dbPath: dbPath || 'library/mosaicraft.db', sort: select.value};
      if (select.value === 'similarity') {
        if (!anchor.valueAsNumber || !anchor.checkValidity()) {
          status.textContent = '请选择或输入一张参考素材的 ID。';
          return;
        }
        payload.anchor = anchor.valueAsNumber;
      }
      const data = offset === null ? await postJson('/api/db/catalog', payload)
        : await getJson(`/api/db/catalog/${encodeURIComponent(current.id)}?offset=${offset}`);
      if (version !== revision || !section.isConnected) return;
      list.replaceChildren();
      current = data.catalog;
      if (!data.ok || !current) {
        status.textContent = data.message || '无法读取素材。';
        return;
      }
      status.textContent = current.total ? `${current.offset + 1}–${current.offset + current.items.length} / ${current.total} 张` : '图库为空';
      previous.disabled = current.offset === 0;
      next.disabled = current.offset + current.items.length >= current.total;
      for (const item of current.items) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'btn btn-secondary';
        button.title = `以 ${item.name} 为参考查找相似素材`;
        if (item.available) {
          const image = document.createElement('img');
          image.loading = 'lazy';
          image.alt = '';
          image.src = `${SERVER_URL}/api/db/catalog/${encodeURIComponent(current.id)}/image/${item.id}-${encodeURIComponent(item.version.replaceAll(':', '-'))}`;
          button.append(image);
        }
        const extra = item.distance === undefined ? (item.tiles === undefined ? '' : ` · ${item.tiles} 格`) : ` · ${item.distance.toFixed(4)}`;
        button.append(`#${item.id}${extra}${item.available ? '' : ' · 缺失'}`);
        button.addEventListener('click', () => {
          anchor.value = item.id;
          select.value = 'similarity';
          void load();
        });
        list.append(button);
      }
    }
    section.querySelector('[data-load]').addEventListener('click', () => void load());
    previous.addEventListener('click', () => void load(Math.max(0, current.offset - 24)));
    next.addEventListener('click', () => void load(current.offset + 24));
    select.addEventListener('change', () => void load());
    section.addEventListener('toggle', () => { if (section.open && !current) void load(); });
    host.append(section);
  }

  coverageButton.addEventListener('click', async () => {
    if (!formModel.valid || formModel.tab !== 'mosaic' || !hasFeature('library-visuals')) return;
    const key = JSON.stringify(formModel.payload);
    coverageResult.textContent = '正在比较颜色和亮度覆盖…';
    coverageButton.disabled = true;
    const data = await postJson('/api/mosaic/coverage', formModel.payload);
    coverageButton.disabled = false;
    if (key !== JSON.stringify(formModel.payload)) {
      coverageResult.textContent = '';
      return;
    }
    const item = data.coverage;
    if (!data.ok || !item) {
      coverageResult.textContent = data.message || '无法计算覆盖。';
      return;
    }
    coverageResult.innerHTML = `<p class="hint">${item.columns} × ${item.rows} 格 · 目标工作图块平均 LAB L ±${item.lRange} · ${item.noCandidates} 格无候选 · ${item.fewCandidates} 格少于 ${item.threshold} 个候选</p>
      <figure><figcaption>亮度候选覆盖 · 红色无候选，黄色偏少，蓝色达到阈值</figcaption><canvas class="color-map" width="${item.mapColumns}" height="${item.mapRows}" role="img" aria-label="目标亮度候选覆盖图"></canvas></figure>
      <div class="color-charts">${colorCharts(item.target, '目标格子')}${colorCharts(item.library, '图库素材')}</div>`;
    canvasPixels(coverageResult.querySelector('canvas'), item.minimumCandidates,
      count => count === 0 ? [205, 94, 108, 255] : count < item.threshold ? [222, 177, 83, 255] : [78, 136, 194, 255]);
    paint(coverageResult);
  });

  return {colorCharts, paint, buildSummary, mountCatalog, clearCoverage() { coverageResult.replaceChildren(); }};
})();
