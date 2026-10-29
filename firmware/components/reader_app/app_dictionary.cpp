#include "app_impl.h"
#include "third_party/phosphor/dictionary_icons.h"

namespace pico_page {
namespace {
// Minimum panel height. The lower panel covers the reading footer. When neither side of the selection
// has this height, fixed panels leave a 96-pixel band that exceeds the largest configured body line
// (56 px x 1.65), so one side still leaves a single selected line visible.
constexpr int kDictionaryPanelHeight = 560;
constexpr int kDictionaryLowerTop = 1216 - kDictionaryPanelHeight;
// Handles extend 12 px beyond the selection and keep a 22-pixel hit radius.
constexpr int kDictionarySelectionClearance = 36;
// Lookups still running after this delay are likely preparing a dictionary on first use.
constexpr auto kDictionarySlowDelay = std::chrono::seconds(3);
// Panel rows: 72-pixel header, status line, definition, then a range, source and page row.
// In the lower and full-screen panels the 88-pixel page row coincides with the reading footer,
// so the sleep notice, which saves and redraws y >= footer_top, replaces exactly that row.
constexpr int kDictionaryHeaderHeight = 72, kDictionaryDefinitionTop = 108;
constexpr int kDictionaryRowHeight = 72, kDictionaryTableHeight = 2 * kDictionaryRowHeight + 88;
constexpr std::size_t kDictionarySourcesPerPage = 5;
// Marks an enabled dictionary that the latest scan no longer found.
constexpr const char* kMissingDictionary = "Missing dictionary";
std::string dictionary_error(const dictionary::Result& status) {
    switch (status.status) {
    case dictionary::Status::Ok: return {};
    case dictionary::Status::NotFound: return "未找到词条，可调整选词或切换词典。";
    case dictionary::Status::Cancelled: return "查询已取消，可重试。";
    case dictionary::Status::IoError: return "无法使用词典，请检查文件和 TF 卡后重试。";
    case dictionary::Status::InvalidFormat: return "词典数据或格式无效。";
    case dictionary::Status::Unsupported: return "暂不支持此词典格式。";
    case dictionary::Status::ResourceLimit: return "词典内容超出资源限制或可用内存不足。";
    }
    return "词典暂不可用。";
}
}  // namespace

void ReaderApp::Impl::queue_dictionary(ContentJob::Request request) {
    abandon_content(); clear_prefetch(); invalidate_counting();
    if (counting_job) counting_job->cancel();
    if (!dictionary_session) dictionary_session = std::make_shared<DictionarySession>();
    request.root = root; request.generation = foreground_generation;
    request.chapter = chapter_index; request.part = chapter.part; request.offset = page.start;
    request.dictionary_session = dictionary_session;
    dictionary_slow = false;
    dictionary_slow_due = request.kind == ContentJob::Kind::DictionaryLookup ?
        std::chrono::steady_clock::now() + kDictionarySlowDelay : std::chrono::steady_clock::time_point{};
    pending_content = std::move(request); content_loading = true;
}

void ReaderApp::Impl::open_dictionary_settings(bool from_lookup) {
    dictionary_settings_from_lookup = from_lookup;
    dictionary_settings_changed = false;
    dictionary_settings_source = dictionary_source < settings.dictionary_enabled.size() ? settings.dictionary_enabled[dictionary_source] : "";
    if (from_lookup && !dictionary_choosing) dictionary_return_notice = dictionary_notice;
    dictionary_choosing = false;
    abandon_content(); dictionary_session.reset();
    edit_snapshot = store.settings_snapshot();
    settings.dictionary_enabled = edit_snapshot.global_settings.dictionary_enabled;
    dictionary_notice.clear(); dictionary_items.clear(); dictionary_list_page = 0;
    view = View::DictionarySettings;
    ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryList;
    queue_dictionary(std::move(request)); draw_dictionary_settings(); overlay();
}

void ReaderApp::Impl::draw_dictionary_settings() {
    view = View::DictionarySettings; canvas.clear(); hits.clear(); header("离线词典", Back, dictionary_settings_from_lookup ? "查词" : "设置");
    text("复制词典目录到 TF 卡 Dictionaries/", 36, 116, 23, 612, 3);
    const auto screens = std::max<std::size_t>(1, (dictionary_items.size() + 3) / 4);
    dictionary_list_page = std::min(dictionary_list_page, screens - 1);
    for (std::size_t row = 0; row < 4; ++row) {
        const auto index = dictionary_list_page * 4 + row;
        if (index >= dictionary_items.size()) break;
        const auto& item = dictionary_items[index];
        const auto found = std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), item.id);
        const bool enabled = found != settings.dictionary_enabled.end();
        const int y = 164 + static_cast<int>(row) * 184;
        title_lines(item.info.name.empty() ? basename(item.id) : item.info.name, 36, y, 450, 26, true, 2);
        const auto label = item.status.message == kMissingDictionary ? "找不到这套词典的文件" :
                           !item.status ? dictionary_error(item.status) : enabled ? "已启用" : "未启用";
        text(label, 36, y + 62, 22, 612, 3);
        if (!content_loading) button(DictionaryToggleBase + static_cast<int>(index), enabled ? "停用" : "启用", 516, y, 144, 60, false, 23);
        if (!content_loading && enabled) {
            const auto order = static_cast<std::size_t>(found - settings.dictionary_enabled.begin());
            if (order) button(DictionaryUpBase + static_cast<int>(index), "上移", 24, y + 100, 198, 60, false, 23);
            if (order + 1 < settings.dictionary_enabled.size()) button(DictionaryDownBase + static_cast<int>(index), "下移", 234, y + 100, 198, 60, false, 23);
        }
        canvas.line(36, y + 176, 648, y + 176, 12, 2);
    }
    // A failed scan reports its cause below instead of claiming that no dictionary exists.
    if (dictionary_items.empty() && !content_loading)
        wrapped(dictionary_notice.empty() ? "没有找到词典。请复制含 .ifo、.idx 和 .dict 或 .dict.dz 的词典文件夹，再重新扫描。"
                                          : "无法读取词典目录，请重新扫描。", 36, 220, 612, 26);
    if (!content_loading && dictionary_list_page) button(Prev, "上一页", 24, 928, 198, 68, false, 24);
    center(std::to_string(dictionary_list_page + 1) + " / " + std::to_string(screens), 222, 950, 240, 24);
    if (!content_loading && dictionary_list_page + 1 < screens) button(Next, "下一页", 462, 928, 198, 68, false, 24);
    const auto status = content_loading ? "正在查找词典，可返回取消…" : !dictionary_notice.empty() ? dictionary_notice :
        !dictionary_list_complete ? "已达到扫描上限，部分目录未扫描。" : "已启用词典按列表顺序查询。";
    title_lines(status, 36, 1010, 612, 23, false, 2);
    button(DictionaryScan, "重新扫描", 24, 1104, 306, 80, false, 26);
    button(Back, dictionary_settings_from_lookup ? "返回查词" : "返回设置", 354, 1104, 306, 80, false, 26);
}

