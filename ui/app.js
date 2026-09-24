/* ===================================================================
   显示器控制 - 前端逻辑
   ===================================================================

   数据流
   ------
     Rust 命令 (invoke)  ->  C 后端 (stdio JSON-RPC)  ->  Dxva2  ->  显示器

   Rust 侧暴露的命令:
     rpc(method, params)   通用转发
     rpc_batch(calls)      批量调用 [{key, method, params}]
     backend_status()      { running: bool }
     restart_backend()     重启 C 进程 (热插拔后用)
     get_log() / clear_log()
     enum_tables(index)    下拉框选项 (会按该显示器的 caps string 过滤)

   C 后端方法:
     ping | list_monitors | get_props {index} | get_info {index}
     enum_tables {index} | set_prop {index, prop, value} | action {index, action}

   ★ get_props 的返回值里已经内嵌了过滤后的下拉框选项
     (color_preset_list / osd_languages_list / power_mode_list / input_src_list),
     所以渲染控制面板只需要一次 get_props, 不用再单独调 enum_tables。

   ★ 关键约束: DDC/CI 是走 I2C 的慢速总线, 一次读写要几十到几百毫秒。
     所以滑块拖动过程中**绝不下发指令**, 只在 pointerup 时发一次。
     这一点是原 Python 版的实测经验 (见 tkui.py 的注释), 必须保留。
   =================================================================== */

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

/* ------------------------------------------------------------------ *
 *  状态
 * ------------------------------------------------------------------ */

const state = {
  /** @type {{index:number, model:string, description:string}[]} */
  monitors: [],
  /** 当前选中的显示器索引 (后端 index) */
  selected: null,
  /** 每个显示器的属性缓存, key = index */
  props: new Map(),
  /** 每个显示器的静态信息缓存 */
  info: new Map(),
  /** 后端是否就绪 */
  ready: false,
  /** 正在提交的写操作计数, 用于禁用交互 */
  busy: 0,
};

/* ------------------------------------------------------------------ *
 *  工具
 * ------------------------------------------------------------------ */

const $ = (id) => document.getElementById(id);

/** HTML 转义 —— 显示器返回的字符串 (型号/caps) 不可信 */
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[c]);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** 把后端错误信息翻译得更友好一点 */
function friendlyError(e) {
  const s = String(e ?? '');
  if (s.includes('NO_MONITOR') || s.includes('no monitor')) return '未检测到支持的显示器';
  if (s.includes('CAPS')) return '读取显示器能力字符串失败 (DDC/CI 通信不稳定, 可重试)';
  if (s.includes('UNSUPPORTED')) return '该显示器不支持此功能';
  if (s.includes('RANGE')) return '数值超出显示器允许范围';
  return s.replace(/^Error:\s*/, '') || '未知错误';
}

/* ------------------------------------------------------------------ *
 *  Toast 提示
 * ------------------------------------------------------------------ */

const TOAST_ICON = {
  ok: '<path d="M20 6 9 17l-5-5"/>',
  error: '<circle cx="12" cy="12" r="9"/><path d="M12 8v5M12 16.5v.01"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 16v-5M12 7.5v.01"/>',
};

function toast(message, kind = 'info', ms = 3200) {
  const el = document.createElement('div');
  el.className = `toast toast--${kind}`;
  el.innerHTML =
    `<svg viewBox="0 0 24 24">${TOAST_ICON[kind] || TOAST_ICON.info}</svg>` +
    `<span>${esc(message)}</span>`;
  $('toast-host').appendChild(el);
  setTimeout(() => {
    el.classList.add('is-out');
    el.addEventListener('animationend', () => el.remove(), { once: true });
  }, ms);
}

/* ------------------------------------------------------------------ *
 *  RPC 封装
 * ------------------------------------------------------------------ */

async function rpc(method, params = null) {
  try {
    return await invoke('rpc', { method, params });
  } catch (e) {
    throw new Error(friendlyError(e));
  }
}

