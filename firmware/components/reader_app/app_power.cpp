#include "app_impl.h"
#include "default_sleep_cover.h"

#include <memory>
#include <new>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

namespace pico_page {
namespace {

constexpr int kSleepCoverWidth = 684;
constexpr int kSleepCoverHeight = 1040;
constexpr int kSleepCoverLowBatteryPercent = 20;

bool draw_default_sleep_cover(graphics::Canvas& canvas) {
    if (canvas.width() != kSleepCoverWidth || canvas.height() < kSleepCoverHeight) return false;
    std::unique_ptr<tinfl_decompressor> decoder(new (std::nothrow) tinfl_decompressor);
    if (!decoder) return false;
    tinfl_init(decoder.get());
    const std::size_t cover_bytes = std::size_t(canvas.stride()) * kSleepCoverHeight;
    std::size_t input_bytes = sizeof(kDefaultSleepCover);
    std::size_t output_bytes = cover_bytes;
    auto* pixels = canvas.data();
    const auto status = tinfl_decompress(decoder.get(), kDefaultSleepCover, &input_bytes,
        pixels, pixels, &output_bytes, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status == TINFL_STATUS_DONE && input_bytes == sizeof(kDefaultSleepCover) && output_bytes == cover_bytes)
        return true;
    canvas.clear();
    return false;
}

}

void ReaderApp::Impl::prepare_custom_sleep(bool sleeping) {
    ContentJob::Request request;
    request.kind = ContentJob::Kind::LockScreen; request.root = root;
    request.sleep_image = settings.sleep_image; request.generation = counting_generation;
    request.sleep_overlay = settings.sleep_overlay;
    request.lock_screen_prepare = !sleeping; request.preview = sleeping;
    request.lock_screen_shown = custom_rotation_shown;
    if (!sleeping) {
        ++custom_prepare_attempts;
        custom_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    if (sleeping) {
        if (!custom_sleep_deadline) custom_sleep_deadline = std::chrono::steady_clock::now() + kLockScreenPrepareBudget;
        request.lock_screen_deadline = *custom_sleep_deadline;
        custom_sleep_attempted = true; custom_sleep_image.reset(); custom_notice.clear();
        custom_notice_pending = false; custom_rotation_name.clear();
        if (std::chrono::steady_clock::now() >= *custom_sleep_deadline) { custom_notice = tr("休眠图片准备超时"); return; }
    }
    if (sleeping && settings.sleep_overlay) {
#ifdef ESP_PLATFORM
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 342U * 1216 + 768U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 342U * 1216) {
            custom_notice = tr("图片准备内存不足"); return;
        }
#endif
        request.lock_background = std::make_shared<graphics::Canvas>(canvas);
        if (sleep_wait_footer && view == sleep_wait_view)
            std::memcpy(request.lock_background->data() + 342 * footer_top, sleep_wait_footer.get(), kSleepWaitFooterBytes);
        if (view == View::Reading) {
            const int top = reading_status_top();
            request.lock_background->fill_rect(0, top, 684, 1216 - top, 15);
        }
    }
    counting_job = std::make_unique<ContentJob>(std::move(request));
    if (!counting_job->start()) {
        counting_job.reset();
        if (sleeping) custom_notice = tr("休眠图片暂时无法准备");
    }
}

void ReaderApp::Impl::draw_power(const std::string& preview_notice) {
    view = View::PowerSettings;
    hits.clear(); canvas.clear(); header(tr("电源与休眠"), Back, tr("设置"));
    text(tr("休眠画面"), 36, 144, 24, 612, 3);
    const int style_actions[] = {SleepCover, SleepReading, SleepSimple, SleepCustom};
    const char* style_values[] = {"cover", "reading", "simple", "custom"};
    const char* style_labels[] = {tr("书籍封面"), tr("保留正文"), tr("简洁屏保"), tr("自定义图片")};
    for (int i = 0; i < 4; ++i) {
        const int x = 36 + (i % 2) * 306, y = 192 + (i / 2) * 92;
        const bool selected = draft.sleep_screen == style_values[i];
        if (selected) canvas.fill_rect(x, y, 306, 92, 1);
        center(style_labels[i], x, y + 29, 306, 28, selected ? 15 : 1, selected);
        hits.push_back({x, y, 306, 92, style_actions[i]});
    }
    outline(36, 192, 612, 184);
    canvas.fill_rect(342, 192, 2, 184, 1);
    canvas.fill_rect(36, 284, 612, 2, 1);
    if (draft.sleep_screen == "custom") {
        button(SleepChoose, tr("选择图片 / 轮播"), 36, 408, 296, 88, false, 26);
        button(SleepPreview, tr("预览"), 352, 408, 296, 88, false, 26);
        text(draft.sleep_image.empty() ? tr("按文件名轮播") : draft.sleep_image, 36, 512, 24, 612, 3);
        value_row(SleepOverlay, tr("叠加当前画面"), tr("点按切换"), draft.sleep_overlay ? tr("开") : tr("关"), 552, 104, true, false);
    } else if (draft.sleep_screen == "cover") note(tr("无可用封面时显示书名与作者。"), 464);
    else if (draft.sleep_screen == "reading") note(tr("保留当前正文，并显示休眠提示。"), 464);
    else note(tr("显示书名、阅读位置与唤醒提示。"), 464);
    const int auto_top = draft.sleep_screen == "custom" ? 680 : 636;
    text(tr("自动休眠"), 36, auto_top, 28, 612, 1, true);
    button(SleepLess, "−", 36, auto_top + 70, 120, 96);
    center(draft.auto_sleep_minutes ? trf("{minutes} 分钟", {{"minutes", std::to_string(draft.auto_sleep_minutes)}}) : tr("关闭"), 168, auto_top + 92, 348, 34);
    button(SleepMore, "+", 528, auto_top + 70, 120, 96);
    note(preview_notice.empty() ? tr("按电源键唤醒；保存失败时不进入休眠。") : preview_notice,
         draft.sleep_screen == "custom" ? 868 : 856);
    button(PowerMenu, tr("关机 / 重启"), 24, 952, 636, 96);
    button(Back, tr("取消"), 24, 1080, 306, 96);
    button(PowerSave, tr("保存设置"), 354, 1080, 306, 96, true);
}

void ReaderApp::Impl::draw_sleep(const std::string &sync_state) {
    sleep_wait_footer.reset();
    sleep_wait_restore_pending = false;
    hits.clear();
    load_fonts();
    const bool show_book = sleep_screen_shows_book();
    if (show_book && !restore_confirmed_position()) return;
    const auto position = show_book ? confirmed_book_position() : std::string{};
    const char *resume_hint = sleep_onboarding ? tr("按电源键继续引导") : sleep_origin == Origin::Settings ? tr("按电源键返回设置") :
                              sleep_origin == Origin::Library ? tr("按电源键返回书库") : tr("按电源键继续阅读");
    if (settings.sleep_screen == "custom" && custom_sleep_image) {
        canvas = *custom_sleep_image;
        custom_sleep_image.reset();
        view = View::Sleeping;
        return;
    }
    if (settings.sleep_screen == "simple" || settings.sleep_screen == "custom") {
        canvas.clear();
        text(tr("已休眠"), 72, 440, 40, 540, 1, true);
        text(resume_hint, 72, 512, 26, 540);
        canvas.line(72, 594, 612, 594, 1, 2);
        cover_title(show_book ? document.metadata().title : "Pico Page", 72, 626, 540, 140, 32);
        if (show_book) {
            const auto chapter = chapter_label();
            const auto suffix = " · " + position;
            const int chapter_width = std::max(0, 540 - text_width(suffix, 22));
            text(body, chapter, 72, 806, 22, chapter_width, 3);
            const int suffix_x = std::min(chapter_width, text_width(body, chapter, 22));
            text(suffix, 72 + suffix_x, 806, 22, 540 - suffix_x, 3);
        } else {
            text(tr("尚未打开书籍"), 72, 806, 22, 540, 3);
        }
        text(show_book ? trf("已保存 · {state}", {{"state", sync_state}}) : "", 72, 854, 22, 540, 3);
        view = View::Sleeping;
        return;
    }
    const bool cover_mode = settings.sleep_screen == "cover";
    const bool reading = show_book && settings.sleep_screen == "reading";
    if (reading) {
        load_fonts();
        if (!render_body(offset)) return;
        for (const auto &line : page.lines)
            if (line.top < 1088 && line.bottom > 1088)
                canvas.fill_rect(0, line.top, 684, line.bottom - line.top + 1, 15);
    } else {
        canvas.clear();
        bool covered = false;
        if (cover_mode) {
            if (!show_book) {
                covered = draw_default_sleep_cover(canvas);
            } else if (async_content) {
                covered = bool(sleep_cover_image);
                if (covered) blit(sleep_cover_image->pixels(), 0, 0, kSleepCoverWidth, kSleepCoverHeight);
                sleep_cover_image.reset();
            } else {
                struct stat info{};
                std::vector<std::uint8_t> cover;
                if (safe_existing(root, book_path) && stat((root + "/" + book_path).c_str(), &info) == 0)
                    cover = cover_pixels(book_path, info, kSleepCoverWidth, kSleepCoverHeight, true);
                covered = !cover.empty();
                if (covered) blit(cover, 0, 0, kSleepCoverWidth, kSleepCoverHeight);
            }
        }
        if (!covered) {
            if (cover_mode) outline(56, 56, 572, 928, 3);
            cover_title(show_book ? document.metadata().title : "Pico Page", 112, 286, 460, 384, 76);
            canvas.fill_rect(112, 724, 96, 4, 1);
            text(show_book ? document.metadata().author : tr("个人阅读"), 112, 780, 28, 460);
        }
    }
    const int top = reading ? 1088 : kSleepCoverHeight;
    canvas.fill_rect(0, top, 684, 1216 - top, 1);
    text(tr("已休眠"), 36, top + 24, 30, 290, 15, true);
    if (cover_mode) {
        text(tr("按电源键唤醒"), 36, top + 72, 22, 290, 15);
        if (battery_percent && *battery_percent <= kSleepCoverLowBatteryPercent && !battery_charging)
            text(tr("电量低，请充电"), 36, top_for_middle(ui, 24, top + 126, true), 24, 290, 15, true);
        if (show_book) {
            const auto chapter_entry = current_toc();
            const auto &toc = document.toc();
            if ((!txt_name(book_path) || !document.metadata().txt_headings.empty()) &&
                chapter_entry < toc.size() && !toc[chapter_entry].label.empty())
                text(body, toc[chapter_entry].label, 364, top + 24, 22, 284, 15);
            text(pages_complete ? position : trf("已读 {percent}", {{"percent", position}}),
                 364, top + 72, 22, 284, 15);
            if (sync_state == tr("已上传")) text(tr("已同步到云端"), 364, top_for_middle(ui, 22, top + 126), 22, 284, 15);
        }
    } else {
        text(resume_hint, 36, top + 72, 22, 290, 15);
        text(show_book ? tr("已保存") : tr("尚未打开书籍"), 364, top + 24, 22, 284, 15);
        text(show_book ? sync_state : "", 364, top + 60, 22, 284, 15);
        if (!reading && show_book) text(position, 364, top + 106, 22, 284, 15);
    }
    view = View::Sleeping;
}

}
