/* ===================================================================
   快捷面板 (popup) - 托盘弹出的小窗口
   ===================================================================

   ★ 重新枚举显示器
     复用主窗口 (app.js -> #btn-refresh) 的逻辑:
       清缓存 -> restart_backend (重启 C 后端进程) -> 重新 list_monitors
     显示器断电重启 / 换线 / 热插拔之后, C 后端进程在枚举那一刻拿到的
     显示器句柄就已经失效了, 只重新调 list_monitors 拿到的还是旧句柄,
     读写一律失败。必须把整个后端进程重启, 让它重新走 Dxva2 枚举。
   =================================================================== */

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const app = document.getElementById('app');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** 当前选中的显示器索引 (后端 index) */
let currentIndex = null;
/** 正在重新枚举中 —— 期间忽略托盘触发的自动刷新, 避免两次枚举打架 */
let enumerating = false;

/** 显示器返回的字符串不可信, 插进 innerHTML 前转义 */
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  })[c]);
}

/* ------------------------------------------------------------------ *
 *  重新枚举显示器
 * ------------------------------------------------------------------ */

async function reenumerate(btn) {
  if (enumerating) return;
  enumerating = true;

  if (btn) {
    btn.disabled = true;
    btn.textContent = '枚举中…';
  }

  try {
    // 重启后端进程: 旧进程 Drop 时会发 shutdown 释放显示器句柄
    await invoke('restart_backend');
    await sleep(120);
    await refresh();
  } catch (e) {
    showMessage(`重新枚举失败: ${e}`, true);
  } finally {
    enumerating = false;
  }
}

/* ------------------------------------------------------------------ *
 *  数据加载
 * ------------------------------------------------------------------ */

async function refresh() {
  try {
    const res = await invoke('rpc', { method: 'list_monitors' });
    const monitors = res?.monitors ?? [];

    if (!monitors.length) {
      showMessage('未检测到显示器', true);
      return;
    }

    // 尽量保留上次选中的那台; 枚举顺序变了就退回第一台
    const keep = monitors.some((m) => m.index === currentIndex);
    currentIndex = keep ? currentIndex : monitors[0].index;

    const props = await invoke('rpc', {
      method: 'get_props',
      params: { index: currentIndex },
    });
    render(props);
  } catch (e) {
    showMessage(`加载失败: ${e}`, true);
  }
}

/* ------------------------------------------------------------------ *
 *  渲染
 * ------------------------------------------------------------------ */

/** 空状态 / 错误状态 —— 一律带上重新枚举入口, 否则用户只能去开大窗口 */
function showMessage(msg, withReenum = false) {
  app.innerHTML = `
    <div class="loading">${esc(msg)}</div>
    ${withReenum ? `
    <div class="section">
      <button class="btn-ghost btn-ghost--accent" id="btn-reenum"
              title="显示器断电重启或热插拔后用">
        重新枚举显示器
      </button>
    </div>` : ''}
  `;
  bindReenum();
}

function bindReenum() {
  const btn = document.getElementById('btn-reenum');
  if (btn) btn.addEventListener('click', () => reenumerate(btn));
}

function render(props) {
  const brightness = props.brightness ?? 50;
  const brightnessMax = props.brightness_max ?? 100;
  const inputSrc = props.input_src ?? '';
  const inputSrcList = props.input_src_list ?? [];

  app.innerHTML = `
    <div class="section">
      <div class="section-title">亮度</div>
      <div class="brightness-row">
        <input type="range" id="brightness" min="0" max="${brightnessMax}" value="${brightness}">
        <span class="brightness-value" id="brightness-val">${brightness}</span>
      </div>
    </div>

    <div class="section">
      <div class="section-title">输入源</div>
      <select id="input-src">
        ${inputSrcList.map(s => `<option value="${esc(s)}" ${s === inputSrc ? 'selected' : ''}>${esc(s)}</option>`).join('')}
      </select>
    </div>

    <div class="section">
      <button class="btn-off" id="btn-off">关闭显示器</button>
    </div>

    <div class="section">
      <button class="btn-ghost" id="btn-reenum" title="显示器断电重启或热插拔后用">
        重新枚举显示器
      </button>
    </div>
  `;

  // 亮度滑块: 拖动时更新数字, 松手时下发
  const slider = document.getElementById('brightness');
  const val = document.getElementById('brightness-val');
  slider.addEventListener('input', () => {
    val.textContent = slider.value;
  });
  slider.addEventListener('change', async () => {
    try {
      await invoke('rpc', {
        method: 'set_prop',
        params: { index: currentIndex, prop: 'brightness', value: parseInt(slider.value) }
      });
    } catch (e) {
      console.error('亮度设置失败:', e);
    }
  });

  // 输入源切换
  document.getElementById('input-src').addEventListener('change', async (e) => {
    try {
      await invoke('rpc', {
        method: 'set_prop',
        params: { index: currentIndex, prop: 'input_src', value: e.target.value }
      });
    } catch (e) {
      console.error('输入源切换失败:', e);
    }
  });

  // 关闭显示器 (DDC/CI power_mode off, 跟主窗口电源模式一致)
  document.getElementById('btn-off').addEventListener('click', async () => {
    try {
      await invoke('rpc', {
        method: 'set_prop',
        params: { index: currentIndex, prop: 'power_mode', value: 'off' }
      });
    } catch (e) {
      console.error('关闭显示器失败:', e);
    }
  });

  bindReenum();
}

/* ------------------------------------------------------------------ *
 *  启动
 * ------------------------------------------------------------------ */

// 初始加载
refresh();

// 托盘弹出时刷新数据 (正在枚举时跳过, 让枚举自己收尾)
listen('popup-refresh', () => {
  if (!enumerating) refresh();
});
