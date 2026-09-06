# A/B 对拍矩阵驱动:同一组用例跑 geobench-base 与 geobench-fast,比对双口径哈希
# 用法: powershell -File tools/run_ab.ps1 [-ExeDir <目录>] [-Case <用例名过滤>]
# 输出: benchmarks/runs/report_<时间戳>.csv,全 PASS 时 exit 0,否则 exit 1
param(
    [string]$ExeDir = "$PSScriptRoot\..\src\geobench\build\Release",
    [string]$ImageDir = "$PSScriptRoot\..\src\testdata\images",
    [string]$Case = "",
    [string]$OutCsv = "",
    # 指定后基线侧用该 exe(任意外部二进制),不再依赖 geobench-base.exe 命名约定
    [string]$BaselineExe = ""
)

$ErrorActionPreference = "Stop"

# 门禁矩阵:名称 | 图片 | steps | threads | types | alpha | bounds | fastExtra | expectDiff | 用途注释
# expectDiff=$true 的用例断言反转:双端输出必须不同(锁定已知变体分叉仍存在,分叉消失即红)
# fastExtra:fast 侧附加参数(算法增强开关,如 --adaptive-step);base 侧永远不带——增强轨道
# 输出与上游不同属预期,故 fastExtra 非空的用例必须配 expectDiff=$true(分叉锁定哨兵)
$cases = @(
    @{ name="e2e_ellipse_seagull";      img="tree_under_clouds.png"; steps=100; threads=8; types="ellipse";   alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="基线全管线" },
    @{ name="circle_rings";             img="rings_512.png";         steps=60;  threads=8; types="circle";     alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="压 Circle 光栅化" },
    @{ name="polygon_mix";              img="triangles_512.png";     steps=80;  threads=8; types="rellipse,tri,rrect"; alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="压 polygon 扁平化" },
    @{ name="line_hatch";               img="hatch_512.png";         steps=60;  threads=8; types="line";       alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="细扫描线 copy/draw" },
    @{ name="threads1_seagull";         img="tree_under_clouds.png"; steps=50;  threads=1; types="ellipse";    alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="单线程对拍" },
    @{ name="threads16_seagull";        img="tree_under_clouds.png"; steps=80;  threads=16; types="ellipse";   alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="过订阅线程对拍" },
    @{ name="tiny64_edge";              img="tiny_64.png";           steps=40;  threads=4; types="ellipse,line"; alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="小图边界退化" },
    @{ name="flat_reject";              img="flat_512.png";          steps=40;  threads=8; types="rect";       alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="高拒绝率回滚路径" },
    @{ name="all_types_gradnoise";      img="gradnoise_512.png";     steps=60;  threads=8; types="all";        alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="九种形状全覆盖" },
    @{ name="bezier_polyline_cover";    img="gradnoise_512.png";     steps=40;  threads=8; types="bezier,polyline"; alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="bezier+polyline 类型补覆盖" },
    @{ name="ellipse_bounds_fork";      img="tiny_64.png";           steps=30;  threads=4; types="ellipse";    alpha=128; bounds="10,0,100,100"; fastExtra=""; expectDiff=$true; note="EXPECTED_DIFF:Ellipse y1>=xMin 笔误修复的分叉锁定,分叉消失即红" },
    @{ name="wide_bandwidth";           img="gradnoise_2048.png";    steps=20;  threads=8; types="ellipse";    alpha=128; bounds=""; fastExtra=""; expectDiff=$false; note="大图带宽(慢)" },
    @{ name="adaptive_step_fork";       img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse,rect"; alpha=128; bounds=""; fastExtra="--adaptive-step"; expectDiff=$true; note="EXPECTED_DIFF:A2.2 自适应步长分叉锁定,增强开关失效(输出变同)即红" },
    @{ name="adaptive_step_threads1";   img="tree_under_clouds.png"; steps=30;  threads=1; types="ellipse";    alpha=128; bounds=""; fastExtra="--adaptive-step"; expectDiff=$true; note="EXPECTED_DIFF:单线程下自适应步长分叉锁定" },
    @{ name="alpha_search_fork";        img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse,tri"; alpha=128; bounds=""; fastExtra="--alpha-search"; expectDiff=$true; note="EXPECTED_DIFF:A2.3 alpha 档位搜索分叉锁定" },
    @{ name="enhanced_combo_fork";      img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse";    alpha=128; bounds=""; fastExtra="--adaptive-step --alpha-search"; expectDiff=$true; note="EXPECTED_DIFF:增强组合分叉锁定" },
    @{ name="error_guide_fork";         img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse,rect"; alpha=128; bounds=""; fastExtra="--error-guide"; expectDiff=$true; note="EXPECTED_DIFF:A2.1 误差图引导分叉锁定,引导失效(输出变同)即红" },
    @{ name="error_guide_threads1";     img="tree_under_clouds.png"; steps=30;  threads=1; types="ellipse";    alpha=128; bounds=""; fastExtra="--error-guide"; expectDiff=$true; note="EXPECTED_DIFF:单线程下误差图引导分叉锁定" },
    @{ name="error_guide_combo";        img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse";    alpha=128; bounds=""; fastExtra="--error-guide --pyramid --adaptive-step --alpha-search"; expectDiff=$true; note="EXPECTED_DIFF:误差图引导与金字塔/增强全组合分叉锁定" },
    @{ name="priority_region_fork";     img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse";    alpha=128; bounds=""; fastExtra="--error-guide --priority-region 25,25,75,75"; expectDiff=$true; note="EXPECTED_DIFF:F3.2 区域优先分叉锁定,区域加权失效(输出变同)即红" },
    @{ name="priority_region_threads1"; img="tree_under_clouds.png"; steps=30;  threads=1; types="ellipse";    alpha=128; bounds=""; fastExtra="--error-guide --priority-region 25,25,75,75"; expectDiff=$true; note="EXPECTED_DIFF:单线程下区域优先分叉锁定" },
    @{ name="segment_color_fork";       img="gradnoise_512.png";     steps=40;  threads=8; types="ellipse,rect"; alpha=128; bounds=""; fastExtra="--segment-colors"; expectDiff=$true; note="EXPECTED_DIFF:A2.4 分段颜色分叉锁定,开关失效(输出变同)即红" },
    @{ name="segment_color_threads1";   img="tree_under_clouds.png"; steps=30;  threads=1; types="ellipse";      alpha=128; bounds=""; fastExtra="--segment-colors"; expectDiff=$true; note="EXPECTED_DIFF:单线程下分段颜色分叉锁定" },
    @{ name="segment_color_lines_bypass"; img="hatch_512.png";       steps=40;  threads=8; types="line";         alpha=128; bounds=""; fastExtra="--segment-colors"; expectDiff=$true; note="EXPECTED_DIFF:line-only 时分段取色不参与(单测锁定)但开关仍路由增强轨道(A2.3 档位穷举恒开),与 base 分叉属增强轨道既有语义" }
)

if($Case -ne "") {
    $cases = $cases | Where-Object { $_.name -like "*$Case*" }
}

if($OutCsv -eq "") {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutCsv = "$PSScriptRoot\..\benchmarks\runs\report_$timestamp.csv"
}

$results = @()
$allPass = $true

# 基线侧可执行:默认走同目录 geobench-base.exe,-BaselineExe 指定时用外部 exe
$baselineExePath = if($BaselineExe -ne "") { $BaselineExe } else { Join-Path $ExeDir "geobench-base.exe" }
$fastExePath = Join-Path $ExeDir "geobench-fast.exe"

foreach($c in $cases) {
    $imgPath = Join-Path $ImageDir $c.img
    if(-not (Test-Path $imgPath)) {
        Write-Host "[SKIP] $($c.name): 缺图 $imgPath"
        continue
    }

    $args = @("--input", $imgPath, "--steps", $c.steps, "--threads", $c.threads,
             "--types", $c.types, "--alpha", $c.alpha)
    if($c.bounds -ne "") {
        $args += @("--shape-bounds", $c.bounds)
    }
    # fast 侧附加参数(算法增强开关);base 侧永远不带。
    # fastExtra 须按空格拆成数组元素再拼接:PowerShell 数组 splatting 不做单词拆分,
    # 含空格的单字符串会被当作一个整体 argv(踩过:组合开关静默失效,输出与 base 相同)
    $fastArgs = $args
    if($c.fastExtra -ne "") {
        $fastArgs += ($c.fastExtra -split '\s+' | Where-Object { $_ -ne "" })
    }

    $baseOut = & $baselineExePath @args
    $fastOut = & $fastExePath @fastArgs

    $baseSha  = ($baseOut | Select-String "^FINAL_SHA256").Line.Split(" ")[1]
    $fastSha  = ($fastOut | Select-String "^FINAL_SHA256").Line.Split(" ")[1]
    $baseFp   = ($baseOut | Select-String "^STEP_FINGERPRINT").Line.Split(" ")[1]
    $fastFp   = ($fastOut | Select-String "^STEP_FINGERPRINT").Line.Split(" ")[1]
    # 双跑基线取自洽性(确定性健全性检查)
    $baseOut2 = & $baselineExePath @args
    $baseSha2 = ($baseOut2 | Select-String "^FINAL_SHA256").Line.Split(" ")[1]

    $baseTimeMs  = [long](($baseOut | Select-String "^TIME_MS").Line.Split(" ")[1])
    $fastTimeMs  = [long](($fastOut | Select-String "^TIME_MS").Line.Split(" ")[1])

    $selfConsistent = ($baseSha -eq $baseSha2)
    $shaMatch       = ($baseSha -eq $fastSha)
    $fpMatch        = ($baseFp -eq $fastFp)

    # expectDiff 用例断言反转:输出必须不同才说明"已知分叉"仍如预期存在
    if($c.expectDiff) {
        $pass = $selfConsistent -and (-not $shaMatch) -and (-not $fpMatch)
    } else {
        $pass = $selfConsistent -and $shaMatch -and $fpMatch
    }

    if($pass) { $verdict = "PASS" } else { $verdict = "FAIL" ; $allPass = $false }

    # 失败留痕:双方最终位图以裸 RGBA 落盘,hexdiff 定位首个发散像素
    if(-not $pass) {
        $runsDir = Split-Path $OutCsv -Parent
        $baseDump = Join-Path $runsDir "$($c.name)_base.raw"
        $fastDump = Join-Path $runsDir "$($c.name)_fast.raw"
        & $baselineExePath @args --dump-final $baseDump | Out-Null
        & $fastExePath @fastArgs --dump-final $fastDump | Out-Null
        Write-Host "  [留痕] $baseDump / $fastDump"
    }

    $speedup = if($fastTimeMs -gt 0) { [math]::Round($baseTimeMs / $fastTimeMs, 3) } else { 0 }
    $shaTag = if($c.expectDiff) { $(if(-not $shaMatch){"DIFF-OK"}else{"UNEXPECTED-MATCH"}) } else { $(if($shaMatch){"OK"}else{"MISMATCH"}) }
    $fpTag = if($c.expectDiff) { $(if(-not $fpMatch){"DIFF-OK"}else{"UNEXPECTED-MATCH"}) } else { $(if($fpMatch){"OK"}else{"MISMATCH"}) }
    Write-Host ("[{0}] {1,-24} base={2}ms fast={3}ms speedup={4}x sha={5} fp={6} self={7}" -f `
        $verdict, $c.name, $baseTimeMs, $fastTimeMs, $speedup, $shaTag, $fpTag, $(if($selfConsistent){"OK"}else{"BAD"}))

    $results += [pscustomobject]@{
        case=$c.name; img=$c.img; steps=$c.steps; threads=$c.threads; types=$c.types; fastExtra=$c.fastExtra
        verdict=$verdict; sha_match=$shaMatch; fingerprint_match=$fpMatch; self_consistent=$selfConsistent
        expect_diff=$c.expectDiff
        base_ms=$baseTimeMs; fast_ms=$fastTimeMs; speedup=$speedup; note=$c.note
    }
}

$results | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8
Write-Host "`n报告已写入 $OutCsv"

if($allPass) {
    Write-Host "== 全部 PASS ==" -ForegroundColor Green
    exit 0
} else {
    Write-Host "== 存在 FAIL,禁止合入下一补丁 ==" -ForegroundColor Red
    exit 1
}