/** 并发安全的写操作包装: 期间禁用面板交互, 避免 DDC/CI 指令打架 */
async function write(method, params) {
  state.busy++;
  document.body.classList.add('is-busy');
  try {
    return await rpc(method, params);
  } finally {
    state.busy--;
    if (state.busy <= 0) {
      state.busy = 0;
      document.body.classList.remove('is-busy');
    }
  }
}

const setProp = (index, prop, value) =>
  write('set_prop', { index, prop, value });

/* ------------------------------------------------------------------ *
 *  后端生命周期
 * ------------------------------------------------------------------ */

function setBackendPill(kind, text) {
  const pill = $('backend-pill');
  pill.className = `pill pill--${kind}`;
  $('backend-text').textContent = text;
}

async function refreshBackendStatus() {
  try {
    const s = await invoke('backend_status');
    if (s && s.running) {
      setBackendPill('ok', '已连接');
      state.ready = true;
      return true;
    }
  } catch { /* 下面统一处理 */ }
  setBackendPill('error', '未连接');
  state.ready = false;
  return false;
}

/* ------------------------------------------------------------------ *
 *  加载显示器
 * ------------------------------------------------------------------ */

function showPlaceholder(title, msg, isError = false) {
  $('placeholder').hidden = false;
  $('panel').hidden = true;
  $('placeholder').classList.toggle('is-error', isError);
  $('placeholder-title').textContent = title;
  $('placeholder-msg').textContent = msg;
  $('btn-retry').hidden = !isError;
}

async function loadMonitors() {
  showPlaceholder('正在检测显示器…', '首次连接 DDC/CI 需要一点时间');

  // 1) 确认后端活着
  if (!(await refreshBackendStatus())) {
    const ok = await tryRestart();
    if (!ok) {
      showPlaceholder(
        '后端未启动',
        '无法启动 DDC/CI 后端进程 (monitor_rpc.exe)。请确认程序完整安装。',
        true,
      );
      return;
    }
  }

  // 2) 枚举显示器
  //    注意: 下拉框选项 (enum_tables) 不在这里取 —— 它需要显示器 index
  //    才能按 caps string 过滤, 放到 selectMonitor() 里按需加载。
  let list;
  try {
    const mon = await rpc('list_monitors');
    list = mon?.monitors ?? [];
  } catch (e) {
    showPlaceholder('检测失败', `${e.message}\n\nDDC/CI 通信偶发失败, 点击重试通常即可。`, true);
    return;
  }

  state.monitors = list;
  $('monitor-count').textContent = String(list.length);

  if (!list.length) {
    renderMonitorList();
    showPlaceholder(
      '未检测到显示器',
      '仅支持通过 DDC/CI 控制的外部显示器 (VGA / DVI / HDMI / DisplayPort)。' +
      '笔记本内屏、虚拟显示器通常不支持。',
      true,
    );
    return;
  }

  // 3) 渲染列表并选中第一个
  renderMonitorList();
  const first = state.selected ?? list[0].index;
  await selectMonitor(first);
}

async function tryRestart() {
  try {
    setBackendPill('pending', '启动中…');
    await invoke('restart_backend');
    await sleep(120);
    return await refreshBackendStatus();
  } catch {
    setBackendPill('error', '未连接');
    return false;
  }
}

/* ------------------------------------------------------------------ *
 *  侧栏显示器列表
 * ------------------------------------------------------------------ */

const MONITOR_ICON =
  '<rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 21h8M12 17v4"/>';