std::pair<int, int> ReaderApp::Impl::dictionary_bounds(bool full) const {
    if (full) return {0, 1216};
    int upper_overlap = 0, lower_overlap = 0;
    int selection_top = 1216, selection_bottom = 0;
    for (const auto& line : page.lines) {
        if (line.end <= dictionary_selection.begin || line.start >= dictionary_selection.end) continue;
        selection_top = std::min(selection_top, line.top); selection_bottom = std::max(selection_bottom, line.bottom);
        upper_overlap += std::max(0, std::min(line.bottom, kDictionaryPanelHeight) - std::max(line.top, 0));
        lower_overlap += std::max(0, std::min(line.bottom, kDictionaryLowerTop + kDictionaryPanelHeight) - std::max(line.top, kDictionaryLowerTop));
    }
    // Prefer the larger side that leaves every selected line and both handle hit areas visible.
    // An edge that would cut a body line moves past it so the line stays whole.
    int upper_bottom = selection_top - kDictionarySelectionClearance;
    int lower_top = selection_bottom + kDictionarySelectionClearance;
    for (const auto& line : page.lines) {
        if (line.top < upper_bottom && line.bottom > upper_bottom) upper_bottom = line.top;
        if (line.top < lower_top && line.bottom > lower_top) lower_top = line.bottom;
    }
    if (std::max(upper_bottom, 1216 - lower_top) >= kDictionaryPanelHeight) {
        const int lower_height = 1216 - lower_top;
        if (upper_bottom != lower_height) return upper_bottom > lower_height ? std::pair{0, upper_bottom} : std::pair{lower_top, 1216};
        return dictionary_touch_y < 600 ? std::pair{lower_top, 1216} : std::pair{0, upper_bottom};
    }
    const int top = upper_overlap != lower_overlap ? (upper_overlap < lower_overlap ? 0 : kDictionaryLowerTop) :
                    dictionary_touch_y < 600 ? kDictionaryLowerTop : 0;
    return {top, top + kDictionaryPanelHeight};
}

graphics::LayoutSpec ReaderApp::Impl::dictionary_layout(bool full) const {
    graphics::LayoutSpec spec;
    const auto [top, bottom] = dictionary_bounds(full);
    const int height = bottom - top;
    const int entry_height = dictionary_entry_base || dictionary_entries.size() > 1 || dictionary_more_entries ? 48 : 0;
    spec.left = 36; spec.top = top + kDictionaryDefinitionTop + entry_height; spec.width = 612;
    spec.height = height - kDictionaryDefinitionTop - entry_height - kDictionaryTableHeight;
    spec.font_px = 28; spec.line_height = 42; spec.indent_chars = 0;
    return spec;
}

void ReaderApp::Impl::reset_dictionary_contact() {
    if (dictionary_contact) router.accept({dictionary_contact, Phase::Cancel, Input::TouchCenter});
    dictionary_contact = 0;
    dictionary_selection_dirty = dictionary_query_pending = dictionary_close_pending = false;
    dictionary_selecting = false;
}

bool ReaderApp::Impl::dictionary_contact_input(const AppInput& input, bool accepted) {
    if (!input.continuous || !input.touch) return false;
    if (input.event.phase == Phase::Begin) {
        if (!accepted || input.queued || display.busy() || sleep_wait_footer) return true;
        if (view == View::Reading && input.long_press && !image_viewing && !image_at(input.x, input.y)) {
            begin_dictionary(input.x, input.y, true);
            if (view != View::Dictionary) return true;
            dictionary_contact_anchor = dictionary_selection;
            dictionary_drag_dx = dictionary_drag_dy = 0;
        } else if (view == View::Dictionary) {
            const auto hit = std::find_if(hits.begin(), hits.end(), [&](const auto& value) {
                return (value.id == DictionaryStart || value.id == DictionaryEnd) && value.contains(input.x, input.y);
            });
            if (hit == hits.end()) return true;
            const auto boxes = graphics::selection_boxes(page, dictionary_selection.begin, dictionary_selection.end);
            if (boxes.empty()) return true;
            const auto& box = hit->id == DictionaryStart ? boxes.front() : boxes.back();
            // Preserve the pointer-to-text offset when grabbing a handle outside the glyphs.
            dictionary_drag_dx = (hit->id == DictionaryStart ? box.x : box.x + box.width - 1) - input.x;
            dictionary_drag_dy = box.y + box.height - 1 - input.y;
            const auto anchor = hit->id == DictionaryStart ? dictionary_selection.end - 1 : dictionary_selection.begin;
            dictionary_contact_anchor = {anchor, anchor + 1};
            abandon_content(); dictionary_selecting = true; dictionary_manual = true;
            dictionary_notice.clear(); dictionary_selection_dirty = true;
        } else return false;
        dictionary_contact = input.event.sequence;
        flush_dictionary_selection();
        return true;
    }
    if (!dictionary_contact || dictionary_contact != input.event.sequence) return true;
    if (view != View::Dictionary) { reset_dictionary_contact(); return true; }
    if (input.event.phase == Phase::Move || input.event.phase == Phase::End) {
        if (input.x >= 0 && input.x < 684 && input.y >= 0 && input.y < footer_top) {
            const auto hit = graphics::text_hit(page, input.x + dictionary_drag_dx, input.y + dictionary_drag_dy, true);
            if (hit) {
                const dictionary::Selection selection{std::min(dictionary_contact_anchor.begin, *hit),
                                                       std::max(dictionary_contact_anchor.end, *hit + 1)};
                if (selection.begin != dictionary_selection.begin || selection.end != dictionary_selection.end) {
                    dictionary_selection = selection;
                    dictionary_manual = true; dictionary_selection_dirty = true;
                }
            }
        }
        if (input.event.phase == Phase::End) { dictionary_contact = 0; dictionary_query_pending = true; }
    } else if (input.event.phase == Phase::Cancel) {
        dictionary_contact = 0; dictionary_close_pending = true;
    }
    // System cancellation defers drawing until the power/card transition has taken ownership.
    if (!input.queued) flush_dictionary_selection();
    return true;
}

