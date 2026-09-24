//! sidecar.rs - C 后端 (monitor_rpc.exe) 的进程管理与 JSON-RPC 客户端
//!
//! 设计要点
//! --------
//! * DDC/CI 走 I2C, 单次读写几十到几百毫秒, 且**必须串行**。
//!   stdin/stdout 管道天然就是单通道串行队列, 所以这里用一把 Mutex 把
//!   "写请求 + 读响应" 作为一个原子操作, 天然满足串行要求。
//! * 后端约定 stdout 只输出 JSON (每行一条), 诊断信息全部走 stderr。
//!   这里把 stderr 转发到 Tauri 的日志, 便于排查问题。

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Mutex;

use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager};

/// 一次 RPC 调用的结果
pub type RpcResult<T> = Result<T, String>;

/// 后端进程句柄
pub struct Sidecar {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    next_id: AtomicI64,
}

impl Sidecar {
    /// 启动后端进程
    pub fn spawn(program: &str) -> std::io::Result<Self> {
        let mut child = Command::new(program)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;

        let stdin = child.stdin.take().expect("stdin 未创建");
        let stdout = BufReader::new(child.stdout.take().expect("stdout 未创建"));
        let stderr = child.stderr.take().expect("stderr 未创建");

        // stderr 单独起线程读取并转发到 Tauri 日志
        std::thread::spawn(move || {
            let reader = BufReader::new(stderr);
            for line in reader.lines().map_while(Result::ok) {
                if !line.trim().is_empty() {
                    eprintln!("[monitor_rpc] {line}");
                }
            }
        });

        Ok(Self {
            child,
            stdin,
            stdout,
            next_id: AtomicI64::new(1),
        })
    }

    /// 发送一次请求并等待响应。调用方负责持锁以保证串行。
    fn call_raw(&mut self, method: &str, params: Value) -> RpcResult<Value> {
        let id = self.next_id.fetch_add(1, Ordering::SeqCst);

        // 组装请求: {"id":N,"method":"...", ...params}
        let mut req = json!({ "id": id, "method": method });
        if let Value::Object(map) = params {
            if let Value::Object(dst) = &mut req {
                for (k, v) in map {
                    dst.insert(k, v);
                }
            }
        }

        let mut line = serde_json::to_string(&req).map_err(|e| e.to_string())?;
        line.push('\n');

        self.stdin
            .write_all(line.as_bytes())
            .and_then(|_| self.stdin.flush())
            .map_err(|e| format!("写入后端失败: {e}"))?;

        // 读取一行响应。后端保证每个请求都有且只有一行响应。
        let mut resp_line = String::new();
        let n = self
            .stdout
            .read_line(&mut resp_line)
            .map_err(|e| format!("读取后端响应失败: {e}"))?;
        if n == 0 {
            return Err("后端进程已退出".into());
        }

        let resp: Value = serde_json::from_str(resp_line.trim())
            .map_err(|e| format!("后端返回了非法 JSON: {e}\n原始内容: {}", resp_line.trim()))?;

        // 校验 id 是否对应, 不对应说明协议错乱
        if resp.get("id").and_then(Value::as_i64) != Some(id) {
            return Err(format!(
                "响应 id 不匹配: 期望 {id}, 实际 {:?}",
                resp.get("id")
            ));
        }

        if resp.get("ok").and_then(Value::as_bool) == Some(true) {
            Ok(resp.get("result").cloned().unwrap_or(Value::Null))
        } else {
            Err(resp
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("未知错误")
                .to_string())
        }
    }

    /// 是否仍在运行
    pub fn is_running(&mut self) -> bool {
        matches!(self.child.try_wait(), Ok(None))
    }
}

