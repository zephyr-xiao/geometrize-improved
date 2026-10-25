// geocli:headless 几何化批处理命令行工具(与 GUI 共享同一套库源码与 Qt 部署)。
// 设计口径:图像加载/缩放/落盘与 GUI 完全同源(QImage + RGBA8888 + KeepAspectRatio 平滑缩放),
// 算法管线走 lib 的 ImageRunner(含全部 opt-in 增强开关);输出 SVG/PNG/JSON 用 lib 纯 C++ 导出器。
// 幂等约定:目标产物已全部存在的图默认跳过(--overwrite 强制重跑),适配计划任务/定时批处理场景。
// 退出码:0 全部成功 / 1 存在失败项 / 2 参数错误。

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/core.h"
#include "geometrize/exporter/shapejsonexporter.h"
#include "geometrize/exporter/svgexporter.h"
#include "geometrize/runner/imagerunner.h"
#include "geometrize/runner/imagerunneroptions.h"
#include "geometrize/shaperesult.h"
#include "geometrize/shape/shapetypes.h"
// GIF 导出器在 app 源树(非 lib 镜像),以 app 源根为 include base
#include "exporter/gifexporter.h"

namespace {

struct Options {
    std::string inputPath;
    std::string outputDir;
    bool formatSvg = true;
    bool formatPng = true;
    bool formatJson = false;
    bool formatGif = false;
    // GIF 参数(默认对齐 GUI 口径:每帧 20 形状、20FPS、原尺寸、无限循环)
    std::uint32_t gifFrameShapes = 20;
    std::uint32_t gifDelayMs = 50;
    std::uint32_t gifScale = 1;
    std::uint32_t gifLoop = 0;
    std::uint32_t steps = 300;
    std::uint32_t shapeCount = 50;
    std::uint32_t maxMutations = 100;
    std::uint32_t alpha = 128;
    std::uint32_t seed = 9001;
    std::uint32_t threads = 0; // 0 = 硬件并发(与 GUI 默认一致;确定性口径下建议显式指定)
    std::uint32_t shapeTypes = geometrize::ShapeTypes::ELLIPSE;
    std::uint32_t maxSize = 1024; // 处理分辨率上限(单边,照 GUI 分支默认);0 = 不限制
    bool overwrite = false;
    // 算法增强开关(opt-in,默认全关;输出与关闭时不同属预期)
    bool pyramid = false;
    bool budgetByArea = false;
    bool adaptiveStep = false;
    bool alphaSearch = false;
    bool errorGuide = false;
    bool segmentColors = false;
};

void printUsage()
{
    std::printf(
        "geocli --input <图片|目录> [选项]\n"
        "  --output DIR        输出目录(默认 <输入旁>/geometrize_out)\n"
        "  --format LIST       逗号分隔输出格式 svg,png,json,gif(默认 svg,png)\n"
        "  --steps N           每图步数(默认 300)\n"
        "  --shape-count N     每步候选形状数(默认 50)\n"
        "  --max-mutations N   每候选最大变异次数(默认 100)\n"
        "  --alpha N           形状透明度 0-255(默认 128)\n"
        "  --types SPEC        逗号分隔: rect,rrect,tri,ellipse,rellipse,circle,line,bezier,polyline 或 all\n"
        "  --seed N            RNG 种子(默认 9001;threads 参与结果确定性)\n"
        "  --threads N         并行线程数(默认 0 = 硬件并发)\n"
        "  --max-size N        处理分辨率单边上限,超限按比例平滑缩小(默认 1024,0 = 不限制)\n"
        "  --overwrite         覆盖已存在的输出(默认跳过产物齐全的图,幂等重跑)\n"
        "增强开关(opt-in,输出与关闭时不同属预期):\n"
        "  --pyramid           金字塔搜索(半分辨率评估,大图大幅提速)\n"
        "  --budget-by-area    候选预算按面积降档(大图 ~2x,与金字塔正交可叠加)\n"
        "  --adaptive-step     自适应变异步长(同步数质量更好,更慢)\n"
        "  --alpha-search      逐形状透明度搜索(同步数质量更好,更慢)\n"
        "  --error-guide       误差图引导形状放置(低预算收益最大)\n"
        "  --segment-colors    分段颜色(行级取色,跨明暗边界不发灰)\n"
        "GIF 参数(--format 含 gif 时生效;帧画面与 GUI 导出同源,帧数自动封顶 1000):\n"
        "  --gif-frame-shapes N 每帧形状数(默认 20)\n"
        "  --gif-delay-ms N     帧延迟毫秒,下限 20(默认 50 = 20FPS)\n"
        "  --gif-scale N        输出尺寸倍率(默认 1;护栏:每边 ≤65535 且宽×高×4 ≤2GB)\n"
        "  --gif-loop N         循环次数(默认 0 = 无限循环)\n"
        "  -h / --help         本帮助\n");
}

// 数值参数统一严格校验:空值/非数字/负号/超范围一律 exit 2(照 geobench 口径,
// 拼错的参数静默退化会让批处理"跑了但不是你以为的配置")
std::uint32_t parseU32Arg(const char* name, const std::string& token, std::uint32_t maxValue)
{
    if(token.empty()) {
        std::fprintf(stderr, "%s 缺少取值\n", name);
        std::exit(2);
    }
    if(token.find_first_not_of("0123456789") != std::string::npos) {
        std::fprintf(stderr, "%s 应为非负整数: %s\n", name, token.c_str());
        std::exit(2);
    }
    try {
        const unsigned long value = std::stoul(token);
        if(value > static_cast<unsigned long>(maxValue)) {
            std::fprintf(stderr, "%s 超出上限 %u: %s\n", name, maxValue, token.c_str());
            std::exit(2);
        }
        return static_cast<std::uint32_t>(value);
    } catch(const std::exception&) {
        std::fprintf(stderr, "%s 数值溢出: %s\n", name, token.c_str());
        std::exit(2);
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
        if(!matched) {
            std::fprintf(stderr, "未知形状类型: '%s'(可用: all 或 rect,rrect,tri,ellipse,rellipse,circle,line,bezier,polyline)\n", token.c_str());
            std::exit(2);
        }
        if(comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    if(types == 0) {
        std::fprintf(stderr, "--types 至少需要一个形状类型\n");
        std::exit(2);
    }
    return types;
}

Options parseArgs(int argc, char** argv)
{
    Options opts;
    for(int i = 1; i < argc; i++) {
        const std::string arg{argv[i]};
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string{argv[++i]} : std::string{}; };
        if(arg == "--input") opts.inputPath = next();
        else if(arg == "--output") opts.outputDir = next();
        else if(arg == "--format") {
            const std::string spec = next();
            opts.formatSvg = opts.formatPng = opts.formatJson = false;
            std::size_t pos = 0;
            bool any = false;
            while(pos <= spec.size()) {
                const std::size_t comma = spec.find(',', pos);
                const std::string token = spec.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
                if(token == "svg") opts.formatSvg = true;
                else if(token == "png") opts.formatPng = true;
                else if(token == "json") opts.formatJson = true;
                else if(token == "gif") opts.formatGif = true;
                else {
                    std::fprintf(stderr, "未知输出格式: '%s'(可用: svg,png,json,gif)\n", token.c_str());
                    std::exit(2);
                }
                any = true;
                if(comma == std::string::npos) break;
                pos = comma + 1;
            }
            if(!any) {
                std::fprintf(stderr, "--format 不能为空\n");
                std::exit(2);
            }
        }
        else if(arg == "--steps") opts.steps = parseU32Arg("--steps", next(), 1000000000U);
        else if(arg == "--shape-count") opts.shapeCount = parseU32Arg("--shape-count", next(), 1000000U);
        else if(arg == "--max-mutations") opts.maxMutations = parseU32Arg("--max-mutations", next(), 1000000U);
        else if(arg == "--alpha") opts.alpha = parseU32Arg("--alpha", next(), 255U);
        else if(arg == "--types") opts.shapeTypes = parseShapeTypes(next());
        else if(arg == "--seed") opts.seed = parseU32Arg("--seed", next(), 0xFFFFFFFFU);
        else if(arg == "--threads") opts.threads = parseU32Arg("--threads", next(), 1024U);
        else if(arg == "--max-size") opts.maxSize = parseU32Arg("--max-size", next(), 65535U);
        else if(arg == "--overwrite") opts.overwrite = true;
        else if(arg == "--gif-frame-shapes") opts.gifFrameShapes = parseU32Arg("--gif-frame-shapes", next(), 1000000U);
        else if(arg == "--gif-delay-ms") opts.gifDelayMs = parseU32Arg("--gif-delay-ms", next(), 60000U);
        else if(arg == "--gif-scale") opts.gifScale = parseU32Arg("--gif-scale", next(), 100U);
        else if(arg == "--gif-loop") opts.gifLoop = parseU32Arg("--gif-loop", next(), 65535U);
        else if(arg == "--pyramid") opts.pyramid = true;
        else if(arg == "--budget-by-area") opts.budgetByArea = true;
        else if(arg == "--adaptive-step") opts.adaptiveStep = true;
        else if(arg == "--alpha-search") opts.alphaSearch = true;
        else if(arg == "--error-guide") opts.errorGuide = true;
        else if(arg == "--segment-colors") opts.segmentColors = true;
        else if(arg == "--help" || arg == "-h") { printUsage(); std::exit(0); }
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            printUsage();
            std::exit(2);
        }
    }
    if(opts.inputPath.empty()) {
        std::fprintf(stderr, "缺少 --input\n");
        printUsage();
        std::exit(2);
    }
    if(opts.formatGif && opts.gifFrameShapes == 0) {
        std::fprintf(stderr, "--gif-frame-shapes 至少为 1\n");
        std::exit(2);
    }
    return opts;
}

// QImage → Bitmap:与 GUI imageloader 同口径(RGBA8888 字节序直搬)
geometrize::Bitmap imageToBitmap(const QImage& image)
{
    const QImage rgba{image.convertToFormat(QImage::Format_RGBA8888)};
    std::vector<std::uint8_t> data(rgba.bits(), rgba.bits() + rgba.sizeInBytes());
    return geometrize::Bitmap(static_cast<std::uint32_t>(rgba.width()), static_cast<std::uint32_t>(rgba.height()), std::move(data));
}

// 终态位图 → PNG:QImage 构造是浅共享,save 前位图必须存活(调用点保证)
bool saveBitmapPng(const geometrize::Bitmap& bitmap, const QString& path)
{
    const QImage image(bitmap.getDataRef().data(), static_cast<qsizetype>(bitmap.getWidth()), static_cast<qsizetype>(bitmap.getHeight()), QImage::Format_RGBA8888);
    return image.save(path, "PNG");
}

bool writeFile(const QString& path, const std::string& content)
{
    QFile file{path};
    if(!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(content.data(), static_cast<qint64>(content.size())) == static_cast<qint64>(content.size());
}

// 收集输入图清单:文件 → 单图;目录 → 单层全部可读格式(不递归,保持行为可预测)
QStringList collectImages(const QString& input)
{
    const QFileInfo info{input};
    if(info.isFile()) {
        return {input};
    }
    if(info.isDir()) {
        // QImageReader 的可读格式含大小写变体(png/PNG),QDir 过滤用不区分大小写的后缀集合
        const QList<QByteArray> supported{QImageReader::supportedImageFormats()};
        QStringList nameFilters;
        nameFilters.reserve(supported.size());
        for(const QByteArray& format : supported) {
            nameFilters.push_back(QLatin1String("*.") + QLatin1String(format));
        }
        // QDir::Files 显式排除目录;QDir::NoSymLinks 避免链接环
        const QFileInfoList entries{QDir(input).entryInfoList(nameFilters, QDir::Files | QDir::Readable, QDir::Name)};
        QStringList result;
        result.reserve(entries.size());
        for(const QFileInfo& entry : entries) {
            result.push_back(entry.absoluteFilePath());
        }
        return result;
    }
    std::fprintf(stderr, "输入既不是文件也不是目录: %s\n", input.toUtf8().constData());
    std::exit(2);
}

struct ProcessResult {
    bool skipped = false;
    bool ok = false;
    std::uint32_t shapeCount = 0;
};

ProcessResult processOne(const Options& opts, const QString& inputPath, const QString& outputDir)
{
    const QString baseName{QFileInfo(inputPath).completeBaseName()};

    // 幂等:请求的全部产物都已存在则跳过(中断重跑只补缺失项;--overwrite 强制重跑)
    QStringList targets;
    if(opts.formatSvg) targets.push_back(outputDir + "/" + baseName + ".svg");
    if(opts.formatPng) targets.push_back(outputDir + "/" + baseName + ".png");
    if(opts.formatJson) targets.push_back(outputDir + "/" + baseName + ".json");
    if(opts.formatGif) targets.push_back(outputDir + "/" + baseName + ".gif");
    if(!opts.overwrite) {
        bool allExist = !targets.isEmpty();
        for(const QString& target : targets) {
            allExist = allExist && QFileInfo::exists(target) && QFileInfo(target).size() > 0;
        }
        if(allExist) {
            return {true, true, 0};
        }
    }

    // 加载 + 处理分辨率上限(与 GUI 同款:KeepAspectRatio 平滑缩放,仅超限时进入缩放链)
    QImage image{inputPath};
    if(image.isNull()) {
        std::fprintf(stderr, "  无法加载图片: %s\n", inputPath.toUtf8().constData());
        return {};
    }
    if(opts.maxSize > 0
            && (static_cast<std::uint32_t>(image.width()) > opts.maxSize
                || static_cast<std::uint32_t>(image.height()) > opts.maxSize)) {
        image = image.scaled(static_cast<int>(opts.maxSize), static_cast<int>(opts.maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    const geometrize::Bitmap target{imageToBitmap(image)};

    geometrize::ImageRunnerOptions options;
    options.alpha = static_cast<std::uint8_t>(opts.alpha);
    options.shapeCount = opts.shapeCount;
    options.maxShapeMutations = opts.maxMutations;
    options.seed = opts.seed;
    options.maxThreads = opts.threads;
    options.shapeTypes = static_cast<geometrize::ShapeTypes>(opts.shapeTypes);
    options.pyramidSearch = opts.pyramid;
    options.budget.byArea = opts.budgetByArea;
    options.enhancements.adaptiveStep = opts.adaptiveStep;
    options.enhancements.alphaSearch = opts.alphaSearch;
    options.enhancements.errorGuide = opts.errorGuide;
    options.enhancements.segmentColors = opts.segmentColors;

    geometrize::ImageRunner runner{target};
    // ShapeResult 全 const 成员:可拷贝构造、拷贝赋值被删除,故用 push_back(构造路径)
    // 而非 insert 区间拷贝(其内部搬移需要赋值)
    std::vector<geometrize::ShapeResult> shapes;
    shapes.reserve(opts.steps);
    for(std::uint32_t step = 0; step < opts.steps; step++) {
        const std::vector<geometrize::ShapeResult> results{runner.step(options)};
        for(const geometrize::ShapeResult& result : results) {
            shapes.push_back(result);
        }
    }

    bool ok = true;
    if(opts.formatSvg) {
        ok = writeFile(outputDir + "/" + baseName + ".svg", geometrize::exporter::exportSVG(shapes, target.getWidth(), target.getHeight())) && ok;
    }
    if(opts.formatPng) {
        ok = saveBitmapPng(runner.getCurrent(), outputDir + "/" + baseName + ".png") && ok;
    }
    if(opts.formatJson) {
        ok = writeFile(outputDir + "/" + baseName + ".json", geometrize::exporter::exportShapeJson(shapes)) && ok;
    }
    if(opts.formatGif) {
        // 0 形状(如纯色图)时 exportGIF 必然 false:GIF 没有可编码的帧。
        // 严格口径:请求的产物必须齐全才算成功,缺失即记失败(批处理退出码可见)
        if(shapes.empty()) {
            std::fprintf(stderr, "  GIF 未生成(0 形状无可编码帧)\n");
            ok = false;
        } else {
            // 帧口径照 GUI:每第 N 个形状处取帧(0 起位,含首帧),末帧必取与封顶抽稀由导出器兜底
            const std::uint32_t frameStep{opts.gifFrameShapes};
            const auto frameSkipPredicate = [frameStep](const std::size_t frameIdx) {
                return frameIdx % frameStep != 0;
            };
            ok = geometrize::exporter::exportGIF(
                     shapes, target.getWidth(), target.getHeight(),
                     target.getWidth() * opts.gifScale, target.getHeight() * opts.gifScale,
                     frameSkipPredicate,
                     (outputDir + "/" + baseName + ".gif").toStdString(),
                     opts.gifDelayMs, opts.gifLoop, 2000) && ok;
        }
    }
    return {false, ok, static_cast<std::uint32_t>(shapes.size())};
}

} // namespace

int main(int argc, char** argv)
{
    const Options opts = parseArgs(argc, argv);

    const QString input{QString::fromStdString(opts.inputPath)};
    if(!QFileInfo::exists(input)) {
        std::fprintf(stderr, "输入不存在: %s\n", opts.inputPath.c_str());
        return 2;
    }

    // 输出目录默认放在输入旁边(与输入目录同级,名字固定),目录输入则在其内部
    QString outputDir;
    if(!opts.outputDir.empty()) {
        outputDir = QString::fromStdString(opts.outputDir);
    } else if(QFileInfo(input).isDir()) {
        outputDir = input + "/geometrize_out";
    } else {
        outputDir = QFileInfo(input).absolutePath() + "/geometrize_out";
    }
    if(!QDir().mkpath(outputDir)) {
        std::fprintf(stderr, "无法创建输出目录: %s\n", outputDir.toUtf8().constData());
        return 1;
    }

    const QStringList images{collectImages(input)};
    if(images.isEmpty()) {
        std::fprintf(stderr, "没有找到可处理的图片\n");
        return 1;
    }

    std::printf("geocli: %d 张图 | 输出 %s | steps=%u types=0x%x max-size=%u%s%s\n",
                images.size(), outputDir.toUtf8().constData(), opts.steps, opts.shapeTypes, opts.maxSize,
                opts.pyramid ? " +pyramid" : "", opts.budgetByArea ? " +budget-by-area" : "");

    int failures = 0;
    int processed = 0;
    int skipped = 0;
    for(int i = 0; i < images.size(); i++) {
        const QString& imagePath = images[i];
        std::printf("[%d/%d] %s ... ", i + 1, images.size(), QFileInfo(imagePath).fileName().toUtf8().constData());
        std::fflush(stdout);
        const auto startTime{std::chrono::steady_clock::now()};
        const ProcessResult result{processOne(opts, imagePath, outputDir)};
        const auto elapsedMs{std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count()};
        if(result.skipped) {
            skipped++;
            std::printf("跳过(产物已存在)\n");
        } else if(result.ok) {
            processed++;
            std::printf("完成(%lld.%llds, %u 形状)\n",
                        static_cast<long long>(elapsedMs) / 1000, static_cast<long long>(elapsedMs) % 1000, result.shapeCount);
        } else {
            failures++;
            std::printf("失败\n");
        }
    }

    std::printf("汇总: 处理 %d, 跳过 %d, 失败 %d\n", processed, skipped, failures);
    return failures > 0 ? 1 : 0;
}