void ReaderApp::Impl::flush_dictionary_selection() {
    if (view != View::Dictionary || display.busy() || sleep_wait_footer) return;
    if (dictionary_close_pending) { close_dictionary(); return; }
    if (std::exchange(dictionary_query_pending, false)) { submit_dictionary_selection(); return; }
    if (std::exchange(dictionary_selection_dirty, false)) { draw_dictionary(); overlay(); }
}

void ReaderApp::Impl::submit_dictionary_selection() {
    const auto status = dictionary::selection_candidates(chapter.text, {page.start, page.end}, dictionary_selection,
                                                         dictionary_trigger, dictionary_manual, dictionary_candidates);
    dictionary_selection_dirty = false;
    if (!status) {
        dictionary_notice = status.status == dictionary::Status::ResourceLimit ? "选词过长，请拖动端点缩短选区。" : dictionary_error(status);
        dictionary_selecting = true; draw_dictionary(); overlay(); return;
    }
    const bool previewed = std::exchange(dictionary_selecting, false);
    // Exact forms precede inflections; longer Chinese candidates precede shorter ones.
    std::stable_sort(dictionary_candidates.begin(), dictionary_candidates.end(), [](const auto& a, const auto& b) {
        if (a.inflection != b.inflection) return !a.inflection;
        return a.selection.end - a.selection.begin > b.selection.end - b.selection.begin;
    });
    dictionary_source = 0; dictionary_search_all = true;
    query_dictionary(false, previewed);
}

void ReaderApp::Impl::begin_dictionary(int x, int y, bool selecting) {
    const auto hit = graphics::text_hit(page, x, y);
    if (!hit || !body_canvas) {
        message = "当前可用内存不足，暂时无法查词。"; footer(); overlay(); return;
    }
    auto status = dictionary::initial_selection(chapter.text, {page.start, page.end}, *hit, dictionary_selection);
    if (!status) { message = dictionary_error(status); footer(); overlay(); return; }
    reset_dictionary_contact(); abandon_content();
    dictionary_trigger = *hit; dictionary_manual = false; dictionary_selecting = selecting;
    dictionary_touch_y = y; dictionary_full = false; dictionary_source = 0;
    dictionary_choosing = dictionary_settings_from_lookup = false;
    dictionary_search_all = true;
    dictionary_page = {}; dictionary_definition.reset(); dictionary_entries.clear(); dictionary_pages.clear();
    dictionary_headword.clear(); dictionary_name.clear(); dictionary_notice.clear();
    view = View::Dictionary;
    if (selecting) { draw_dictionary(); overlay(); }
    else submit_dictionary_selection();
}

void ReaderApp::Impl::query_dictionary(bool retry, bool defer_panel) {
    dictionary_retry_direction = 0;
    dictionary_previous_exhausted = dictionary_next_exhausted = false;
    if (retry) {
        dictionary_failed_sources.erase(std::remove_if(dictionary_failed_sources.begin(), dictionary_failed_sources.end(),
            [&](const auto& item) {
                return dictionary_search_all || (dictionary_source < settings.dictionary_enabled.size() &&
                       item.id == settings.dictionary_enabled[dictionary_source]);
            }), dictionary_failed_sources.end());
    }
    dictionary_definition.reset(); dictionary_page = {}; dictionary_pages.clear(); dictionary_entry = 0; dictionary_entry_base = 0;
    dictionary_entries.clear(); dictionary_headword.clear(); dictionary_name.clear();
    dictionary_missing_glyphs = dictionary_omitted_fields = false;
    dictionary_page_number = 0; dictionary_layout_anchor = 0; dictionary_next = {}; dictionary_notice.clear();
    if (settings.dictionary_enabled.empty()) {
        abandon_content(); dictionary_notice = "尚未启用词典。请先设置词典，再继续查词。";
        draw_dictionary(); overlay(); return;
    }
    dictionary_source %= settings.dictionary_enabled.size();
    ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryLookup;
    if (dictionary_search_all) { dictionary_source = 0; request.dictionary_sources = settings.dictionary_enabled; }
    else request.relative_path = settings.dictionary_enabled[dictionary_source];
    request.dictionary_candidates = dictionary_candidates; request.dictionary_retry = retry;
    if (request.dictionary_candidates.empty()) { dictionary_notice = "未找到词条。"; draw_dictionary(); overlay(); return; }
    queue_dictionary(std::move(request));
    if (defer_panel) dictionary_panel_due = std::chrono::steady_clock::now() + dictionary_panel_delay;
    else { draw_dictionary(); overlay(); }
}

void ReaderApp::Impl::poll_dictionary_panel() {
    if (dictionary_slow_due != std::chrono::steady_clock::time_point{}) {
        if (view != View::Dictionary || !content_loading || dictionary_choosing) dictionary_slow_due = {};
        else if (!dictionary_selecting && !display.busy() && std::chrono::steady_clock::now() >= dictionary_slow_due) {
            dictionary_slow_due = dictionary_panel_due = {}; dictionary_slow = true;
            draw_dictionary(); overlay(); return;
        }
    }
    if (dictionary_panel_due == std::chrono::steady_clock::time_point{}) return;
    if (view != View::Dictionary || !content_loading || dictionary_selecting) { dictionary_panel_due = {}; return; }
    if (display.busy() || std::chrono::steady_clock::now() < dictionary_panel_due) return;
    dictionary_panel_due = {}; draw_dictionary(); overlay();
}