function renderMonitorList() {
  const host = $('monitor-list');
  host.innerHTML = '';

  for (const m of state.monitors) {
    const btn = document.createElement('button');
    btn.className = 'monitor-item';
    btn.setAttribute('role', 'tab');
    btn.setAttribute('aria-selected', String(m.index === state.selected));
    btn.dataset.index = String(m.index);
    btn.innerHTML =
      `<span class="monitor-item-icon"><svg viewBox="0 0 24 24">${MONITOR_ICON}</svg></span>` +
      `<span class="monitor-item-body">` +
      `<span class="monitor-item-name">${esc(m.model || m.description || `显示器 ${m.index + 1}`)}</span>` +
      `<span class="monitor-item-sub">#${m.index + 1}</span>` +
      `</span>`;
    btn.addEventListener('click', () => selectMonitor(m.index));
    host.appendChild(btn);
  }
}

/* ------------------------------------------------------------------ *
 *  选中显示器
 * ------------------------------------------------------------------ */

async function selectMonitor(index) {
  state.selected = index;
  renderMonitorList();

  const meta = state.monitors.find((m) => m.index === index) || {};
  $('panel-title').textContent = meta.model || meta.description || `显示器 ${index + 1}`;
  $('panel-desc').textContent = meta.description || '';

  $('placeholder').hidden = true;
  $('panel').hidden = false;

  // 先用缓存快速渲染, 避免界面空白
  if (state.props.has(index)) {
    renderControls(state.props.get(index));
  } else {
    $('controls').innerHTML =
      '<div class="unsupported-note">正在读取显示器属性…</div>';
  }

  // get_props 已经带上按 caps string 过滤后的下拉框选项, 无需再单独取
  await reloadProps(index, true);
}

async function reloadProps(index, render) {
  try {
    const props = await rpc('get_props', { index });
    state.props.set(index, props);
    if (render && state.selected === index) renderControls(props);
    return props;
  } catch (e) {
    if (state.selected === index) {
      $('controls').innerHTML =
        `<div class="unsupported-note">读取属性失败: ${esc(e.message)}</div>`;
      toast(e.message, 'error');
    }
    return null;
  }
}

/* ------------------------------------------------------------------ *
 *  控件构造
 * ------------------------------------------------------------------ */

/** 分组容器 */
function makeGroup(title, grid = false) {
  const g = document.createElement('div');
  g.className = 'group';
  if (title) {
    const t = document.createElement('div');
    t.className = 'group-title';
    t.textContent = title;
    g.appendChild(t);
  }
  if (grid) {
    const wrap = document.createElement('div');
    wrap.className = 'group-grid';
    g.appendChild(wrap);
    return { group: g, host: wrap };
  }
  return { group: g, host: g };
}

/**
 * 单值滑块。
 * 只在 pointerup / keyup 时下发 —— 拖动期间一个字节都不发给显示器。
 */
function buildSlider({ label, value, max, onCommit }) {
  const card = document.createElement('div');
  card.className = 'slider-card';

  const head = document.createElement('div');
  head.className = 'slider-head';
  const lab = document.createElement('span');
  lab.className = 'slider-label';
  lab.textContent = label;
  const val = document.createElement('span');
  val.className = 'slider-value';
  val.innerHTML = `${value}<span class="slider-value-max"> / ${max}</span>`;
  head.append(lab, val);

  const input = document.createElement('input');
  input.type = 'range';
  input.className = 'slider';
  input.min = '0';
  input.max = String(max);
  input.value = String(value);
  input.setAttribute('aria-label', label);

  // 拖动时只更新数字显示, 标黄表示"未提交"
  input.addEventListener('input', () => {
    val.innerHTML = `${input.value}<span class="slider-value-max"> / ${max}</span>`;
    val.classList.add('is-dirty');
  });

  let committing = false;
  const commit = async () => {
    const target = Number(input.value);
    if (committing || target === value) {
      val.classList.remove('is-dirty');
      return;
    }
    committing = true;
    try {
      await onCommit(target);
      value = target;
      val.classList.remove('is-dirty');
    } catch (e) {
      toast(`${label}: ${e.message}`, 'error');
      input.value = String(value); // 回滚
      val.innerHTML = `${value}<span class="slider-value-max"> / ${max}</span>`;
      val.classList.remove('is-dirty');
    } finally {
      committing = false;
    }
  };

  input.addEventListener('change', commit);
  input.addEventListener('pointerup', commit);
  input.addEventListener('keyup', (e) => {
    if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End', 'PageUp', 'PageDown'].includes(e.key)) {
      commit();
    }
  });

  card.append(head, input);
  return card;
}

