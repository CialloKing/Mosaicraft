const renderTaskAnalysis = (() => {
  const host = document.getElementById('task-analysis');
  const low = [67, 127, 202];
  const high = [245, 179, 106];
  const missing = [69, 75, 94];
  const palette = Array.from({length: 256}, (_, value) =>
    [...low.map((channel, index) => Math.round(channel + (high[index] - channel) * value / 255)), 255]);
  const rgb = channels => `rgb(${channels.join(',')})`;
  let renderedKey = '';

  function validAnalysis(data) {
    const {grid, scores, heatmap, worst} = data;
    const positiveInteger = value => Number.isSafeInteger(value) && value > 0;
    return grid && positiveInteger(grid.columns) && positiveInteger(grid.rows) &&
      scores && ['min', 'median', 'p90', 'p99', 'max'].every(key => Number.isFinite(scores[key])) &&
      heatmap && heatmap.columns === Math.min(256, grid.columns) && heatmap.rows === Math.min(256, grid.rows) &&
      Array.isArray(heatmap.bins) && heatmap.bins.length === heatmap.columns * heatmap.rows &&
      heatmap.bins.every(value => Number.isInteger(value) && value >= -1 && value <= 255) &&
      Array.isArray(worst) && worst.length <= 20 && worst.every(tile => tile &&
        Number.isInteger(tile.row) && tile.row >= 0 && tile.row < grid.rows &&
        Number.isInteger(tile.column) && tile.column >= 0 && tile.column < grid.columns &&
        Number.isFinite(tile.score) && Number.isSafeInteger(tile.imageId));
  }

  function showHeatmap(section, heatmap, aspectRatio) {
    const canvas = section.querySelector('canvas');
    canvas.width = heatmap.columns;
    canvas.height = heatmap.rows;
    canvas.parentElement.style.aspectRatio = aspectRatio;
    const context = canvas.getContext('2d');
    if (context) {
      const pixels = context.createImageData(canvas.width, canvas.height);
      const missingPixel = [...missing, 255];
      heatmap.bins.forEach((value, index) => pixels.data.set(palette[value] || missingPixel, index * 4));
      context.putImageData(pixels, 0, 0);
    }
    section.querySelector('.analysis-gradient').style.background = `linear-gradient(90deg, ${rgb(low)}, ${rgb(high)})`;
    section.querySelector('.analysis-missing').style.background = rgb(missing);
  }

  function thumbnail(job, images, id, title) {
    const figure = document.createElement('figure');
    const label = document.createElement('figcaption');
    label.textContent = title;
    const hint = document.createElement('p');
    hint.className = 'hint';
    hint.textContent = '缩略图不可用。';
    figure.append(label, hint);
    if (images.has(id)) {
      const image = document.createElement('img');
      image.alt = title;
      image.decoding = 'async';
      hint.hidden = true;
      image.addEventListener('error', () => {
        image.remove();
        hint.hidden = false;
      }, {once: true});
      figure.append(image);
      image.src = SERVER_URL + artifactUrl(job.id, id);
    }
    return figure;
  }

  function showAnalysis(section, job, data) {
    const {grid, scores, heatmap, worst} = data;
    const images = new Set(job.artifacts.filter(item => item.kind === 'analysis-image').map(item => item.id));
    section.innerHTML = `<p class="hint">分数衡量合成前的选图偏差，分数越高，偏差越大。</p>
      <p class="analysis-scores"></p>
      <div class="analysis-layout">
        <div><div class="analysis-map"><canvas role="img" aria-label="选图误差热力图"></canvas>
          <span class="analysis-map-pin" aria-hidden="true" hidden></span></div>
          <div class="analysis-legend"><span>${scores.min.toFixed(4)} 低</span><span class="analysis-gradient" aria-hidden="true"></span><span>${scores.max.toFixed(4)} 高</span>
            <span><span class="analysis-missing" aria-hidden="true"></span>无分数</span></div></div>
        <div><h4>误差最大的格子</h4><ol class="analysis-worst"></ol></div>
      </div>
      <p class="analysis-selection hint" role="status"></p><div class="analysis-pair"></div>`;
    section.querySelector('.analysis-scores').textContent =
      `中位数 ${scores.median.toFixed(4)} · P90 ${scores.p90.toFixed(4)} · P99 ${scores.p99.toFixed(4)}`;
    if (Array.isArray(scores.histogram) && scores.histogram.length === 32 && scores.histogram.every(Number.isFinite)) {
      const peak = Math.max(1, ...scores.histogram);
      const histogram = document.createElement('details');
      histogram.innerHTML = `<summary>逐格分数分布</summary><div class="color-hue-bars" aria-hidden="true">${scores.histogram.map(count =>
        `<span style="height:${100 * count / peak}%;background:var(--accent)"></span>`).join('')}</div>
        <p class="hint">${scores.min.toFixed(4)}–${scores.max.toFixed(4)} · 32 档格数 · ${scores.histogram.join(' / ')}</p>`;
      section.querySelector('.analysis-scores').after(histogram);
    }
    const {width, height} = job.runtime || {};
    const aspectRatio = width > 0 && height > 0 ? `${width} / ${height}` : `${grid.columns} / ${grid.rows}`;
    showHeatmap(section, heatmap, aspectRatio);
    const list = section.querySelector('.analysis-worst');
    const pin = section.querySelector('.analysis-map-pin');
    const pair = section.querySelector('.analysis-pair');
    const selection = section.querySelector('.analysis-selection');
    pin.style.width = `${100 / heatmap.columns}%`;
    pin.style.height = `${100 / heatmap.rows}%`;
    let selected = null;
    worst.forEach((tile, index) => {
      const item = document.createElement('li');
      const button = document.createElement('button');
      const location = `第 ${tile.row + 1} 行 · 第 ${tile.column + 1} 列`;
      button.type = 'button';
      button.className = 'btn btn-secondary';
      button.textContent = `${index + 1}. ${location} · ${tile.score.toFixed(4)}`;
      button.setAttribute('aria-pressed', 'false');
      button.addEventListener('click', () => {
        selected?.setAttribute('aria-pressed', 'false');
        button.setAttribute('aria-pressed', 'true');
        selected = button;
        // 定位沿用服务端的向下取整规则，缩小后的同一区域可包含多个原始格子。
        const column = Math.floor(tile.column * heatmap.columns / grid.columns);
        const row = Math.floor(tile.row * heatmap.rows / grid.rows);
        pin.style.left = `${100 * (column + .5) / heatmap.columns}%`;
        pin.style.top = `${100 * (row + .5) / heatmap.rows}%`;
        pin.hidden = false;
        selection.textContent = `${location} · 素材 #${tile.imageId}`;
        pair.replaceChildren(thumbnail(job, images, tile.targetArtifactId, '目标裁块'),
          thumbnail(job, images, tile.matchArtifactId, '入选素材'));
      });
      item.append(button);
      list.append(item);
    });
  }

  return job => {
    const artifact = job?.artifacts?.find(item => item.id === 'analysis-data');
    const state = artifact ? 'ready' : job?.request?.analyze ? job.state : '';
    const key = state ? `${job.id}:${state}` : '';
    if (key === renderedKey) return;
    renderedKey = key;
    host.replaceChildren();
    if (!state) return;
    if (!artifact) {
      const hint = document.createElement('p');
      hint.className = 'hint';
      hint.textContent = ['queued', 'running'].includes(state) ? '质量分析完成后将在这里显示。' : '未保存可视化质量分析数据。';
      host.append(hint);
      return;
    }
    const details = document.createElement('details');
    details.className = 'task-analysis';
    details.innerHTML = '<summary>质量分析</summary><div class="analysis-content"></div>';
    const content = details.querySelector('.analysis-content');
    let loading = false;
    let loaded = false;
    // 此区域独立于轮询刷新的任务详情，保留展开和格子选择状态。
    details.addEventListener('toggle', async () => {
      if (!details.open || loading || loaded) return;
      loading = true;
      content.textContent = '正在读取质量分析…';
      const data = await getJson(artifactUrl(job.id, artifact.id));
      loading = false;
      if (!details.isConnected) return;
      if (data.ok === false || !validAnalysis(data)) {
        content.textContent = '质量分析数据不可用，文件可能已删除或改写。';
        return;
      }
      loaded = true;
      showAnalysis(content, job, data);
    });
    host.append(details);
  };
})();
