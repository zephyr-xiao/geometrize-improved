#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""修复工作区内 exe 继承来的 Low 完整性标签（详见 docs/ROADMAP.md 陷阱 #24）。

背景：dsh（DeepSeek Harness，本机 0.2.0-rc.2）的 Windows 沙箱会给授权根目录（本仓库
所在的工作区根）盖一条可继承的 Low 强制完整性标签；此后在该目录树内新建或更新的每个
文件都会物化出 Low 标签，而由 Low 文件启动的进程按 Windows MIC 规则以低完整性
运行——写不了任何普通目录，表现为 Geometrize 另存为时 Windows 原生对话框误报
「你没有权限在此位置中保存文件」。本脚本把交付入口/构建产物的标签显式重置为
Medium（显式标签优先于继承标签，无需管理员）。

注意：每次重新构建 exe 之后都要再跑一次，否则新生成的 exe 会重新继承 Low 标签。
工作区层面已有自动修复（计划任务 dsh-low-integrity-autofix，每 15 分钟跑
scripts\dsh-label-autofix\autofix.py），本脚本保留作单项目手动兜底。

用法：
    python tools/fix_integrity_label.py [exe 路径...]
    不带参数时修复默认入口与构建产物；指定路径时只修指定文件。
"""
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

DEFAULT_TARGETS = [
    REPO / "dist" / "Geometrize-Improved" / "Geometrize.exe",
    REPO / "Geometrize.exe",
    REPO / "src" / "improved-app" / "build" / "Release" / "Geometrize.exe",
]


def reset_label(path: Path) -> bool:
    """把目标文件的强制完整性标签显式设为 Medium；返回是否成功。"""
    r = subprocess.run(["icacls", str(path), "/setintegritylevel", "Medium"],
                       capture_output=True, text=True, encoding="gbk", errors="replace")
    return r.returncode == 0


def show_label(path: Path) -> str:
    """读取当前的完整性标签行用于回显。"""
    r = subprocess.run(["icacls", str(path)], capture_output=True, text=True,
                       encoding="gbk", errors="replace")
    for line in r.stdout.splitlines():
        if "Mandatory" in line:
            return line.strip()
    return "(无完整性标签行 = 默认 Medium)"


def main() -> int:
    targets = [Path(p) for p in sys.argv[1:]] or DEFAULT_TARGETS
    failed = 0
    for p in targets:
        if not p.exists():
            print("[SKIP] %s 不存在" % p)
            continue
        ok = reset_label(p)
        print("[%s] %s" % ("OK  " if ok else "FAIL", p))
        if ok:
            print("        %s" % show_label(p))
        else:
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
