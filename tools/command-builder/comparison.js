function comparisonTable(rows) {
  return `<div class="help-table-wrap"><table class="db-result-table comparison-data"><thead><tr><th scope="col">指标</th><th scope="col">A</th><th scope="col">B</th></tr></thead>
    <tbody>${rows.map(([label, a, b]) => `<tr><th scope="row">${escapeHtml(label)}</th><td>${escapeHtml(a ?? '—')}</td><td>${escapeHtml(b ?? '—')}</td></tr>`).join('')}</tbody></table></div>`;
}

const previewComparison = (() => {
  const panel = document.getElementById('preview-comparison');
  const status = document.getElementById('ab-status');
  const result = document.getElementById('ab-result');
  const run = document.getElementById('ab-run');
  const cancel = document.getElementById('ab-cancel');
  const buttons = [document.getElementById('ab-pin-a'), document.getElementById('ab-pin-b')];
  const slots = [null, null];
  let request = 0;
  let running = false;
  let jobId = '';
  let session = '';
  let pinBusy = false;

  const jobPath = id => '/api/mosaic/preview/' + encodeURIComponent(id);
  const present = () => slots.map((slot, index) => `${index ? 'B' : 'A'} ${slot ? '已保存' : '待保存'}`).join(' · ');

  function update() {
    const server = serviceInfo?.runtime?.sessionId;
    if (server && session && server !== session) {
      slots.fill(null);
      stop();
      result.replaceChildren();
      status.textContent = '';
    }
    if (server) session = server;
    panel.hidden = !hasFeature('preview-comparison');
    const input = interactivePreview.comparisonInput();
    for (const button of buttons) button.disabled = !input || pinBusy;
    run.disabled = !slots[0] || !slots[1] || running || pinBusy;
    cancel.hidden = !running;
  }

  function stop() {
    ++request;
    if (jobId) void deleteJson(jobPath(jobId));
    jobId = '';
    running = false;
  }

  function show(data, left, right) {
    const total = left.columns * left.rows;
    const percent = count => `${(100 * count / total).toFixed(1)}%`;
    const fixed = value => Number.isFinite(value) ? value.toFixed(4) : null;
    const performance = slot => {
      const elapsed = Number.isFinite(slot.metrics.matchingMs) ? `${(slot.metrics.matchingMs / 1000).toFixed(3)} 秒` : '耗时不可用';
      const warm = ['previewIndexReused', 'previewCandidatesReused', 'previewScoresReused'].filter(key => slot.metrics[key] === 1).length;
      return `${elapsed} · ${matchingBackendName(slot.featureBackend)} / ${matchingBackendName(slot.scoreBackend)} · 复用 ${warm} 项缓存`;
    };
    result.innerHTML = `<p>选图变化 ${data.changed} / ${total} 格 · 共同评分 ${data.compared} 格</p>
      ${comparisonTable([
        ['固定权重平均误差', fixed(data.a.mean), fixed(data.b.mean)], ['固定权重 P90', fixed(data.a.p90), fixed(data.b.p90)],
        ['匹配覆盖率', percent(data.a.matched), percent(data.b.matched)], ['唯一素材数', data.a.unique, data.b.unique],
        ['重复占比（按匹配格）', data.a.matched ? `${(100 * (1 - data.a.unique / data.a.matched)).toFixed(1)}%` : null,
          data.b.matched ? `${(100 * (1 - data.b.unique / data.b.matched)).toFixed(1)}%` : null],
        ['匹配耗时及后端', performance(left), performance(right)]
      ])}
      <div class="preview-toolbar"><label>对照方式 <select data-ab-mode><option value="side">并排</option><option value="split">分割滑块</option></select></label>
        <label><input type="checkbox" data-ab-changes> 高亮选图变化</label><input type="range" min="0" max="100" value="50" aria-label="A/B 分割位置" hidden></div>
      <div class="ab-overviews"><figure><figcaption>A</figcaption><div class="ab-image"><img alt="方案 A 概览"></div></figure>
      <figure><figcaption>B</figcaption><div class="ab-image"><img alt="方案 B 概览"><canvas class="ab-layer" aria-hidden="true" hidden></canvas></div></figure></div>
      <div class="ab-image" data-ab-split hidden><img alt="方案 A 概览"><img class="ab-layer" alt="方案 B 概览"><canvas class="ab-layer" aria-hidden="true" hidden></canvas></div>
      <p class="hint">概览按区域缩略显示。橙色覆盖表示区域内至少一个格子发生变化。</p>`;
    const side = result.querySelector('.ab-overviews');
    const split = result.querySelector('[data-ab-split]');
    const slider = result.querySelector('input[type=range]');
    const mode = result.querySelector('[data-ab-mode]');
    const changes = result.querySelector('[data-ab-changes]');
    for (const view of [side, split]) {
      const images = view.querySelectorAll('img');
      images[0].src = data.a.image;
      images[1].src = data.b.image;
      const canvas = view.querySelector('canvas');
      canvas.width = data.mapColumns;
      canvas.height = data.mapRows;
      const context = canvas.getContext('2d');
      const pixels = context.createImageData(canvas.width, canvas.height);
      data.changedMap.forEach((marked, index) => {
        if (marked) pixels.data.set([244, 161, 78, 156], index * 4);
      });
      context.putImageData(pixels, 0, 0);
    }
    function paint() {
      side.hidden = mode.value !== 'side';
      split.hidden = slider.hidden = mode.value !== 'split';
      split.querySelector('img.ab-layer').style.clipPath = `inset(0 0 0 ${slider.value}%)`;
      for (const view of [side, split]) view.querySelector('canvas').hidden = !changes.checked;
    }
    mode.addEventListener('change', paint);
    changes.addEventListener('change', paint);
    slider.addEventListener('input', paint);
    paint();
  }

  async function poll(version, left, right) {
    const data = await getJson(jobPath(jobId));
    if (version !== request) return;
    const preview = data.preview;
    if (preview?.state === 'queued' || preview?.state === 'running') {
      setTimeout(() => { if (version === request) void poll(version, left, right); }, 550);
      return;
    }
    stop();
    update();
    if (!preview?.comparison) {
      status.textContent = preview?.result?.message || data.message || '对比不可用。';
      return;
    }
    status.textContent = present();
    show(preview.comparison, left, right);
  }

  buttons.forEach((button, index) => button.addEventListener('click', async () => {
    const input = interactivePreview.comparisonInput();
    if (!input) return;
    pinBusy = true;
    update();
    const data = await postJson('/api/mosaic/preview/pin', {slot: index ? 'B' : 'A', previewId: input.previewId});
    pinBusy = false;
    if (data.ok) {
      slots[index] = input;
      stop();
      result.replaceChildren();
    }
    status.textContent = data.ok ? present() : data.message || '无法保存快照。';
    update();
  }));
  run.addEventListener('click', async () => {
    const [left, right] = slots;
    if (!left || !right) return;
    if (left.editKey !== right.editKey || left.seed !== right.seed) {
      status.textContent = '请选择同一目标、图库、网格与随机种子的两个预览。';
      return;
    }
    const fields = slot => ({previewId: slot.previewId, previewEdits: slot.previewEdits});
    const version = ++request;
    running = true;
    result.replaceChildren();
    update();
    status.textContent = '正在用固定权重比较两个预览…';
    const data = await postJson('/api/mosaic/preview/compare', {a: fields(left), b: fields(right)});
    if (version !== request) {
      if (data.preview?.id) void deleteJson(jobPath(data.preview.id));
      return;
    }
    if (!data.preview?.id) {
      stop();
      update();
      status.textContent = data.message || '无法开始对比。';
      return;
    }
    jobId = data.preview.id;
    void poll(version, left, right);
  });
  cancel.addEventListener('click', () => { stop(); status.textContent = '已取消比较。'; update(); });
  document.getElementById('ab-clear').addEventListener('click', async () => {
    stop();
    const response = await Promise.all(['A', 'B'].map(slot => postJson('/api/mosaic/preview/pin', {slot, previewId: ''})));
    if (response.every(item => item.ok)) {
      slots.fill(null);
      result.replaceChildren();
      status.textContent = present();
    } else status.textContent = '无法清空全部快照。';
    update();
  });
  return {update, seed: () => slots[0]?.seed ?? slots[1]?.seed};
})();

