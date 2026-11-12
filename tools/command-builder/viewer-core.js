// 预览和离线作品共用几何及缓存预算，坐标单位为图块。
const MosaicraftViewport = (() => {
  const cacheBudget = 64 * 1024 * 1024;
  const parallelLoads = 8;
  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
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
  class BitmapCache {
    constructor(budget = cacheBudget) { this.budget = budget; this.bytes = 0; this.entries = new Map(); }
    get(key) {
      const item = this.entries.get(key);
      if (!item) return null;
      this.entries.delete(key); this.entries.set(key, item);
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
  return {cacheBudget, parallelLoads, clamp, geometry, visible, BitmapCache};
})();
