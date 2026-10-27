#include "app_impl.h"

namespace pico_page {
namespace {
constexpr int kDictionaryPanelHeight = 560;
constexpr int kDictionaryLowerTop = 568;
constexpr std::size_t kDictionarySourcesPerPage = 5;
// Marks an enabled dictionary that the latest scan no longer found.
constexpr const char* kMissingDictionary = "Missing dictionary";
std::string dictionary_error(const dictionary::Result& status) {
    switch (status.status) {
    case dictionary::Status::Ok: return {};
    case dictionary::Status::NotFound: return "未找到词条，可调整选词或切换词典。";
    case dictionary::Status::Cancelled: return "查询已取消，可重试。";
    case dictionary::Status::IoError: return "词典读取或缓存写入失败，请检查文件和 TF 卡。";
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
    pending_content = std::move(request); content_loading = true;
}

void ReaderApp::Impl::open_dictionary_settings(bool from_lookup) {
    dictionary_settings_from_lookup = from_lookup;
    dictionary_settings_changed = false;
    dictionary_settings_source = dictionary_source < settings.dictionary_enabled.size() ? settings.dictionary_enabled[dictionary_source] : "";
    if (from_lookup && !dictionary_choosing) dictionary_return_notice = dictionary_notice;
    dictionary_choosing = false;
    abandon_content(); dictionary_session.reset(); dictionary_ready.clear();
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
        const bool ready = std::find(dictionary_ready.begin(), dictionary_ready.end(), item.id) != dictionary_ready.end();
        const auto label = item.status.message == kMissingDictionary ? "找不到这套词典的文件" :
                           !item.status ? dictionary_error(item.status) : ready ? "已准备" : "待准备";
        text(label, 36, y + 62, 22, 612, 3);
        if (!content_loading) button(DictionaryToggleBase + static_cast<int>(index), enabled ? "停用" : "启用", 516, y, 144, 60, enabled, 23);
        if (!content_loading && enabled) {
            const auto order = static_cast<std::size_t>(found - settings.dictionary_enabled.begin());
            if (order) button(DictionaryUpBase + static_cast<int>(index), "上移", 24, y + 100, 198, 60, false, 23);
            if (order + 1 < settings.dictionary_enabled.size()) button(DictionaryDownBase + static_cast<int>(index), "下移", 234, y + 100, 198, 60, false, 23);
        }
        if (!content_loading && item.status) button(DictionaryPrepareBase + static_cast<int>(index), "准备词典", 444, y + 100, 216, 60, false, 23);
        canvas.line(36, y + 176, 648, y + 176, 10);
    }
    // A failed scan reports its cause below instead of claiming that no dictionary exists.
    if (dictionary_items.empty() && !content_loading)
        wrapped(dictionary_notice.empty() ? "没有找到词典。请复制含 .ifo、.idx 和 .dict 或 .dict.dz 的词典文件夹，再重新扫描。"
                                          : "无法读取词典目录，请重新扫描。", 36, 220, 612, 26);
    if (!content_loading && dictionary_list_page) button(Prev, "上一页", 24, 928, 198, 68, false, 24);
    center(std::to_string(dictionary_list_page + 1) + " / " + std::to_string(screens), 222, 950, 240, 24);
    if (!content_loading && dictionary_list_page + 1 < screens) button(Next, "下一页", 462, 928, 198, 68, false, 24);
    const auto status = content_loading ? "正在读取词典，可关闭取消…" : !dictionary_notice.empty() ? dictionary_notice :
        !dictionary_list_complete ? "已达到扫描上限，部分目录未扫描。" : "已启用词典按列表顺序查询。";
    title_lines(status, 36, 1010, 612, 23, false, 2);
    button(DictionaryScan, "重新扫描", 24, 1104, 306, 80, false, 26);
    button(Back, dictionary_settings_from_lookup ? "返回查词" : "返回设置", 354, 1104, 306, 80, false, 26);
}

int ReaderApp::Impl::dictionary_top(bool full) const {
    if (full) return 0;
    int upper_overlap = 0, lower_overlap = 0;
    for (const auto& line : page.lines) {
        if (line.end <= dictionary_selection.begin || line.start >= dictionary_selection.end) continue;
        upper_overlap += std::max(0, std::min(line.bottom, kDictionaryPanelHeight) - std::max(line.top, 0));
        lower_overlap += std::max(0, std::min(line.bottom, kDictionaryLowerTop + kDictionaryPanelHeight) - std::max(line.top, kDictionaryLowerTop));
    }
    if (upper_overlap != lower_overlap) return upper_overlap < lower_overlap ? 0 : kDictionaryLowerTop;
    return dictionary_touch_y < 600 ? kDictionaryLowerTop : 0;
}

graphics::LayoutSpec ReaderApp::Impl::dictionary_layout(bool full) const {
    graphics::LayoutSpec spec;
    const int top = dictionary_top(full);
    const int height = full ? 1216 : kDictionaryPanelHeight;
    const int entry_height = dictionary_entry_base || dictionary_entries.size() > 1 || dictionary_more_entries ? 48 : 0;
    spec.left = 36; spec.top = top + 148 + entry_height; spec.width = 612;
    spec.height = height - 232 - entry_height;
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
    dictionary_source = 0; dictionary_search_all = true; dictionary_partial_error = false;
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
    dictionary_partial_error = false;
    dictionary_page = {}; dictionary_definition.reset(); dictionary_entries.clear(); dictionary_pages.clear();
    dictionary_headword.clear(); dictionary_name.clear(); dictionary_notice.clear();
    view = View::Dictionary;
    if (selecting) { draw_dictionary(); overlay(); }
    else submit_dictionary_selection();
}

void ReaderApp::Impl::query_dictionary(bool retry, bool defer_panel) {
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
    if (dictionary_panel_due == std::chrono::steady_clock::time_point{}) return;
    if (view != View::Dictionary || !content_loading || dictionary_selecting) { dictionary_panel_due = {}; return; }
    if (display.busy() || std::chrono::steady_clock::now() < dictionary_panel_due) return;
    dictionary_panel_due = {}; draw_dictionary(); overlay();
}

void ReaderApp::Impl::page_dictionary(int movement, bool redraw) {
    if (!dictionary_definition || dictionary_pages.empty()) return;
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
    const int top = dictionary_top(dictionary_full);
    const int height = dictionary_full ? 1216 : kDictionaryPanelHeight, bottom = top + height;
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
    text(body, heading, 24, top + 16, 30, 402, 1, true);
    button(DictionaryEdit, "选词", 444, top + 12, 96, 48, false, 23);
    button(DictionaryClose, "关闭", 552, top + 12, 108, 48, false, 23);
    if (!settings.dictionary_enabled.empty()) {
        const auto index = std::min(dictionary_source, settings.dictionary_enabled.size() - 1);
        const auto& id = settings.dictionary_enabled[index];
        const auto found = std::find_if(dictionary_items.begin(), dictionary_items.end(), [&](const auto& item) { return item.id == id; });
        const auto name = !dictionary_name.empty() ? dictionary_name : found != dictionary_items.end() && !found->info.name.empty() ? found->info.name : basename(id);
        outline(24, top + 68, 636, 52);
        text(body, name, 36, top + 80, 23, 466, 1, false);
        text(std::to_string(index + 1) + "/" + std::to_string(settings.dictionary_enabled.size()), 514, top + 80, 23, 80);
        canvas.line(622, top + 89, 632, top + 99, 1, 2); canvas.line(632, top + 99, 642, top + 89, 1, 2);
        if (!content_loading) hits.push_back({24, top + 68, 636, 52, DictionarySources});
    }
    const auto heading_scalars = decode_utf8(heading);
    const bool missing_glyphs = dictionary_missing_glyphs || std::any_of(heading_scalars.begin(), heading_scalars.end(),
        [&](char32_t scalar) { return body.missing(scalar); });
    const auto warning = !dictionary_notice.empty() && !dictionary_page.glyphs.empty() ? dictionary_notice :
        dictionary_omitted_fields ? (missing_glyphs ? "部分内容无法显示，且有缺字。" : "部分内容无法显示。") :
        missing_glyphs ? "部分字符缺字，请更换支持音标和生僻字的字体。" :
        dictionary_next.degraded && dictionary_definition ? "部分格式已简化显示。" :
        dictionary_partial_error ? "部分词典不可用，结果可能不完整。" : "";
    if (warning.empty() && heading != selected) text(body, selected + " → " + heading, 36, top + 124, 20, 612, 3, false);
    else text(warning, 36, top + 124, 20, 612, 3);
    if (dictionary_entry_base || dictionary_entries.size() > 1 || dictionary_more_entries) {
        const auto number = dictionary_entry_base + dictionary_entry + 1;
        text("词条 " + std::to_string(number), 36, top + 156, 23, 380);
        if (!content_loading) button(DictionaryNextEntry, "下一词条", 480, top + 148, 180, 44, false, 22);
    }
    const auto area = dictionary_layout(dictionary_full);
    if (content_loading) wrapped("正在读取，可关闭取消…", 36, area.top + 8, 612, 24);
    else if (!dictionary_page.glyphs.empty()) {
        if (!graphics::render_page(canvas, body, dictionary_page)) {
            canvas.fill_rect(area.left, area.top, area.width, area.height, 15);
            wrapped("释义绘制失败，可关闭后重试。", 36, area.top + 8, 612, 24);
        }
    } else wrapped(dictionary_notice.empty() ? "暂无可显示释义。" : dictionary_notice, 36, area.top + 8, 612, 24);
    if (!content_loading) {
        if (dictionary_definition) {
            if (dictionary_page_number) button(Prev, "上一页", 24, bottom - 76, 144, 64, false, 23);
            else center("上一页", 24, bottom - 58, 144, 23, 8);
            if (!dictionary_notice.empty()) button(DictionaryRetry, "重试", 180, bottom - 76, 144, 64, false, 23);
            else center("第 " + std::to_string(dictionary_page_number + 1) + " 页", 180, bottom - 58, 144, 23);
            if (!dictionary_page_end) button(Next, "下一页", 348, bottom - 76, 144, 64, false, 23);
            else center("下一页", 348, bottom - 58, 144, 23, 8);
            button(DictionaryExpand, dictionary_full ? "收起" : "展开", 516, bottom - 76, 144, 64, false, 23);
        } else if (settings.dictionary_enabled.empty()) button(DictionaryManage, "设置词典", 24, bottom - 76, 636, 64, false, 24);
        else button(DictionaryRetry, "重新查询", 24, bottom - 76, 636, 64, false, 24);
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
        canvas.line(24, y + 128, 660, y + 128, 10);
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
    dictionary_partial_error = false; dictionary_search_all = false;
    query_dictionary();
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
    auto& result = job.result;
    dictionary_notice = dictionary_error(result.dictionary_status);
    if (job.request.kind == ContentJob::Kind::DictionaryList) {
        dictionary_items = std::move(result.dictionaries); dictionary_list_complete = result.dictionary_complete;
        for (const auto& id : settings.dictionary_enabled)
            if (std::none_of(dictionary_items.begin(), dictionary_items.end(), [&](const auto& item) { return item.id == id; }))
                dictionary_items.push_back({id, {}, {dictionary::Status::IoError, kMissingDictionary}});
    } else if (job.request.kind == ContentJob::Kind::DictionaryPrepare) {
        if (result.dictionary_status && std::find(dictionary_ready.begin(), dictionary_ready.end(), job.request.relative_path) == dictionary_ready.end())
            dictionary_ready.push_back(job.request.relative_path);
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
                if (!job.request.dictionary_sources.empty()) dictionary_source = result.dictionary_source;
                if (!job.request.dictionary_entry_lookup) dictionary_partial_error = result.dictionary_partial;
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
            dictionary_pages.assign(1, {}); dictionary_page_number = 0; dictionary_layout_anchor = 0; page_dictionary(0, false); return;
        } else if (job.request.kind == ContentJob::Kind::DictionaryLookup && !job.request.dictionary_entry_lookup &&
                   result.dictionary_status.status != dictionary::Status::Cancelled) {
            dictionary_partial_error = result.dictionary_partial || result.dictionary_status.status != dictionary::Status::NotFound;
            // A single failure keeps its own cause; several sources may have failed differently.
            if (result.dictionary_partial) dictionary_notice = "没有找到词条；部分词典无法使用，结果可能不完整。";
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

void ReaderApp::Impl::dictionary_control(int id) {
    if (id == Back || id == DictionaryClose) { back(); return; }
    if (view == View::DictionarySettings) {
        if (id == DictionaryScan) {
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
            const int base = id >= DictionaryPrepareBase ? DictionaryPrepareBase : id >= DictionaryDownBase ? DictionaryDownBase : id >= DictionaryUpBase ? DictionaryUpBase : DictionaryToggleBase;
            if (id < base || static_cast<std::size_t>(id - base) >= dictionary_items.size()) return;
            const auto& item = dictionary_items[static_cast<std::size_t>(id - base)];
            if (base == DictionaryPrepareBase) {
                ContentJob::Request request; request.kind = ContentJob::Kind::DictionaryPrepare; request.relative_path = item.id;
                queue_dictionary(std::move(request)); draw_dictionary_settings(); overlay(); return;
            }
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
    if (id == DictionaryEdit) {
        abandon_content(); dictionary_selecting = true; dictionary_manual = true;
        dictionary_notice.clear(); draw_dictionary(); overlay(); return;
    }
    if (dictionary_selecting) {
        if (id == DictionaryRetry && !dictionary_contact) submit_dictionary_selection();
        return;
    }
    if (content_loading) return;
    if (id == DictionaryRetry) {
        if (dictionary_definition) { page_dictionary(); return; }
        dictionary_partial_error = false;
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
