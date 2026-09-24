fn main() {
    // 把目标三元组编译期注入, 供 sidecar 路径解析使用
    // (Tauri 的 externalBin 文件名带三元组后缀)
    let triple = std::env::var("TARGET").unwrap_or_else(|_| "x86_64-pc-windows-msvc".into());
    println!("cargo:rustc-env=SIDECAR_TRIPLE={triple}");

    tauri_build::build()
}
