//! monitor_ctrl_tauri - DDC/CI 显示器控制 (C 后端 + Tauri 前端)
//!
//! 架构
//! ----
//!   [前端 HTML/CSS/JS]  --invoke-->  [Rust 层]  --stdio JSON-->  [C 后端]  --Dxva2-->  显示器
//!
//! 为什么 C 后端独立成进程 (sidecar) 而不是编成 N-API / FFI:
//!   * DDC/CI 是慢速 I2C, 必须串行。stdio 管道天然就是单通道串行队列。
//!   * 后端崩溃不会带崩界面, 且重启成本极低 (进程级隔离)。
//!   * 无需处理 Node ABI / FFI 内存所有权问题。

mod sidecar;

use sidecar::AppState;
use tauri::Manager;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .manage(AppState::new())
        .invoke_handler(tauri::generate_handler![
            sidecar::rpc,
            sidecar::rpc_batch,
            sidecar::backend_status,
            sidecar::restart_backend,
            sidecar::get_log,
            sidecar::clear_log,
            sidecar::enum_tables,
        ])
        .setup(|app| {
            // 启动 C 后端。失败不致命 —— 前端会显示错误并提供"重试"按钮,
            // 这比直接崩溃友好得多。
            let handle = app.handle().clone();
            tauri::async_runtime::spawn_blocking(move || {
                match sidecar::start_sidecar(&handle) {
                    Ok(path) => eprintln!("[app] 后端已启动: {path}"),
                    Err(e) => eprintln!("[app] 后端启动失败: {e}"),
                }
            });
            Ok(())
        })
        .on_window_event(|window, event| {
            // 窗口关闭时主动释放后端, 让显示器句柄干净地还给系统
            if let tauri::WindowEvent::Destroyed = event {
                if let Some(state) = window.try_state::<AppState>() {
                    if let Ok(mut guard) = state.sidecar.lock() {
                        *guard = None; // Drop 里会发 shutdown 并 kill
                    }
                }
            }
        })
        .run(tauri::generate_context!())
        .expect("Tauri 应用启动失败");
}
