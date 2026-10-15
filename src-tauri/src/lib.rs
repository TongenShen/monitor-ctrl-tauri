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
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{Manager, WindowEvent};

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

            // ---- 系统托盘 ----
            // 右键菜单: 显示主窗口 / 退出
            let show_item = MenuItem::with_id(app, "show", "显示主窗口", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "退出", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show_item, &quit_item])?;

            TrayIconBuilder::with_id("main-tray")
                .icon(app.default_window_icon().unwrap().clone())
                .tooltip("MonitorCtrl - 显示器控制")
                .menu(&menu)
                .on_tray_icon_event(|tray, event| {
                    // 左键单击托盘图标: 切换窗口显示/隐藏
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        let app = tray.app_handle();
                        if let Some(window) = app.get_webview_window("main") {
                            if window.is_visible().unwrap_or(false) {
                                let _ = window.hide();
                            } else {
                                let _ = window.show();
                                let _ = window.set_focus();
                            }
                        }
                    }
                })
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                    "quit" => {
                        // 真正退出: 先清理 sidecar, 再退出应用
                        if let Some(state) = app.try_state::<AppState>() {
                            if let Ok(mut guard) = state.sidecar.lock() {
                                *guard = None; // Drop 里会发 shutdown 并 kill
                            }
                        }
                        app.exit(0);
                    }
                    _ => {}
                })
                .build(app)?;

            Ok(())
        })
        .on_window_event(|window, event| {
            match event {
                // 点窗口 × 按钮: 不真正退出, 而是隐藏到托盘。
                // 只有从托盘菜单选"退出"才会真正结束进程。
                WindowEvent::CloseRequested { api, .. } => {
                    let _ = window.hide();
                    api.prevent_close();
                }
                WindowEvent::Destroyed => {
                    // 真正销毁时才清理 sidecar (从托盘菜单退出时会走到这里)
                    if let Some(state) = window.try_state::<AppState>() {
                        if let Ok(mut guard) = state.sidecar.lock() {
                            *guard = None;
                        }
                    }
                }
                _ => {}
            }
        })
        .run(tauri::generate_context!())
        .expect("Tauri 应用启动失败");
}
