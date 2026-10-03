#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Qt 渲染侧对拍工具（Qt6 迁移验收 + 长期回归门禁）

用途
----
1) 迁移验收 / 双 exe 对比：同一批 headless ChaiScript 用例分别在两个 exe 上跑
   （典型：Qt5 旧包 vs Qt6 新包），对导出物按"分层标准"比对。
2) 长期回归：当前 exe 对冻结参考（tools/qt_goldens.csv + tools/qt_render_ref/）校验，
   改动应用层渲染代码或升级 Qt 版本后可重跑。

判据分层
--------
- *.json / *.gif            : 逐字节严格（lib 导出 / BurstLinker 编码，与 Qt 无关）
- *.bitmap.png              : 解码后逐像素严格（位图数据直转 QImage，无绘制参与）
- *.raster.png              : 走 QSvgRenderer 光栅化（Qt 6.7+ 重写过 SVG 模块）→
                              量化比对（最大通道差 / 差异像素占比），阈值见 TOL_*

用法
----
  python tools/qt_render_ab.py run     --exe <exe> [--out <dir>] [--cases 01,03]
  python tools/qt_render_ab.py compare --ref-dir <dirA> --cand-dir <dirB>
  python tools/qt_render_ab.py check   --exe <exe>          # 对冻结参考校验
  python tools/qt_render_ab.py accept  --exe <exe>          # 用当前 exe 输出重采冻结参考

用例脚本在 tools/qt_render_ab/cases/，@IN@ / @OUT@ 由本脚本替换。
注意：exe 为 GUI 子系统程序，脚本异常时 runScript 会弹模态框挂死——
本脚本带超时强杀，脚本内也用 try/catch 落 status 文件兜底。
"""

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CASES_DIR = REPO / "tools" / "qt_render_ab" / "cases"
GOLDENS_CSV = REPO / "tools" / "qt_goldens.csv"
REF_DIR = REPO / "tools" / "qt_render_ref"
INPUT_DIR = REPO / "src" / "testdata" / "images"
# 输出必须落工作区内:本机沙箱下应用进程写工作区外路径会被拒(QFile open 失败,导出静默 false)
DEFAULT_OUT_ROOT = REPO / ".tmp_qt_render_ab"

# SVG 光栅化量化阈值（仅对 *.raster.png 生效；其余产物零容忍）
TOL_MAX_CHANNEL_DIFF = 16   # 单通道最大绝对差（0-255）
TOL_MAX_DIFF_RATIO = 0.02   # 差异像素占比上限
RUN_TIMEOUT_SEC = 300

# 不是产物的辅助文件
AUX_SUFFIXES = (".runner.chai", ".run.log", ".status.txt")


def classify(name: str) -> str:
    """按产物名后缀判定比对类别（命名约定见用例脚本）。"""
    if name.endswith(".qtinput.png"):
        return "png-qtinput"
    if name.endswith(".bitmap.png"):
        return "png-strict"
    if name.endswith(".raster.png"):
        return "png-tolerant"
    if name.endswith(".json"):
        return "bytes-json"
    if name.endswith(".gif"):
        return "bytes-gif"
    if name.endswith(".svg"):
        return "bytes-svg"
    raise ValueError(f"无法判定产物类别: {name}")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def png_metrics(path: Path) -> dict:
    """PNG 解码后的像素指标（文件字节哈希受编码器版本影响，不作跨版本判据）。"""
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    return {
        "width": im.width,
        "height": im.height,
        "pixel_sha256": hashlib.sha256(im.tobytes()).hexdigest(),
    }


def png_quant_diff(a: Path, b: Path) -> dict:
    """两图量化差异：最大通道差 + 差异像素数与占比。"""
    import numpy as np
    from PIL import Image
    ia = np.asarray(Image.open(a).convert("RGBA"), dtype=np.int16)
    ib = np.asarray(Image.open(b).convert("RGBA"), dtype=np.int16)
    if ia.shape != ib.shape:
        return {"shape_match": False}
    d = np.abs(ia - ib)
    per_pixel = d.max(axis=2)
    diff_pixels = int((per_pixel > 0).sum())
    total = int(ia.shape[0] * ia.shape[1])
    return {
        "shape_match": True,
        "max_channel_diff": int(d.max()) if d.size else 0,
        "diff_pixels": diff_pixels,
        "total_pixels": total,
        "diff_ratio": (diff_pixels / total) if total else 0.0,
    }


def artifact_metrics(path: Path) -> dict:
    kind = classify(path.name)
    m = {"kind": kind, "bytes": path.stat().st_size, "sha256": sha256_file(path)}
    if kind.startswith("png-"):
        m.update(png_metrics(path))
    return m


def list_cases() -> dict:
    return {p.stem: p for p in sorted(CASES_DIR.glob("*.chai"))}


def filter_cases(cases: dict, spec: str):
    if not spec:
        return cases
    keys = []
    for token in spec.split(","):
        token = token.strip()
        if token:
            keys.append(token)
    chosen = {k: v for k, v in cases.items() if any(t in k for t in keys)}
    if not chosen:
        raise SystemExit(f"没有匹配的用例: {spec}")
    return chosen


def run_case(exe: Path, case_id: str, case_path: Path, out_dir: Path) -> dict:
    """跑单个用例：替换占位符 → 生成 runner → 执行（带超时强杀）→ 收集状态。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    code = case_path.read_text(encoding="utf-8")
    code = code.replace("@IN@", INPUT_DIR.as_posix()).replace("@OUT@", out_dir.as_posix())
    runner = out_dir / f"{case_id}.runner.chai"
    runner.write_text(code, encoding="utf-8")

    log = out_dir / f"{case_id}.run.log"
    started = time.time()
    timed_out = False
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.Popen(
            [str(exe), "--script_file", str(runner)],
            stdout=f, stderr=subprocess.STDOUT,
        )
        try:
            rc = proc.wait(timeout=RUN_TIMEOUT_SEC)
        except subprocess.TimeoutExpired:
            timed_out = True
            proc.kill()
            proc.wait()
            rc = None

    status_file = out_dir / f"{case_id}.status.txt"
    status = status_file.read_text(encoding="utf-8", errors="replace").strip() if status_file.exists() else "<missing>"
    return {
        "case": case_id,
        "exit_code": rc,
        "seconds": round(time.time() - started, 1),
        "timed_out": timed_out,
        "status": status,
        "pass": (status == "PASS") and (rc == 0),
    }