void ReaderApp::Impl::page_dictionary(int movement, bool redraw) {
    if (!dictionary_definition || dictionary_pages.empty()) return;
    dictionary_retry_direction = 0;
    ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryPage;
    request.dictionary_definition = dictionary_definition;
    request.dictionary_cursor = dictionary_pages.back(); request.layout = dictionary_layout(dictionary_full);
    request.dictionary_layout_anchor = dictionary_layout_anchor;
    if (movement > 0) request.dictionary_cursor = dictionary_next;
    else if (movement < 0) {
        if (dictionary_pages.size() > 1) request.dictionary_cursor = dictionary_pages[dictionary_pages.size() - 2];
        else {
            request.dictionary_cursor = {}; request.dictionary_replay = true;
            request.dictionary_seek_pages = dictionary_page_number - 1;
        }
    }
    request.font = builtin_body ? std::string() : root + "/" + effective_font;
    dictionary_page_move = movement; dictionary_notice.clear();
    queue_dictionary(std::move(request));
    if (redraw) { draw_dictionary(); overlay(); }
}

void ReaderApp::Impl::draw_dictionary() {
    if (!body_canvas) return;
    if (dictionary_choosing) { draw_dictionary_sources(); return; }
    view = View::Dictionary; canvas = *body_canvas; hits.clear();
    const auto boxes = graphics::selection_boxes(page, dictionary_selection.begin, dictionary_selection.end);
    for (const auto& box : boxes)
        for (int y = box.y; y < box.y + box.height; ++y)
            for (int x = box.x; x < box.x + box.width; ++x) canvas.pixel(x, y, 15 - canvas.pixel(x, y));
    const auto bounds = dictionary_bounds(dictionary_full);
    const int top = bounds.first, bottom = bounds.second, height = bottom - top;
    if (!boxes.empty()) {
        const auto handle = [&](const graphics::SelectionBox& box, bool start) {
            const int x = std::clamp(start ? box.x : box.x + box.width, 22, 662);
            const int y = std::clamp(start ? box.y - 12 : box.y + box.height + 12, 22, footer_top - 24);
            if (!dictionary_selecting && y + 22 >= top && y - 22 < bottom) return;
            canvas.line(x, start ? box.y + 2 : box.y + box.height - 2, x, y, 0, 2);
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx)
                    if (dx * dx + dy * dy <= 64) canvas.pixel(x + dx, y + dy, 0);
            hits.push_back({x - 22, y - 22, 44, 44, start ? DictionaryStart : DictionaryEnd});
        };
        handle(boxes.front(), true); handle(boxes.back(), false);
    }
    if (dictionary_selecting) {
        canvas.fill_rect(0, footer_top, 684, 1216 - footer_top, 15);
        text(dictionary_notice.empty() ? "拖动端点，松手查词" : dictionary_notice, 24, footer_top + 8, 21, 636, 3);
        if (!dictionary_contact) button(DictionaryRetry, "查词", 24, footer_top + 40, 306, 44, false, 23);
        button(DictionaryClose, "取消", 354, footer_top + 40, 306, 44, false, 23);
        return;
    }
    canvas.fill_rect(0, top, 684, height, 15); canvas.line(0, top, 683, top, 1, 2);
    const auto selected = snippet(std::u32string_view(chapter.text).substr(dictionary_selection.begin, dictionary_selection.end - dictionary_selection.begin), 64);
    const auto heading = dictionary_headword.empty() ? selected : dictionary_headword;
    const auto icon = [&](int id, const std::uint8_t* pixels, int cx, int cy, int width, int height, bool enabled = true) {
        for (int y = 0; y < dictionary_icons::kSize; ++y)
            for (int x = 0; x < dictionary_icons::kSize; ++x) {
                const int offset = y * dictionary_icons::kSize + x;
                const int alpha = (pixels[offset / 2] >> ((offset % 2) * 4)) & 15;
                if (alpha) canvas.pixel(cx - dictionary_icons::kSize / 2 + x,
                                        cy - dictionary_icons::kSize / 2 + y,
                                        15 - alpha * (enabled ? 14 : 7) / 15);
            }
        if (enabled && id) hits.push_back({cx - width / 2, cy - height / 2, width, height, id});
    };
    const bool page_ready = dictionary_definition && !content_loading;
    const bool paged = page_ready && dictionary_notice.empty();
    const auto page_label = "第 " + std::to_string(dictionary_page_number + 1) + " 页";
    const int page_width = paged ? text_width(page_label, 24) : 0;
    if (paged) text(page_label, 572 - page_width, top_for_middle(ui, 24, top + 36), 24, page_width, 3);
    text(body, heading, 24, top_for_middle(body, 30, top + 36), 30, (paged ? 556 - page_width : 572) - 24, 1, true);
    icon(DictionaryClose, dictionary_icons::kClose, 636, top + 36, 96, 72);
    canvas.line(0, top + kDictionaryHeaderHeight, 683, top + kDictionaryHeaderHeight, 1, 2);
    const auto heading_scalars = decode_utf8(heading);
    const bool missing_glyphs = dictionary_missing_glyphs || std::any_of(heading_scalars.begin(), heading_scalars.end(),
        [&](char32_t scalar) { return body.missing(scalar); });
    const auto warning = !dictionary_notice.empty() && !dictionary_page.glyphs.empty() ? dictionary_notice :
        dictionary_omitted_fields ? (missing_glyphs ? "部分内容无法显示，且有缺字。" : "部分内容无法显示。") :
        missing_glyphs ? "部分字符缺字，请更换支持音标和生僻字的字体。" :
        dictionary_next.degraded && dictionary_definition ? "部分格式已简化显示。" : "";
    if (warning.empty() && heading != selected) text(body, selected + " → " + heading, 36, top + 80, 20, 612, 3, false);
    else text(warning, 36, top + 80, 20, 612, 3);
    if (dictionary_entry_base || dictionary_entries.size() > 1 || dictionary_more_entries) {
        const auto number = dictionary_entry_base + dictionary_entry + 1;
        text("同名词条 " + std::to_string(number), 36, top + kDictionaryDefinitionTop + 8, 23, 380);
        if (!content_loading) button(DictionaryNextEntry, "下一条", 480, top + kDictionaryDefinitionTop, 180, 44, false, 22);
    }
    const auto area = dictionary_layout(dictionary_full);
    if (content_loading)
        wrapped(dictionary_slow ? "查词耗时较长。首次使用词典需要建立索引，请稍候；可关闭取消。" : "正在查词，可关闭取消…",
                36, area.top + 8, 612, 24);
    else if (!dictionary_page.glyphs.empty()) {
        if (!graphics::render_page(canvas, body, dictionary_page)) {
            canvas.fill_rect(area.left, area.top, area.width, area.height, 15);
            wrapped("释义绘制失败，可关闭后重试。", 36, area.top + 8, 612, 24);
        }
    } else wrapped(dictionary_notice.empty() ? "暂无可显示释义。" : dictionary_notice, 36, area.top + 8, 612, 24);

    // Connected cells share their rules; an unavailable cell keeps its place with a disabled label and no hit area.
    const int range_top = bottom - kDictionaryTableHeight, source_top = range_top + kDictionaryRowHeight;
    const int page_top = source_top + kDictionaryRowHeight;
    canvas.line(0, range_top, 683, range_top, 1, 2);
    canvas.fill_rect(0, source_top, 684, 1, 1); canvas.fill_rect(0, page_top, 684, 1, 1);
    const auto cell = [&](int id, const std::string& label, int x, int y, int w, int h, bool enabled) {
        center(label, x, top_for_middle(ui, 28, y + h / 2), w, 28, enabled ? 1 : 5);
        if (enabled) hits.push_back({x, y, w, h, id});
    };
    const bool lookup_ready = !settings.dictionary_enabled.empty();
    const std::pair<int, const char*> ranges[] = {{DictionaryStartPrevious, "‹ 起点"}, {DictionaryStartNext, "起点 ›"},
                                                  {DictionaryEndPrevious, "‹ 终点"}, {DictionaryEndNext, "终点 ›"}};
    for (int i = 0; i < 4; ++i) {
        if (i) canvas.fill_rect(i * 171, range_top + 2, 1, kDictionaryRowHeight - 2, 1);
        cell(ranges[i].first, ranges[i].second, i * 171, range_top + 2, 171, kDictionaryRowHeight - 2,
             lookup_ready && (dictionary_range_step(ranges[i].first, false).has_value() ||
                              dictionary_range_step(ranges[i].first, true).has_value()));
    }
    if (!lookup_ready) cell(DictionaryManage, "设置词典", 0, source_top + 1, 684, kDictionaryRowHeight - 1, true);
    else {
        const auto index = std::min(dictionary_source, settings.dictionary_enabled.size() - 1);
        const auto& id = settings.dictionary_enabled[index];
        const auto found = std::find_if(dictionary_items.begin(), dictionary_items.end(), [&](const auto& item) { return item.id == id; });
        const auto name = !dictionary_name.empty() ? dictionary_name : found != dictionary_items.end() && !found->info.name.empty() ? found->info.name : basename(id);
        // Numbering skips sources already known to be unusable, as source navigation does; the
        // current source keeps its place so a failing choice still shows where it is. Only format,
        // support and missing-file results are definitive; memory and I/O failures may recover.
        const auto unusable = [&](const std::string& source) {
            return std::any_of(dictionary_failed_sources.begin(), dictionary_failed_sources.end(),
                               [&](const auto& item) { return item.id == source; }) ||
                   std::any_of(dictionary_items.begin(), dictionary_items.end(), [&](const auto& item) {
                       return item.id == source && (item.status.status == dictionary::Status::InvalidFormat ||
                              item.status.status == dictionary::Status::Unsupported ||
                              item.status.message == kMissingDictionary);
                   });
        };
        std::size_t usable_before = 0, usable_after = 0;
        for (std::size_t i = 0; i < settings.dictionary_enabled.size(); ++i)
            if (i != index && !unusable(settings.dictionary_enabled[i])) ++(i < index ? usable_before : usable_after);
        const auto order = std::to_string(usable_before + 1) + "/" + std::to_string(usable_before + usable_after + 1);
        canvas.fill_rect(171, source_top + 1, 1, kDictionaryRowHeight - 1, 1);
        canvas.fill_rect(513, source_top + 1, 1, kDictionaryRowHeight - 1, 1);
        const int middle = source_top + kDictionaryRowHeight / 2;
        icon(DictionaryPreviousSource, dictionary_icons::kPreviousSource, 85, middle, 171, kDictionaryRowHeight - 1,
             !content_loading && usable_before && !dictionary_previous_exhausted);
        icon(DictionaryNextSource, dictionary_icons::kNextSource, 598, middle, 171, kDictionaryRowHeight - 1,
             !content_loading && usable_after && !dictionary_next_exhausted);
        const std::uint8_t gray = content_loading ? 5 : 1;
        const int order_width = text_width(body, order, 26);
        const int name_width = std::min(text_width(body, name, 26), 342 - 40 - 16 - order_width);
        const int left = 171 + (342 - name_width - 16 - order_width) / 2;
        text(body, name, left, top_for_middle(body, 26, middle), 26, name_width, gray, false);
        // The order shares the name's face, size and top so both sit on one baseline.
        text(body, order, left + name_width + 16, top_for_middle(body, 26, middle), 26, order_width, gray, false);
        if (!content_loading) hits.push_back({172, source_top + 1, 341, kDictionaryRowHeight - 1, DictionarySources});
    }
    const int page_height = bottom - page_top - 1;
    canvas.fill_rect(171, page_top + 1, 1, page_height, 1); canvas.fill_rect(513, page_top + 1, 1, page_height, 1);
    cell(Prev, "‹ 上一页", 0, page_top + 1, 171, page_height, page_ready && dictionary_page_number != 0);
    cell(Next, "下一页 ›", 513, page_top + 1, 171, page_height, page_ready && !dictionary_page_end);
    if (!content_loading && (dictionary_definition ? !dictionary_notice.empty() : lookup_ready))
        cell(DictionaryRetry, dictionary_definition ? "重试" : "重新查询", 171, page_top + 1, 342, page_height, true);
    else {
        const auto label = dictionary_full ? "收起" : "展开";
        const bool expandable = !content_loading && dictionary_definition;
        const int label_width = text_width(label, 28), left = 171 + (342 - 40 - 8 - label_width) / 2;
        const int middle = page_top + 1 + page_height / 2;
        icon(0, dictionary_full ? dictionary_icons::kCollapse : dictionary_icons::kExpand, left + 20, middle, 0, 0, expandable);
        text(label, left + 48, top_for_middle(ui, 28, middle), 28, label_width, expandable ? 1 : 5);
        if (expandable) hits.push_back({172, page_top + 1, 341, page_height, DictionaryExpand});
    }
    canvas.line(0, bottom - 1, 683, bottom - 1, 1, 2);
}

