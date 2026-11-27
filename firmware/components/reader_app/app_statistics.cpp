#include "app_impl.h"

namespace pico_page {
namespace {
constexpr unsigned books_per_statistics_page = 7;
constexpr std::uint64_t minute_ms = 60000;
constexpr int calendar_start = 200, month_width = 212, month_height = 198;
constexpr int month_grid_left = 34, month_grid_top = 308, day_width = 88, day_height = 100;

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int month_length(int year, int month) {
    constexpr int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[month - 1] + (month == 2 && leap_year(year));
}

int first_day_of_year(int year) {
    const auto leaps = [](int y) { return y / 4 - y / 100 + y / 400; };
    return (year - 1970) * 365 + leaps(year - 1) - leaps(1969);
}

int first_day_of_month(int year, int month) {
    int result = first_day_of_year(year);
    for (int i = 1; i < month; ++i) result += month_length(year, i);
    return result;
}

struct CalendarDate { int year, month, day; };
CalendarDate calendar_date(int day_number) {
    int low = 1970, high = 10000;
    while (high - low > 1) {
        const int middle = (low + high) / 2;
        if (first_day_of_year(middle) <= day_number) low = middle;
        else high = middle;
    }
    int month = 1, offset = day_number - first_day_of_year(low);
    while (month < 12 && offset >= month_length(low, month)) {
        offset -= month_length(low, month);
        ++month;
    }
    return {low, month, offset + 1};
}

int monday_column(int day_number) { return (day_number + 3) % 7; }

std::string two_digits(int number) {
    return (number < 10 ? "0" : "") + std::to_string(number);
}

std::string date_label(int day_number) {
    if (day_number < 0) return "—";
    const auto date = calendar_date(day_number);
    return std::to_string(date.year) + "-" + two_digits(date.month) + "-" + two_digits(date.day);
}

std::uint64_t day_duration(const StatisticsReport& report, int first, int day_number) {
    const auto index = static_cast<std::size_t>(day_number - first);
    return index < report.day_ms.size() ? report.day_ms[index] : 0;
}
}

const char* ReaderApp::Impl::statistics_entry(bool book) const {
    return book ? tr("本书统计")
                : tr("阅读统计");
}

std::string ReaderApp::Impl::statistics_duration(std::uint64_t duration_ms) const {
    const auto seconds = duration_ms / 1000, minutes = seconds / 60;
    if (duration_ms && !seconds) return tr("不足 1 秒");
    if (seconds < 60) return std::to_string(seconds) + tr(" 秒");
    if (minutes < 60) {
        if (!(seconds % 60)) return std::to_string(minutes) + tr(" 分钟");
        return std::to_string(minutes) + tr(" 分 ") + std::to_string(seconds % 60) +
               tr(" 秒");
    }
    const auto hours = std::to_string(minutes / 60) + tr(" 小时");
    return minutes % 60 ? hours + " " + std::to_string(minutes % 60) + tr(" 分") : hours;
}

void ReaderApp::Impl::open_statistics(bool book) {
    if ((book && view != View::BookDetails) || (!book && view != View::Library)) return;
    statistics_from_details = book;
    statistics_book_parent = StatisticsPage::Books;
    statistics_book_title = book ? (detail_metadata.title.empty() ? basename(detail_path) : detail_metadata.title) : "";
    statistics_book_path = book ? detail_path : "";
    statistics_book_id = 0;
    statistics_list_page = 0;
    statistics_page = book ? StatisticsPage::Book : StatisticsPage::Overview;
    view = View::Statistics;
    load_statistics_page();
}

void ReaderApp::Impl::load_statistics_page() {
    const auto utc = statistics_now().unix_seconds;
    constexpr auto calendar_end_utc = static_cast<std::int64_t>(statistics_day_limit) * 86400 - 8 * 3600;
    statistics_today = utc && *utc >= 0 && *utc < calendar_end_utc
        ? static_cast<int>((*utc + 8 * 3600) / 86400) : -1;
    StatisticsQuery query;
    std::optional<StatisticsBookSource> lookup;
    switch (statistics_page) {
    case StatisticsPage::Overview:
        if (statistics_today >= 0) {
            const auto date = calendar_date(statistics_today);
            query.from_day = std::max(0, std::min(statistics_today - 6, first_day_of_month(date.year, date.month)));
            query.to_day = statistics_today;
        }
        break;
    case StatisticsPage::Year:
        query.from_day = first_day_of_year(statistics_year);
        query.to_day = std::min<int>(statistics_day_limit, first_day_of_year(statistics_year + 1)) - 1;
        break;
    case StatisticsPage::Month:
        query.from_day = first_day_of_month(statistics_year, statistics_month);
        query.to_day = std::min<int>(statistics_day_limit, query.from_day + month_length(statistics_year, statistics_month)) - 1;
        break;
    case StatisticsPage::Day:
        query.selected_day = statistics_day;
        if (statistics_day >= 0) query.from_day = query.to_day = statistics_day;
        [[fallthrough]];
    case StatisticsPage::Books:
        query.limit = books_per_statistics_page;
        query.offset = statistics_list_page * books_per_statistics_page;
        break;
    case StatisticsPage::Book:
        query.limit = 1;
        query.book_id = statistics_book_id;
        if (statistics_from_details) lookup = StatisticsBookSource{detail_path, detail_metadata.book_id,
            {}, -1, revision_for(detail_path, detail_metadata)};
        break;
    case StatisticsPage::Settings:
    case StatisticsPage::ClearBook:
    case StatisticsPage::ClearAll:
        break;
    }
    request_statistics(query, std::move(lookup));
    draw_statistics();
    overlay();
}

void ReaderApp::Impl::statistics_footer(const std::string& middle, bool previous, bool next) {
    canvas.line(0, footer_zone, 683, footer_zone, 1, 2);
    for (int x : {205, 479}) canvas.line(x, footer_zone, x, 1215, 1, 2);
    if (previous) {
        center(tr("‹ 上一屏"), 0, 1152, 205, 24);
        hits.push_back({0, footer_zone, 205, 96, Prev});
    }
    if (next) {
        center(tr("下一屏 ›"), 479, 1152, 205, 24);
        hits.push_back({479, footer_zone, 205, 96, Next});
    }
    center(middle, 205, 1152, 274, 24, 3);
}

void ReaderApp::Impl::statistics_heat_mark(int x, int y, int size, std::uint64_t duration_ms) {
    outline(x, y, size, size, 1);
    if (!duration_ms) return;
    if (duration_ms >= 45 * minute_ms) canvas.fill_rect(x, y, size, size, 1);
    else if (duration_ms >= 15 * minute_ms) canvas.fill_rect(x, y + size / 2, size, size - size / 2, 1);
    else {
        const int dot = std::max(3, size / 3);
        canvas.fill_rect(x + (size - dot) / 2, y + (size - dot) / 2, dot, dot, 1);
    }
}

void ReaderApp::Impl::draw_statistics_overview() {
    const auto& report = statistics_report;
    int first = 0, month_start = 0;
    if (statistics_today >= 0) {
        const auto date = calendar_date(statistics_today);
        month_start = first_day_of_month(date.year, date.month);
        first = std::max(0, std::min(statistics_today - 6, month_start));
    }
    text(tr("今日阅读"), 36, 130, 28, 612, 3);
    text(statistics_today < 0 ? "—" : statistics_duration(day_duration(report, first, statistics_today)), 36, 182, 32, 612, 1, true);
    text(tr("近 7 天 · 分钟"), 36, 263, 24, 612, 3);
    if (statistics_today < 0) {
        text(tr("时间未校准"), 36, 364, 24, 612);
    } else {
        std::uint64_t peak = 1;
        for (int i = 0; i < 7; ++i) peak = std::max(peak, day_duration(report, first, statistics_today - i));
        for (int i = 0; i < 7; ++i) {
            const int day_number = statistics_today - 6 + i;
            const auto value = day_duration(report, first, day_number);
            const int height = value ? std::max(4, static_cast<int>(104.0 * value / peak)) : 0;
            const int x = 34 + i * 88;
            if (height) canvas.fill_rect(x + 26, 449 - height, 36, height, 1);
            const auto minutes = value / minute_ms;
            center(value && !minutes ? "<1" : std::to_string(minutes), x, 411 - height, 88, 20);
            if (day_number >= 0) {
                const auto date = calendar_date(day_number);
                center(std::to_string(date.month) + "/" + std::to_string(date.day), x, 456, 88, 20, 3);
            }
        }
        canvas.line(36, 449, 648, 449, 1, 2);
    }
    std::uint64_t month_ms = 0;
    for (int i = month_start; i <= statistics_today; ++i) month_ms += day_duration(report, first, i);
    const auto metric = [&](const char* label, const std::string& value, int x, int y) {
        text(label, x, y, 24, 282, 3);
        text(value, x, y + 38, 28, 282, 1, true);
    };
    metric(tr("本月"), statistics_today < 0 ? "—" : statistics_duration(month_ms), 36, 512);
    metric(tr("累计时长"), statistics_duration(report.total_ms), 352, 512);
    metric(tr("有效阅读天数"), std::to_string(report.days), 36, 610);
    metric(tr("读过的书"), std::to_string(report.book_count), 352, 610);
    if (report.unknown_ms)
        value_row(StatisticsUnknown, tr("日期未知"), "", statistics_duration(report.unknown_ms), 702, 86);
    else if (!report.total_ms)
        text(tr("尚无有效阅读记录"), 36, 724, 24, 612, 3);
    if (statistics_today >= 0 || report.last_day >= 0)
        value_row(StatisticsCalendar, tr("年度热力图"), "", "", 800, 104);
    else value_row(0, tr("年度热力图"), "", tr("日期未知"), 800, 104);
    value_row(StatisticsBooks, tr("统计书单"), "", "", 904, 104);
    value_row(StatisticsSettings, tr("统计设置"), "", "", 1008, 104);
    if (statistics_save_failed || statistics_collector.dropped_ms())
        footer_bar(tr("部分阅读时长未保存"));
    else if (!statistics_options.enabled)
        footer_bar(tr("已暂停采集，历史保留"));
    else footer_bar(tr("本机记录 · 时长为估计值"));
}

void ReaderApp::Impl::draw_statistics_calendar() {
    const bool annual = statistics_page == StatisticsPage::Year;
    const int first = annual ? first_day_of_year(statistics_year) : first_day_of_month(statistics_year, statistics_month);
    const auto heading = std::to_string(statistics_year) + (annual ? "" : " / " + two_digits(statistics_month));
    text(heading, 36, 130, 32, 612, 1, true);
    if (annual) {
        for (int month = 1; month <= 12; ++month) {
            const int x = 24 + (month - 1) % 3 * month_width;
            const int y = calendar_start + (month - 1) / 3 * month_height;
            const int start = first_day_of_month(statistics_year, month);
            if (start >= statistics_day_limit) continue;
            const int weekday = monday_column(start);
            text(two_digits(month) + " ›", x + 12, y, 28, month_width - 24, 1, true);
            for (int i = 0; i < month_length(statistics_year, month) && start + i < statistics_day_limit; ++i) {
                const int cell = weekday + i;
                statistics_heat_mark(x + 14 + cell % 7 * 24, y + 48 + cell / 7 * 22, 16,
                                     day_duration(statistics_report, first, start + i));
            }
            canvas.line(x + 12, y + month_height - 12, x + month_width - 12, y + month_height - 12, 12, 2);
            hits.push_back({x, y, month_width, month_height, StatisticsMonthBase + month});
        }
    } else {
        constexpr const char* weekdays[] = {"一", "二", "三", "四", "五", "六", "日"};
        for (int i = 0; i < 7; ++i) center(tr(weekdays[i]), month_grid_left + i * day_width, 242, day_width, 24, 3);
        const int weekday = monday_column(first);
        for (int i = 0; i < month_length(statistics_year, statistics_month) && first + i < statistics_day_limit; ++i) {
            const int cell = weekday + i, day_number = first + i;
            const int x = month_grid_left + cell % 7 * day_width, y = month_grid_top + cell / 7 * day_height;
            outline(x + 2, y + 2, day_width - 4, day_height - 4, day_number == statistics_today ? 3 : 1);
            center(std::to_string(i + 1), x, y + 7, day_width, 28, 1, day_number == statistics_today);
            statistics_heat_mark(x + day_width / 2 - 10, y + 60, 20, day_duration(statistics_report, first, day_number));
            hits.push_back({x, y, day_width, day_height, StatisticsDayBase + i});
        }
    }
    const int legend_top = annual ? 1000 : 944;
    text(tr("有效阅读时长（分钟）"), 36, legend_top, 24, 612, 3);
    constexpr const char* legend[] = {"0", "<15", "15–44", "45+"};
    constexpr std::uint64_t durations[] = {0, 1, 15 * minute_ms, 45 * minute_ms};
    for (int i = 0; i < 4; ++i) {
        const int x = 36 + 154 * i;
        statistics_heat_mark(x, legend_top + 55, 22, durations[i]);
        text(legend[i], x + 32, legend_top + 47, 24, 118);
    }
    const int earliest = statistics_report.first_day >= 0 ? statistics_report.first_day : statistics_today;
    const int latest = std::max<int>(statistics_report.last_day, statistics_today);
    const int first_year = calendar_date(std::min(earliest, statistics_today >= 0 ? statistics_today : earliest)).year;
    const int last_year = calendar_date(latest).year;
    const bool previous = statistics_year > first_year || (!annual && statistics_month > 1);
    const bool next = statistics_year < last_year || (!annual && statistics_month < 12 && first + month_length(statistics_year, statistics_month) < statistics_day_limit);
    if (previous || next) statistics_footer(heading, previous, next);
}

void ReaderApp::Impl::draw_statistics_books() {
    const auto& report = statistics_report;
    if (statistics_page == StatisticsPage::Book) {
        title_lines(library_text_font(), statistics_book_title, 36, 144, 612, 32, true, 2);
        text(library_text_font(), statistics_book_path, 36, 248, 24, 612, 3);
        value_row(0, tr("累计时长"), "", statistics_duration(report.total_ms), 316);
        value_row(0, tr("有效阅读天数"), "", std::to_string(report.days), 420);
        value_row(0, tr("首次记录"), "", date_label(report.first_day), 524);
        value_row(0, tr("最近阅读"), "", date_label(report.last_day), 628);
        const auto* record = report.books.empty() ? nullptr : &report.books.front();
        int progress = record ? record->progress_percent : -1;
        if (has_book() && statistics_book_path == book_path && session.visible_record() && page.end > page.start) {
            const auto& current_id = document.metadata().book_id;
            const bool same_copy = statistics_from_details ? detail_metadata.book_id == current_id
                                                           : record && record->content_id == current_id;
            if (same_copy) progress = book_percent_value(page.start, page.end);
        }
        std::string position = progress < 0 ? "—" : std::to_string(progress) + "%";
        if (progress < 0 && statistics_from_details && detail_progress.record) {
            position = detail_position == tr("已保存阅读位置") ?
                tr("已保存位置") : detail_position;
        }
        value_row(0, tr("当前进度"), "", position, 732);
        if (report.total_ms) {
            if (report.unknown_ms)
                value_row(0, tr("日期未知"), "", statistics_duration(report.unknown_ms), 836, 88);
            if (statistics_resolved_id) danger_button(StatisticsClear, tr("清除本书统计"), 36, 984, 612);
        } else text(tr("本书暂无有效阅读记录"), 36, 882, 24, 612, 3);
        footer_bar(tr("本机记录 · 时长为估计值"));
        return;
    }
    const bool daily = statistics_page == StatisticsPage::Day;
    const auto heading = daily ? statistics_day < 0 ? tr("日期未知") : date_label(statistics_day)
                               : trf("{count} 本书", {{"count", std::to_string(report.matching_books)}});
    text(heading, 36, 138, 32, 612, 1, true);
    if (daily) {
        const auto day_ms = statistics_day < 0 ? report.unknown_ms : day_duration(report, statistics_day, statistics_day);
        text(statistics_duration(day_ms), 36, 200, 28, 612);
    }
    const int top = daily ? 288 : 228;
    if (report.books.empty())
        text(tr("暂无有效阅读记录"), 36, top + 40, 28, 612, 3);
    for (unsigned i = 0; i < report.books.size(); ++i) {
        const auto& book = report.books[i];
        const int y = top + static_cast<int>(i) * 112;
        const auto time = statistics_duration(daily ? book.selected_day_ms : book.total_ms);
        const int time_width = std::min(270, text_width(time, 24));
        text(library_text_font(), book.title.empty() ? basename(book.path) : book.title, 36, y + 9, 28, 566 - time_width, 1, true);
        text(time, 612 - time_width, y + 12, 24, time_width);
        text("›", 634, y + 9, 28, 28);
        text(library_text_font(), book.path, 36, y + 58, 24, 612, 3);
        canvas.line(36, y + 109, 647, y + 109, 12, 2);
        hits.push_back({24, y, 636, 112, StatisticsBookBase + static_cast<int>(i)});
    }
    const auto pages = (report.matching_books + books_per_statistics_page - 1) / books_per_statistics_page;
    if (pages > 1)
        statistics_footer(std::to_string(statistics_list_page + 1) + " / " + std::to_string(pages), statistics_list_page > 0, statistics_list_page + 1 < pages);
}

void ReaderApp::Impl::draw_statistics_settings() {
    value_row(StatisticsToggle, tr("采集阅读时长"),
              tr("关闭后保留历史"),
              statistics_draft.enabled ? tr("开") : tr("关"), 122, 104, true, false);
    const auto stepper = [&](const char* title, int y, unsigned value, int down, bool can_down, bool can_up) {
        text(title, 36, y + 29, 28, 318);
        if (can_down) button(down, "−", 364, y, 88, 88);
        else disabled_button("−", 364, y, 88);
        center(std::to_string(value), 454, y + 25, 104, 28, 1, true);
        if (can_up) button(down + 1, "+", 560, y, 88, 88);
        else disabled_button("+", 560, y, 88);
    };
    stepper(tr("最短停留（秒）"), 378, statistics_draft.minimum_seconds, StatisticsMinDown,
            statistics_draft.minimum_seconds > 1, statistics_draft.minimum_seconds < std::min(60u, statistics_draft.maximum_seconds));
    stepper(tr("单次上限（秒）"), 492, statistics_draft.maximum_seconds, StatisticsMaxDown,
            statistics_draft.maximum_seconds > std::max(5u, statistics_draft.minimum_seconds), statistics_draft.maximum_seconds < 600);
    wrapped(tr("同一页的有效停留达到最短时长后，按实际时长计入，到达上限后停止。"), 36, 634, 612, 24, true, 3);
    const bool changed = statistics_draft.enabled != statistics_options.enabled ||
        statistics_draft.minimum_seconds != statistics_options.minimum_seconds || statistics_draft.maximum_seconds != statistics_options.maximum_seconds;
    if (changed) button(StatisticsApply, tr("保存设置"), 36, 804, 612, 96, true);
    else disabled_button(tr("已保存"), 36, 804, 612, 96);
    danger_button(StatisticsClear, tr("清除全部统计"), 36, 988, 612);
}

void ReaderApp::Impl::draw_statistics() {
    hits.clear();
    canvas.clear();
    if (statistics_loading && statistics_mutating()) {
        header(statistics_entry(false), 0);
        text(tr("正在保存…"), 36, 172, 28, 612);
        return;
    }
    const auto* overview = tr("总览");
    const auto* library = tr("书库");
    const auto* books = tr("书单");
    const auto* settings_title = tr("统计设置");
    const auto* day = tr("日期详情");
    const auto* year = tr("年度热力图");
    const auto* month = tr("月历");
    const auto* confirm = tr("确认清除");
    switch (statistics_page) {
    case StatisticsPage::Overview: header(statistics_entry(false), Back, library); break;
    case StatisticsPage::Year: header(year, Back, overview); break;
    case StatisticsPage::Month: header(month, Back, year); break;
    case StatisticsPage::Day: header(day, Back, statistics_day < 0 ? overview : month); break;
    case StatisticsPage::Books: header(tr("统计书单"), Back, overview); break;
    case StatisticsPage::Book: header(statistics_entry(true), Back, statistics_from_details ? tr("详情") :
                                     statistics_book_parent == StatisticsPage::Day ? day : books); break;
    case StatisticsPage::Settings: header(settings_title, Back, overview); break;
    case StatisticsPage::ClearBook: header(confirm, Back, statistics_entry(true)); break;
    case StatisticsPage::ClearAll: header(confirm, Back, settings_title); break;
    }
    if (statistics_loading) {
        text(tr("正在处理统计…"), 36, 172, 28, 612);
        return;
    }
    if (statistics_status != StoreStatus::Ok && statistics_status != StoreStatus::Missing && statistics_status != StoreStatus::Cleared) {
        outline(36, 166, 612, 212, 3);
        canvas.fill_rect(60, 194, 40, 40, 1);
        center("!", 60, 194, 40, 28, 15, true);
        text(tr("统计操作未完成"), 118, 194, 28, 500, 1, true);
        wrapped(tr("请检查 TF 卡，再重新读取统计。"), 60, 268, 560, 24, true, 3);
        button(StatisticsRetry, tr("重新读取"), 36, 428, 612);
        if (statistics_page == StatisticsPage::Overview)
            value_row(StatisticsSettings, tr("统计设置"), "", "", 552);
        return;
    }
    switch (statistics_page) {
    case StatisticsPage::Overview: draw_statistics_overview(); break;
    case StatisticsPage::Year:
    case StatisticsPage::Month: draw_statistics_calendar(); break;
    case StatisticsPage::Day:
    case StatisticsPage::Books:
    case StatisticsPage::Book: draw_statistics_books(); break;
    case StatisticsPage::Settings: draw_statistics_settings(); break;
    case StatisticsPage::ClearBook:
    case StatisticsPage::ClearAll: {
        const bool all = statistics_page == StatisticsPage::ClearAll;
        text(all ? tr("清除全部阅读统计？") :
                   tr("清除本书阅读统计？"), 36, 164, 32, 612, 1, true);
        if (!all) {
            title_lines(library_text_font(), statistics_book_title, 36, 256, 612, 28, true);
            text(library_text_font(), statistics_book_path, 36, 360, 24, 612, 3);
        }
        wrapped(tr("清除后无法恢复。原书、阅读进度和书签将保留。"), 36, all ? 264 : 446, 612, 24);
        button(Back, tr("取消"), 24, 1000, 306, 96, true);
        danger_button(StatisticsConfirm, all ? tr("清除全部统计") :
                                               tr("清除本书统计"), 354, 1000, 306);
        break;
    }
    }
}

void ReaderApp::Impl::statistics_back() {
    if (statistics_mutating()) return;
    cancel_statistics_query();
    switch (statistics_page) {
    case StatisticsPage::Overview:
        draw_library(); overlay(); return;
    case StatisticsPage::Year:
    case StatisticsPage::Books:
    case StatisticsPage::Settings:
        statistics_page = StatisticsPage::Overview; break;
    case StatisticsPage::Month:
        statistics_page = StatisticsPage::Year; break;
    case StatisticsPage::Day:
        statistics_page = statistics_day < 0 ? StatisticsPage::Overview : StatisticsPage::Month; break;
    case StatisticsPage::Book:
        if (statistics_from_details) { draw_details(); overlay(); return; }
        statistics_page = statistics_book_parent; break;
    case StatisticsPage::ClearBook:
        statistics_page = StatisticsPage::Book; break;
    case StatisticsPage::ClearAll:
        statistics_page = StatisticsPage::Settings;
        draw_statistics(); overlay(); return;
    }
    load_statistics_page();
}

void ReaderApp::Impl::statistics_control(int id) {
    if (id == Back) { statistics_back(); return; }
    if (statistics_loading) return;
    if (id == StatisticsSettings && statistics_page == StatisticsPage::Overview) {
        statistics_page = StatisticsPage::Settings;
        load_statistics_page(); return;
    }
    if (id == StatisticsRetry) { load_statistics_page(); return; }
    if (statistics_status != StoreStatus::Ok && statistics_status != StoreStatus::Missing && statistics_status != StoreStatus::Cleared) return;
    if (id == Prev || id == Next) {
        const int delta = id == Prev ? -1 : 1;
        if (statistics_page == StatisticsPage::Year || statistics_page == StatisticsPage::Month) {
            if (std::none_of(hits.begin(), hits.end(), [id](const Hit& hit) { return hit.id == id; })) return;
            if (statistics_page == StatisticsPage::Year) statistics_year += delta;
            else {
                statistics_month += delta;
                if (statistics_month < 1) { statistics_month = 12; --statistics_year; }
                if (statistics_month > 12) { statistics_month = 1; ++statistics_year; }
            }
        } else if (statistics_page == StatisticsPage::Day || statistics_page == StatisticsPage::Books) {
            if (id == Prev && statistics_list_page > 0) --statistics_list_page;
            else if (id == Next && (statistics_list_page + 1) * books_per_statistics_page < statistics_report.matching_books) ++statistics_list_page;
            else return;
        } else return;
        load_statistics_page();
        return;
    }
    if (statistics_page == StatisticsPage::Year && id > StatisticsMonthBase && id <= StatisticsMonthBase + 12) {
        statistics_month = id - StatisticsMonthBase;
        statistics_page = StatisticsPage::Month;
    } else if (statistics_page == StatisticsPage::Month && id >= StatisticsDayBase && id < StatisticsDayBase + month_length(statistics_year, statistics_month)) {
        statistics_day = first_day_of_month(statistics_year, statistics_month) + id - StatisticsDayBase;
        statistics_page = StatisticsPage::Day;
        statistics_list_page = 0;
    } else if ((statistics_page == StatisticsPage::Books || statistics_page == StatisticsPage::Day) && id >= StatisticsBookBase) {
        const auto index = static_cast<unsigned>(id - StatisticsBookBase);
        if (index >= statistics_report.books.size()) return;
        const auto& book = statistics_report.books[index];
        statistics_book_title = book.title.empty() ? basename(book.path) : book.title;
        statistics_book_path = book.path;
        statistics_book_id = book.id;
        statistics_book_parent = statistics_page;
        statistics_page = StatisticsPage::Book;
    } else if (statistics_page == StatisticsPage::Overview) {
        switch (id) {
        case StatisticsCalendar: {
            const int day_number = statistics_today >= 0 ? statistics_today : statistics_report.last_day;
            if (day_number < 0) return;
            statistics_year = calendar_date(day_number).year;
            statistics_page = StatisticsPage::Year;
            break;
        }
        case StatisticsUnknown:
            statistics_day = -1; statistics_list_page = 0; statistics_page = StatisticsPage::Day; break;
        case StatisticsBooks:
            statistics_list_page = 0; statistics_page = StatisticsPage::Books; break;
        default: return;
        }
    } else if (id == StatisticsClear) {
        if (statistics_page == StatisticsPage::Settings) statistics_page = StatisticsPage::ClearAll;
        else if (statistics_page == StatisticsPage::Book && statistics_resolved_id) statistics_page = StatisticsPage::ClearBook;
        else return;
        draw_statistics(); overlay(); return;
    } else if (id == StatisticsConfirm) {
        const bool all = statistics_page == StatisticsPage::ClearAll;
        if (!all && (statistics_page != StatisticsPage::ClearBook || !statistics_resolved_id)) return;
        statistics_page = all ? StatisticsPage::Settings : StatisticsPage::Book;
        clear_statistics(all ? 0 : statistics_resolved_id);
        draw_statistics(); overlay(); return;
    } else if (statistics_page == StatisticsPage::Settings) {
        switch (id) {
        case StatisticsToggle: statistics_draft.enabled = !statistics_draft.enabled; break;
        case StatisticsMinDown: if (statistics_draft.minimum_seconds > 1) --statistics_draft.minimum_seconds; break;
        case StatisticsMinUp:
            if (statistics_draft.minimum_seconds < std::min(60u, statistics_draft.maximum_seconds)) ++statistics_draft.minimum_seconds;
            break;
        case StatisticsMaxDown: {
            const auto minimum = std::max(5u, statistics_draft.minimum_seconds);
            statistics_draft.maximum_seconds = std::max(minimum, statistics_draft.maximum_seconds - 5);
            break;
        }
        case StatisticsMaxUp: statistics_draft.maximum_seconds = std::min(600u, statistics_draft.maximum_seconds + 5); break;
        case StatisticsApply: save_statistics_options(statistics_draft); break;
        default: return;
        }
        draw_statistics(); overlay(); return;
    } else return;
    load_statistics_page();
}
}
