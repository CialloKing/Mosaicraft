function createPreviewPicker(byId, actions) {
  const pane = byId('picker');
  const edit = byId('edit');
  const tile = byId('tile');
  const undo = byId('undo');
  const reset = byId('reset-edits');
  const options = byId('picker-options');
  const recommended = byId('recommended');
  const library = byId('library');
  const search = byId('search');
  const searchField = byId('search-field');
  const status = byId('picker-status');
  const items = byId('items');
  const pages = byId('pages');
  const previous = byId('prev');
  const next = byId('next');
  const page = byId('page');
  const pageTotal = byId('page-total');
  const pageGo = byId('page-go');
  const pageSize = 24;
  let active = false;
  let usable = false;
  let visible = false;
  let plan = null;
  let position = -1;
  let browse = false;
  let offset = 0;
  let totalPages = 0;
  let generation = 0;
  let timer = 0;
  let loadedKey = '';
  const thumbs = window.IntersectionObserver ? new IntersectionObserver(entries => {
    for (const entry of entries) {
      if (entry.isIntersecting && entry.target.isConnected) {
        entry.target.src = entry.target.dataset.src;
        thumbs.unobserve(entry.target);
      }
    }
  }) : null;

  function load(delay = 0) {
    const key = visible && active && usable && position >= 0
      ? JSON.stringify([plan.id, position, browse, search.value.trim(), offset]) : '';
    if (key === loadedKey) return;
    loadedKey = key;
    const revision = ++generation;
    clearTimeout(timer);
    thumbs?.disconnect();
    items.replaceChildren();
    status.textContent = key ? '加载中…' : '';
    previous.disabled = next.disabled = true;
    page.disabled = pageGo.disabled = true;
    if (!key) return;

    timer = setTimeout(async () => {
      const base = '/api/mosaic/preview/' + plan.id;
      const path = browse ? '/library?q=' + encodeURIComponent(search.value.trim()) + '&offset=' + offset
        : '/recommendations?position=' + position;
      const data = await getJson(base + path);
      if (revision !== generation) return;
      const list = browse ? data.library : data.recommendations;
      if (!data.ok || !list) {
        status.textContent = data.message || '素材列表加载失败。';
        if (data.status === 404) actions.stale();
        return;
      }
      previous.disabled = offset === 0;
      next.disabled = !list.more;
      if (browse) {
        totalPages = Math.ceil(list.total / pageSize);
        page.setCustomValidity('');
        page.value = totalPages ? String(Math.floor(offset / pageSize) + 1) : '';
        page.max = String(Math.max(1, totalPages));
        pageTotal.textContent = '页 / 共 ' + totalPages + ' 页';
        page.disabled = pageGo.disabled = totalPages === 0;
      }
      status.textContent = list.images.length ? '' :
        browse ? '没有找到素材。' : '此格暂无推荐，可从整个图库选图。';
      const cards = document.createDocumentFragment();
      for (const image of list.images) {
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'preview-picker-item';
        button.dataset.imageId = image.id;
        button.disabled = !image.available;
        button.title = image.name + ' · #' + image.id;
        button.setAttribute('aria-label', '选用素材 ' + image.name + '，编号 ' + image.id);
        const thumb = document.createElement('img');
        thumb.alt = '';
        thumb.loading = 'lazy';
        thumb.decoding = 'async';
        if (image.available) {
          const url = SERVER_URL + base + '/image/' + image.id + '-128';
          if (thumbs) {
            thumb.dataset.src = url;
            thumbs.observe(thumb);
          } else {
            thumb.src = url;
          }
        }
        const label = document.createElement('span');
        label.textContent = '#' + image.id + ' ' + image.name;
        button.append(thumb, label);
        button.addEventListener('click', () => {
          if (revision === generation && usable) actions.replace(position, image);
        });
        cards.appendChild(button);
      }
      items.appendChild(cards);
      show();
    }, delay);
  }

  function show() {
    edit.disabled = !usable;
    edit.setAttribute('aria-pressed', String(active && usable));
    pane.hidden = !visible || !active || !plan;
    const current = position < 0 ? null : actions.current(position);
    undo.disabled = !usable || !current?.edited;
    reset.disabled = !usable || actions.count() === 0;
    options.hidden = !usable || !current;
    searchField.hidden = pages.hidden = !browse;
    recommended.setAttribute('aria-pressed', String(!browse));
    library.setAttribute('aria-pressed', String(browse));
    if (!usable) tile.textContent = '等待当前预览更新。';
    else if (!current) tile.textContent = '点击画布中的格子。';
    else {
      const row = Math.floor(position / plan.columns) + 1;
      const col = position % plan.columns + 1;
      tile.textContent = '第 ' + row + ' 行 · 第 ' + col + ' 列 · ' +
        (current.id > 0 ? '素材 #' + current.id : '无匹配素材') + (current.edited ? ' · 手动' : '');
    }
    for (const item of items.children) {
      item.setAttribute('aria-pressed', String(Number(item.dataset.imageId) === current?.id));
    }
  }

  function chooseSource(all) {
    if (browse === all) return;
    browse = all;
    offset = 0;
    show();
    load();
  }

  edit.addEventListener('click', () => {
    active = !active;
    show();
    load();
    actions.paint();
  });
  undo.addEventListener('click', () => actions.undo(position));
  reset.addEventListener('click', actions.clear);
  recommended.addEventListener('click', () => chooseSource(false));
  library.addEventListener('click', () => chooseSource(true));
  search.addEventListener('input', () => {
    offset = 0;
    load(200);
  });
  previous.addEventListener('click', () => {
    offset = Math.max(0, offset - pageSize);
    load();
  });
  next.addEventListener('click', () => {
    offset += pageSize;
    load();
  });
  function goToPage() {
    const value = Number(page.value);
    if (!Number.isSafeInteger(value) || value < 1 || value > totalPages) {
      page.setCustomValidity('请输入 1 到 ' + totalPages + ' 之间的整数页码。');
      page.reportValidity();
      return;
    }
    page.setCustomValidity('');
    offset = (value - 1) * pageSize;
    load();
  }
  page.addEventListener('input', () => page.setCustomValidity(''));
  page.addEventListener('keydown', event => {
    if (event.key === 'Enter') {
      event.preventDefault();
      goToPage();
    }
  });
  pageGo.addEventListener('click', goToPage);

  return {
    get position() { return active && usable ? position : -1; },
    get active() { return active && usable; },
    select(index) {
      if (!active || !usable) return;
      position = index;
      show();
      load();
      actions.paint();
    },
    sync(selection, canEdit, onPage) {
      if (selection?.editKey !== plan?.editKey || !selection) {
        position = -1;
        offset = 0;
      }
      if (!selection) active = false;
      plan = selection;
      usable = canEdit;
      visible = onPage;
      show();
      load();
    }
  };
}