impl Drop for Sidecar {
    fn drop(&mut self) {
        // 优雅关闭: 发 shutdown 让后端释放显示器句柄, 再兜底 kill
        let _ = self.stdin.write_all(b"{\"id\":0,\"method\":\"shutdown\"}\n");
        let _ = self.stdin.flush();
        std::thread::sleep(std::time::Duration::from_millis(120));
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// 应用全局状态
pub struct AppState {
    pub sidecar: Mutex<Option<Sidecar>>,
    /// 最近一次操作日志 (前端可拉取)
    pub log: Mutex<Vec<LogEntry>>,
}

impl AppState {
    pub fn new() -> Self {
        Self {
            sidecar: Mutex::new(None),
            log: Mutex::new(Vec::new()),
        }
    }

    /// 执行一次 RPC。整段持锁 => 对后端而言天然串行。
    pub fn rpc(&self, method: &str, params: Value) -> RpcResult<Value> {
        let mut guard = self
            .sidecar
            .lock()
            .map_err(|_| "状态锁已中毒".to_string())?;
        let sc = guard.as_mut().ok_or("后端未启动")?;
        sc.call_raw(method, params)
    }

    /// 记录一条操作日志
    pub fn push_log(&self, entry: LogEntry) {
        if let Ok(mut log) = self.log.lock() {
            log.push(entry);
            // 只保留最近 500 条, 避免无限增长
            if log.len() > 500 {
                let excess = log.len() - 500;
                log.drain(0..excess);
            }
        }
    }
}

#[derive(Clone, Serialize)]
pub struct LogEntry {
    pub time: String,
    pub level: String,
    pub message: String,
}

/// 启动后端。返回启动结果字符串。
pub fn start_sidecar(app: &AppHandle) -> RpcResult<String> {
    let state = app.state::<AppState>();
    let mut guard = state.sidecar.lock().map_err(|_| "状态锁已中毒")?;

    if let Some(sc) = guard.as_mut() {
        if sc.is_running() {
            return Ok("后端已在运行".into());
        }
    }

    // 解析 sidecar 可执行文件路径
    let path = resolve_sidecar_path(app)?;
    let sc = Sidecar::spawn(&path).map_err(|e| format!("启动后端失败 ({path}): {e}"))?;
    *guard = Some(sc);

    // 启动后做一次握手, 确认协议通
    {
        let sc = guard.as_mut().unwrap();
        sc.call_raw("ping", Value::Null)?;
    }

    Ok(path)
}

/// 找到 sidecar 可执行文件的真实路径。
///
/// 开发时 Tauri 会把 externalBin 放到 target/debug 下并带三元组后缀;
/// 安装后则与主程序同目录。这里按几种常见位置依次探测。
fn resolve_sidecar_path(_app: &AppHandle) -> RpcResult<String> {
    let triple = env!("SIDECAR_TRIPLE");

    let mut candidates: Vec<std::path::PathBuf> = Vec::new();

    // 1) 与当前 exe 同目录 (打包后 / 部分开发场景)
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            candidates.push(dir.join("monitor_rpc.exe"));
            candidates.push(dir.join(format!("monitor_rpc-{triple}.exe")));
        }
    }

    // 2) 源码树内的 native/ 目录 (开发场景兜底)
    let manifest_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"));
    if let Some(root) = manifest_dir.parent() {
        candidates.push(root.join("native").join("monitor_rpc.exe"));
    }
    candidates.push(manifest_dir.join("binaries").join(format!("monitor_rpc-{triple}.exe")));

    for c in &candidates {
        if c.is_file() {
            return Ok(c.to_string_lossy().into_owned());
        }
    }

    let tried = candidates
        .iter()
        .map(|p| format!("  {}", p.display()))
        .collect::<Vec<_>>()
        .join("\n");
    Err(format!("找不到 monitor_rpc.exe, 已尝试:\n{tried}"))
}

