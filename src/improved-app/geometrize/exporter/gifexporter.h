#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace geometrize
{
class Bitmap;

struct ShapeResult;
}

namespace geometrize
{

namespace exporter
{

/**
 * @brief exportGIF Exports shape data to a GIF image.
 * @param data The shape data to export.
 * @param inputWidth The width of the canvas each frame will be rendered to.
 * @param inputHeight The height of the canvas each frame will be rendered to.
 * @param outputWidth The width of the image each frame will be rasterized into.
 * @param outputHeight The height of the image each frame will be rasterized into.
 * @param frameSkipPredicate Returns true if the frame at the given index should be skipped.
 * @param filePath The full path to the GIF image file target (include the filename and .gif extension).
 * @param frameDelayMs Fixed delay between frames in milliseconds; 0 keeps the legacy dynamic
 * curve (1000/(i+1), floored at 20ms — more shapes means faster playback).
 * @param loopCount How many times the animation loops before stopping; 0 loops forever.
 * @param endPauseMs Extra delay on the final frame in milliseconds; 0 adds no pause.
 * @return True if the GIF was saved, else false.
 */
bool exportGIF(
        const std::vector<geometrize::ShapeResult>& data,
        std::uint32_t inputWidth,
        std::uint32_t inputHeight,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight,
        const std::function<bool(std::size_t)>& frameSkipPredicate,
        const std::string& filePath,
        std::uint32_t frameDelayMs = 0,
        std::uint32_t loopCount = 0,
        std::uint32_t endPauseMs = 2000);

}

}
