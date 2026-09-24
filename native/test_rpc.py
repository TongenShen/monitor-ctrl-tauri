#!/usr/bin/env python3
# coding = utf-8
"""
test_rpc.py - monitor_rpc.exe 的交互式调试脚本

用法:
    python test_rpc.py            # 跑一遍内置的冒烟测试
    python test_rpc.py -i         # 进入交互模式, 手动输入 JSON 请求

不修改显示器任何设置, 只做读取与枚举; 写操作需要显式传入 --write 才会执行。
"""
import json
import subprocess
import sys
import os
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, 'monitor_rpc.exe')


class RpcClient:
    """通过 stdio 与 C 后端通信的极简客户端"""

    def __init__(self, exe=EXE):
        if not os.path.exists(exe):
            raise SystemExit('not found: {}\n请先编译: gcc -std=c99 -O2 -o monitor_rpc.exe '
                             'monitor_rpc.c monitor_core.c vcp_code.c mjson.c -ldxva2 -luser32'
                             .format(exe))
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._id = 0

    def call(self, method, **params):
        self._id += 1
        req = {'id': self._id, 'method': method}
        req.update(params)
        line = json.dumps(req, ensure_ascii=False) + '\n'
        self.proc.stdin.write(line.encode('utf-8'))
        self.proc.stdin.flush()

        resp_line = self.proc.stdout.readline()
        if not resp_line:
            err = self.proc.stderr.read().decode('utf-8', 'replace')
            raise RuntimeError('backend closed stdout. stderr:\n' + err)
        return json.loads(resp_line.decode('utf-8'))

    def stderr_text(self):
        """非阻塞读取 stderr 里已有的内容"""
        import threading
        chunks = []

        def reader():
            try:
                chunks.append(self.proc.stderr.read())
            except Exception:
                pass

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        t.join(timeout=0.4)
        return b''.join(chunks).decode('utf-8', 'replace')

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()


def smoke_test(client, do_write=False):
    print('=' * 62)
    print('1. ping')
    print('=' * 62)
    r = client.call('ping')
    print(json.dumps(r, ensure_ascii=False, indent=2))

    print()
    print('=' * 62)
    print('2. list_monitors  (枚举 + 打开 DDC/CI 通道)')
    print('=' * 62)
    r = client.call('list_monitors')
    print(json.dumps(r, ensure_ascii=False, indent=2))
    if not r.get('ok'):
        print('\n!! 后端报告失败:', r.get('error'))
        print('!! stderr:')
        print(client.stderr_text())
        return

    monitors = r['result']['monitors']
    if not monitors:
        print('\n!! 没有检测到可用显示器 (可能全是笔记本内屏, 或未开启 DDC/CI)')
        return

    idx = monitors[0]['index']

    print()
    print('=' * 62)
    print('3. get_info  (型号 / 面板 / 开机小时 / caps)')
    print('=' * 62)
    r = client.call('get_info', index=idx)
    print(json.dumps(r, ensure_ascii=False, indent=2))

    print()
    print('=' * 62)
    print('4. get_props  (全部可读属性, 每个属性一次 I2C 往返, 稍慢)')
    print('=' * 62)
    r = client.call('get_props', index=idx)
    print(json.dumps(r, ensure_ascii=False, indent=2))

    print()
    print('=' * 62)
    print('5. enum_tables  (纯数据表, 不碰硬件)')
    print('=' * 62)
    r = client.call('enum_tables')
    print(json.dumps(r, ensure_ascii=False, indent=2))

    print()
    print('=' * 62)
    print('6. 边界情况')
    print('=' * 62)
    print('  不存在的 index :', json.dumps(client.call('get_props', index=99), ensure_ascii=False))
    print('  未知 method    :', json.dumps(client.call('no_such_method'), ensure_ascii=False))
    print('  未知 prop      :', json.dumps(
        client.call('set_prop', index=idx, prop='nope', value=1), ensure_ascii=False))
    print('  超出范围的值   :', json.dumps(
        client.call('set_prop', index=idx, prop='brightness', value=999999),
        ensure_ascii=False))

    if do_write:
        print()
        print('=' * 62)
        print('7. 写操作测试 (恢复原值)')
        print('=' * 62)
        cur = client.call('get_props', index=idx)['result']['brightness']
        print('  当前亮度 =', cur)
        print('  设为 {}:'.format(cur), json.dumps(
            client.call('set_prop', index=idx, prop='brightness', value=cur),
            ensure_ascii=False))


def interactive(client):
    print('交互模式: 直接输入 JSON 请求, 空行或 Ctrl+C 退出')
    print('例: {"id":1,"method":"get_props","index":0}')
    print()
    while True:
        try:
            line = input('> ').strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            break
        try:
            req = json.loads(line)
        except json.JSONDecodeError as e:
            print('JSON 解析失败:', e)
            continue
        method = req.pop('method', None)
        if not method:
            print('缺少 method 字段')
            continue
        resp = client.call(method, **req)
        print(json.dumps(resp, ensure_ascii=False, indent=2))


def main():
    ap = argparse.ArgumentParser(description='monitor_rpc.exe 调试工具')
    ap.add_argument('-i', '--interactive', action='store_true', help='进入交互模式')
    ap.add_argument('--write', action='store_true', help='执行写操作测试(会把亮度设回原值)')
    args = ap.parse_args()

    client = RpcClient()
    try:
        if args.interactive:
            interactive(client)
        else:
            smoke_test(client, do_write=args.write)
    finally:
        err = client.stderr_text()
        client.close()
        if err.strip():
            print()
            print('=' * 62)
            print('后端 stderr')
            print('=' * 62)
            print(err.strip())


if __name__ == '__main__':
    main()