void ReaderApp::Impl::draw_dictionary_sources() {
    view = View::Dictionary; canvas.clear(); hits.clear();
    header("选择词典", 0); button(Back, "取消", 540, 24, 120, 60, false, 24);
    text(body, "当前选词：" + snippet(std::u32string_view(chapter.text).substr(dictionary_selection.begin,
         dictionary_selection.end - dictionary_selection.begin), 64), 36, 120, 26, 612, 1, false);
    const auto count = settings.dictionary_enabled.size();
    const auto screens = std::max<std::size_t>(1, (count + kDictionarySourcesPerPage - 1) / kDictionarySourcesPerPage);
    dictionary_picker_page = std::min(dictionary_picker_page, screens - 1);
    for (std::size_t row = 0; row < kDictionarySourcesPerPage; ++row) {
        const auto index = dictionary_picker_page * kDictionarySourcesPerPage + row;
        if (index >= count) break;
        const auto& id = settings.dictionary_enabled[index];
        const auto found = std::find_if(dictionary_items.begin(), dictionary_items.end(), [&](const auto& item) { return item.id == id; });
        const auto name = found != dictionary_items.end() && !found->info.name.empty() ? found->info.name : basename(id);
        const int y = 184 + static_cast<int>(row) * 132;
        text(body, name, 36, y + 24, 26, 490, 1, index == dictionary_source);
        if (index == dictionary_source) text("当前", 560, y + 24, 23, 100);
        if (found != dictionary_items.end() && !found->status) text("暂不可用", 36, y + 70, 21, 490, 3);
        canvas.line(24, y + 128, 660, y + 128, 12, 2);
        if (!content_loading) hits.push_back({24, y, 636, 128, DictionarySourceBase + static_cast<int>(index)});
    }
    if (!content_loading && dictionary_picker_page) button(Prev, "上一页", 24, 872, 198, 64, false, 24);
    if (!content_loading && dictionary_picker_page + 1 < screens) button(Next, "下一页", 462, 872, 198, 64, false, 24);
    if (screens > 1) center(std::to_string(dictionary_picker_page + 1) + " / " + std::to_string(screens), 222, 890, 240, 24);
    title_lines(content_loading ? "正在读取词典名称，可取消…" : dictionary_notice, 36, 966, 612, 23, false, 2);
    if (!content_loading) button(DictionaryManage, "管理词典", 24, 1104, 636, 80, false, 26);
}

