#pragma once

#include "pico_page/clock_time.h"
#include "pico_page/i18n.h"
#include "reading_footer.h"

namespace pico_page::clock_panel {
struct LastCalibration {
    std::string value;
    std::string detail;
};

inline std::string local_date(std::int64_t utc) {
    const auto local = static_cast<std::time_t>(utc + clock_time::utc8_offset_seconds);
    std::tm date{};
    if (!gmtime_r(&local, &date)) return {};
    char value[6];
    return std::strftime(value, sizeof(value), "%m-%d", &date) ? std::string(value) : std::string();
}

inline LastCalibration last_calibration(std::int64_t now, std::int64_t last, bool saved_network,
                                        i18n::Language language) {
    const auto tr = [&](const char* key) { return i18n::text(language, key); };
    const auto trf = [&](const char* key, const char* name, const std::string& value) {
        return i18n::format(language, key, {{name, value}});
    };
    if (!clock_time::valid(last)) return {tr("无记录"), saved_network ? tr("联网校时后记录") : tr("添加网络后可校准")};

    const auto time = reading_footer::clock(static_cast<std::time_t>(last), clock_time::utc8_offset_seconds / 60);
    const auto date_time = local_date(last) + " " + time;
    if (now < clock_time::floor) return {date_time, tr("断电后需重新校准")};
    if (!clock_time::valid(now) || last > now) return {date_time, tr("UTC+8 · 时间异常")};
    const auto day = [](std::int64_t utc) {
        return (utc + clock_time::utc8_offset_seconds) / clock_time::unix_seconds_per_day;
    };
    const auto days = day(now) - day(last);
    if (now - last < 120) return {tr("刚刚"), days ? trf("昨天 {time}", "time", time)
                                                : trf("今天 {time}", "time", time)};
    if (days == 1) return {trf("昨天 {time}", "time", time), date_time};
    if (days > 1) return {trf("{count} 天前", "count", std::to_string(days)), date_time};

    const auto minutes = (now - last) / 60;
    const auto elapsed = minutes < 60 ? trf("约 {count} 分钟前", "count", std::to_string(minutes))
        : trf("约 {count} 小时前", "count", std::to_string(minutes / 60));
    return {trf("今天 {time}", "time", time), elapsed};
}
}
