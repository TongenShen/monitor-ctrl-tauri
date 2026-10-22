//! monitor_ctrl_tauri - DDC/CI 显示器控制 (C 后端 + Tauri 前端)
//!
//! 架构
//! ----
//!   [前端 HTML/CSS/JS]  --invoke-->  [Rust 层]  --stdio JSON-->  [C 后端]  --Dxva2-->  显示器

mod sidecar;

use sidecar::AppState;
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{Emitter, Manager, PhysicalPosition, WebviewUrl, WebviewWindowBuilder, WindowEvent};

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
            // 启动 C 后端
            let handle = app.handle().clone();
            tauri::async_runtime::spawn_blocking(move || {
                match sidecar::start_sidecar(&handle) {
                    Ok(path) => eprintln!("[app] 后端已启动: {path}"),
                    Err(e) => eprintln!("[app] 后端启动失败: {e}"),
                }
            });

            // ---- 创建快捷面板 popup 窗口 (启动时创建, 默认隐藏) ----
            WebviewWindowBuilder::new(app, "popup", WebviewUrl::App("popup.html".into()))
                .title("快捷面板")
                .decorations(false)
                .always_on_top(true)
                .resizable(false)
                // 高度按内容算: 亮度 + 输入源 + 关闭显示器 + 重新枚举, 约 290 才不挤
                .inner_size(260.0, 290.0)
                .skip_taskbar(true)
                .visible(false)
                .build()?;

            // ---- 系统托盘右键菜单 ----
            let show_item = MenuItem::with_id(app, "show", "显示主窗口", true, None::<&str>)?;
            let quit_item = MenuItem::with_id(app, "quit", "退出", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show_item, &quit_item])?;

            TrayIconBuilder::with_id("main-tray")
                .icon(app.default_window_icon().unwrap().clone())
                .tooltip("MonitorCtrl - 显示器控制")
                .menu(&menu)
                .on_tray_icon_event(|tray, event| {
                    // 左键单击托盘: 弹出/隐藏快捷面板 popup
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        position,
                        ..
                    } = event
                    {
                        let app = tray.app_handle();
                        if let Some(popup) = app.get_webview_window("popup") {
                            if popup.is_visible().unwrap_or(false) {
                                let _ = popup.hide();
                            } else {
                                // 动态获取 popup 实际物理尺寸 (DPI 缩放下跟逻辑尺寸不同)
                                let size = popup.outer_size().unwrap_or(
                                    tauri::PhysicalSize { width: 280, height: 320 }
                                );
                                let popup_w = size.width as f64;
                                let popup_h = size.height as f64;

                                // 定位: 托盘图标上方居中, 底部留 12px 边距不贴任务栏
                                let x = position.x as f64 - popup_w / 2.0;
                                let y = position.y as f64 - popup_h - 12.0;

                                let _ = popup.set_position(PhysicalPosition::new(x as i32, y as i32));
                                let _ = popup.show();
                                let _ = popup.set_focus();
                                let _ = popup.emit("popup-refresh", ());
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
                        if let Some(state) = app.try_state::<AppState>() {
                            if let Ok(mut guard) = state.sidecar.lock() {
                                *guard = None;
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
                WindowEvent::CloseRequested { api, .. } => {
                    let _ = window.hide();
                    api.prevent_close();
                }
                // popup 窗口失去焦点时自动隐藏 (点别处自动关)
                WindowEvent::Focused(false) => {
                    if window.label() == "popup" {
                        let _ = window.hide();
                    }
                }
                WindowEvent::Destroyed => {
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