void ReaderApp::Impl::cancel_dictionary_sources() {
    abandon_content(); dictionary_choosing = false; dictionary_notice = dictionary_return_notice;
    draw_dictionary(); overlay();
}

void ReaderApp::Impl::select_dictionary(std::size_t source) {
    dictionary_choosing = false; dictionary_source = source;
    dictionary_search_all = false;
    query_dictionary();
}

void ReaderApp::Impl::navigate_dictionary(int direction) {
    ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryLookup;
    request.dictionary_navigation = direction;
    request.dictionary_candidates = dictionary_candidates;
    for (int source = static_cast<int>(dictionary_source) + direction;
         source >= 0 && source < static_cast<int>(settings.dictionary_enabled.size()); source += direction)
        request.dictionary_sources.push_back(settings.dictionary_enabled[static_cast<std::size_t>(source)]);
    if (request.dictionary_sources.empty()) return;
    dictionary_retry_direction = direction; dictionary_notice.clear();
    queue_dictionary(std::move(request)); draw_dictionary(); overlay();
}

void ReaderApp::Impl::leave_dictionary_settings() {
    abandon_content();
    if (!dictionary_settings_from_lookup) { draw_settings(); overlay(); return; }
    dictionary_settings_from_lookup = false;
    view = View::Dictionary;
    if (dictionary_settings_changed) {
        const auto found = std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), dictionary_settings_source);
        select_dictionary(found == settings.dictionary_enabled.end() ? 0 : static_cast<std::size_t>(found - settings.dictionary_enabled.begin()));
    } else { dictionary_notice = dictionary_return_notice; draw_dictionary(); overlay(); }
}