/** RGB 三通道 —— 一次提交三个值, 所以三条滑条共用一个 commit */
function buildRgbCard({ value, max, onCommit }) {
  const card = document.createElement('div');
  card.className = 'slider-card';

  const head = document.createElement('div');
  head.className = 'slider-head';
  head.innerHTML =
    '<span class="slider-label">RGB 增益</span>' +
    `<span class="slider-value"><span class="slider-value-max">0 – ${max}</span></span>`;
  card.appendChild(head);

  const row = document.createElement('div');
  row.className = 'rgb-row';

  const cur = [...value];
  const inputs = [];
  const nums = [];

  ['R', 'G', 'B'].forEach((tag, i) => {
    const line = document.createElement('div');
    line.className = 'rgb-line';

    const t = document.createElement('span');
    t.className = `rgb-tag rgb-tag--${tag.toLowerCase()}`;
    t.textContent = tag;

    const input = document.createElement('input');
    input.type = 'range';
    input.className = 'slider';
    input.min = '0';
    input.max = String(max);
    input.value = String(cur[i]);
    input.setAttribute('aria-label', `${tag} 通道`);

    const num = document.createElement('span');
    num.className = 'rgb-num';
    num.textContent = String(cur[i]);

    input.addEventListener('input', () => {
      num.textContent = input.value;
      num.classList.add('is-dirty');
    });

    inputs.push(input);
    nums.push(num);
    line.append(t, input, num);
    row.appendChild(line);
  });

  let committing = false;
  const commit = async () => {
    const next = inputs.map((el) => Number(el.value));
    if (committing || next.every((v, i) => v === cur[i])) {
      nums.forEach((n) => n.classList.remove('is-dirty'));
      return;
    }
    committing = true;
    try {
      await onCommit(next);
      cur.splice(0, 3, ...next);
      nums.forEach((n) => n.classList.remove('is-dirty'));
    } catch (e) {
      toast(`RGB 增益: ${e.message}`, 'error');
      inputs.forEach((el, i) => { el.value = String(cur[i]); });
      nums.forEach((n, i) => { n.textContent = String(cur[i]); n.classList.remove('is-dirty'); });
    } finally {
      committing = false;
    }
  };

  inputs.forEach((el) => {
    el.addEventListener('change', commit);
    el.addEventListener('pointerup', commit);
    el.addEventListener('keyup', (e) => {
      if (e.key.startsWith('Arrow') || e.key === 'Home' || e.key === 'End') commit();
    });
  });

  card.appendChild(row);
  return card;
}

/** 下拉框 —— 与当前值相同时不下发 (原 Python 版就是这么避免重复写 EEPROM 的) */
function buildSelect({ label, current, options, onCommit }) {
  const card = document.createElement('div');
  card.className = 'select-card';

  const lab = document.createElement('label');
  lab.className = 'select-label';
  lab.textContent = label;

  const sel = document.createElement('select');
  sel.className = 'select';

  const known = options.includes(current);
  if (!known && current) {
    // 显示器报了一个不在码表里的值: 保留它, 否则用户一碰就被改掉
    const o = document.createElement('option');
    o.value = current;
    o.textContent = `${current} (未在码表中)`;
    sel.appendChild(o);
  }
  for (const opt of options) {
    const o = document.createElement('option');
    o.value = opt;
    o.textContent = opt;
    sel.appendChild(o);
  }
  sel.value = current ?? '';
  if (!current) {
    sel.disabled = true;
    lab.textContent = `${label} (不支持)`;
  }

  sel.addEventListener('change', async () => {
    const next = sel.value;
    if (next === current) return;
    const prev = current;
    sel.disabled = true;
    try {
      await onCommit(next);
      current = next;
    } catch (e) {
      toast(`${label}: ${e.message}`, 'error');
      sel.value = prev ?? '';
    } finally {
      sel.disabled = false;
    }
  });

  card.append(lab, sel);
  return card;
}

