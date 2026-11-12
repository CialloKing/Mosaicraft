// 共用窗口几何和中心优先队列；离线工程按窗口保留位图，不设置大小上限。
const MosaicraftViewport = (() => {
  const cacheBudget = 64 * 1024 * 1024;
  const parallelLoads = 8;
  // 初始浏览尺度独立于放大上限，避免提高上限后首次打开只显示素材局部。
  const initialTileHeight = 256;
  const maxTileHeight = 4096;
  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
  function clip(canvas) {
    if (document.hidden || !canvas.clientWidth || !canvas.clientHeight) return null;
    const rect = canvas.getBoundingClientRect(), visual = window.visualViewport;
    const left = visual?.offsetLeft ?? 0, top = visual?.offsetTop ?? 0;
    const right = left + (visual?.width ?? document.documentElement.clientWidth);
    const bottom = top + (visual?.height ?? document.documentElement.clientHeight);
    const x0 = Math.max(0, left - rect.left), y0 = Math.max(0, top - rect.top);
    const x1 = Math.min(rect.width, right - rect.left), y1 = Math.min(rect.height, bottom - rect.top);
    return x1 > x0 && y1 > y0 ? {x0, y0, x1, y1} : null;
  }
  function geometry(width, height, columns, rows, aspect, scale, centerX, centerY) {
    const tw = scale * aspect, th = scale;
    const worldW = tw * columns, worldH = th * rows;
    const x = worldW <= width ? (width - worldW) / 2 : clamp(width / 2 - centerX * tw, width - worldW, 0);
    const y = worldH <= height ? (height - worldH) / 2 : clamp(height / 2 - centerY * th, height - worldH, 0);
    return {width, height, tw, th, x, y};
  }
  function visible(g, clip, columns, rows) {
    return {
      firstCol: clamp(Math.floor((clip.x0 - g.x) / g.tw), 0, columns),
      lastCol: clamp(Math.ceil((clip.x1 - g.x) / g.tw), 0, columns),
      firstRow: clamp(Math.floor((clip.y0 - g.y) / g.th), 0, rows),
      lastRow: clamp(Math.ceil((clip.y1 - g.y) / g.th), 0, rows)
    };
  }
  function pixelBands(first, last, origin, step, ratio) {
    const bands = [];
    let start = Math.round((origin + first * step) * ratio);
    for (let index = first; index < last; ++index) {
      const end = Math.round((origin + (index + 1) * step) * ratio);
      // 相邻格共用物理像素边界，零像素格不请求图片。
      if (end > start) bands.push({index, start, end});
      start = end;
    }
    return bands;
  }
  function centerFirst(keys, slotsByAsset, visible, priorityKey = '') {
    const cx = (visible.x0 + visible.x1) / 2, cy = (visible.y0 + visible.y1) / 2;
    return Array.from(keys, key => {
      let distance = key === priorityKey ? -1 : Infinity;
      for (const slot of slotsByAsset.get(key) ?? []) {
        distance = Math.min(distance, (slot.x + slot.w / 2 - cx) ** 2 + (slot.y + slot.h / 2 - cy) ** 2);
      }
      return {key, distance};
    }).sort((a, b) => a.distance - b.distance).map(item => item.key);
  }
  function releaseBitmap(bitmap) {
    if (typeof bitmap.close === 'function') bitmap.close();
    else if (typeof bitmap.removeAttribute === 'function') bitmap.removeAttribute('src');
    else bitmap.src = '';
  }
  class BitmapCache {
    constructor(budget = Infinity) { this.budget = budget; this.bytes = 0; this.entries = new Map(); }
    get(key) {
      const item = this.entries.get(key);
      if (!item) return null;
      if (Number.isFinite(this.budget)) { this.entries.delete(key); this.entries.set(key, item); }
      return item.image;
    }
    delete(key) {
      const item = this.entries.get(key);
      if (!item) return;
      this.entries.delete(key); this.bytes -= item.bytes;
      releaseBitmap(item.image);
    }
    retain(keys) {
      for (const key of this.entries.keys()) if (!keys.has(key)) this.delete(key);
    }
    add(key, image) {
      const bytes = (image.naturalWidth ?? image.width) * (image.naturalHeight ?? image.height) * 4;
      if (bytes > this.budget) { releaseBitmap(image); return; }
      const previous = this.entries.get(key);
      if (previous) {
        this.entries.delete(key); this.bytes -= previous.bytes;
        if (previous.image !== image) releaseBitmap(previous.image);
      }
      while (this.bytes + bytes > this.budget && this.entries.size) {
        this.delete(this.entries.keys().next().value);
      }
      this.entries.set(key, {image, bytes}); this.bytes += bytes;
    }
  }
  return {cacheBudget, parallelLoads, initialTileHeight, maxTileHeight, clamp, clip, geometry, visible, pixelBands,
    centerFirst, releaseBitmap, BitmapCache};
})();