void ReaderApp::Impl::finish_dictionary(ContentJob& job) {
    if (view != View::Dictionary && view != View::DictionarySettings) return;
    if (job.request.kind == ContentJob::Kind::DictionaryLookup &&
        (view != View::Dictionary || job.request.chapter != chapter_index ||
         job.request.part != chapter.part || job.request.offset != page.start)) return;
    auto& result = job.result;
    if (job.request.kind == ContentJob::Kind::DictionaryLookup) {
        for (auto& failed : result.dictionary_failed_sources) {
            const auto existing = std::find_if(dictionary_failed_sources.begin(), dictionary_failed_sources.end(),
                [&](const auto& item) { return item.id == failed.id; });
            if (existing != dictionary_failed_sources.end()) *existing = std::move(failed);
            else if (dictionary_failed_sources.size() < 32) dictionary_failed_sources.push_back(std::move(failed));
        }
    }
    if (job.request.dictionary_navigation && !result.dictionary_status) {
        dictionary_retry_direction = 0;
        if (job.request.dictionary_navigation < 0) dictionary_previous_exhausted = true;
        else dictionary_next_exhausted = true;
        if (dictionary_definition && !dictionary_page.glyphs.empty()) {
            draw_dictionary(); overlay(); return;
        }
    }
    dictionary_notice = dictionary_error(result.dictionary_status);
    if (job.request.kind == ContentJob::Kind::DictionaryList) {
        dictionary_items = std::move(result.dictionaries); dictionary_list_complete = result.dictionary_complete;
        for (auto& item : dictionary_items) {
            const auto failed = std::find_if(dictionary_failed_sources.begin(), dictionary_failed_sources.end(),
                [&](const auto& error) { return error.id == item.id; });
            if (item.status && failed != dictionary_failed_sources.end()) item.status = failed->status;
        }
        for (const auto& id : settings.dictionary_enabled)
            if (std::none_of(dictionary_items.begin(), dictionary_items.end(), [&](const auto& item) { return item.id == id; }))
                dictionary_items.push_back({id, {}, {dictionary::Status::IoError, kMissingDictionary}});
    } else if (view == View::Dictionary && job.request.chapter == chapter_index && job.request.part == chapter.part && job.request.offset == page.start) {
        if (job.request.kind == ContentJob::Kind::DictionaryPage) {
            if (result.dictionary_status) {
                if (job.request.dictionary_reflow) {
                    dictionary_full = job.request.dictionary_full_layout;
                    dictionary_layout_anchor = job.request.dictionary_layout_anchor;
                    dictionary_pages.assign(1, result.dictionary_current);
                    dictionary_page_number = result.dictionary_page_number;
                } else if (dictionary_page_move > 0) {
                    if (dictionary_pages.size() == 128) dictionary_pages.erase(dictionary_pages.begin());
                    dictionary_pages.push_back(result.dictionary_current); ++dictionary_page_number;
                } else if (dictionary_page_move < 0) {
                    --dictionary_page_number;
                    if (job.request.dictionary_replay) dictionary_pages.assign(1, result.dictionary_current);
                    else dictionary_pages.pop_back();
                }
                dictionary_page = std::move(result.dictionary_page); dictionary_next = result.dictionary_next;
                dictionary_page_end = result.dictionary_complete;
                dictionary_missing_glyphs = result.dictionary_missing_glyphs;
            }
        } else if (result.dictionary_status) {
            if (job.request.kind == ContentJob::Kind::DictionaryLookup) {
                if (!job.request.dictionary_sources.empty()) {
                    const auto& source = job.request.dictionary_sources[result.dictionary_source];
                    const auto found = std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), source);
                    if (found == settings.dictionary_enabled.end()) return;
                    dictionary_source = static_cast<std::size_t>(found - settings.dictionary_enabled.begin());
                }
                dictionary_failed_sources.erase(std::remove_if(dictionary_failed_sources.begin(), dictionary_failed_sources.end(),
                    [&](const auto& item) { return item.id == settings.dictionary_enabled[dictionary_source]; }),
                    dictionary_failed_sources.end());
                if (job.request.dictionary_navigation) {
                    dictionary_search_all = false;
                    dictionary_previous_exhausted = dictionary_next_exhausted = false;
                    dictionary_entry_base = 0; dictionary_layout_anchor = 0; dictionary_page = {};
                    dictionary_missing_glyphs = false;
                }
                if (job.request.dictionary_entry_lookup)
                    dictionary_entry_base = job.request.dictionary_ordinal ? dictionary_entry_base + dictionary_entries.size() : 0;
                dictionary_entries = std::move(result.dictionary_entries); dictionary_entry = 0;
                dictionary_more_entries = result.dictionary_more;
                dictionary_selection = result.dictionary_match->selection;
                dictionary_matched_word = result.dictionary_match->word;
            }
            dictionary_name = result.dictionary_info.name;
            dictionary_omitted_fields = result.dictionary_status.omitted_fields;
            dictionary_headword = dictionary_entries[dictionary_entry].headword;
            dictionary_definition = std::make_shared<const std::vector<dictionary::Field>>(std::move(result.dictionary_fields));
            dictionary_next = {}; dictionary_page_end = true;
            dictionary_pages.assign(1, {}); dictionary_page_number = 0; dictionary_layout_anchor = 0; page_dictionary(0, false); return;
        } else if (job.request.kind == ContentJob::Kind::DictionaryLookup && !job.request.dictionary_entry_lookup &&
                   result.dictionary_status.status != dictionary::Status::Cancelled) {
            // A real miss and a failed source cannot establish absence across the full query.
            if (result.dictionary_partial && result.dictionary_status.status == dictionary::Status::NotFound)
                dictionary_notice = "没有查到可用释义，可重试或检查词典设置。";
            // Every enabled source was read and lacks the selection, so source navigation cannot find it either.
            else if (!job.request.dictionary_navigation && dictionary_search_all &&
                     result.dictionary_status.status == dictionary::Status::NotFound)
                dictionary_previous_exhausted = dictionary_next_exhausted = true;
        }
    }
    if (view == View::DictionarySettings) {
        std::stable_sort(dictionary_items.begin(), dictionary_items.end(), [&](const auto& a, const auto& b) {
            const auto ai = std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), a.id);
            const auto bi = std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), b.id);
            return ai < bi;
        });
        draw_dictionary_settings();
    } else draw_dictionary();
    // A successful lookup returns earlier and keeps the deadline while its first page is laid out.
    dictionary_panel_due = {};
    overlay();
}

void ReaderApp::Impl::close_dictionary() {
    reset_dictionary_contact(); dictionary_choosing = dictionary_settings_from_lookup = false;
    abandon_content(); dictionary_definition.reset(); dictionary_page = {}; dictionary_pages.clear();
    dictionary_entries.clear(); dictionary_candidates.clear();
    if (body_canvas) { canvas = *body_canvas; links = body_links; view = View::Reading; hits.clear(); footer(); overlay(); }
    else recover_body();
}

std::optional<dictionary::Selection> ReaderApp::Impl::dictionary_range_step(int id, bool by_scalar) const {
    const auto edge = id == DictionaryStartPrevious || id == DictionaryStartNext ? dictionary::SelectionEdge::Begin
                                                                                : dictionary::SelectionEdge::End;
    const int direction = id == DictionaryStartPrevious || id == DictionaryEndPrevious ? -1 : 1;
    auto selection = dictionary_selection;
    const auto step = [&](bool scalar) {
        return dictionary::adjust_selection(chapter.text, {page.start, page.end}, edge, direction, scalar, selection);
    };
    auto status = step(by_scalar);
    // Shrinking a single word falls back to one scalar; expansion requires another lookup unit.
    const bool shrinking = id == DictionaryStartNext || id == DictionaryEndPrevious;
    if (!by_scalar && shrinking && status.status == dictionary::Status::NotFound) status = step(true);
    if (!status) return std::nullopt;
    return selection;
}

