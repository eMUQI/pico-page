#pragma once

#include "pico_page/i18n.h"
#include <string>
#include <string_view>

namespace pico_page {

inline std::string font_error_message(std::string_view detail, i18n::Language language = i18n::Language::SimplifiedChinese) {
    detail = detail.substr(0, detail.find("; path="));
    struct Failure { std::string_view detail, message; };
    static constexpr Failure failures[] = {
        {"FreeType out of memory", "字体处理内存不足（F12）"},
        {"FreeType initialization failed", "字体引擎初始化失败（F01）"},
        {"Cannot open embedded font", "内置字体打开失败（F02）"},
        {"Cannot open font", "字体文件打开失败（F02）"},
        {"Font has no Unicode character map", "字体字符表不可用（F03）"},
        {"Invalid font size", "字体字号设置失败（F04）"},
        {"Font glyph metrics failed", "字形尺寸读取失败（F05）"},
        {"Font glyph load failed", "字形读取失败（F06）"},
        {"Font stroke adjustment failed", "字形加粗处理失败（F07）"},
        {"Font glyph rasterization failed", "字形绘制失败（F08）"},
        {"Unsupported glyph pixel format", "字形像素格式不支持（F09）"},
        {"Font glyph exceeds cache budget", "单个字形超过缓存上限（F10）"},
        {"Font face unavailable", "正文字体未就绪（F11）"},
    };
    for (const auto& failure : failures) {
        if (detail.find(failure.detail) == std::string_view::npos) continue;
        std::string message(i18n::text(language, failure.message.data()));
        for (const auto tag : {" [FT=", " [U+"}) {
            const auto begin = detail.find(tag);
            if (begin == std::string_view::npos) continue;
            const auto end = detail.find(']', begin);
            if (end != std::string_view::npos) message.append(detail.substr(begin, end - begin + 1));
        }
        return message;
    }
    return {};
}

}