def collect_artifacts(run_dir: Path, cases) -> dict:
    """按用例归集产物：{case_id: {artifact_name: path}}"""
    result = {c: {} for c in cases}
    for p in sorted(run_dir.iterdir()):
        if not p.is_file() or p.name.endswith(AUX_SUFFIXES):
            continue
        for c in cases:
            if p.name.startswith(c + "."):
                result[c][p.name] = p
                break
    return result


def run_all(exe: Path, out_dir: Path, cases: dict) -> dict:
    print(f"== 运行用例集: {exe}")
    print(f"   输出目录: {out_dir}")
    runs = {}
    for case_id, case_path in cases.items():
        r = run_case(exe, case_id, case_path, out_dir)
        flag = "PASS" if r["pass"] else "FAIL"
        print(f"   [{flag}] {case_id}  exit={r['exit_code']}  {r['seconds']}s"
              + ("  (超时强杀)" if r["timed_out"] else "")
              + ("" if r["status"] == "PASS" else f"  status={r['status']}"))
        runs[case_id] = r
    return runs


def compare_artifact(kind: str, a: Path, b: Path) -> dict:
    """分层比对单个产物（a=参考, b=候选）。"""
    ma = artifact_metrics(a)
    mb = artifact_metrics(b)
    out = {"kind": kind, "ref": ma, "cand": mb}
    if kind in ("bytes-json", "bytes-gif", "bytes-svg"):
        out["same"] = (ma["sha256"] == mb["sha256"])
        out["verdict"] = "SAME" if out["same"] else "DIFF"
    elif kind == "png-strict":
        same = (ma["pixel_sha256"] == mb["pixel_sha256"]) and (ma["width"], ma["height"]) == (mb["width"], mb["height"])
        out["same"] = same
        out["verdict"] = "SAME" if same else "DIFF"
    elif kind == "png-qtinput":
        # Qt 图像缩放输入路径:跨 Qt 版本存在取整差异属预期(Qt 缩放实现变更,见用例 06 注释),
        # 量化记录但不判失败;同版本内重跑仍逐像素一致(由 golden check 严格把关)。
        q = png_quant_diff(a, b)
        out["quant"] = q
        if not q.get("shape_match", False):
            out["verdict"] = "QTINPUT-DIFF(形状不符)"
        elif q["diff_pixels"] == 0:
            out["verdict"] = "SAME"
        else:
            out["verdict"] = "QTINPUT-DIFF(预期)"
    elif kind == "png-tolerant":
        q = png_quant_diff(a, b)
        out["quant"] = q
        if not q.get("shape_match", False):
            out["verdict"] = "DIFF"
        elif q["max_channel_diff"] <= TOL_MAX_CHANNEL_DIFF and q["diff_ratio"] <= TOL_MAX_DIFF_RATIO:
            out["verdict"] = "OK(within-tolerance)" if q["diff_pixels"] else "SAME"
        else:
            out["verdict"] = "OVER-TOLERANCE"
    else:
        raise ValueError(kind)
    return out


