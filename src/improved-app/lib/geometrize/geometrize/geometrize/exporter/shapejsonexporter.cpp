#include "shapejsonexporter.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "shapeserializer.h"
#include "../shape/shape.h"
#include "../shape/shapetypes.h"
#include "../shaperesult.h"

namespace geometrize
{

namespace exporter
{

std::string exportShapeJson(const std::vector<geometrize::ShapeResult>& data)
{
    std::ostringstream stream;
    stream << "{\"shapes\":\n[";

    for(std::size_t i = 0; i < data.size(); i++) {
        const geometrize::ShapeResult& s(data[i]);
        const geometrize::ShapeTypes type{s.shape->getType()};
        const std::vector<float> shapeData{getRawShapeData(*s.shape.get())};
        const geometrize::rgba color(s.color);
        const double score{s.score};

        stream << "{" << "\"type\":" << static_cast<std::underlying_type<geometrize::ShapeTypes>::type>(type) << ", \"data\":[";
        for(std::size_t d = 0; d < shapeData.size(); d++) {
            stream << shapeData[d];
            if(d <= shapeData.size() - 2) {
                stream << ",";
            }
        }
        stream << "],\"color\":[" << static_cast<std::uint32_t>(color.r) << "," << static_cast<std::uint32_t>(color.g) << "," << static_cast<std::uint32_t>(color.b) << "," << static_cast<std::uint32_t>(color.a) << "],";
        stream << "\"score\":" << score;

        // A2.4:可选分段颜色数组(旧消费者忽略未知字段;线型/开关关时字段不出现)。
        // 每段 {"y","x1","x2","color"},区间语义与库内 Scanline 一致(闭区间)。
        if(!s.segments.empty()) {
            stream << ",\"segments\":[";
            for(std::size_t k = 0; k < s.segments.size(); k++) {
                const geometrize::ScanlineColor& seg(s.segments[k]);
                stream << "{\"y\":" << seg.y
                       << ",\"x1\":" << seg.x1
                       << ",\"x2\":" << seg.x2
                       << ",\"color\":[" << static_cast<std::uint32_t>(seg.color.r)
                       << "," << static_cast<std::uint32_t>(seg.color.g)
                       << "," << static_cast<std::uint32_t>(seg.color.b)
                       << "," << static_cast<std::uint32_t>(seg.color.a) << "]}";
                if(k <= s.segments.size() - 2) {
                    stream << ",";
                }
            }
            stream << "]";
        }

        stream << "}";

        if(i <= data.size() - 2) {
            stream << ",\n";
        }
    }

    stream << "\n]}";
    return stream.str();
}

}

}
