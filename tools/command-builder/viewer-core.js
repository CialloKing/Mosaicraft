// 共用视口几何；实时预览保留自己的预算，离线工程缓存默认不设上限。
const MosaicraftViewport = (() => {
  const cacheBudget = 64 * 1024 * 1024;
  const parallelLoads = 8;
  const maxTileHeight = 256;
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
  class BitmapCache {
    constructor(budget = Infinity) { this.budget = budget; this.bytes = 0; this.entries = new Map(); }
    get(key) {
      const item = this.entries.get(key);
      if (!item) return null;
      if (Number.isFinite(this.budget)) { this.entries.delete(key); this.entries.set(key, item); }
      return item.image;
    }
    add(key, image) {
      const bytes = image.naturalWidth * image.naturalHeight * 4;
      if (bytes > this.budget) { image.src = ''; return; }
      const previous = this.entries.get(key);
      if (previous) {
        this.entries.delete(key); this.bytes -= previous.bytes;
        if (previous.image !== image) previous.image.src = '';
      }
      while (this.bytes + bytes > this.budget && this.entries.size) {
        const [oldKey, old] = this.entries.entries().next().value;
        this.entries.delete(oldKey); this.bytes -= old.bytes;
        old.image.src = '';
      }
      this.entries.set(key, {image, bytes}); this.bytes += bytes;
    }
  }
  return {cacheBudget, parallelLoads, maxTileHeight, clamp, clip, geometry, visible, pixelBands, BitmapCache};
})();
