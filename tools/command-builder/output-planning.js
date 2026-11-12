const outputPlanning = (() => {
  const panel = document.getElementById('m-output-plan');
  const status = document.getElementById('m-plan-status');
  const result = document.getElementById('m-plan-result');
  const sampleButton = document.getElementById('m-plan-sample');
  const cancelButton = document.getElementById('m-plan-cancel');
  const sampleStatus = document.getElementById('m-plan-sample-status');
  const warningLabels = {
    output_scaled: '显式格式超过像素上限，输出 Tile 将缩小。',
    output_format_changed: '自动推导的格式超过像素上限，将使用 PNG。',
    tile_aspect_mismatch: '输出 Tile 和图库素材的宽高比不同。'
  };
  let requestKey = '';
  let timer = 0;
  let ready = false;
  let sampleId = '';
  let sampleVersion = 0;
  let activeSample = false;
  let sampleKey = '';
  const previewPath = id => '/api/mosaic/preview/' + encodeURIComponent(id);
  const bytes = value => {
    if (!Number.isFinite(value)) return '不可用';
    if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(2)} GiB`;
    if (value >= 1024 ** 2) return `${(value / 1024 ** 2).toFixed(2)} MiB`;
    if (value >= 1024) return `${(value / 1024).toFixed(1)} KiB`;
    return `${value} B`;
  };
  const range = value => value ? `${bytes(value.low)}–${bytes(value.high)}` : '不可用';

  function render(plan) {
    const rows = [
      ['工作图', `${plan.workingWidth} × ${plan.workingHeight}`],
      ['匹配网格', `${plan.columns} × ${plan.rows} · ${plan.tileCount.toLocaleString()} 格`],
      ['成品尺寸', `${plan.width} × ${plan.height} · 输出 Tile ${plan.tileWidth} × ${plan.tileHeight}`],
      ['格式与路径', `${plan.mode === 'deepzoom' ? 'DeepZoom / ' : ''}${plan.format.toUpperCase()} · ${plan.path}`],
      ['未压缩 RGB', bytes(plan.rawBytes)],
      ['匹配 RAM', range(plan.matchingRam)],
      ['匹配显存', range(plan.matchingVram)],
      ['写入 RAM', `${range(plan.writingRam)} · ${plan.writeMode}`],
      ['预计文件体积', `${range(plan.fileBytes)} · 最多 ${plan.files.toLocaleString()} 个文件`]
    ];
    result.innerHTML = `<dl>${rows.map(([label, value]) => `<dt>${label}</dt><dd>${escapeHtml(value)}</dd>`).join('')}</dl>` +
      plan.warnings.map(code => `<p class="hint">${escapeHtml(warningLabels[code] || code)}</p>`).join('');
    status.textContent = 'RAM、显存和文件体积为估算范围。';
  }

  function updatePreview() {
    const key = interactivePreview.outputSampleKey();
    if (sampleKey && sampleKey !== key) cancelSample('预览已变更，显示模型估算。');
    sampleButton.disabled = !ready || !key || !hasFeature('mosaic-size-sample') || activeSample;
    cancelButton.hidden = !activeSample;
  }

  function cancelSample(message = '') {
    ++sampleVersion;
    if (sampleId) void deleteJson(previewPath(sampleId));
    sampleId = '';
    sampleKey = '';
    activeSample = false;
    sampleStatus.textContent = message;
    updatePreview();
  }

  function sync() {
    if (formModel.tab !== 'mosaic') return;
    panel.hidden = !serverAvailable || !hasFeature('mosaic-plan');
    const payload = !panel.hidden && formModel.valid ? formModel.payload : null;
    const key = payload ? JSON.stringify(payload) : '';
    if (key === requestKey) {
      updatePreview();
      return;
    }
    requestKey = key;
    libraryVisuals.clearCoverage();
    ready = false;
    clearTimeout(timer);
    cancelSample();
    result.replaceChildren();
    status.textContent = key ? '正在读取输出规划…' : '填写目标图片和图库后显示。';
    if (!key) return;
    timer = setTimeout(async () => {
      const data = await postJson('/api/mosaic/plan', payload);
      if (key !== requestKey) return;
      if (!data.ok || !data.plan) {
        status.textContent = data.message || '无法读取输出规划。';
        return;
      }
      render(data.plan);
      ready = true;
      updatePreview();
    }, 350);
  }

  async function pollSample(version) {
    const data = await getJson(previewPath(sampleId));
    if (version !== sampleVersion) return;
    const preview = data.preview;
    if (preview?.state === 'queued' || preview?.state === 'running') {
      setTimeout(() => { if (version === sampleVersion) void pollSample(version); }, 500);
      return;
    }
    if (!preview?.fileSample) {
      cancelSample('当前预览无法完成采样，显示模型估算。');
      return;
    }
    const sample = preview.fileSample;
    activeSample = false;
    sampleStatus.textContent = `采样估算 ${range(sample.bytes)} · ${sample.regions} 个区域`;
    void deleteJson(previewPath(sampleId));
    sampleId = '';
    updatePreview();
  }

  sampleButton.addEventListener('click', async () => {
    if (sampleButton.disabled) return;
    const payload = {...formModel.payload};
    interactivePreview.bind(payload);
    const version = ++sampleVersion;
    sampleKey = interactivePreview.outputSampleKey();
    activeSample = true;
    sampleStatus.textContent = '正在用当前预览采样…';
    updatePreview();
    const data = await postJson('/api/mosaic/sample-size', payload);
    const id = data.preview?.id;
    if (version !== sampleVersion) {
      if (id) void deleteJson(previewPath(id));
      return;
    }
    if (!id) {
      cancelSample(data.message || '无法采样，显示模型估算。');
      return;
    }
    sampleId = id;
    void pollSample(version);
  });
  cancelButton.addEventListener('click', () => cancelSample('已取消采样，显示模型估算。'));
  return {sync, updatePreview};
})();
