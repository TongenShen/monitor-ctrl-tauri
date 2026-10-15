/* ===================================================================
   快捷面板 (popup) - 托盘弹出的小窗口
   =================================================================== */

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const app = document.getElementById('app');
let currentIndex = 0;

async function refresh() {
  try {
    const monitors = await invoke('rpc', { method: 'list_monitors' });
    if (!monitors.monitors || monitors.monitors.length === 0) {
      app.innerHTML = '<div class="loading">未检测到显示器</div>';
      return;
    }
    currentIndex = monitors.monitors[0].index;
    const props = await invoke('rpc', { method: 'get_props', params: { index: currentIndex } });
    render(props);
  } catch (e) {
    app.innerHTML = `<div class="loading">加载失败: ${e}</div>`;
  }
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
        ${inputSrcList.map(s => `<option value="${s}" ${s === inputSrc ? 'selected' : ''}>${s}</option>`).join('')}
      </select>
    </div>

    <div class="section">
      <button class="btn-off" id="btn-off">关闭显示器</button>
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
}

// 初始加载
refresh();

// 托盘弹出时刷新数据
listen('popup-refresh', () => refresh());
