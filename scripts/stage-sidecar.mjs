/**
 * stage-sidecar.mjs - 把编译好的 C 后端复制到 Tauri 的 sidecar 目录。
 *
 * Tauri 的 externalBin 机制要求二进制文件名带目标三元组后缀,
 * 例如 Windows MSVC 下必须是 monitor_rpc-x86_64-pc-windows-msvc.exe。
 * 这个脚本负责做这个改名与拷贝, 让 npm run dev / build 前自动完成。
 */
import { copyFileSync, existsSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { execFileSync } from 'node:child_process';

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = resolve(__dirname, '..');
const nativeDir = join(root, 'native');
const binDir = join(root, 'src-tauri', 'binaries');

/** 当前 Rust 目标三元组 */
function targetTriple() {
  try {
    const out = execFileSync('rustc', ['-vV'], { encoding: 'utf8' });
    const m = out.match(/^host:\s*(.+)$/m);
    if (m) return m[1].trim();
  } catch {
    /* rustc 不在 PATH, 退回默认 */
  }
  return 'x86_64-pc-windows-msvc';
}

const triple = targetTriple();
const src = join(nativeDir, 'monitor_rpc.exe');

if (!existsSync(src)) {
  console.error(`[sidecar] 找不到 ${src}`);
  console.error('[sidecar] 请先编译 C 后端:  cd native && build.bat');
  process.exit(1);
}

mkdirSync(binDir, { recursive: true });

const dest = join(binDir, `monitor_rpc-${triple}.exe`);

// 先删除旧文件, 否则 Windows 上若文件被占用会拷贝失败
if (existsSync(dest)) {
  rmSync(dest, { force: true });
}
copyFileSync(src, dest);

const kb = (await import('node:fs')).statSync(dest).size / 1024;
console.log(`[sidecar] ${src}  ->  ${dest}  (${kb.toFixed(1)} KB)`);