/** 色温: 数值输入 + 提交按钮 (范围由显示器决定, 用 spinner 更合适) */
function buildNumber({ label, current, onCommit }) {
  const card = document.createElement('div');
  card.className = 'select-card';

  const lab = document.createElement('label');
  lab.className = 'select-label';
  lab.textContent = label;

  const row = document.createElement('div');
  row.className = 'number-row';

  const input = document.createElement('input');
  input.type = 'number';
  input.className = 'input';
  input.min = '0';
  input.step = '50';
  input.value = current == null ? '' : String(current);
  input.placeholder = '不支持';

  const btn = document.createElement('button');
  btn.className = 'btn';
  btn.textContent = '应用';

  if (current == null) {
    input.disabled = true;
    btn.disabled = true;
  }

  const commit = async () => {
    const next = Number(input.value);
    if (!Number.isFinite(next) || next === current) return;
    btn.disabled = true;
    input.disabled = true;
    try {
      await onCommit(next);
      current = next;
      toast(`${label} 已设为 ${next}K`, 'ok');
    } catch (e) {
      toast(`${label}: ${e.message}`, 'error');
      input.value = current == null ? '' : String(current);
    } finally {
      btn.disabled = false;
      input.disabled = false;
    }
  };

  btn.addEventListener('click', commit);
  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') commit(); });

  row.append(input, btn);
  card.append(lab, row);
  return card;
}

/* ------------------------------------------------------------------ *
 *  渲染整个控制面板
 * ------------------------------------------------------------------ */

function renderControls(p) {
  const idx = state.selected;
  const host = $('controls');
  host.innerHTML = '';

  /* ---- 亮度 / 对比度 ---- */
  const basics = makeGroup('基础', true);
  if (p.brightness != null && p.brightness_max != null) {
    basics.host.appendChild(buildSlider({
      label: '亮度', value: p.brightness, max: p.brightness_max,
      onCommit: (v) => setProp(idx, 'brightness', v),
    }));
  }
  if (p.contrast != null && p.contrast_max != null) {
    basics.host.appendChild(buildSlider({
      label: '对比度', value: p.contrast, max: p.contrast_max,
      onCommit: (v) => setProp(idx, 'contrast', v),
    }));
  }
  if (basics.host.children.length) host.appendChild(basics.group);

  /* ---- 色彩 ---- */
  const color = makeGroup('色彩', true);
  if (Array.isArray(p.rgb_gain) && p.rgb_gain_max != null) {
    color.host.appendChild(buildRgbCard({
      value: p.rgb_gain, max: p.rgb_gain_max,
      onCommit: (rgb) => setProp(idx, 'rgb_gain', rgb),
    }));
  }
  if (p.color_temperature != null) {
    color.host.appendChild(buildNumber({
      label: '色温 (K)', current: p.color_temperature,
      onCommit: (v) => setProp(idx, 'color_temperature', v),
    }));
  }
  if (p.color_preset_list?.length) {
    color.host.appendChild(buildSelect({
      label: '颜色预设', current: p.color_preset,
      options: p.color_preset_list,
      onCommit: (v) => setProp(idx, 'color_preset', v),
    }));
  }
  if (color.host.children.length) host.appendChild(color.group);

  /* ---- 显示器 ---- */
  const display = makeGroup('显示器', true);
  if (p.input_src_list?.length) {
    display.host.appendChild(buildSelect({
      label: '输入源', current: p.input_src,
      options: p.input_src_list,
      onCommit: (v) => setProp(idx, 'input_src', v),
    }));
  }
  if (p.osd_languages_list?.length) {
    display.host.appendChild(buildSelect({
      label: 'OSD 语言', current: p.osd_language,
      options: p.osd_languages_list,
      onCommit: (v) => setProp(idx, 'osd_language', v),
    }));
  }
  if (p.power_mode_list?.length) {
    display.host.appendChild(buildSelect({
      label: '电源模式', current: p.power_mode,
      options: p.power_mode_list,
      onCommit: (v) => setProp(idx, 'power_mode', v),
    }));
  }
  if (display.host.children.length) host.appendChild(display.group);

  /* ---- 全空: 说明这台显示器基本不支持 DDC/CI ---- */
  if (!host.children.length) {
    host.innerHTML =
      '<div class="unsupported-note">' +
      '该显示器未返回任何可控制的属性。可能是 DDC/CI 未在显示器 OSD 中启用, ' +
      '或使用了转接器 (部分 KVM / 视频采集卡不透传 DDC/CI)。' +
      '</div>';
  }
}