const taskHistory = (() => {
  const byId = name => document.getElementById(`history-${name}`);
  const panel = document.getElementById('task-history');
  const type = byId('type');
  const state = byId('state');
  const selectors = [byId('a'), byId('b')];
  const restore = [byId('restore-a'), byId('restore-b')];
  const status = byId('status');
  const result = byId('result');
  let records = [];
  let version = 0;

  const selected = index => records.find(item => item.key === selectors[index].value);
  const formatTime = value => value > 0 ? new Date(value * 1000).toLocaleString() : '时间未知';
  const metric = (value, unit = '', precision = 2) => Number.isFinite(value) ? `${value.toFixed(precision)}${unit}` : null;
  function compare() {
    const [a, b] = selectors.map((_, index) => selected(index));
    [a, b].forEach((job, index) => restore[index].disabled = !job?.request);
    if (!a && !b) {
      result.replaceChildren();
      return;
    }
    const output = job => job?.outputs?.find(item => ['main', 'library', 'tiles', 'manifest'].includes(item.id));
    const quality = job => job?.runtime?.quality;
    const time = job => job?.runtime?.metrics?.totalMs ?? (job?.startedAt > 0 ? (job.finishedAt - job.startedAt) * 1000 : null);
    const resource = (job, key, statistic = 'peak') => job?.runtime?.resources?.summary?.[key]?.[statistic];
    const bytes = value => Number.isFinite(value) ? metric(value / (1024 * 1024), ' MiB', 1) : null;
    const dimensions = job => job?.runtime?.width > 0 ? `${job.runtime.width} × ${job.runtime.height}` : null;
    const rows = [
      ['时间', a && formatTime(a.finishedAt), b && formatTime(b.finishedAt)],
      ['目标或输入目录', a?.inputPath, b?.inputPath], ['实际输出', output(a)?.path, output(b)?.path],
      ['输出尺寸', dimensions(a), dimensions(b)], ['输出体积', bytes(output(a)?.bytes), bytes(output(b)?.bytes)],
      ['评分后端', a?.runtime?.scoreBackend, b?.runtime?.scoreBackend],
      ['总耗时', metric(time(a), ' ms', 1), metric(time(b), ' ms', 1)],
      ['原任务平均误差', metric(quality(a)?.mean, '', 4), metric(quality(b)?.mean, '', 4)],
      ['原任务 P90', metric(quality(a)?.p90, '', 4), metric(quality(b)?.p90, '', 4)],
      ['进程内存峰值', bytes(resource(a, 'rssBytes')), bytes(resource(b, 'rssBytes'))],
      ['设备显存峰值', bytes(resource(a, 'deviceVramBytes')), bytes(resource(b, 'deviceVramBytes'))],
      ['进程 CPU 均值', metric(resource(a, 'cpuPct', 'mean'), '%', 1), metric(resource(b, 'cpuPct', 'mean'), '%', 1)]
    ];
    result.innerHTML = comparisonTable(rows) + '<p class="hint">历史误差按各自的运行参数记录。需要统一评分时，可恢复参数后使用预览 A/B。</p>';
  }

  function filter() {
    const visible = records.filter(item => (!type.value || item.type === type.value) && (!state.value || item.state === state.value));
    selectors.forEach(select => {
      const previous = select.value;
      select.replaceChildren(new Option('选择任务', ''), ...visible.map(item => new Option(
        `${formatTime(item.finishedAt)} · ${jobTypeLabel(item.type)} · ${item.state} · ${item.id}`, item.key)));
      if (visible.some(item => item.key === previous)) select.value = previous;
    });
    status.textContent = visible.length ? `${visible.length} 条 · 本地保留最近 100 个已结束任务` : '暂无符合条件的历史任务。';
    compare();
  }

  async function load() {
    const current = ++version;
    status.textContent = '正在读取本地历史…';
    const data = await getJson('/api/history');
    if (current !== version) return;
    if (!data.ok || !Array.isArray(data.history)) {
      status.textContent = data.message || '本地历史不可用。';
      return;
    }
    records = data.history;
    filter();
  }

  panel.addEventListener('toggle', () => { if (panel.open) void load(); });
  byId('refresh').addEventListener('click', () => void load());
  byId('clear').addEventListener('click', async () => {
    if (!confirm('清除本地任务历史摘要？')) return;
    const data = await deleteJson('/api/history');
    if (data.ok) void load();
    else status.textContent = data.message || '无法清除历史。';
  });
  type.addEventListener('change', filter);
  state.addEventListener('change', filter);
  selectors.forEach(select => select.addEventListener('change', compare));
  restore.forEach((button, index) => button.addEventListener('click', () => restoreRequestParameters(selected(index))));
  return {update() { panel.hidden = !hasFeature('task-history'); }};
})();
