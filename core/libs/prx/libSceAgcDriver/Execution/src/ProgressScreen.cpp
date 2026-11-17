#include "prx/libSceAgcDriver/Execution/include/ProgressScreen.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AgcDriver {

std::string FormatProgressEta(double seconds) {
    if (!(seconds >= 0.0) || seconds > 99.0 * 3600.0) return "ETA --";
    const auto total = static_cast<long>(std::lround(seconds));
    char text[32];
    if (total >= 3600) std::snprintf(text, sizeof(text), "ETA %ldH %02ldM", total / 3600, (total / 60) % 60);
    else if (total >= 60) std::snprintf(text, sizeof(text), "ETA %ldM %02ldS", total / 60, total % 60);
    else std::snprintf(text, sizeof(text), "ETA %ldS", total);
    return text;
}

namespace {

constexpr std::uint32_t Dark = static_cast<std::uint32_t>(OverlayColor::Panel);
constexpr std::uint32_t Track = static_cast<std::uint32_t>(OverlayColor::Key);
constexpr std::uint32_t Fill = static_cast<std::uint32_t>(OverlayColor::Accent);
constexpr std::uint32_t Ink = static_cast<std::uint32_t>(OverlayColor::Text);

std::int32_t textWidth(const std::string& text, std::int32_t scale) {
    return text.empty() ? 0 : (static_cast<std::int32_t>(text.size()) * OverlayMetrics::Advance - 1) * scale;
}

void drawText(OverlayLayout& layout, const std::string& line, std::int32_t left, std::int32_t top, std::int32_t scale) {
    const auto text = OverlayText(line);
    for (std::size_t column = 0; column < text.size(); ++column) {
        const auto& glyph = OverlayGlyph(text[column]);
        const auto glyphLeft = left + static_cast<std::int32_t>(column) * OverlayMetrics::Advance * scale;
        for (std::int32_t y = 0; y < OverlayMetrics::GlyphHeight; ++y) {
            const auto bits = glyph[static_cast<std::size_t>(y)];
            std::int32_t x = 0;
            while (x < OverlayMetrics::GlyphWidth) {
                if ((bits & (0x10u >> x)) == 0) {
                    ++x;
                    continue;
                }
                auto end = x;
                while (end < OverlayMetrics::GlyphWidth && (bits & (0x10u >> end)) != 0) ++end;
                layout.rects.push_back({glyphLeft + x * scale, top + y * scale, (end - x) * scale, scale, Ink});
                x = end;
            }
        }
    }
}

// A pill of `width` x `height` at (left, top) as one rect per row, the ends rounded.
void drawPill(OverlayLayout& layout, std::int32_t left, std::int32_t top, std::int32_t width, std::int32_t height, std::uint32_t color) {
    const double radius = height / 2.0;
    for (std::int32_t row = 0; row < height; ++row) {
        const double dy = radius - (row + 0.5);
        const auto inset = static_cast<std::int32_t>(std::lround(radius - std::sqrt(std::max(0.0, radius * radius - dy * dy))));
        const auto span = width - 2 * inset;
        if (span > 0) layout.rects.push_back({left + inset, top + row, span, 1, color});
    }
}

}

OverlayLayout LayOutProgress(const ProgressScreenInfo& info, VkExtent2D extent) {
    OverlayLayout layout;
    const auto width = static_cast<std::int32_t>(extent.width);
    const auto height = static_cast<std::int32_t>(extent.height);
    layout.bounds = {0, 0, width, height, Dark};
    layout.rects.push_back(layout.bounds);
    const double f = std::max(0.5, height / 720.0);
    const auto titleScale = std::max(2, static_cast<std::int32_t>(std::lround(3.0 * f)));
    const auto smallScale = std::max(1, static_cast<std::int32_t>(std::lround(2.0 * f)));
    layout.scale = smallScale;
    const auto barWidth = std::min(static_cast<std::int32_t>(std::lround(400.0 * f)), width * 3 / 4);
    const auto barHeight = std::max(6, static_cast<std::int32_t>(std::lround(12.0 * f)));
    const auto titleHeight = OverlayMetrics::GlyphHeight * titleScale;
    const auto smallHeight = OverlayMetrics::GlyphHeight * smallScale;
    const auto gap = static_cast<std::int32_t>(std::lround(18.0 * f));
    const auto blockHeight = titleHeight + gap + barHeight + gap + smallHeight + gap / 2 + smallHeight;
    const auto top = std::max(0, (height - blockHeight) / 2);
    const auto left = std::max(0, (width - barWidth) / 2);
    const auto centered = [&](const std::string& text, std::int32_t y, std::int32_t scale) {
        const auto shown = OverlayText(text);
        drawText(layout, shown, std::max(0, (width - textWidth(shown, scale)) / 2), y, scale);
    };
    centered(info.title, top, titleScale);
    const auto barTop = top + titleHeight + gap;
    drawPill(layout, left, barTop, barWidth, barHeight, Track);
    const double fraction = info.total != 0 ? std::clamp(static_cast<double>(info.done) / static_cast<double>(info.total), 0.0, 1.0) : 0.0;
    const auto filled = std::max(barHeight, static_cast<std::int32_t>(std::lround(fraction * barWidth)));
    if (info.total != 0) drawPill(layout, left, barTop, filled, barHeight, Fill);
    const auto textTop = barTop + barHeight + gap;
    char count[48];
    std::snprintf(count, sizeof(count), "%llu / %llu", static_cast<unsigned long long>(info.done), static_cast<unsigned long long>(info.total));
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(std::lround(fraction * 100.0)));
    drawText(layout, count, left, textTop, smallScale);
    const std::string percentText = OverlayText(percent);
    drawText(layout, percentText, left + barWidth - textWidth(percentText, smallScale), textTop, smallScale);
    centered(FormatProgressEta(info.etaSeconds), textTop + smallHeight + gap / 2, smallScale);
    return layout;
}

}