/* ------------------------------------------------------------------ *
 *  详细信息弹层
 * ------------------------------------------------------------------ */

async function openInfo() {
  const idx = state.selected;
  if (idx == null) return;

  const modal = $('modal-info');
  const body = $('info-body');
  modal.hidden = false;

  const cached = state.info.get(idx);
  if (cached) {
    renderInfo(body, cached);
    return;
  }

  body.innerHTML = '<div class="unsupported-note">正在读取…</div>';
  try {
    const info = await rpc('get_info', { index: idx });
    state.info.set(idx, info);
    if (!$('modal-info').hidden) renderInfo(body, info);
  } catch (e) {
    body.innerHTML = `<div class="unsupported-note">读取失败: ${esc(e.message)}</div>`;
  }
}

function renderInfo(body, info) {
  const rows = [
    ['型号', info.model],
    ['系统描述', info.description],
    ['显示类型', info.display_type],
    ['面板像素排列', info.panel_type],
    ['累计开机时长', info.poweron_hours != null ? `${info.poweron_hours} 小时` : null],
  ].filter(([, v]) => v != null && v !== '');

  let html = '<div class="info-grid">';
  for (const [k, v] of rows) {
    html += `<div class="info-key">${esc(k)}</div><div class="info-val">${esc(v)}</div>`;
  }
  html += '</div>';

  if (info.caps) {
    html +=
      '<div class="group-title" style="margin-top:20px">DDC/CI 能力字符串</div>' +
      `<div class="info-caps">${esc(info.caps)}</div>`;
  }

  body.innerHTML = html;
}

/* ------------------------------------------------------------------ *
 *  日志弹层
 * ------------------------------------------------------------------ */

async function openLog() {
  $('modal-log').hidden = false;
  await renderLog();
}

async function renderLog() {
  const body = $('log-body');
  let entries = [];
  try {
    entries = await invoke('get_log');
  } catch { /* 忽略 */ }

  if (!entries.length) {
    body.innerHTML = '<div class="log-empty">还没有任何写操作记录</div>';
    return;
  }

  body.innerHTML = entries
    .slice()
    .reverse()
    .map((e) =>
      `<div class="log-row log-row--${esc(e.level)}">` +
      `<span class="log-time">${esc(e.time)}</span>` +
      `<span class="log-msg">${esc(e.message)}</span>` +
      `</div>`)
    .join('');
}

/* ------------------------------------------------------------------ *
 *  确认对话框
 * ------------------------------------------------------------------ */