def compare_runs(ref_dir: Path, cand_dir: Path, cases: dict) -> dict:
    ref_art = collect_artifacts(ref_dir, cases)
    cand_art = collect_artifacts(cand_dir, cases)
    report = {"cases": {}}
    for case_id in cases:
        rows = []
        names = sorted(set(ref_art[case_id]) | set(cand_art[case_id]))
        for name in names:
            a = ref_art[case_id].get(name)
            b = cand_art[case_id].get(name)
            if a is None or b is None:
                rows.append({"artifact": name, "kind": classify(name), "verdict": "MISSING",
                             "missing_side": "cand" if b is None else "ref"})
                continue
            row = {"artifact": name}
            row.update(compare_artifact(classify(name), a, b))
            rows.append(row)
        report["cases"][case_id] = rows
    return report


def print_compare_report(report: dict) -> int:
    worst = 0
    expected_diffs = 0
    for case_id, rows in report["cases"].items():
        print(f"== {case_id}")
        for row in rows:
            verdict = row["verdict"]
            extra = ""
            if "quant" in row and row["quant"].get("shape_match", False):
                q = row["quant"]
                extra = f"  最大通道差={q['max_channel_diff']} 差异像素={q['diff_pixels']}/{q['total_pixels']} ({q['diff_ratio'] * 100:.3f}%)"
            print(f"   [{verdict}] {row['artifact']}{extra}")
            if verdict.startswith("QTINPUT-DIFF"):
                expected_diffs += 1
            elif verdict == "DIFF" or verdict == "MISSING":
                worst = max(worst, 1)
            elif verdict == "OVER-TOLERANCE":
                worst = max(worst, 3)
    if expected_diffs:
        print(f"   注:{expected_diffs} 项属预期差异(Qt 图像缩放输入路径,见用例 06 注释),不计失败")
    return worst


# ---------------- 冻结参考（golden） ----------------

def golden_record(run_dir: Path, cases: dict) -> dict:
    art = collect_artifacts(run_dir, cases)
    records = {}
    for case_id in cases:
        records[case_id] = {name: artifact_metrics(p) for name, p in art[case_id].items()}
    return records


def golden_write(runs: dict, run_dir: Path, cases: dict) -> None:
    """把当前输出冻结为参考：严格产物哈希写 CSV，光栅化参考图存 ref 目录。"""
    records = golden_record(run_dir, cases)
    lines = ["case,artifact,kind,bytes,sha256,pixel_sha256,width,height"]
    for case_id, items in records.items():
        for name, m in sorted(items.items()):
            lines.append(",".join([
                case_id, name, m["kind"], str(m["bytes"]), m["sha256"],
                m.get("pixel_sha256", ""), str(m.get("width", "")), str(m.get("height", "")),
            ]))
    GOLDENS_CSV.write_text("\n".join(lines) + "\n", encoding="utf-8")

    if REF_DIR.exists():
        shutil.rmtree(REF_DIR)
    for case_id, items in records.items():
        (REF_DIR / case_id).mkdir(parents=True, exist_ok=True)
        for name in items:
            shutil.copy2(run_dir / name, REF_DIR / case_id / name)
    print(f"[参考已更新] {GOLDENS_CSV.relative_to(REPO)} + {REF_DIR.relative_to(REPO)}/")


def golden_load() -> dict:
    records = {}
    if not GOLDENS_CSV.exists():
        raise SystemExit(f"缺少冻结参考: {GOLDENS_CSV}（先跑 accept）")
    for line in GOLDENS_CSV.read_text(encoding="utf-8").splitlines()[1:]:
        if not line.strip():
            continue
        case, artifact, kind, size, sha, px, w, h = line.split(",")
        records.setdefault(case, {})[artifact] = {
            "kind": kind, "bytes": int(size), "sha256": sha,
            "pixel_sha256": px, "width": w, "height": h,
        }
    return records


