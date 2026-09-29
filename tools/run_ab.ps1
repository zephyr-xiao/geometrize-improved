# A/B 对拍矩阵驱动:同一组用例跑 geobench-base 与 geobench-fast,比对双口径哈希 + 入库 golden 值
# 用法: powershell -File tools/run_ab.ps1 [-ExeDir <目录>] [-Case <用例名过滤>] [-AcceptGolden]
# 输出: benchmarks/runs/report_<时间戳>.csv,全 PASS 时 exit 0,否则 exit 1
#
# 三层判据(缺一不可):
#   1) 结构断言:bit-exact 用例两侧 sha/fp 必须相同;EXPECTED_DIFF 用例必须不同(分叉消失即红)
#   2) 自洽断言:base 连跑两次 sha 必须一致(确定性健全性)
#   3) golden 值:每条用例的 base/fast 双口径哈希与 tools/goldens.csv 逐值比对
#      —— 只断言"与 base 不同"抓不到"分叉的值变了"(把区域倍率 10 改成 11 照样 PASS),
#         入库 golden 才是值级锁定;-AcceptGolden 显式重采并打印旧→新差异
#
# 可审计性:报告含两个 exe 的 SHA-256、mtime、重试次数、实际哈希值;跑前拒绝陈旧 exe 与漂移的基线快照
param(
    [string]$ExeDir = "$PSScriptRoot\..\src\geobench\build\Release",
    [string]$ImageDir = "$PSScriptRoot\..\src\testdata\images",
    [string]$Case = "",
    [string]$OutCsv = "",
    # 指定后基线侧用该 exe(任意外部二进制),不再依赖 geobench-base.exe 命名约定
    [string]$BaselineExe = "",
    [string]$GoldenFile = "$PSScriptRoot\goldens.csv",
    # 显式重采 golden(会打印每条旧→新差异);不传时 golden 漂移即 FAIL
    [switch]$AcceptGolden,
    # 跳过"基线快照 == 上游快照"预检(仅在明确知道为何漂移时使用)
    [switch]$SkipBaselineCheck,
    # 允许被测 exe 比源码旧(默认拒绝:漏编译会让门禁验的是旧二进制)
    [switch]$AllowStaleExe,
    [int]$TimeoutSec = 900
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

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
    @{ name="segment_color_lines_bypass"; img="hatch_512.png";       steps=40;  threads=8; types="line";         alpha=128; bounds=""; fastExtra="--segment-colors"; expectDiff=$true; note="EXPECTED_DIFF:line-only 时分段取色不参与(单测锁定)但开关仍路由增强轨道(A2.3 档位穷举恒开),与 base 分叉属增强轨道既有语义" },
    @{ name="fix_bounds_fork";          img="tiny_64.png";           steps=40;  threads=4; types="ellipse,rect,line"; alpha=128; bounds=""; fastExtra="--fix-shape-bounds"; expectDiff=$true; note="EXPECTED_DIFF:C.1.4 形状边界 off-by-one 修复分叉锁定(整图排他上界,最右列/最下行可落画),修复失效(输出变同)即红" },
    @{ name="fix_bounds_explicit_fork"; img="tiny_64.png";           steps=30;  threads=4; types="rect";        alpha=128; bounds="0,0,100,100"; fastExtra="--fix-shape-bounds"; expectDiff=$true; note="EXPECTED_DIFF:C.1.4 显式 bounds 路径(百分比按整幅像素跨度换算)分叉锁定" }
)

if($Case -ne "") {
    # 过滤结果必须重新包成数组:单条命中时 Where-Object 会返回标量 Hashtable,.Count 变成键数
    $cases = @($cases | Where-Object { $_.name -like "*$Case*" })
}
# 空矩阵禁入:过滤无匹配时以"零用例全 PASS"放行等于门禁失效
if(@($cases).Count -eq 0) {
    Write-Host "== 门禁矩阵为空:过滤 '$Case' 无匹配用例,拒绝放行 ==" -ForegroundColor Red
    exit 1
}

if($OutCsv -eq "") {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutCsv = Join-Path $repoRoot "benchmarks\runs\report_$timestamp.csv"
}
# 裸文件名时 Split-Path -Parent 返回空串,留痕路径会退化成相对路径并抛错(恰好只在 FAIL 时触发)
if(-not [System.IO.Path]::IsPathRooted($OutCsv)) {
    $OutCsv = Join-Path $repoRoot $OutCsv
}
$OutCsv = [System.IO.Path]::GetFullPath($OutCsv)

$results = @()
$allPass = $true
$totalRetries = 0

