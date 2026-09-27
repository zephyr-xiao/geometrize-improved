#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""补丁 ↔ 工作树一致性校验(行尾无关)。

背景:patches/ 下的按特性拆分补丁系列存在格式混杂(LF/CRLF 混行尾、裸 @@ hunk、
新文件缺 @@ 头、各批次对同一文件重复导出导致内容重叠),无法按编号顺序重放。
本工具改用「规范全量补丁」做双门禁:

  G1 重放复现:把 patches/regen/ 的全量补丁应用到基线副本,与工作树逐文件对比,
     必须零漂移(fresh clone + 补丁 = 当前改进树,行尾无关)。
  G2 归档新鲜度:已提交的 regen 补丁与按当前工作树现生成的内容一致;
     不一致 = 工作树有改动未重新导出补丁(逐文件列出)。

两侧统一在 EOL 归一(LF)世界做 diff/patch/对比:内容级等价门禁,不修改工作树。
行尾分布作为 INFO 输出(improved-lib 有 24 个纯 LF 文件,与树的 CRLF 约定不一致,已如实上报)。

app 侧为报告口径:构建产物/第三方 pin(vendored libs)/二进制文件豁免,漂移只告警不失败。
用法:python tools/verify_patches.py [--skip-app] [--keep-temp] [--init]
  --init 显式建档/重建(缺失或漂移的归档按当前工作树重写);缺省时缺失/漂移 = FAIL
