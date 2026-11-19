const localPathBrowser = (() => {
  const byId = name => document.getElementById('local-path-' + name);
  const dialog = byId('dialog');
  const address = byId('address');
  const parent = byId('parent');
  const root = byId('root');
  const entries = byId('entries');
  const status = byId('status');
  const previous = byId('previous');
  const next = byId('next');
  const select = byId('select');
  const buttons = [...document.querySelectorAll('[data-browse-path]')];
  let target = null;
  let kind = '';
  let page = null;
  let revision = 0;

  function pick(path) {
    target.value = path;
    target.dispatchEvent(new Event('change', {bubbles: true}));
    dialog.close();
  }

  async function load(path, offset = 0) {
    const request = ++revision;
    page = null;
    parent.disabled = true;
    previous.hidden = next.hidden = select.hidden = true;
    entries.replaceChildren();
    status.textContent = '正在读取目录…';
    const data = await postJson('/api/local/paths', {kind, path, offset});
    if (request !== revision || !dialog.open) return;
    if (!data.ok || !data.paths) {
      status.textContent = data.status === 400 ? '无法读取此目录。请检查路径或返回服务目录。' : data.message || '无法读取目录。';
      return;
    }
    page = data.paths;
    address.value = page.directory;
    parent.disabled = !page.parent;
    previous.hidden = !page.offset;
    next.hidden = !page.more;
    select.hidden = kind !== 'directory';
    const defaultRoot = new Option('切换磁盘', '');
    root.replaceChildren(defaultRoot, ...page.roots.map(path => new Option(path, path)));
    root.hidden = !page.roots.length;
    status.textContent = page.entries.length ? '' : (kind === 'image' ? '此目录没有可选图片。' : '此目录没有子目录。');
    for (const entry of page.entries) {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'btn btn-secondary';
      button.textContent = entry.directory ? `目录 · ${entry.name}` : entry.name;
      button.addEventListener('click', () => {
        if (entry.directory) {
          address.focus();
          void load(entry.path);
        } else pick(entry.path);
      });
      entries.append(button);
    }
  }

  for (const button of buttons) {
    button.addEventListener('click', () => {
      target = document.getElementById(button.dataset.browsePath);
      kind = target.id === 'm-input' ? 'image' : 'directory';
      byId('heading').textContent = kind === 'image' ? '选择目标图片' : '选择素材目录';
      address.value = val(target.id);
      root.hidden = true;
      dialog.showModal();
      void load(address.value);
    });
  }
  byId('form').addEventListener('submit', event => {
    event.preventDefault();
    void load(normalizePath(address.value));
  });
  root.addEventListener('change', () => {
    if (root.value) void load(root.value);
  });
  parent.addEventListener('click', () => { if (page?.parent) void load(page.parent); });
  byId('home').addEventListener('click', () => { void load(''); });
  previous.addEventListener('click', () => { if (page) void load(page.directory, Math.max(0, page.offset - 100)); });
  next.addEventListener('click', () => { if (page) void load(page.directory, page.offset + page.entries.length); });
  select.addEventListener('click', () => { if (page) pick(page.directory); });
  byId('cancel').addEventListener('click', () => dialog.close());

  return {
    update() {
      const available = serverAvailable && hasFeature('local-path-browser');
      for (const button of buttons) button.hidden = !available;
    }
  };
})();