/// 前端可调用的通用 RPC 命令。
///
/// 之所以做一个通用入口而不是为每个方法写一个命令:
/// 后端的方法集合与属性集合本来就是数据驱动的 (VCP 码表),
/// 前端从 `enum_tables` 拿到数据后可以自由组合, 无需每次改 Rust 层。
#[tauri::command]
pub async fn rpc(
    app: AppHandle,
    method: String,
    params: Option<Value>,
) -> RpcResult<Value> {
    // 写操作记录日志, 便于用户回看"我刚才改了什么"
    let is_write = matches!(method.as_str(), "set_prop" | "action");
    let snapshot = params.clone();

    // RPC 是阻塞 IO (DDC/CI 慢), 放到阻塞线程池, 避免卡住 async 运行时。
    // AppHandle 是 Send + Sync + Clone, 可以安全地移进闭包。
    let app2 = app.clone();
    let method2 = method.clone();
    let params2 = params.unwrap_or(Value::Null);

    let result = tauri::async_runtime::spawn_blocking(move || {
        let state = app2.state::<AppState>();
        state.rpc(&method2, params2)
    })
    .await
    .map_err(|e| format!("任务调度失败: {e}"))?;

    let state = app.state::<AppState>();
    let time = now_string();
    match &result {
        Ok(_) if is_write => state.push_log(LogEntry {
            time,
            level: "ok".into(),
            message: format!("{} {}", method, compact(&snapshot)),
        }),
        Err(e) if is_write => state.push_log(LogEntry {
            time,
            level: "error".into(),
            message: format!("{} {} 失败: {}", method, compact(&snapshot), e),
        }),
        _ => {}
    }

    // 通知前端日志有更新
    if is_write {
        let _ = app.emit("log-updated", ());
    }

    result
}

fn compact(v: &Option<Value>) -> String {
    match v {
        Some(Value::Object(m)) => m
            .iter()
            .map(|(k, val)| format!("{k}={val}"))
            .collect::<Vec<_>>()
            .join(" "),
        Some(other) => other.to_string(),
        None => String::new(),
    }
}

fn now_string() -> String {
    // 不引入 chrono, 用系统时间手动格式化
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let h = (secs / 3600) % 24;
    let m = (secs / 60) % 60;
    let s = secs % 60;
    format!("{h:02}:{m:02}:{s:02}")
}

/// 获取后端运行状态
#[tauri::command]
pub async fn backend_status(app: AppHandle) -> RpcResult<Value> {
    let state = app.state::<AppState>();
    let mut guard = state.sidecar.lock().map_err(|_| "状态锁已中毒")?;
    let running = guard.as_mut().map(|s| s.is_running()).unwrap_or(false);
    Ok(json!({ "running": running }))
}

/// 重启后端 (显示器热插拔后使用)
#[tauri::command]
pub async fn restart_backend(app: AppHandle) -> RpcResult<String> {
    {
        let state = app.state::<AppState>();
        let mut guard = state.sidecar.lock().map_err(|_| "状态锁已中毒")?;
        *guard = None; // Drop 会关闭旧进程
    }
    start_sidecar(&app)
}

/// 读取操作日志
#[tauri::command]
pub async fn get_log(app: AppHandle) -> RpcResult<Vec<LogEntry>> {
    let state = app.state::<AppState>();
    let log = state.log.lock().map_err(|_| "状态锁已中毒")?;
    Ok(log.clone())
}

/// 清空操作日志
#[tauri::command]
pub async fn clear_log(app: AppHandle) -> RpcResult<()> {
    let state = app.state::<AppState>();
    let mut log = state.log.lock().map_err(|_| "状态锁已中毒")?;
    log.clear();
    Ok(())
}

/// 把 VCP 码表导出给前端 (数据驱动 UI 的基础)。
///
/// 传 index 是有意义的: 枚举表会按该显示器的 caps string 过滤,
/// 只返回它真正声明支持的取值 (否则前端会列出本机没接的 HDMI/Tuner 等)。
#[tauri::command]
pub async fn enum_tables(app: AppHandle, index: Option<i64>) -> RpcResult<Value> {
    let params = index.map(|i| json!({ "index": i }));
    rpc(app, "enum_tables".into(), params).await
}

/// 批量读取: 把多个方法的调用打包, 减少前端往返。
/// 注意: 后端仍是串行的, 这里只是省掉 JS 侧的 Promise 编排。
#[tauri::command]
pub async fn rpc_batch(
    app: AppHandle,
    calls: Vec<BatchCall>,
) -> RpcResult<HashMap<String, Value>> {
    let mut out = HashMap::new();
    for c in calls {
        let key = c.key.clone();
        match rpc(app.clone(), c.method, c.params).await {
            Ok(v) => {
                out.insert(key, json!({ "ok": true, "result": v }));
            }
            Err(e) => {
                out.insert(key, json!({ "ok": false, "error": e }));
            }
        }
    }
    Ok(out)
}

#[derive(serde::Deserialize)]
pub struct BatchCall {
    pub key: String,
    pub method: String,
    pub params: Option<Value>,
}
