// geobench: Geometrize A/B 对拍与基准 CLI
// 用同一份源码分别链接 baseline-lib 与 improved-lib,产出 geobench-base.exe / geobench-fast.exe
// 对拍机制:固定参数跑 N 步,输出最终位图 SHA-256(一级口径)+ 逐步 FNV-1a 滚动指纹(二级口径)
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "sha256.h"
#include "geometrize/bitmap/bitmap.h"
#include "geometrize/core.h"
#include "geometrize/runner/imagerunner.h"
#include "geometrize/runner/imagerunneroptions.h"
#include "geometrize/exporter/shapejsonexporter.h"
#include "geometrize/rasterizer/scanline.h"
#include "geometrize/shape/shape.h"
#include "geometrize/shaperesult.h"

namespace {

struct Options {
    std::string inputPath;
    std::uint32_t steps = 300;
    std::uint32_t alpha = 128;
    std::uint32_t shapeCount = 50;
    std::uint32_t maxMutations = 100;
    std::uint32_t seed = 9001;
    std::uint32_t threads = 8;
    std::uint32_t shapeTypes = 8; // ELLIPSE 默认
    bool verbose = false;
    // shapeBounds(百分比,启用后形状绘制限制在该矩形内):非对称视口可放大
    // 上游 Ellipse 光栅化的 y1>=xMin 笔误对输出的影响,是分叉用例的参数来源
    bool hasBounds = false;
    double boundsXMinPercent = 0.0;
    double boundsYMinPercent = 0.0;
    double boundsXMaxPercent = 100.0;
    double boundsYMaxPercent = 100.0;
    // 最终位图落盘路径(裸 RGBA 字节):对拍失败时人工 diff 用
    std::string dumpFinalPath;
    // 金字塔搜索开关(算法增强轨道,fast 侧专属能力):开启后输出与关闭时不同属预期
    bool pyramid = false;
    // 增强爬山开关(fast 侧专属):--adaptive-step 自适应变异步长;--alpha-search 逐形状 alpha 档位搜索;
    // --error-guide 误差图引导形状放置
    bool adaptiveStep = false;
    bool alphaSearch = false;
    std::vector<std::uint32_t> alphaTiers; // 空则用默认 {64,128,192}
    bool errorGuide = false;
    std::uint32_t guideEpsilonPermill = 100; // 引导轨道 ε-greedy 千分比(0-1000)
    // --segment-colors:分段颜色(A2.4,按扫描线行级取色;fast 侧专属)
    bool segmentColors = false;
    // --priority-region x1,y1,x2,y2(百分比,可重复出现累积):区域优先绘制(F3.2,需配 --error-guide)
    std::vector<std::array<double, 4>> priorityRegions;
    // 每 N 步打印一次全分辨率 differenceFull 相似度分数(质量曲线验收用;0=关)
    std::uint32_t qualityReportInterval = 0;
    // --fix-shape-bounds:形状边界 off-by-one 修复(C.1.4,fast 侧专属)。
    // 库侧默认保持上游语义(闭区间上界)以维持 bit-exact 门禁,本开关是分叉锁定哨兵。
    bool fixShapeBounds = false;
};

void printUsage()
{
    std::printf(
        "geobench --input <image.png> [options]\n"
        "  --steps N          模型步数 (默认 300)\n"
        "  --alpha N          形状透明度 0-255 (默认 128)\n"
        "  --shape-count N    每步候选形状数 (默认 50)\n"
        "  --max-mutations N  每候选最大变异次数 (默认 100)\n"
        "  --seed N           RNG 种子 (默认 9001)\n"
        "  --threads N        并行线程数,参与结果确定性 (默认 8)\n"
        "  --types SPEC       逗号分隔: rect,rrect,tri,ellipse,rellipse,circle,line,bezier,polyline 或 all\n"
        "  --shape-bounds SPEC  x1,y1,x2,y2 百分比,限制形状绘制区域(影响输出,参与确定性)\n"
        "  --dump-final PATH  最终位图以裸 RGBA 写入 PATH(对拍失败留痕用)\n"
        "  --pyramid          金字塔搜索(hill-climb 半分辨率评估;算法增强,输出与关闭时不同属预期)\n"
        "  --adaptive-step    自适应变异步长(1/5 成功法则,算法增强)\n"
        "  --alpha-search     逐形状 alpha 档位搜索(算法增强;档位默认 64,128,192)\n"
        "  --alpha-tiers LIST 逗号分隔 alpha 档位(1-255),如 64,128,192(需配 --alpha-search)\n"
        "  --error-guide      误差图引导形状放置(高误差区优先,算法增强)\n"
        "  --guide-epsilon N  引导探索率千分比 0-1000(默认 100,即 10%% 候选保持均匀原位)\n"
        "  --priority-region SPEC  优先区域 x1,y1,x2,y2(百分比 0-100,可重复累积;需配 --error-guide)\n"
        "  --segment-colors  按扫描线行级取色(形状跨明暗边界不发灰,算法增强)\n"
        "  --fix-shape-bounds 形状边界 off-by-one 修复(C.1.4:画布最右列/最下行可落画;\n"
        "                     默认关=上游闭区间语义,开启后输出与上游不同属预期)\n"
        "  --quality-report N 每 N 步打印一次全分辨率相似度分数,质量曲线验收用(0=关,默认)\n"
        "  -v                 逐步打印分数\n");
}

// 数值参数统一校验:空值/非数字/负号/超范围一律 exit 1。
// 旧实现直接用 std::stoul:拼错的值会被静默接受("-3" → 4294967293 步)或直接 abort(无错误信息),
// 前者让用例实际空转却仍以"哈希相同"判 PASS
std::uint32_t parseU32Arg(const char* name, const std::string& token, std::uint32_t maxValue)
{
    if(token.empty()) {
        std::fprintf(stderr, "%s 缺少取值\n", name);
        std::exit(1);
    }
    if(token.find_first_not_of("0123456789") != std::string::npos) {
        std::fprintf(stderr, "%s 应为非负整数: %s\n", name, token.c_str());
        std::exit(1);
    }
    try {
        const unsigned long value = std::stoul(token);
        if(value > static_cast<unsigned long>(maxValue)) {
            std::fprintf(stderr, "%s 超出上限 %u: %s\n", name, maxValue, token.c_str());
            std::exit(1);
        }
        return static_cast<std::uint32_t>(value);
    } catch(const std::exception&) {
        std::fprintf(stderr, "%s 数值溢出: %s\n", name, token.c_str());
        std::exit(1);
    }
}

std::uint32_t parseShapeTypes(const std::string& spec)
{
    if(spec == "all") {
        return 511U; // 九种形状全部位的掩码 = 2^9 - 1
    }
    static const std::pair<const char*, std::uint32_t> table[] = {
        {"rect", geometrize::ShapeTypes::RECTANGLE},
        {"rrect", geometrize::ShapeTypes::ROTATED_RECTANGLE},
        {"tri", geometrize::ShapeTypes::TRIANGLE},
        {"ellipse", geometrize::ShapeTypes::ELLIPSE},
        {"rellipse", geometrize::ShapeTypes::ROTATED_ELLIPSE},
        {"circle", geometrize::ShapeTypes::CIRCLE},
        {"line", geometrize::ShapeTypes::LINE},
        {"bezier", geometrize::ShapeTypes::QUADRATIC_BEZIER},
        {"polyline", geometrize::ShapeTypes::POLYLINE},
    };
    std::uint32_t types = 0;
    std::size_t pos = 0;
    while(pos < spec.size()) {
        const std::size_t comma = spec.find(',', pos);
        const std::string token = spec.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
        bool matched = false;
        for(const auto& entry : table) {
            if(token == entry.first) {
                types |= entry.second;
                matched = true;
                break;
            }
        }
        // 未知形状名必须报错:静默忽略会让 types=0,一步都不画,两侧哈希相同 → 门禁判 PASS
        if(!matched) {
            std::fprintf(stderr, "未知形状类型: '%s'(可用: all 或 rect,rrect,tri,ellipse,rellipse,circle,line,bezier,polyline)\n", token.c_str());
            std::exit(1);
        }
        if(comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return types;
}

std::vector<std::string> splitCsv(const std::string& spec)
{
    std::vector<std::string> tokens;
    std::size_t pos = 0;
    while(pos <= spec.size()) {
        const std::size_t comma = spec.find(',', pos);
        const std::string token = spec.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
        if(!token.empty()) {
            tokens.push_back(token);
        }
        if(comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return tokens;
}

Options parseArgs(int argc, char** argv)
{
    Options opts;
    for(int i = 1; i < argc; i++) {
        const std::string arg{argv[i]};
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string{argv[++i]} : std::string{}; };
        if(arg == "--input") opts.inputPath = next();
        else if(arg == "--steps") opts.steps = parseU32Arg("--steps", next(), 1000000000U);
        else if(arg == "--alpha") opts.alpha = parseU32Arg("--alpha", next(), 255U);
        else if(arg == "--shape-count") opts.shapeCount = parseU32Arg("--shape-count", next(), 1000000U);
        else if(arg == "--max-mutations") opts.maxMutations = parseU32Arg("--max-mutations", next(), 1000000U);
        else if(arg == "--seed") opts.seed = parseU32Arg("--seed", next(), 0xFFFFFFFFU);
        else if(arg == "--threads") opts.threads = parseU32Arg("--threads", next(), 1024U);
        else if(arg == "--types") opts.shapeTypes = parseShapeTypes(next());
        else if(arg == "--shape-bounds") {
            // 四个逗号分隔百分比,即 ImageRunnerShapeBoundsOptions 的启用形态
            const std::string spec = next();
            double vals[4] = {0.0, 0.0, 0.0, 0.0};
            std::size_t pos = 0;
            for(int k = 0; k < 4; k++) {
                const std::size_t comma = spec.find(',', pos);
                const std::string token = spec.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
                pos = (comma == std::string::npos) ? spec.size() : comma + 1;
                try {
                    std::size_t consumed = 0;
                    vals[k] = std::stod(token, &consumed);
                    if(consumed != token.size() || vals[k] < 0.0 || vals[k] > 100.0) {
                        throw std::invalid_argument("range");
                    }
                } catch(const std::exception&) {
                    std::fprintf(stderr, "--shape-bounds 应为四个 0-100 的百分比 x1,y1,x2,y2: '%s'\n", spec.c_str());
                    std::exit(1);
                }
            }
            opts.boundsXMinPercent = vals[0];
            opts.boundsYMinPercent = vals[1];
            opts.boundsXMaxPercent = vals[2];
            opts.boundsYMaxPercent = vals[3];
            opts.hasBounds = true;
        }
        else if(arg == "--dump-final") opts.dumpFinalPath = next();
        else if(arg == "--pyramid") opts.pyramid = true;
        else if(arg == "--adaptive-step") opts.adaptiveStep = true;
        else if(arg == "--alpha-search") opts.alphaSearch = true;
        else if(arg == "--alpha-tiers") {
            for(const std::string& token : splitCsv(next())) {
                const std::uint32_t tier = parseU32Arg("--alpha-tiers", token, 255U);
                if(tier < 1U) {
                    std::fprintf(stderr, "alpha 档位超出 1-255: %s\n", token.c_str());
                    std::exit(1);
                }
                opts.alphaTiers.push_back(tier);
            }
        }
        else if(arg == "--quality-report") opts.qualityReportInterval = parseU32Arg("--quality-report", next(), 100000U);
        else if(arg == "--error-guide") opts.errorGuide = true;
        else if(arg == "--segment-colors") opts.segmentColors = true;
        else if(arg == "--fix-shape-bounds") opts.fixShapeBounds = true;
        else if(arg == "--guide-epsilon") opts.guideEpsilonPermill = parseU32Arg("--guide-epsilon", next(), 1000U);
        else if(arg == "--priority-region") {
            // x1,y1,x2,y2 百分比(0-100);格式非法/越界 exit 1
            const std::string spec = next();
            double vals[4] = {0.0, 0.0, 0.0, 0.0};
            if(std::sscanf(spec.c_str(), "%lf,%lf,%lf,%lf", &vals[0], &vals[1], &vals[2], &vals[3]) != 4) {
                std::fprintf(stderr, "priority-region 格式应为 x1,y1,x2,y2(百分比): %s' + bs_n + '", spec.c_str());
                std::exit(1);
            }
            for(const double v : vals) {
                if(v < 0.0 || v > 100.0) {
                    std::fprintf(stderr, "priority-region 百分比越界 0-100: %s' + bs_n + '", spec.c_str());
                    std::exit(1);
                }
            }
            opts.priorityRegions.push_back({vals[0], vals[1], vals[2], vals[3]});
        }
        else if(arg == "-v") opts.verbose = true;
        else if(arg == "--help" || arg == "-h") { printUsage(); std::exit(0); }
        else {
            // 未知参数一律报错:拼错的 flag 被静默忽略会让用例退化成默认配置,而门禁只比对哈希,
            // 结果"验的是另一组参数"却照样 PASS
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            printUsage();
            std::exit(1);
        }
    }
    if(opts.inputPath.empty()) {
        printUsage();
        std::exit(1);
    }
    return opts;
}

geometrize::Bitmap loadImageAsBitmap(const std::string& path)
{
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if(pixels == nullptr) {
        std::fprintf(stderr, "无法加载图片: %s (%s)\n", path.c_str(), stbi_failure_reason());
        std::exit(1);
    }

    // stb_image 强制输出的 RGBA8888 字节序与库的 rgba 布局一致(R,G,B,A 各一字节),直接搬
    std::vector<std::uint8_t> data(static_cast<std::size_t>(width) * height * 4U);
    std::memcpy(data.data(), pixels, data.size());
    stbi_image_free(pixels);

    return geometrize::Bitmap(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), data);
}

// FNV-1a 单字折叠 —— 二级对拍滚动指纹用,FNV 足够定位发散步号且可流式累计
void mixBytes(std::uint64_t& fingerprint, const void* data, std::size_t len)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for(std::size_t i = 0; i < len; i++) {
        fingerprint ^= bytes[i];
        fingerprint *= 1099511628211ULL;
    }
}

void mixScore(std::uint64_t& fingerprint, double score)
{
    // 分数按 double 位模式折叠:最细微的浮点漂移也会被曝光
    std::uint64_t bits = 0;
    std::memcpy(&bits, &score, sizeof(bits));
    mixBytes(fingerprint, &bits, sizeof(bits));
}

} // namespace

int main(int argc, char** argv)
{
    const Options opts = parseArgs(argc, argv);

    const geometrize::Bitmap target = loadImageAsBitmap(opts.inputPath);
    std::printf("图片: %s (%ux%u)\n", opts.inputPath.c_str(), target.getWidth(), target.getHeight());

    geometrize::ImageRunner runner(target);
    geometrize::ImageRunnerOptions options;
    options.alpha = static_cast<std::uint8_t>(opts.alpha);
    options.shapeCount = opts.shapeCount;
    options.maxShapeMutations = opts.maxMutations;
    options.seed = opts.seed;
    options.maxThreads = opts.threads;
    options.shapeTypes = static_cast<geometrize::ShapeTypes>(opts.shapeTypes);
#ifdef GEOBENCH_FAST
    // fast 专属选项:baseline 库的 ImageRunnerOptions 无对应字段,宏守卫保证 base 侧可编译
    options.pyramidSearch = opts.pyramid;
    options.enhancements.adaptiveStep = opts.adaptiveStep;
    options.enhancements.alphaSearch = opts.alphaSearch;
    options.enhancements.errorGuide = opts.errorGuide;
    options.enhancements.guideEpsilonPermill = opts.guideEpsilonPermill;
    options.enhancements.segmentColors = opts.segmentColors;
    for(const auto& region : opts.priorityRegions) {
        options.priorityRegions.push_back({region[0], region[1], region[2], region[3]});
    }
    if(!opts.alphaTiers.empty()) {
        options.enhancements.alphaCandidates.clear();
        for(const std::uint32_t tier : opts.alphaTiers) {
            options.enhancements.alphaCandidates.push_back(static_cast<std::uint8_t>(tier));
        }
    }
    options.fixShapeBoundsOffByOne = opts.fixShapeBounds;
#endif
    if(opts.hasBounds) {
        options.shapeBounds.enabled = true;
        options.shapeBounds.xMinPercent = opts.boundsXMinPercent;
        options.shapeBounds.yMinPercent = opts.boundsYMinPercent;
        options.shapeBounds.xMaxPercent = opts.boundsXMaxPercent;
        options.shapeBounds.yMaxPercent = opts.boundsYMaxPercent;
    }

    std::uint64_t stepFingerprint = 0xcbf29ce484222325ULL; // FNV-1a 64 位 offset basis
    std::uint64_t totalShapes{0}; // 累计接受的形状数:报告留证"这一步到底画了东西没有"

    const auto startTime = std::chrono::steady_clock::now();

    for(std::uint32_t step = 0; step < opts.steps; step++) {
        const std::vector<geometrize::ShapeResult> results = runner.step(options);

        // 质量曲线:按间隔打印全分辨率 differenceFull 分数(与步进指纹无关,纯观测)
        if(opts.qualityReportInterval != 0 && (step + 1) % opts.qualityReportInterval == 0) {
            std::printf("QUALITY step=%u diff=%.6f\n", step + 1,
                geometrize::core::differenceFull(runner.getTarget(), runner.getCurrent()));
        }

        if(results.empty()) {
            // 该步没有可接受形状(所有候选被拒绝),对拍同样要计入这个事实
            mixBytes(stepFingerprint, "REJECT", 6);
            if(opts.verbose) {
                std::printf("step %u: rejected\n", step);
            }
            continue;
        }
        totalShapes += results.size();

        for(const geometrize::ShapeResult& result : results) {
            // 形状轨迹签名:形状类型 + 光栅化扫描线(实际落画的几何)+ 分数 + 颜色。
            // 只用两套库都有的 API,不再复用 exportShapeJson 的文本——那是调试用序列化,
            // 其格式缺陷修正(单元素数组尾逗号)不应改变门禁口径,否则"修好一个导出 bug"会误判成轨迹分叉。
            const std::uint32_t typeBits{static_cast<std::uint32_t>(
                static_cast<std::underlying_type<geometrize::ShapeTypes>::type>(result.shape->getType()))};
            mixBytes(stepFingerprint, &typeBits, sizeof(typeBits));
            const std::vector<geometrize::Scanline> shapeLines{result.shape->rasterize(*result.shape)};
            const std::uint64_t lineCount{shapeLines.size()};
            mixBytes(stepFingerprint, &lineCount, sizeof(lineCount));
            for(const geometrize::Scanline& line : shapeLines) {
                mixBytes(stepFingerprint, &line.y, sizeof(line.y));
                mixBytes(stepFingerprint, &line.x1, sizeof(line.x1));
                mixBytes(stepFingerprint, &line.x2, sizeof(line.x2));
            }
#if defined(GEBENCH_FAST)
            // 分段色是改进版独有字段(base 无此概念);经典路径下恒为空,故不影响 bit-exact 口径
            for(const geometrize::ScanlineColor& segment : result.segments) {
                mixBytes(stepFingerprint, &segment.y, sizeof(segment.y));
                mixBytes(stepFingerprint, &segment.x1, sizeof(segment.x1));
                mixBytes(stepFingerprint, &segment.x2, sizeof(segment.x2));
                const std::uint32_t segmentColorBits =
                    segment.color.r | (static_cast<std::uint32_t>(segment.color.g) << 8)
                    | (static_cast<std::uint32_t>(segment.color.b) << 16) | (static_cast<std::uint32_t>(segment.color.a) << 24);
                mixBytes(stepFingerprint, &segmentColorBits, sizeof(segmentColorBits));
            }
#endif
            break; // step 至多返回一个结果,取一次签名即可覆盖
        }
        for(const geometrize::ShapeResult& result : results) {
            mixScore(stepFingerprint, result.score);
            const std::uint32_t colorBits =
                result.color.r | (static_cast<std::uint32_t>(result.color.g) << 8)
                | (static_cast<std::uint32_t>(result.color.b) << 16) | (static_cast<std::uint32_t>(result.color.a) << 24);
            mixBytes(stepFingerprint, &colorBits, sizeof(colorBits));
            if(opts.verbose) {
                std::printf("step %u: score=%.10f shape=%d\n", step, result.score, static_cast<int>(result.shape->getType()));
            }
        }
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();
    const double seconds = static_cast<double>(elapsedMs) / 1000.0;

    // 一级对拍:最终位图全字节 SHA-256
    const std::string finalHash = geobench::sha256Hex(runner.getCurrent().getDataRef());

    std::printf("FINAL_SHA256 %s\n", finalHash.c_str());
    std::printf("STEP_FINGERPRINT %016llx\n", static_cast<unsigned long long>(stepFingerprint));
    std::printf("SHAPES_TOTAL %llu\n", static_cast<unsigned long long>(totalShapes));
    std::printf("TIME_MS %lld\n", static_cast<long long>(elapsedMs));
    std::printf("STEPS_PER_SEC %.2f\n", opts.steps / seconds);

    // 对拍失败留痕:裸 RGBA 直接落盘,hexdiff 即可定位首个发散像素。
    // 先写 .part 再整体改名:进程中途被杀不再留下"看似完整"的半截文件
    // (改名失败时 .part 保留现场,目标文件要么完整要么缺失,两态可辨)
    if(!opts.dumpFinalPath.empty()) {
        const std::string partPath{opts.dumpFinalPath + ".part"};
        std::FILE* dump = std::fopen(partPath.c_str(), "wb");
        if(dump != nullptr) {
            const auto& data = runner.getCurrent().getDataRef();
            std::fwrite(data.data(), 1, data.size(), dump);
            std::fclose(dump);
            std::remove(opts.dumpFinalPath.c_str()); // Windows rename 不覆盖已存在目标(目标不存在亦无妨)
            if(std::rename(partPath.c_str(), opts.dumpFinalPath.c_str()) != 0) {
                std::fprintf(stderr, "最终位图改名失败,保留现场: %s\n", partPath.c_str());
            }
        } else {
            std::fprintf(stderr, "无法写入最终位图: %s\n", partPath.c_str());
        }
    }
    return 0;
}