# 基线侧可执行:默认走同目录 geobench-base.exe,-BaselineExe 指定时用外部 exe
$baselineExePath = if($BaselineExe -ne "") { $BaselineExe } else { Join-Path $ExeDir "geobench-base.exe" }
$fastExePath = Join-Path $ExeDir "geobench-fast.exe"

# ---- 预检 1:基线信任锚 ----
# 全部判据都是 base↔fast 相对比较,而 base 由可写的 src/baseline-lib 现场编译。
# 若该快照被改动(误同步/误回退),两侧同时改变 → 矩阵照样全绿,"与上游 bit-exact"的声明静默失效。
if(-not $SkipBaselineCheck) {
    $baselineSnap = Join-Path $repoRoot "src\baseline-lib\geometrize"
    $upstreamSnap = Join-Path $repoRoot "upstream\geometrize-lib\geometrize"
    if(-not (Test-Path $baselineSnap) -or -not (Test-Path $upstreamSnap)) {
        Write-Host "== 预检失败:基线快照或上游快照缺失($baselineSnap / $upstreamSnap) ==" -ForegroundColor Red
        exit 1
    }
    $drift = @(Compare-Object (Get-ChildItem -Recurse -File $baselineSnap | ForEach-Object { $_.FullName.Substring($baselineSnap.Length) + ":" + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) `
                              (Get-ChildItem -Recurse -File $upstreamSnap | ForEach-Object { $_.FullName.Substring($upstreamSnap.Length) + ":" + (Get-FileHash $_.FullName -Algorithm SHA256).Hash }))
    if($drift.Count -gt 0) {
        Write-Host "== 预检失败:src\baseline-lib 与 upstream\geometrize-lib 快照漂移($($drift.Count) 项)==" -ForegroundColor Red
        $drift | Select-Object -First 5 | ForEach-Object { Write-Host "   $($_.SideIndicator) $($_.InputObject)" }
        Write-Host "   基线不可信时 base↔fast 相对比较无意义;确认无误后可用 -SkipBaselineCheck 跳过" -ForegroundColor Yellow
        exit 1
    }
}

# ---- 预检 2:被测 exe 必须比源码新 ----
# 漏掉一次 cmake --build 就会让门禁验的是旧二进制,报告全绿却什么也没证明
if(-not $AllowStaleExe) {
    if(-not (Test-Path $fastExePath)) {
        Write-Host "== 预检失败:找不到 $fastExePath(先构建 geobench-fast)==" -ForegroundColor Red
        exit 1
    }
    $exeTime = (Get-Item $fastExePath).LastWriteTime
    $newerSources = @(
        Get-ChildItem -Recurse -File (Join-Path $repoRoot "src\improved-lib") -ErrorAction SilentlyContinue
        Get-ChildItem -Recurse -File (Join-Path $repoRoot "src\baseline-lib") -ErrorAction SilentlyContinue
        Get-Item (Join-Path $repoRoot "src\geobench\main.cpp")
    ) | Where-Object { $_.LastWriteTime -gt $exeTime }
    if(@($newerSources).Count -gt 0) {
        Write-Host "== 预检失败:被测 exe 比源码旧(疑似漏编译),共 $($newerSources.Count) 个源文件更新 ==" -ForegroundColor Red
        $newerSources | Select-Object -First 5 | ForEach-Object { Write-Host "   $($_.FullName)" }
        Write-Host "   重新构建后再跑,或确认无误后用 -AllowStaleExe 跳过" -ForegroundColor Yellow
        exit 1
    }
}

# ---- golden 值加载 ----
$goldens = @{}
if(Test-Path $GoldenFile) {
    foreach($row in (Import-Csv -Path $GoldenFile)) {
        $goldens[$row.case] = $row
    }
} elseif(-not $AcceptGolden) {
    Write-Host "== 预检失败:golden 文件不存在($GoldenFile)==" -ForegroundColor Red
    Write-Host "   值级锁定依赖入库 golden;首次建档请用 -AcceptGolden 显式采集" -ForegroundColor Yellow
    exit 1
}
$goldenNew = @{}

function Quote-BenchArg([string]$a) {
    if($a -match '[\s"]') { return '"' + ($a -replace '"', '\"') + '"' }
    return $a
}

# 单次运行:返回 out(输出行数组)/ exit(退出码)/ timedOut。
# 关键:必须区分"干净失败"与"被杀/超时",且不能因被测 exe 挂死而让门禁无限期挂起
function Invoke-BenchRun([string]$exePath, [string[]]$benchArgs, [int]$timeoutSec) {
    $stdout = [System.IO.Path]::GetTempFileName()
    $stderr = [System.IO.Path]::GetTempFileName()
    try {
        $argString = (($benchArgs | ForEach-Object { Quote-BenchArg $_ }) -join ' ')
        $proc = Start-Process -FilePath $exePath -ArgumentList $argString -NoNewWindow -PassThru `
                              -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if(-not $proc.WaitForExit($timeoutSec * 1000)) {
            try { $proc.Kill() } catch { }
            return [pscustomobject]@{ out = @(); exit = -1; timedOut = $true }
        }
        $lines = @(Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue)
        return [pscustomobject]@{ out = $lines; exit = $proc.ExitCode; timedOut = $false }
    } finally {
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue
    }
}

# 单次运行的三个指标(sha/指纹/耗时);任一缺失返回 $null。
# 门禁脚本自身不能因被测 exe 的异常而中断——缺行必须折叠成 FAIL 而不是 InvokeMethodOnNull
function Get-RunMetrics([object[]]$runOut) {
    if($null -eq $runOut -or @($runOut).Count -eq 0) { return $null }
    $sha = @($runOut | Select-String "^FINAL_SHA256")
    $fp  = @($runOut | Select-String "^STEP_FINGERPRINT")
    $tm  = @($runOut | Select-String "^TIME_MS")
    $sh  = @($runOut | Select-String "^SHAPES_TOTAL")
    if($sha.Count -eq 0 -or $fp.Count -eq 0 -or $tm.Count -eq 0) { return $null }
    return @{ sha = $sha[0].Line.Split(" ")[1]; fp = $fp[0].Line.Split(" ")[1]; ms = [long]($tm[0].Line.Split(" ")[1]); shapes = $(if($sh.Count -gt 0) { [long]($sh[0].Line.Split(" ")[1]) } else { -1 }) }
}

# 带重试的运行:返回 metrics + 重试次数。
# 重试是"偶发故障"的直接证据(ROADMAP 陷阱 #11 的堆损坏就是这么被洗掉的),因此重试次数要
# 进报告并且非零即 FAIL——否则这条逻辑会系统性地掩盖它本来要防的那类 bug。
function Invoke-WithRetry([string]$exePath, [string[]]$benchArgs, [string]$label, [ref]$retries) {
    $run = Invoke-BenchRun $exePath $benchArgs $TimeoutSec
    if($run.timedOut) {
        Write-Host "  [超时] $label 侧超过 ${TimeoutSec}s 被终止" -ForegroundColor Yellow
        return @{ metrics = $null; timedOut = $true }
    }
    $metrics = Get-RunMetrics $run.out
    if($null -ne $metrics) { return @{ metrics = $metrics; timedOut = $false } }

    Write-Host "  [重试] $label 侧无输出(退出码 $($run.exit),疑似瞬态崩溃),重跑一次" -ForegroundColor Yellow
    $retries.Value++
    $run = Invoke-BenchRun $exePath $benchArgs $TimeoutSec
    if($run.timedOut) { return @{ metrics = $null; timedOut = $true } }
    return @{ metrics = (Get-RunMetrics $run.out); timedOut = $false }
}

# 报告可审计:两个 exe 的身份(路径 + SHA-256 + mtime)与重试次数都进 CSV,
# 否则事后无法回答"这一轮到底验的是哪个二进制"
$baseExeHash = (Get-FileHash $baselineExePath -Algorithm SHA256).Hash
$fastExeHash = (Get-FileHash $fastExePath -Algorithm SHA256).Hash
$fastExeTime = (Get-Item $fastExePath).LastWriteTime.ToString("s")

foreach($c in $cases) {
    $imgPath = Join-Path $ImageDir $c.img
    if(-not (Test-Path $imgPath)) {
        # 缺图按 FAIL 计:测试资产缺失不允许静默跳过后以"全 PASS"放行
        Write-Host "[FAIL] $($c.name): 缺图 $imgPath(资产缺失按失败计)" -ForegroundColor Red
        $allPass = $false
        $results += [pscustomobject]@{
            case=$c.name; img=$c.img; verdict="FAIL"; expect_diff=$c.expectDiff
            sha_match=$false; fingerprint_match=$false; self_consistent=$false; golden="N/A"
            base_ms=0; fast_ms=0; speedup=0; base_retries=0; fast_retries=0; self_retries=0
            base_sha=""; fast_sha=""; base_fp=""; fast_fp=""
            note="缺图:$($c.img)"
        }
        continue
    }

    $baseArgs = @("--input", $imgPath, "--steps", $c.steps, "--threads", $c.threads,
                  "--types", $c.types, "--alpha", $c.alpha)
    if($c.bounds -ne "") {
        $baseArgs += @("--shape-bounds", $c.bounds)
    }
    # fast 侧附加参数(算法增强开关);base 侧永远不带。
    # fastExtra 须按空格拆成数组元素再拼接:PowerShell 数组 splatting 不做单词拆分,
    # 含空格的单字符串会被当作一个整体 argv(踩过:组合开关静默失效,输出与 base 相同)
    $fastArgs = $baseArgs
    if($c.fastExtra -ne "") {
        $fastArgs += ($c.fastExtra -split '\s+' | Where-Object { $_ -ne "" })
    }

    $baseRetries = 0
    $fastRetries = 0
    $selfRetries = 0
    $baseRun = Invoke-WithRetry $baselineExePath $baseArgs "base" ([ref]$baseRetries)
    $fastRun = Invoke-WithRetry $fastExePath $fastArgs "fast" ([ref]$fastRetries)
    # 双跑基线取自洽性(确定性健全性检查)
    $selfRun = Invoke-WithRetry $baselineExePath $baseArgs "base(自洽)" ([ref]$selfRetries)
    $retries = $baseRetries + $fastRetries + $selfRetries
    $totalRetries += $retries

    $baseMetrics = $baseRun.metrics
    $fastMetrics = $fastRun.metrics
    $baseMetrics2 = $selfRun.metrics

    if($null -eq $baseMetrics -or $null -eq $fastMetrics -or $null -eq $baseMetrics2) {
        $why = if($baseRun.timedOut -or $fastRun.timedOut -or $selfRun.timedOut) { "超时被终止" } else { "exe 无输出(含一次重试)" }
        Write-Host "[FAIL] $($c.name): $why,记 FAIL 继续" -ForegroundColor Red
        $allPass = $false
        $results += [pscustomobject]@{
            case=$c.name; img=$c.img; verdict="FAIL"; expect_diff=$c.expectDiff
            sha_match=$false; fingerprint_match=$false; self_consistent=$false; golden="N/A"
            base_ms=0; fast_ms=0; speedup=0; base_retries=$baseRetries; fast_retries=$fastRetries; self_retries=$selfRetries
            base_sha=""; fast_sha=""; base_fp=""; fast_fp=""
            note="$($c.note); $why"
        }
        $results | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8
        continue
    }

    $baseSha  = $baseMetrics.sha
    $fastSha  = $fastMetrics.sha
    $baseFp   = $baseMetrics.fp
    $fastFp   = $fastMetrics.fp
    $baseSha2 = $baseMetrics2.sha

    $baseTimeMs  = $baseMetrics.ms
    $fastTimeMs  = $fastMetrics.ms

    $selfConsistent = ($baseSha -eq $baseSha2)
    $shaMatch       = ($baseSha -eq $fastSha)
    $fpMatch        = ($baseFp -eq $fastFp)

    # 结构断言:expectDiff 用例断言反转,输出必须不同才说明"已知分叉"仍如预期存在
    if($c.expectDiff) {
        $structural = $selfConsistent -and (-not $shaMatch) -and (-not $fpMatch)
    } else {
        $structural = $selfConsistent -and $shaMatch -and $fpMatch
    }

    # golden 值断言:双端双口径哈希逐值比对(这才是"分叉的值没变"的判据)
    $goldenStatus = "MISSING"
    $g = $goldens[$c.name]
    if($null -ne $g) {
        if($g.base_sha -eq $baseSha -and $g.base_fp -eq $baseFp -and $g.fast_sha -eq $fastSha -and $g.fast_fp -eq $fastFp) {
            $goldenStatus = "OK"
        } else {
            $goldenStatus = "DRIFT"
            if($AcceptGolden) {
                Write-Host "  [golden] $($c.name) 值变更(已重采):" -ForegroundColor Yellow
                if($g.base_sha -ne $baseSha) { Write-Host "      base_sha $($g.base_sha.Substring(0,16))… -> $($baseSha.Substring(0,16))…" }
                if($g.base_fp  -ne $baseFp)  { Write-Host "      base_fp  $($g.base_fp) -> $($baseFp)" }
                if($g.fast_sha -ne $fastSha) { Write-Host "      fast_sha $($g.fast_sha.Substring(0,16))… -> $($fastSha.Substring(0,16))…" }
                if($g.fast_fp  -ne $fastFp)  { Write-Host "      fast_fp  $($g.fast_fp) -> $($fastFp)" }
            }
        }
    } elseif($AcceptGolden) {
        $goldenStatus = "NEW"
    }

    $goldenOk = ($goldenStatus -eq "OK") -or $AcceptGolden
    # 重试非零 = 本可复现的偶发故障:不静默放过
    $retryOk = ($retries -eq 0)
    $pass = $structural -and $goldenOk -and $retryOk
    if($pass) { $verdict = "PASS" } else { $verdict = "FAIL" ; $allPass = $false }

    if(-not $pass) {
        if(-not $structural) { Write-Host "  [诊断] 结构断言不满足(sha_match=$shaMatch fp_match=$fpMatch self=$selfConsistent expectDiff=$($c.expectDiff))" -ForegroundColor Red }
        if(-not $goldenOk)   { Write-Host "  [诊断] golden $goldenStatus:双端哈希与入库值不符(分叉的值变了或基线漂移)" -ForegroundColor Red }
        if(-not $retryOk)    { Write-Host "  [诊断] 发生 $retries 次重试:存在偶发故障,不按 PASS 放行" -ForegroundColor Red }

        # 失败留痕:双方最终位图以裸 RGBA 落盘,hexdiff 定位首个发散像素
        $runsDir = Split-Path $OutCsv -Parent
        $baseDump = Join-Path $runsDir "$($c.name)_base.raw"
        $fastDump = Join-Path $runsDir "$($c.name)_fast.raw"
        $null = Invoke-BenchRun $baselineExePath ($baseArgs + @("--dump-final", $baseDump)) $TimeoutSec
        $null = Invoke-BenchRun $fastExePath ($fastArgs + @("--dump-final", $fastDump)) $TimeoutSec
        Write-Host "  [留痕] $baseDump / $fastDump"
    }

    $goldenNew[$c.name] = [pscustomobject]@{ case=$c.name; base_sha=$baseSha; base_fp=$baseFp; fast_sha=$fastSha; fast_fp=$fastFp }

    $speedup = if($fastTimeMs -gt 0) { [math]::Round($baseTimeMs / $fastTimeMs, 3) } else { 0 }
    $shaTag = if($c.expectDiff) { $(if(-not $shaMatch){"DIFF-OK"}else{"UNEXPECTED-MATCH"}) } else { $(if($shaMatch){"OK"}else{"MISMATCH"}) }
    $fpTag = if($c.expectDiff) { $(if(-not $fpMatch){"DIFF-OK"}else{"UNEXPECTED-MATCH"}) } else { $(if($fpMatch){"OK"}else{"MISMATCH"}) }
    $retryTag = if($retries -gt 0) { "retries=$retries" } else { "-" }
    Write-Host ("[{0}] {1,-26} base={2}ms fast={3}ms speedup={4}x sha={5} fp={6} self={7} golden={8} {9}" -f `
        $verdict, $c.name, $baseTimeMs, $fastTimeMs, $speedup, $shaTag, $fpTag, $(if($selfConsistent){"OK"}else{"BAD"}), $goldenStatus, $retryTag)

    $results += [pscustomobject]@{
        case=$c.name; img=$c.img; verdict=$verdict; expect_diff=$c.expectDiff
        sha_match=$shaMatch; fingerprint_match=$fpMatch; self_consistent=$selfConsistent; golden=$goldenStatus
        base_ms=$baseTimeMs; fast_ms=$fastTimeMs; speedup=$speedup
        shapes_total=$($fastMetrics.shapes)
        base_retries=$baseRetries; fast_retries=$fastRetries; self_retries=$selfRetries
        base_sha=$baseSha; fast_sha=$fastSha; base_fp=$baseFp; fast_fp=$fastFp
        baseline_exe=$baselineExePath; baseline_exe_sha256=$baseExeHash
        fast_exe=$fastExePath; fast_exe_sha256=$fastExeHash; fast_exe_mtime=$fastExeTime
        note=$c.note
    }
    # 增量落盘:中途中断也不丢已完成用例的证据
    $results | Export-Csv -Path $OutCsv -NoTypeInformation -Encoding UTF8
}

if($AcceptGolden) {
    $goldenNew.Values | Export-Csv -Path $GoldenFile -NoTypeInformation -Encoding UTF8
    Write-Host "`ngolden 已重采: $GoldenFile ($($goldenNew.Count) 条)" -ForegroundColor Yellow
}

Write-Host "`n报告已写入 $OutCsv"
if($totalRetries -gt 0) {
    Write-Host "本轮共发生 $totalRetries 次重试(偶发故障信号,已按 FAIL 计)" -ForegroundColor Yellow
}

if($allPass) {
    Write-Host "== 全部 PASS ==" -ForegroundColor Green
    exit 0
} else {
    Write-Host "== 存在 FAIL,禁止合入下一补丁 ==" -ForegroundColor Red
    exit 1
}