function confirmDialog(title, message, okText = '确定') {
  return new Promise((resolve) => {
    const modal = $('modal-confirm');
    $('confirm-title').textContent = title;
    $('confirm-msg').textContent = message;
    $('confirm-ok').textContent = okText;
    modal.hidden = false;

    const done = (v) => {
      modal.hidden = true;
      $('confirm-ok').removeEventListener('click', onOk);
      $('confirm-cancel').removeEventListener('click', onCancel);
      modal.removeEventListener('click', onBackdrop);
      resolve(v);
    };
    const onOk = () => done(true);
    const onCancel = () => done(false);
    const onBackdrop = (e) => { if (e.target === modal) done(false); };

    $('confirm-ok').addEventListener('click', onOk);
    $('confirm-cancel').addEventListener('click', onCancel);
    modal.addEventListener('click', onBackdrop);
  });
}

/* ------------------------------------------------------------------ *
 *  动作
 * ------------------------------------------------------------------ */

async function doAction(action, label) {
  const idx = state.selected;
  if (idx == null) return;

  if (action === 'reset_factory') {
    const yes = await confirmDialog(
      '恢复出厂设置',
      '这会把显示器所有可调参数 (亮度、对比度、色彩、语言…) 重置为出厂默认值, ' +
      '且通常无法撤销。确定继续吗?',
      '恢复出厂设置',
    );
    if (!yes) return;
  }

  try {
    await write('action', { index: idx, action });
    toast(`${label} 已执行`, 'ok');
    await sleep(400); // 显示器需要时间应用, 稍等再读回
    state.info.delete(idx);
    await reloadProps(idx, true);
  } catch (e) {
    toast(`${label} 失败: ${e.message}`, 'error');
  }
}

/* ------------------------------------------------------------------ *
 *  事件绑定
 * ------------------------------------------------------------------ */

function bindEvents() {
  $('btn-refresh').addEventListener('click', async () => {
    // 热插拔后必须重启后端 —— 显示器句柄在枚举时就固定了
    const btn = $('btn-refresh');
    btn.disabled = true;
    try {
      state.props.clear();
      state.info.clear();
      state.selected = null;
      if (!(await tryRestart())) {
        showPlaceholder('后端未启动', '无法启动 DDC/CI 后端进程。', true);
        return;
      }
      await loadMonitors();
      toast('已重新枚举显示器', 'ok');
    } finally {
      btn.disabled = false;
    }
  });

  $('btn-retry').addEventListener('click', loadMonitors);
  $('btn-info').addEventListener('click', openInfo);
  $('btn-log').addEventListener('click', openLog);
  $('btn-auto-setup').addEventListener('click', () => doAction('auto_setup', '自动调整'));
  $('btn-reset').addEventListener('click', () => doAction('reset_factory', '恢复出厂设置'));

  $('btn-clear-log').addEventListener('click', async () => {
    try {
      await invoke('clear_log');
      await renderLog();
    } catch (e) {
      toast(String(e), 'error');
    }
  });

  // 弹层关闭
  document.querySelectorAll('[data-close]').forEach((el) => {
    el.addEventListener('click', () => { $(el.dataset.close).hidden = true; });
  });
  document.querySelectorAll('.modal').forEach((modal) => {
    modal.addEventListener('click', (e) => {
      if (e.target === modal && modal.id !== 'modal-confirm') modal.hidden = true;
    });
  });

  // 快捷键
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      document.querySelectorAll('.modal:not([hidden])').forEach((m) => {
        if (m.id !== 'modal-confirm') m.hidden = true;
      });
    }
    if (e.key === 'F5') { e.preventDefault(); $('btn-refresh').click(); }
  });

  // 后端日志更新 -> 若日志弹层开着就刷新
  listen('log-updated', () => {
    if (!$('modal-log').hidden) renderLog();
  });
}

/* ------------------------------------------------------------------ *
 *  启动
 * ------------------------------------------------------------------ */

(async function main() {
  bindEvents();

  // 等后端 ping 通 —— Tauri 的 setup 是异步 spawn 的, 界面可能先起来
  for (let i = 0; i < 20; i++) {
    if (await refreshBackendStatus()) break;
    await sleep(250);
  }

  await loadMonitors();
})();
