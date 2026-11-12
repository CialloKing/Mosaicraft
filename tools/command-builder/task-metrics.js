const taskMetrics = (() => {
  const rates = new Map();
  const timings = {
    annLoadMs: 'ANN 加载', annBuildMs: 'ANN 构建', annQueryMs: 'ANN 查询', featureMs: '特征提取',
    cpuScoreMs: 'CPU 评分处理', gpuScoreMs: 'GPU 评分', neighborMs: '邻域去重', sortMs: '候选排序',
    matchingMs: '匹配阶段', placementMs: '素材拼贴', encodingMs: '编码写入', analysisMs: '质量分析',
    normalizeMs: '归一化', libraryFeaturesMs: '图库特征', reorderMs: '颜色重排', cacheMs: '图库缓存'
  };
  const resourceLabels = {
    cpuPct: '进程 CPU（单核 100%）', rssBytes: '进程驻留内存', gpuPct: '设备 GPU 利用率',
    deviceVramBytes: '设备已用显存', processVramBytes: '进程显存'
  };

  function rate(id, progress) {
    const activity = progress?.activities?.find(item => item.unit === 'tiles' || item.id === 'matching');
    if (!activity || !Number.isFinite(activity.completed) || !progress.updatedAt) return null;
    const key = `${id}:${progress.stage}:${progress.startedAt}:${activity.id}`;
    const previous = rates.get(key);
    if (previous?.time === progress.updatedAt) return previous.value;
    const seconds = (progress.updatedAt - (previous?.time || progress.startedAt)) / 1000;
    const delta = activity.completed - (previous?.completed || 0);
    const value = seconds > 0 && delta >= 0 ? delta / seconds : null;
    rates.set(key, {time: progress.updatedAt, completed: activity.completed, value});
    if (rates.size > 100) rates.delete(rates.keys().next().value);
    return value;
  }

  function resourceChart(samples, summary, key) {
    const label = resourceLabels[key];
    if (!summary) return `<p>${label} · 不可用</p>`;
    const valid = samples.filter(sample => Number.isFinite(sample[key]));
    const start = samples[0].elapsedMs;
    const span = Math.max(1, samples.at(-1).elapsedMs - start);
    const max = Math.max(1, summary.peak);
    const path = valid.map((sample, index) => `${index ? 'L' : 'M'}${(100 * (sample.elapsedMs - start) / span).toFixed(2)},${(38 - 36 * sample[key] / max).toFixed(2)}`).join(' ');
    const number = value => key.endsWith('Bytes') ? `${(value / (1024 * 1024)).toFixed(1)} MiB` : `${value.toFixed(1)}%`;
    const plot = valid.length > 1 ? `<svg class="task-metric-plot" viewBox="0 0 100 40" preserveAspectRatio="none" role="img" aria-label="${label}随时间变化"><path d="${path}" fill="none" stroke="currentColor" vector-effect="non-scaling-stroke" stroke-width="1.5"/></svg>` : '';
    return `<section><p>${label} · 峰值 ${number(summary.peak)} · 均值 ${number(summary.mean)}</p>${plot}</section>`;
  }

  function render(job) {
    const metrics = job.runtime?.metrics || {};
    const samples = job.runtime?.resources?.samples || [];
    const summary = job.runtime?.resources?.summary || {};
    const measured = Object.entries(timings).filter(([key]) => Number.isFinite(metrics[key]) && metrics[key] > 0);
    const peak = Math.max(1, ...measured.map(([key]) => metrics[key]));
    const ms = value => value >= 1000 ? `${(value / 1000).toFixed(2)} 秒` : `${value.toFixed(1)} 毫秒`;
    const rateNow = job.state === 'running' ? rate(job.id, job.progress) : null;
    let html = Number.isFinite(rateNow) ? `<p>处理速度 · ${rateNow.toFixed(1)} 格/秒</p>` : '';
    if (Number.isFinite(metrics.totalMs)) html += `<p>总耗时 · ${ms(metrics.totalMs)}</p>`;
    if (measured.length) {
      html += `<details data-section="timings"><summary>阶段耗时</summary><p class="hint">阶段可重叠，分别计时。</p>${measured.map(([key,label]) =>
        `<div class="task-metric-row"><span>${label}</span><span class="db-rank-track" aria-hidden="true"><span class="db-rank-fill" style="width:${100 * metrics[key] / peak}%"></span></span><span>${ms(metrics[key])}</span></div>`).join('')}</details>`;
    }
    if (samples.length || job.request?.resourceMonitor) {
      html += `<details data-section="resources"><summary>资源监控</summary>${samples.length
        ? `<div class="color-charts">${Object.keys(resourceLabels).map(key => resourceChart(samples, summary[key], key)).join('')}</div>`
        : '<p class="hint">暂无资源样本。</p>'}</details>`;
    }
    return html;
  }

  return {rate, render};
})();