def golden_check(run_dir: Path, cases: dict) -> int:
    records = golden_load()
    art = collect_artifacts(run_dir, cases)
    worst = 0
    for case_id in cases:
        print(f"== {case_id}")
        expected = records.get(case_id, {})
        actual = art[case_id]
        for name in sorted(set(expected) | set(actual)):
            if name not in actual:
                print(f"   [MISSING] {name}（本次运行未产出）")
                worst = max(worst, 1)
                continue
            if name not in expected:
                print(f"   [NEW] {name}（参考里没有，需 accept 更新）")
                worst = max(worst, 3)
                continue
            exp = expected[name]
            kind = exp["kind"]
            if kind == "png-tolerant":
                q = png_quant_diff(REF_DIR / case_id / name, actual[name])
                ok = q.get("shape_match", False) and q["max_channel_diff"] <= TOL_MAX_CHANNEL_DIFF and q["diff_ratio"] <= TOL_MAX_DIFF_RATIO
                verdict = "OK" if ok else "OVER-TOLERANCE"
                qs = f"最大通道差={q.get('max_channel_diff')} 差异像素={q.get('diff_pixels')}/{q.get('total_pixels')} ({q.get('diff_ratio', 0) * 100:.3f}%)"
                print(f"   [{verdict}] {name}  {qs}")
                if not ok:
                    worst = max(worst, 3)
                continue
            m = artifact_metrics(actual[name])
            if kind in ("png-strict", "png-qtinput"):
                # 同版本内这两类都逐像素确定(png-qtinput 只跨 Qt 版本允许差异)
                ok = (m["pixel_sha256"] == exp["pixel_sha256"]) and (str(m["width"]) == exp["width"]) and (str(m["height"]) == exp["height"])
            else:
                ok = m["sha256"] == exp["sha256"]
            print(f"   [{'SAME' if ok else 'DIFF'}] {name}")
            if not ok:
                worst = max(worst, 1)
    return worst


def main() -> int:
    ap = argparse.ArgumentParser(description="Qt 渲染侧对拍工具（详见文件头注释）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="只运行用例，产物留在输出目录供人工检查")
    p_run.add_argument("--exe", required=True)
    p_run.add_argument("--out", default="")
    p_run.add_argument("--cases", default="", help="逗号分隔的用例子串过滤，如 01,03")

    p_cmp = sub.add_parser("compare", help="比对两次运行的产物目录（分层判据）")
    p_cmp.add_argument("--ref-dir", required=True)
    p_cmp.add_argument("--cand-dir", required=True)
    p_cmp.add_argument("--cases", default="")
    p_cmp.add_argument("--json", default="", help="可选：把完整比对结果写成 JSON")

    p_chk = sub.add_parser("check", help="当前 exe 对冻结参考校验")
    p_chk.add_argument("--exe", required=True)
    p_chk.add_argument("--cases", default="")

    p_acc = sub.add_parser("accept", help="用当前 exe 输出重采冻结参考（需人工确认输出合理）")
    p_acc.add_argument("--exe", required=True)
    p_acc.add_argument("--cases", default="")

    args = ap.parse_args()
    cases = filter_cases(list_cases(), getattr(args, "cases", ""))

    if args.cmd == "run":
        out = Path(args.out) if args.out else (DEFAULT_OUT_ROOT / time.strftime("%Y%m%d-%H%M%S"))
        runs = run_all(Path(args.exe), out, cases)
        failed = [c for c, r in runs.items() if not r["pass"]]
        if failed:
            print(f"[FAIL] 用例未通过: {', '.join(failed)}（看 *.status.txt / *.run.log）")
            return 2
        return 0

    if args.cmd == "compare":
        report = compare_runs(Path(args.ref_dir), Path(args.cand_dir), cases)
        code = print_compare_report(report)
        if args.json:
            Path(args.json).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f"[报告已写出] {args.json}")
        print("== 结论: " + ("全部一致/在容差内" if code == 0 else ("存在严格项差异" if code == 1 else "光栅化差异超容差")))
        return code

    if args.cmd == "check":
        out = DEFAULT_OUT_ROOT / ("check-" + time.strftime("%Y%m%d-%H%M%S"))
        runs = run_all(Path(args.exe), out, cases)
        failed = [c for c, r in runs.items() if not r["pass"]]
        if failed:
            print(f"[FAIL] 用例未通过: {', '.join(failed)}（看 *.status.txt / *.run.log）")
            return 2
        code = golden_check(out, cases)
        print("== 结论: " + ("全部一致/在容差内" if code == 0 else ("存在差异" if code == 1 else "存在差异需确认")))
        return code

    if args.cmd == "accept":
        out = DEFAULT_OUT_ROOT / ("accept-" + time.strftime("%Y%m%d-%H%M%S"))
        runs = run_all(Path(args.exe), out, cases)
        failed = [c for c, r in runs.items() if not r["pass"]]
        if failed:
            print(f"[FAIL] 用例未通过，拒绝采参考: {', '.join(failed)}")
            return 2
        golden_write(runs, out, cases)
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