exit:0=通过;1=lib 门禁失败
"""
import argparse
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 二进制扩展名:不进补丁(diff 无法表达),重放对比时豁免,单独扫描上报
BIN_EXT = {".png", ".jpg", ".jpeg", ".gif", ".ico", ".icns", ".cur", ".qm",
           ".ttf", ".woff", ".woff2", ".exe", ".dll", ".pdf", ".zip", ".raw",
           ".wav", ".mp3", ".ogg", ".svgz", ".lib", ".pdb"}

SKIP_DIRS = {"build", "build-asan", "obj", "qt_gen", "release", "debug",
             ".git-disabled", "__pycache__"}
SKIP_FILE_PATTERNS = ["Makefile", "Makefile.*", ".qmake.stash", "*.pro.user*",
                      "aqtinstall.log", "moc_*", "ui_*.h", "qrc_*.cpp",
                      "*.o", "*.obj", "*.exe", "*.dll", "*.rej", "*.orig"]

# app 侧第三方 pin 目录(整目录豁免:由 docs/vendored-commits.txt 管理,不是应用层改动)
APP_SKIP_PREFIXES = ("/lib/chaiscript/", "/lib/cereal/", "/lib/dataslinger/",
                     "/lib/burstlinker/")

# app 侧上游仓库元数据(README/LICENSE/CI 配置等,非应用源码;improved-app 工作树本就不携带)
APP_META_FILE_SKIP = [".appveyor.yml", ".gitignore", ".gitmodules", "LICENSE",
                      "LICENSE.txt", "README.md", "README"]
APP_META_DIR_SKIP = {"screenshots"}

REGEN_DIR = os.path.join(REPO, "patches", "regen")
LIB_REGEN = os.path.join(REGEN_DIR, "geometrize-lib-full.diff")
APP_REGEN = os.path.join(REGEN_DIR, "geometrize-app-full.diff")

# 剥 ---/+++ 行的时间戳。[^\t\n] 把匹配钉死在单行内:[^\t] 会跨越换行,遇到无时间戳的
# "+++ /dev/null"(删除型文件头)时会吞掉 hunk 体里首个含 \t 的行尾内容(实测丢 3 字节)
TS_RE = re.compile(rb"^((?:---|\+\+\+) [^\t\n]*)\t[^\n]*$", re.M)


def is_binary(data):
    return b"\x00" in data


def eol_normalize(data):
    """CRLF/CR -> LF;二进制原样返回。返回 (数据, 是否二进制)。"""
    if is_binary(data):
        return data, True
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n"), False


def copy_tree_normalized(src, dst, app_mode=False):
    """复制源码树到 tmp,统一 EOL 为 LF;跳过构建产物/第三方 pin。
    返回 (文件数, eol统计 dict, 二进制文件相对路径 list)。"""
    n_files, eol_stats, binaries = 0, {"CRLF": 0, "LF": 0, "MIXED": 0, "BIN": 0}, []
    for root, dirs, files in os.walk(src):
        rel_root = os.path.relpath(root, src).replace("\\", "/")
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS
                   and not (app_mode and rel_root == "." and d in APP_META_DIR_SKIP)]
        for fn in files:
            if any(fnmatch.fnmatch(fn, p) for p in SKIP_FILE_PATTERNS):
                continue
            if app_mode and fn in APP_META_FILE_SKIP:
                continue
            rel = (rel_root + "/" if rel_root != "." else "") + fn
            if app_mode and any(("/" + rel).replace("//", "/").startswith(p) for p in APP_SKIP_PREFIXES):
                continue
            s = os.path.join(root, fn)
            d = os.path.join(dst, rel)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            data = open(s, "rb").read()
            norm, was_bin = eol_normalize(data)
            if was_bin:
                eol_stats["BIN"] += 1
                binaries.append(rel)
            else:
                crlf = data.count(b"\r\n")
                lf = data.count(b"\n") - crlf
                if crlf and lf:
                    eol_stats["MIXED"] += 1
                elif crlf:
                    eol_stats["CRLF"] += 1
                else:
                    eol_stats["LF"] += 1
                n_files += 1
            open(d, "wb").write(norm)
    return n_files, eol_stats, binaries


def run(cmd, cwd=None, stdin_path=None):
    stdin = open(stdin_path, "rb") if stdin_path else subprocess.DEVNULL
    p = subprocess.run(cmd, cwd=cwd, stdin=stdin, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if stdin_path:
        stdin.close()
    return p.returncode, p.stdout


def regen_diff(a_dir, b_dir, out_path, label_a="a", label_b="b"):
    """在 a/b 兄弟目录布局下生成规范全量 diff(-ruN,已归一 LF,无时间戳)。"""
    parent = os.path.dirname(a_dir)
    # -x 参数必须排序:diff 会把命令行回显进补丁首行,set 遍历序受哈希随机化影响会破坏 G2 确定性
    bin_flags = sum([["-x", "*" + e] for e in sorted(BIN_EXT)], [])
    rc, out = run(["diff", "-ruN", "-x", "*.rej", "-x", "*.orig"] + bin_flags
                  + [label_a, label_b], cwd=parent)
    # 剥掉 ---/+++ 行的时间戳,保证 G2 新鲜度比对确定性
    out = TS_RE.sub(rb"\1", out)
    with open(out_path, "wb") as f:
        f.write(out)
    return rc, out


def apply_patch(patch_path, target_dir):
    rc, out = run(["patch", "--batch", "--no-backup-if-mismatch", "-s", "-p1"],
                  cwd=target_dir, stdin_path=patch_path)
    rej = []
    for root, dirs, files in os.walk(target_dir):
        rej += [os.path.join(root, f) for f in files if f.endswith(".rej")]
    return rc, out, rej


def tree_diff(a_dir, b_dir):
    rc, out = run(["diff", "-r", "-q", "-x", "*.rej", "-x", "*.orig"]
                  + sum([["-x", "*" + e] for e in BIN_EXT], []) + [a_dir, b_dir])
    return rc, out.decode("utf-8", "replace")


def patch_sections(patch_bytes):
    """按目标文件切分补丁文本:{minus路径: 该文件的补丁段}。"""
    sections, cur_key, cur = {}, None, []
    for line in patch_bytes.splitlines(keepends=True):
        m = re.match(rb"^--- (?:a/)?(\S+)", line)
        if m:
            if cur_key is not None:
                sections[cur_key] = b"".join(cur)
            cur_key, cur = m.group(1).decode(), [line]
        elif cur_key is not None:
            cur.append(line)
    if cur_key is not None:
        sections[cur_key] = b"".join(cur)
    return sections


def verify_side(name, baseline_src, improved_src, regen_path, app_mode=False, keep=False, init=False):
    """单侧验证:返回 (ok, 漂移行)。G1 重放复现 + G2 归档新鲜度。"""
    tmp = tempfile.mkdtemp(prefix="geometrize_verify_%s_" % name)
    print("\n== %s 侧(tmp: %s)" % (name, tmp))
    ok = True

    n_a, eol_a, bin_a = copy_tree_normalized(baseline_src, os.path.join(tmp, "a"), app_mode)
    n_b, eol_b, bin_b = copy_tree_normalized(improved_src, os.path.join(tmp, "b"), app_mode)
    print("[INFO] 归一 LF 复制:基线 %d 文件 %s | 改进 %d 文件 %s"
          % (n_a, eol_a, n_b, eol_b))
    if eol_b["MIXED"] or eol_b["LF"]:
        print("[INFO] 注意:改进树存在 %d 个 LF / %d 个 MIXED 行尾文件(树约定 CRLF),门禁为行尾无关故不失败"
              % (eol_b["LF"], eol_b["MIXED"]))

    # G2:现生成规范补丁 vs 已提交归档
    rc, _ = regen_diff(os.path.join(tmp, "a"), os.path.join(tmp, "b"),
                       os.path.join(tmp, "regen_now.diff"))
    now = open(os.path.join(tmp, "regen_now.diff"), "rb").read()
    if not os.path.exists(regen_path):
        # 归档缺失 = 信任锚缺失:只有显式 --init 才允许建档,否则按 FAIL 处理。
        # 自动建档会让"删除归档 → 双门禁静默通过"成为门禁绕过路径。
        if not init:
            ok = False
            print("[FAIL] G2 归档缺失:%s(信任锚缺失,拒绝自动生成;确认工作树状态后用 --init 显式建档)"
                  % os.path.relpath(regen_path, REPO))
        else:
            os.makedirs(REGEN_DIR, exist_ok=True)
            for _ in range(2):
                shutil.copy(os.path.join(tmp, "regen_now.diff"), regen_path)
                if open(regen_path, "rb").read() == now:
                    break  # 防写坏:回读校验,不一致则重写一次
            print("[INIT] 显式建档:已生成规范补丁 %s" % os.path.relpath(regen_path, REPO))
    else:
        archived = TS_RE.sub(rb"\1", open(regen_path, "rb").read())
        if archived == now:
            print("[PASS] G2 归档新鲜度:已提交补丁与工作树现生成一致")
        elif init:
            # 显式重建:工作树改动经确认后,按当前树重写归档(带回读校验)
            for _ in range(2):
                shutil.copy(os.path.join(tmp, "regen_now.diff"), regen_path)
                if open(regen_path, "rb").read() == now:
                    break
            print("[INIT] 显式重建:已按当前工作树重新生成规范补丁 %s" % os.path.relpath(regen_path, REPO))
        else:
            ok = False
            old_s, new_s = patch_sections(archived), patch_sections(now)
            print("[FAIL] G2 归档新鲜度:工作树与已提交补丁存在漂移(归档 %d B vs 现生成 %d B)"
                  % (len(archived), len(now)))
            for t in sorted(set(old_s) - set(new_s)):
                print("       归档独有(工作树已无此差异): %s" % t)
            for t in sorted(set(new_s) - set(old_s)):
                print("       新增差异文件: %s" % t)
            for t in sorted(set(old_s) & set(new_s)):
                if old_s[t] != new_s[t]:
                    print("       补丁内容有变: %s" % t)

    # G1:重放复现(归档缺失且未 --init 时无锚可验,G2 已报 FAIL,这里跳过)
    if os.path.exists(regen_path):
        shutil.copytree(os.path.join(tmp, "a"), os.path.join(tmp, "apply"))
        rc, out, rej = apply_patch(regen_path, os.path.join(tmp, "apply"))
        if rc != 0 or rej:
            ok = False
            print("[FAIL] G1 补丁应用失败 exit=%d,.rej %d 个" % (rc, len(rej)))
            print("       " + out.decode("utf-8", "replace")[:500])
        else:
            rc, drift = tree_diff(os.path.join(tmp, "apply"), os.path.join(tmp, "b"))
            if rc == 0:
                print("[PASS] G1 重放复现:fresh 基线 + 规范补丁 == 工作树(逐文件零漂移,行尾无关)")
            else:
                ok = False
                lines = [l for l in drift.splitlines() if l.strip()]
                print("[FAIL] G1 重放后与工作树存在漂移:%d 处" % len(lines))
                for l in lines[:30]:
                    print("       " + l.replace(tmp, "<tmp>"))

    # 二进制豁免上报(app 报告口径;lib 侧二进制应恒为 0)
    both_a, both_b = set(bin_a), set(bin_b)
    diff_bin = [b for b in sorted(both_b & both_a)
                if open(os.path.join(baseline_src, b), "rb").read()
                != open(os.path.join(improved_src, b), "rb").read()]
    only_new = sorted(both_b - both_a)
    if diff_bin or only_new:
        tag = "INFO" if app_mode else "FAIL"
        if not app_mode:
            ok = False
        print("[%s] 二进制文件差异(不进补丁):改动 %d、新增 %d" % (tag, len(diff_bin), len(only_new)))
        for b in (diff_bin + only_new)[:10]:
            print("       %s" % b)

    if not keep:
        shutil.rmtree(tmp, ignore_errors=True)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-app", action="store_true", help="跳过 app 侧(报告口径)")
    ap.add_argument("--keep-temp", action="store_true", help="保留临时目录供排查")
    ap.add_argument("--init", action="store_true",
                    help="显式建档/重建:归档缺失或与工作树漂移时,按当前工作树重写规范补丁(缺省时缺失/漂移=FAIL)")
    args = ap.parse_args()

    lib_ok = verify_side("lib",
                         os.path.join(REPO, "src", "baseline-lib", "geometrize"),
                         os.path.join(REPO, "src", "improved-lib", "geometrize"),
                         LIB_REGEN, app_mode=False, keep=args.keep_temp, init=args.init)

    app_ok = True
    if not args.skip_app:
        app_ok = verify_side("app",
                             os.path.join(REPO, "upstream", "geometrize-app"),
                             os.path.join(REPO, "src", "improved-app"),
                             APP_REGEN, app_mode=True, keep=args.keep_temp, init=args.init)
        if not app_ok:
            print("\n[WARN] app 侧存在差异(报告口径,不失败):构建产物/第三方 pin/二进制已豁免,"
                  "其余逐文件差异请人工分类")

    if lib_ok and app_ok:
        print("\n== 全部门禁 PASS ==")
        return 0
    print("\n== lib 门禁 FAIL(禁止合入)== " if not lib_ok else "\n== lib PASS,app 侧有报告项 ==")
    return 0 if lib_ok else 1


if __name__ == "__main__":
    sys.exit(main())
