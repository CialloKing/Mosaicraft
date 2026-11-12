function createPreviewInsights(byId, actions) {
  const frequency = byId('frequency');
  const usage = frequency.querySelector('p');
  const ranking = frequency.querySelector('.preview-toolbar');
  const inspection = byId('tile-inspection');
  const inspectButton = byId('inspect-tile');
  const quality = byId('mark-quality');
  const missing = byId('mark-missing');
  const status = byId('insights-status');
  const counts = new Map();
  const scores = new Map();
  const unavailable = new Set();
  let plan = null;
  let highlighted = 0;
  let matched = 0;
  let version = 0;
  let inspectionVersion = 0;
  let inspected = -1;
  let active = false;
  let pending = false;
  let unavailableView = '';

  function bump(id, delta) {
    if (!(id > 0)) return;
    matched += delta;
    const count = (counts.get(id) || 0) + delta;
    if (count > 0) counts.set(id, count);
    else counts.delete(id);
  }

  function drawFrequency() {
    if (!frequency.open) return;
    const top = [...counts.entries()].sort((a, b) => b[1] - a[1] || a[0] - b[0]).slice(0, 10);
    const topCount = top.reduce((sum, [, count]) => sum + count, 0);
    usage.textContent = `${counts.size} 张不同素材 · 平均 ${counts.size ? (matched / counts.size).toFixed(2) : 0} 格/张 · 前 10 名占 ${matched ? (100 * topCount / matched).toFixed(1) : 0}%`;
    ranking.replaceChildren();
    for (const [id, count] of top) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'btn btn-secondary';
      button.setAttribute('aria-pressed', String(highlighted === id));
      button.textContent = `#${id} · ${count} 格`;
      button.addEventListener('click', () => {
        highlighted = highlighted === id ? 0 : id;
        if (highlighted) actions.locate(id);
        for (const peer of ranking.children) peer.setAttribute('aria-pressed', String(peer === button && !!highlighted));
        ++version;
        actions.repaint();
      });
      ranking.append(button);
    }
  }

  function setPlan(next, edits) {
    plan = next;
    counts.clear();
    scores.clear();
    unavailable.clear();
    matched = highlighted = 0;
    inspected = -1;
    inspection.hidden = true;
    ++inspectionVersion;
    ++version;
    unavailableView = '';
    status.textContent = '';
    frequency.hidden = !plan;
    inspectButton.disabled = quality.disabled = !plan || !hasFeature('preview-quality');
    missing.disabled = !plan;
    if (!plan) return;
    for (const id of plan.imageIds) bump(id, 1);
    for (const [position, edit] of edits) {
      bump(plan.imageIds[position], -1);
      bump(edit.imageId, 1);
    }
    drawFrequency();
  }

  function changed(changes) {
    for (const [position, before, after] of changes) {
      bump(before, -1);
      bump(after, 1);
      scores.delete(position);
    }
    ++version;
    drawFrequency();
    if (inspected >= 0 && changes.some(([position]) => position === inspected)) void inspect(inspected);
  }

  function probe(position) {
    return actions.edit(position) || {position};
  }

  async function inspect(position) {
    if (!plan || !active) return;
    inspected = position;
    const current = plan;
    const request = ++inspectionVersion;
    inspection.hidden = false;
    inspection.textContent = '正在读取单格评分…';
    const data = await postJson('/api/mosaic/preview/quality', {previewId: plan.id, positions: [probe(position)], explain: true});
    if (request !== inspectionVersion || plan !== current) return;
    const tile = data.quality?.[0];
    if (!data.ok || !tile) {
      inspection.textContent = data.message || '单格评分暂不可用。';
      return;
    }
    const location = `第 ${Math.floor(position / plan.columns) + 1} 行 · 第 ${position % plan.columns + 1} 列`;
    const decision = tile.manual ? '手动替换' : tile.decision
      ? `在前 ${tile.decision.topN} 名中选中第 ${tile.decision.rank} 名 · 邻域惩罚 ${tile.decision.neighborPenalty.toFixed(3)} · 复用惩罚 ${tile.decision.reusePenalty.toFixed(3)}`
      : '当前格的自动选图排名不可用。';
    const rows = tile.candidates.map(candidate => `<tr><th scope="row">#${candidate.imageId}${candidate.imageId === tile.imageId ? '（入选）' : ''}</th><td>${candidate.score.toFixed(4)}</td>${candidate.features.map(value => `<td>${value.toFixed(4)}</td>`).join('')}</tr>`).join('');
    const thumbs = tile.imageId > 0 ? `${SERVER_URL}/api/mosaic/preview/${encodeURIComponent(plan.id)}/image/${tile.imageId}-128` : '';
    inspection.innerHTML = `<h4>${location}</h4><p>${escapeHtml(decision)}</p><div class="analysis-pair">
      <figure><figcaption>目标裁块</figcaption><img src="${escapeAttr(tile.target)}" alt="目标裁块"></figure>
      <figure><figcaption>入选素材</figcaption>${thumbs ? `<img src="${escapeAttr(thumbs)}" alt="入选素材">` : '无候选'}</figure></div>
      <div class="help-table-wrap"><table class="db-result-table"><thead><tr><th>素材</th><th>误差</th><th>LAB</th><th>网格</th><th>缩略图</th><th>边缘</th><th>LBP</th></tr></thead><tbody>${rows}</tbody></table></div>`;
  }

  async function loadScores(slots, viewKey, dense) {
    if (!quality.checked || dense || pending || !plan || unavailableView === viewKey) return;
    const positions = [];
    for (const {position} of slots) {
      const id = actions.id(position);
      if (id > 0 && scores.get(position)?.imageId !== id) positions.push(probe(position));
      if (positions.length === 256) break;
    }
    if (!positions.length) return;
    pending = true;
    const current = plan;
    const data = await postJson('/api/mosaic/preview/quality', {previewId: plan.id, positions});
    pending = false;
    if (current === plan) {
      if (data.ok && Array.isArray(data.quality)) {
        for (const score of data.quality) scores.set(score.position, score);
        while (scores.size > 32768) scores.delete(scores.keys().next().value);
        status.textContent = '合成前匹配误差 · 蓝色较低，橙色较高。';
        ++version;
      } else if (data.diagnostic?.code === 'library_busy') {
        setTimeout(actions.repaint, 500);
        return;
      } else {
        unavailableView = viewKey;
        status.textContent = data.message || '当前区域无法读取误差。';
      }
    }
    actions.repaint();
  }

  function paintMark(context, {position, x, y, w, h}) {
    const id = actions.id(position);
    const score = scores.get(position);
    if (quality.checked && score?.imageId === id && Number.isFinite(score.score)) {
      const hue = 215 - 190 * Math.max(0, Math.min(1, score.score));
      context.fillStyle = `hsla(${hue},85%,55%,.46)`;
      context.fillRect(x, y, w, h);
    }
    if (missing.checked && (id < 0 || unavailable.has(id))) {
      context.fillStyle = id < 0 ? 'rgba(235,120,122,.65)' : 'rgba(235,179,103,.65)';
      context.fillRect(x, y, w, h);
    }
    if (highlighted && id === highlighted) {
      context.strokeStyle = '#fff5b0';
      context.strokeRect(x + 1, y + 1, Math.max(0, w - 2), Math.max(0, h - 2));
    }
  }

  frequency.addEventListener('toggle', drawFrequency);
  inspectButton.addEventListener('click', () => {
    active = !active;
    ++inspectionVersion;
    inspectButton.setAttribute('aria-pressed', String(active));
    inspection.hidden = !active;
    if (active) inspection.textContent = '点击格子查看目标裁块和候选评分。';
    actions.repaint();
  });
  quality.addEventListener('change', () => { unavailableView = ''; status.textContent = ''; ++version; actions.repaint(); });
  missing.addEventListener('change', () => { ++version; actions.repaint(); });
  return {setPlan, changed, inspect, loadScores, paintMark, checking: () => active,
    activeMarkers: () => missing.checked || quality.checked || highlighted !== 0, revision: () => version,
    unavailable(id) { if (!unavailable.has(id)) { unavailable.add(id); ++version; } }};
}