void ReaderApp::Impl::dictionary_control(int id, bool by_scalar) {
    if (id == Back || id == DictionaryClose) { back(); return; }
    if (view == View::DictionarySettings) {
        if (id == DictionaryScan) {
            dictionary_failed_sources.clear();
            const bool changed = dictionary_settings_changed;
            const auto source = dictionary_settings_source;
            const auto notice = dictionary_return_notice;
            open_dictionary_settings(dictionary_settings_from_lookup);
            dictionary_settings_changed = changed; dictionary_settings_source = source; dictionary_return_notice = notice; return;
        }
        if (content_loading) return;
        if (id == Prev || id == Next) {
            if (id == Prev && dictionary_list_page) --dictionary_list_page;
            else if (id == Next) ++dictionary_list_page;
        } else {
            const int base = id >= DictionaryDownBase ? DictionaryDownBase : id >= DictionaryUpBase ? DictionaryUpBase : DictionaryToggleBase;
            if (id < base || static_cast<std::size_t>(id - base) >= dictionary_items.size()) return;
            const auto& item = dictionary_items[static_cast<std::size_t>(id - base)];
            auto values = edit_snapshot.global_settings;
            auto& enabled = values.dictionary_enabled;
            auto found = std::find(enabled.begin(), enabled.end(), item.id);
            if (base == DictionaryToggleBase) {
                if (found != enabled.end()) enabled.erase(found);
                else if (enabled.size() < 32) enabled.push_back(item.id);
                else { dictionary_notice = "最多启用 32 套词典。"; draw_dictionary_settings(); overlay(); return; }
            } else if (found != enabled.end()) {
                if (base == DictionaryUpBase && found != enabled.begin()) std::iter_swap(found, std::prev(found));
                if (base == DictionaryDownBase && std::next(found) != enabled.end()) std::iter_swap(found, std::next(found));
            }
            std::string error;
            if (store.apply_settings(edit_snapshot, values, false, error) == StoreStatus::Ok) {
                edit_snapshot = store.settings_snapshot(); settings.dictionary_enabled = edit_snapshot.global_settings.dictionary_enabled;
                dictionary_settings_changed = true;
                dictionary_notice = "词典顺序已保存。";
                std::stable_sort(dictionary_items.begin(), dictionary_items.end(), [&](const auto& a, const auto& b) {
                    return std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), a.id) <
                           std::find(settings.dictionary_enabled.begin(), settings.dictionary_enabled.end(), b.id);
                });
            } else dictionary_notice = "设置未保存，请重新扫描后重试。";
        }
        draw_dictionary_settings(); overlay(); return;
    }
    if (id == DictionaryManage) { open_dictionary_settings(true); return; }
    if (dictionary_choosing) {
        if (content_loading) return;
        if (id == Prev && dictionary_picker_page) --dictionary_picker_page;
        else if (id == Next) ++dictionary_picker_page;
        else if (id >= DictionarySourceBase && static_cast<std::size_t>(id - DictionarySourceBase) < settings.dictionary_enabled.size()) {
            const auto source = static_cast<std::size_t>(id - DictionarySourceBase);
            if (source == dictionary_source) cancel_dictionary_sources();
            else select_dictionary(source);
            return;
        }
        draw_dictionary_sources(); overlay(); return;
    }
    if (id == DictionarySources && !content_loading) {
        dictionary_return_notice = dictionary_notice; dictionary_notice.clear();
        dictionary_choosing = true; dictionary_picker_page = dictionary_source / kDictionarySourcesPerPage;
        ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryList;
        queue_dictionary(std::move(request)); draw_dictionary(); overlay(); return;
    }
    if (dictionary_selecting) {
        if (id == DictionaryRetry && !dictionary_contact) submit_dictionary_selection();
        return;
    }
    // A range step replaces any lookup in flight; the full-screen view returns to the panel to show the selection.
    if (id >= DictionaryStartPrevious && id <= DictionaryEndNext) {
        if (settings.dictionary_enabled.empty()) return;
        const auto selection = dictionary_range_step(id, by_scalar);
        if (!selection) return;
        dictionary_selection = *selection; dictionary_manual = true; dictionary_full = false;
        submit_dictionary_selection(); return;
    }
    if (content_loading) return;
    if (id == DictionaryPreviousSource && dictionary_source && !dictionary_previous_exhausted) {
        navigate_dictionary(-1); return;
    }
    if (id == DictionaryNextSource && dictionary_source + 1 < settings.dictionary_enabled.size() && !dictionary_next_exhausted) {
        navigate_dictionary(1); return;
    }
    if (id == DictionaryRetry) {
        if (dictionary_retry_direction) { navigate_dictionary(dictionary_retry_direction); return; }
        if (dictionary_definition) { page_dictionary(); return; }
        query_dictionary(true); return;
    }
    if (id == DictionaryExpand) {
        if (!dictionary_definition || dictionary_pages.empty()) return;
        ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryPage;
        request.dictionary_definition = dictionary_definition;
        request.dictionary_reflow = true; request.dictionary_full_layout = !dictionary_full;
        request.dictionary_layout_anchor = dictionary_pages.back().scalars;
        request.layout = dictionary_layout(!dictionary_full);
        request.font = builtin_body ? std::string() : root + "/" + effective_font;
        dictionary_notice.clear();
        queue_dictionary(std::move(request)); draw_dictionary(); overlay(); return;
    }
    if (id == Prev && dictionary_page_number) {
        page_dictionary(-1);
        return;
    }
    if (id == Next && dictionary_definition && !dictionary_page_end) {
        page_dictionary(1); return;
    }
    if (id == DictionaryNextEntry && !dictionary_entries.empty()) {
        dictionary_definition.reset(); dictionary_page = {}; dictionary_pages.clear();
        ContentJob::Request request; request.relative_path = settings.dictionary_enabled[dictionary_source];
        if (dictionary_entry + 1 == dictionary_entries.size()) {
            request.kind = ContentJob::Kind::DictionaryLookup;
            request.dictionary_ordinal = dictionary_more_entries ? dictionary_entries.back().ordinal + 1 : 0;
            request.dictionary_entry_lookup = true;
            request.dictionary_candidates = {{dictionary_selection, dictionary_matched_word, false}};
            dictionary_search_all = false;
        } else {
            dictionary_entry = (dictionary_entry + 1) % dictionary_entries.size();
            request.kind = ContentJob::Kind::DictionaryRead; request.dictionary_ordinal = dictionary_entries[dictionary_entry].ordinal;
        }
        queue_dictionary(std::move(request)); draw_dictionary(); overlay();
    }
}
}  // namespace pico_page
