#include "app_impl.h"

namespace pico_page {

bool ReaderApp::Impl::chapter_prefetch_due() const {
    return async_content && background_content_ready() && !counting_job &&
        !prepared_chapter && !chapter_prefetch_attempted && !chapter.text.empty() &&
        (readable_offset(page.end) == chapter.text.size() ||
         (next_page && readable_offset(next_page->end) == chapter.text.size()));
}

void ReaderApp::Impl::prepare_chapter() {
    chapter_prefetch_attempted = true;
    auto index = chapter_index;
    auto part = chapter.part + 1;
    if (chapter.last_part) {
        part = 0;
        do { ++index; } while (index < document.spine().size() && !document.spine()[index].linear);
    }
    if (index >= document.spine().size()) return;
    // Near the boundary, release optional counting resources before assessing reading headroom.
    yield_counting_session();
    // Reading keeps source anchors; use the existing parser reserve as well as the counting budget.
    if (!counting_memory(document.spine()[index].bytes) ||
        !chapter_memory_available(4U * std::min(document.spine()[index].bytes, 2 * kChapterPartScalars))) return;
    ContentJob::Request request;
    request.kind = ContentJob::Kind::Chapter; request.target = ContentJob::Target::Start;
    request.chapter_prefetch = true;
    request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
    request.chapter = index; request.part = part; request.chapter_resource = document.spine()[index].resource;
    request.generation = foreground_generation;
    if (!chapter_session) chapter_session = std::make_shared<ChapterSession>();
    request.chapter_session = chapter_session;
    counting_job = std::make_unique<ContentJob>(std::move(request));
    if (!counting_job->start()) counting_job.reset();
}

namespace {
std::size_t chapter_result_bytes(const ContentJob& job) {
    const auto& content = job.result.chapter;
    auto bytes = content.resource.capacity() + content.text.capacity() * sizeof(char32_t) +
        content.blocks.capacity() * sizeof(TextBlock) + content.runs.capacity() * sizeof(TextRun) +
        content.fragments.capacity() * sizeof(FragmentTarget) + content.links.capacity() * sizeof(DocumentLink) +
        content.images.capacity() * sizeof(DocumentImage) + content.warnings.capacity() * sizeof(std::string) +
        job.result.content_cache.bytes.capacity();
    for (const auto& run : content.runs) bytes += run.source.resource.capacity() + run.source.node.capacity();
    for (const auto& fragment : content.fragments) bytes += fragment.id.capacity();
    for (const auto& link : content.links) bytes += link.resource.capacity() + link.fragment.capacity();
    for (const auto& image : content.images)
        bytes += image.resource.capacity() + image.alt.capacity() + image.source.resource.capacity() + image.source.node.capacity();
    for (const auto& warning : content.warnings) bytes += warning.capacity();
    return bytes;
}
}

void ReaderApp::Impl::prepare_next(const std::function<bool()>& stop) {
    if (stop && stop()) return;
    const StageTimer timer{"next_page_prepare"};
    if (!next_page) {
        const auto start = readable_offset(page.end);
        // Adjacent content uses the worker after the current page preparation finishes.
        if (start >= chapter.text.size()) { prefetch_done = true; return; }
#ifdef ESP_PLATFORM
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2048U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 256U * 1024) {
            prefetch_done = true;
            return;
        }
#endif
        next_spec = layout(settings, link_return ? 144 : 64);
        auto planned = plan_page(chapter, start, next_spec, true, stop);
        if (planned.error == "Layout cancelled") return;
        const auto bytes = planned.glyphs.capacity() * sizeof(graphics::PositionedGlyph) +
                           planned.lines.capacity() * sizeof(graphics::TextLine);
        if (!planned.error.empty() || planned.end <= planned.start || bytes > 256U * 1024) {
            prefetch_done = true;
            return;
        }
        next_page = std::move(planned);
        return;
    }
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2048U * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 64U * 1024) {
        next_page.reset(); prefetch_done = true; return;
    }
#endif
    // Images are prepared only for a displayed page; the planned layout is kept for the turn.
    if (!next_page->images.empty()) { prefetch_done = true; return; }
    if (!body.set_pixel_size(next_page->font_px)) { next_page.reset(); prefetch_done = true; return; }
    int warm_size = next_page->font_px;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
    while (warm_cursor < next_page->glyphs.size()) {
        if ((stop && stop()) || std::chrono::steady_clock::now() >= deadline) return;
        const auto& glyph = next_page->glyphs[warm_cursor];
        if ((glyph.font_px != warm_size && !body.set_pixel_size(glyph.font_px)) ||
            !body.warm(glyph.scalar, glyph.bold)) {
            next_page.reset(); prefetch_done = true; return;
        }
        warm_size = glyph.font_px;
        ++warm_cursor;
    }
    // A text page is drawn here, after its glyphs are warm, because the glyph cache belongs to
    // this task; a cancelled attempt is discarded and repeated in a later step.
    if (!next_bitmap && body_cache_room()) {
        const StageTimer timer{"next_page_bitmap"};
        auto bitmap = std::make_unique<graphics::Canvas>();
        if (graphics::render_page(*bitmap, body, *next_page, 1, stop)) next_bitmap = std::move(bitmap);
        else if (stop && stop()) return;
    }
    prefetch_done = true;
}


void ReaderApp::Impl::poll_covers() {
    if (view != View::Library) cancel_covers();
    if (cover_job) {
        if (!cover_job->done()) return;
        auto job = std::move(cover_job);
        if (view == View::Library && job->request.generation == cover_generation &&
            job->result.covers.size() == job->request.covers.size()) {
            if (display.busy()) { cover_job = std::move(job); return; }
            for (std::size_t i = 0; i < job->request.covers.size(); ++i) {
                const auto& source = job->request.covers[i];
                if (covers.size() == kCoverSlots) covers.erase(covers.begin());
                covers.push_back({source.path, source.size, source.modified, std::move(job->result.covers[i])});
            }
            draw_library(); overlay();
        }
    }
    if (!pending_covers || display.busy() || save_job || bookmark_job || foreground_job || pending_content || counting_job) return;
    cover_job = std::make_unique<ContentJob>(std::move(*pending_covers)); pending_covers.reset();
    if (!cover_job->start()) {
        cover_job.reset(); message = "封面暂未生成，可继续阅读";
        for (const auto& source : missing_covers) {
            if (covers.size() == kCoverSlots) covers.erase(covers.begin());
            covers.push_back({source.path, source.size, source.modified, {}});
        }
        draw_library(); overlay();
    }
}


ReaderApp::Impl::Origin ReaderApp::Impl::origin_for(View source) const {
    const auto body_origin = has_book() && (session.visible_record() || session.saved_record())
        ? Origin::Reading : Origin::Library;
    switch (source) {
    case View::Reading: case View::Tools: case View::Contents: case View::Sync: case View::Confirmation:
    case View::Dictionary:
        return body_origin;
    case View::Library: case View::BookDetails: case View::BookManagement:
        return Origin::Library;
    case View::Typography:
        return typography_from_settings ? Origin::Settings : body_origin;
    case View::Refresh: case View::RefreshConfirm: case View::RefreshInterval: case View::PageEffects:
        return refresh_from_settings ? Origin::Settings : body_origin;
    case View::Onboarding:
        return onboarding_from_help ? Origin::Settings : Origin::Library;
    case View::Help:
        return help_from_settings ? Origin::Settings : Origin::Library;
    case View::Import: case View::UsbTransfer: case View::Portal:
        return import_from_settings ? Origin::Settings : Origin::Library;
    case View::Settings: case View::WifiSettings: case View::PowerSettings: case View::StorageSettings:
    case View::InputSettings: case View::SyncSettings: case View::TextEntry: case View::LockScreens:
    case View::About: case View::Update: case View::LockScreenPreview:
        return Origin::Settings;
    case View::DictionarySettings:
        return dictionary_settings_from_lookup ? body_origin : Origin::Settings;
    case View::Error: return error_origin;
    case View::Sleeping: return sleep_origin;
    case View::Loading: return loading_origin;
    case View::NetworkClosing: return origin_for(network_return_view);
    case View::PowerMenu: case View::PowerOff: return origin_for(power_return_view);
    }
    return Origin::Library;
}


void ReaderApp::Impl::draw_image_viewer(bool full) {
    if (!viewer_original) return;
    canvas = *viewer_original;
    if (image_contrast_level) {
        // Each curve preserves black and white while progressively separating middle tones.
        static constexpr std::uint8_t tones[viewer_contrast_levels][16] = {
            {0, 0, 1, 2, 3, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 15},
            {0, 0, 0, 1, 2, 3, 5, 7, 8, 10, 12, 13, 14, 15, 15, 15},
            {0, 0, 0, 0, 1, 2, 4, 6, 9, 11, 13, 14, 15, 15, 15, 15},
        };
        const auto* curve = tones[image_contrast_level - 1];
        for (int y = viewer_top; y < viewer_top + viewer_height; ++y) {
            auto* row = canvas.data() + y * canvas.stride();
            for (int x = viewer_left / 2; x < (viewer_left + viewer_width) / 2; ++x) {
                const auto packed = row[x];
                row[x] = curve[packed & 15] | (curve[packed >> 4] << 4);
            }
        }
    }
    canvas.fill_rect(0, 0, 684, viewer_toolbar_height, 15);
    text("‹ 返回正文", 28, 30, 28, 300);
    canvas.line(viewer_action_left - 1, 0, viewer_action_left - 1, viewer_toolbar_height - 1, 1, 2);
    center("对比度 " + std::to_string(image_contrast_level) + "/3", viewer_action_left, 30,
           684 - viewer_action_left, 24);
    canvas.line(0, viewer_toolbar_height - 1, 683, viewer_toolbar_height - 1, 1, 2);
    image_viewing = true;
    view = View::Reading;
    overlay(full);
}


bool ReaderApp::Impl::begin_image(std::size_t start, const graphics::Page& planned, bool preview, bool progressive) {
    abandon_content();
    yield_counting_session();
    release_counting();
    ContentJob::Request request;
    request.kind = ContentJob::Kind::Image;
    PP_DIAGNOSTIC(
        request.diagnostic_session = diagnostic_session;
        request.diagnostic_layout = diagnostic_layout;
    )
    request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
    request.preview = preview;
    request.progressive = progressive;
    request.layout = preview ? layout(draft, 64, typography_preview_bottom()) : layout(settings, link_return ? 144 : 64);
    request.page = planned;
    request.image_resource = planned.images.front().resource;
    if (planned.images.size() == 1 && planned.glyphs.empty())
        request.cache_name = image_cache_key(request.identity.book_id, request.image_resource, request.layout);
    request.generation = foreground_generation; request.chapter = chapter_index;
    request.part = chapter.part; request.offset = start;
    pending_content = std::move(request);
    clear_prefetch(); covers.clear(); make_image_room();
    if (progressive) { inline_image_pending = true; return true; }
    // The typography panel draws the preview status itself.
    if (preview) { loading_origin = origin_for(view); content_loading = true; view = View::Loading; return true; }
    loading("正在准备图片…");
    clear_body_cache();
    return true;
}


bool ReaderApp::Impl::begin_chapter(std::size_t index, std::uint32_t part, ContentJob::Target target,
                   std::size_t position, const Anchor& anchor, const std::string& fragment) {
    poll_counting_job();
    std::unique_ptr<ContentJob> prepared;
    if (target == ContentJob::Target::Start && prepared_chapter &&
        prepared_chapter->request.generation == foreground_generation &&
        prepared_chapter->request.identity.book_id == document.metadata().book_id &&
        prepared_chapter->request.chapter == index && prepared_chapter->request.part == part)
        prepared = std::move(prepared_chapter);
    auto continuing_session = std::move(chapter_session);
    const auto continuing_bytes = chapter_stream_bytes;
    auto current_cache = std::move(chapter_cache);
    auto recent_cache = std::move(recent_chapter_cache);
    const bool current_write = chapter_cache_write_needed, recent_write = recent_cache_write_needed;
    abandon_content();
    invalidate_counting();
    release_counting();
    ContentJob::Request request;
    request.kind = ContentJob::Kind::Chapter; request.target = target;
    chapter_session = continuing_session ? std::move(continuing_session) : std::make_shared<ChapterSession>();
    chapter_stream_bytes = continuing_bytes;
    request.chapter_session = chapter_session;
    request.root = root; request.path = root + "/" + book_path; request.identity = document.metadata();
    request.font = body_font_path(); request.layout = layout(settings, link_return ? 144 : 64);
    request.chapter = index; request.part = part; request.offset = position;
    if (index < document.spine().size()) request.chapter_resource = document.spine()[index].resource;
    for (const auto& candidate : {std::make_pair(current_cache, current_write), std::make_pair(recent_cache, recent_write)})
        if (candidate.first && content_cache_matches(*candidate.first, request.identity.book_id, index, part)) {
            request.content_cache = candidate.first; request.content_cache_write_needed = candidate.second; break;
        }
    if (current_cache && current_cache != request.content_cache) {
        recent_chapter_cache = std::move(current_cache); recent_cache_write_needed = current_write;
    } else if (recent_cache != request.content_cache) {
        recent_chapter_cache = std::move(recent_cache); recent_cache_write_needed = recent_write;
    }
    request.anchor = anchor; request.fragment = fragment; request.generation = foreground_generation;
    if (target == ContentJob::Target::End && !link_return && index < counted_live.size() && counted_live[index]) {
        const auto& parts = chapter_pages[index];
        const auto selected = part == kLastChapterPart && !parts.empty() ? parts.size() - 1 : part;
        if (selected < parts.size() && !parts[selected].empty()) request.known_start = parts[selected].back();
    }
    if (target == ContentJob::Target::End) {
        for (auto i = page_history.rbegin(); i != page_history.rend(); ++i)
            if (i->chapter == index && i->return_bar == link_return.has_value() && i->part_end &&
                (part == kLastChapterPart ? i->last_part : i->part == part)) {
                request.part = i->part;
                request.known_start = i->start; request.known_end = i->end;
                break;
            }
    }
    if (!prepared) pending_content = std::move(request);
    if (view == View::Reading) {
        loading_origin = origin_for(view);
        content_loading = true; view = View::Loading; hits.clear(); loading_cancel = {};
        chapter_feedback_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    } else loading("正在准备章节…");
    clear_body_cache(); clear_prefetch(); covers.clear(); release(chapter); page = {};
    if (prepared) { ++chapter_prefetch_hits; finish_chapter(*prepared); }
    return true;
}


void ReaderApp::Impl::complete_inline_image(const ContentJob& job) {
    const StageTimer timer{"image_patch", 0};
    const auto& request = job.request;
    inline_image_pending = false;
    if (view != View::Reading || image_viewing || request.generation != foreground_generation ||
        request.identity.book_id != document.metadata().book_id || request.chapter != chapter_index ||
        request.part != chapter.part || request.offset != page.start ||
        !same_layout(request.layout, layout(settings, link_return ? 144 : 64))) return;
    body_images_pending = false;
    PP_DIAGNOSTIC(const auto patch = diagnostics::begin(diagnostics::Kind::ImagePatch,
        {request.diagnostic_session, request.diagnostic_layout, ui_sequence + 1,
         request.generation, job.result.decoded_images});)
#ifdef ESP_PLATFORM
    if (!job.result.error.empty())
        ESP_LOGW("reader_app", "Inline image unavailable: error=%s psram_free=%u largest=%u",
                 job.result.error.c_str(), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#endif
    RefreshRegion region{684, 1216, 0, 0};
    int right = 0, bottom = 0;
    for (const auto& box : request.page.images) {
        const bool failed = !job.result.image || !job.result.error.empty() ||
            std::find(job.result.failed_images.begin(), job.result.failed_images.end(), box.begin) != job.result.failed_images.end();
        if (failed) {
            canvas.fill_rect(box.x, box.y, box.width, box.height, 15);
            draw_image_error(box);
        } else {
            // Preserve neighboring nibbles at odd edges; the interior is a packed row copy.
            const int left = (box.x + 1) & ~1;
            const int end = box.x + box.width;
            const int last = end & ~1;
            for (int y = box.y; y < box.y + box.height; ++y) {
                if (box.x & 1) canvas.pixel(box.x, y, job.result.image->pixel(box.x, y));
                if (last > left) std::memcpy(canvas.data() + y * canvas.stride() + left / 2,
                    job.result.image->data() + y * job.result.image->stride() + left / 2, (last - left) / 2);
                if (end & 1) canvas.pixel(end - 1, y, job.result.image->pixel(end - 1, y));
            }
        }
        region.x = std::min(region.x, box.x); region.y = std::min(region.y, box.y);
        right = std::max(right, box.x + box.width); bottom = std::max(bottom, box.y + box.height);
    }
    if (body_canvas) *body_canvas = canvas;
    region.width = right - region.x; region.height = bottom - region.y;
    bool submitted = false;
    if (region.width > 0 && region.height > 0)
        submitted = display.present({{"ui", 0, 0, ++ui_sequence}, {}, FramePurpose::Overlay, RefreshIntent::Content, region});
    PP_DIAGNOSTIC(diagnostics::end(patch,
        submitted && job.result.image && job.result.error.empty() && job.result.failed_images.empty()
            ? diagnostics::Outcome::Success : diagnostics::Outcome::Failure);)
    (void)submitted;
}


void ReaderApp::Impl::poll_content(bool allow_inline) {
    if (inline_image_pending && (view != View::Reading || image_viewing)) abandon_content();
    if (foreground_job && ((!content_loading && !inline_image_pending) || foreground_job->request.generation != foreground_generation)) {
        if (foreground_job->started() && !foreground_job->done()) return;
        foreground_job.reset();
    }
    if (!foreground_job && pending_content) {
        foreground_job = std::make_unique<ContentJob>(std::move(*pending_content));
        pending_content.reset();
        mapping_start_attempts = 0;
        mapping_start_retry = {};
    }
    if (!foreground_job) return;
    if (foreground_job->request.progressive && !allow_inline) return;
    if (!foreground_job->started()) {
        if (!content_loading && !inline_image_pending) { foreground_job.reset(); return; }
        if (display.busy() || counting_job || save_job || cover_job || bookmark_job) return;
        if ((foreground_job->request.kind == ContentJob::Kind::Open || ContentJob::dictionary_kind(foreground_job->request.kind)) && session.needs_save()) {
            if (save_failed && ContentJob::dictionary_kind(foreground_job->request.kind)) {
                foreground_job.reset(); content_loading = false; dictionary_notice = "进度尚未保存，请先关闭词典并重试保存。";
                if (view == View::Dictionary) draw_dictionary(); else draw_dictionary_settings(); overlay(); return;
            }
            if (save_failed) { foreground_job.reset(); content_loading = false; error(kSaveFailed); }
            else schedule_save();
            return;
        }
        const bool mapping = foreground_job->request.kind == ContentJob::Kind::MapDownload ||
                             foreground_job->request.kind == ContentJob::Kind::MapUpload;
        if (mapping && std::chrono::steady_clock::now() < mapping_start_retry) return;
        if (!foreground_job->start()) {
            if (ContentJob::dictionary_kind(foreground_job->request.kind)) {
                foreground_job.reset(); content_loading = false; dictionary_notice = "词典任务无法启动，请稍后重试。";
                if (view == View::Dictionary) draw_dictionary(); else draw_dictionary_settings(); overlay(); return;
            }
            if (mapping) {
                // Initialization or a full worker queue may recover on a later poll.
                // Retries remain bounded and do not block input or repeat HTTP requests.
                if (++mapping_start_attempts < 3) {
                    mapping_start_retry = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                    return;
                }
                const bool memory = foreground_job->start_failure() == ContentJob::StartFailure::InternalMemory;
                foreground_job.reset(); content_loading = false;
                cancel_sync();
                sync_status = memory ? "内部内存不足，无法启动位置准备。请重新检查，本机位置未改动。"
                                     : "位置准备任务创建失败。请重新检查，本机位置未改动。";
                draw_sync(); overlay(); return;
            }
            if (foreground_job->request.kind == ContentJob::Kind::LockScreenPreview) {
                foreground_job.reset(); content_loading = false;
                draw_power("预览任务无法启动，请稍后重试。"); overlay(); return;
            }
            if (foreground_job->request.progressive) {
                foreground_job->result.error = "Image worker unavailable";
                complete_inline_image(*foreground_job); foreground_job.reset(); return;
            }
            if (foreground_job->request.preview && font_picker_open) {
                foreground_job.reset(); content_loading = false;
                draw_typography(false); overlay(); return;
            }
            foreground_job.reset(); content_loading = false;
            error("内容任务无法启动，请返回后重试。");
        }
        return;
    }
    if (!foreground_job->done() && chapter_feedback_after && !display.busy() &&
        std::chrono::steady_clock::now() >= *chapter_feedback_after) {
        // Chapter parsing must acknowledge a checkpoint before the panel starts scanning.
        if (foreground_job->pause()) {
            chapter_feedback_after.reset();
            loading("正在准备章节…");
        }
    }
    if (!chapter_feedback_after && !display.busy()) foreground_job->resume();
    if (!foreground_job->done() || display.busy()) return;
    chapter_feedback_after.reset();
    auto job = std::move(foreground_job);
    const auto& request = job->request;
    if ((!content_loading && !inline_image_pending) || request.generation != foreground_generation) return;
    if (request.progressive) { complete_inline_image(*job); return; }
    if (ContentJob::dictionary_kind(request.kind)) { content_loading = false; finish_dictionary(*job); return; }
    if (request.kind == ContentJob::Kind::LockScreenList) {
        content_loading = false;
        lock_screens = std::move(job->result.lock_screens);
        lock_screen_error = job->result.error;
        draw_lock_screens(); overlay(); return;
    }
    if (request.kind == ContentJob::Kind::LockScreenPreview) {
        content_loading = false; custom_prepare_finished = false;
        if (!job->result.image) { draw_power("图片无法预览，请检查图片或稍后重试。"); overlay(); return; }
        canvas = std::move(*job->result.image);
        view = View::LockScreenPreview; hits.clear();
        canvas.fill_rect(0, 0, 684, 120, 15); header("休眠图片预览", Back, "休眠");
        canvas.fill_rect(0, 1048, 684, 168, 15);
        text(draft.sleep_overlay ? "透明区域以进入预览前画面为底。" : "透明区域显示为白色。", 36, 1054, 24, 612, 1);
        button(Back, "返回", 24, 1104, 636, 88, true);
        overlay(true); return;
    }
    if (request.kind == ContentJob::Kind::ImageViewer) {
        content_loading = false;
        if (request.chapter != chapter_index || request.part != chapter.part || !body_canvas) return;
        if (!job->result.error.empty() || !job->result.failed_images.empty() || !job->result.image) {
            canvas = *body_canvas;
            view = View::Reading;
            message = "图片无法放大";
            footer(); overlay();
            return;
        }
        viewer_original = std::move(job->result.image);
        image_contrast_level = 0;
        draw_image_viewer();
        return;
    }
    if (request.kind == ContentJob::Kind::MapUpload || request.kind == ContentJob::Kind::MapDownload) {
        content_loading = false;
        sync::Config current; std::string detail;
        if (!sync::load_config(root, current, detail) || !same_account(current, sync_config)) {
            cancel_sync(); sync_state = SyncState::ConfigChanged;
        } else if (request.kind == ContentJob::Kind::MapDownload) {
            accept_remote_mapping(job->result.mapped_anchor, job->result.mapped_place, job->result.error);
        } else if (!job->result.error.empty()) {
            upload_mapping_failed(job->result.error);
        } else if (!sync_checked || !request.record || !session.saved_record() || !(*session.saved_record() == *request.record)) {
            cancel_sync();
            sync_status = "阅读位置已经改变，请重新检查。";
        } else { send_upload(*request.record, job->result.xpointer, job->result.progress_fraction); return; }
        draw_sync(); overlay(); return;
    }
    if (request.kind == ContentJob::Kind::Open || request.kind == ContentJob::Kind::Inspect) {
        content_loading = false;
        if (!job->result.error.empty()) {
            if (request.kind == ContentJob::Kind::Inspect) {
                detail_error = job->result.error; draw_details(); overlay();
            } else error(reason(job->result.error, "无法解析文件内容") + "。原书没有被修改。", request.relative_path);
            return;
        }
        const auto& loaded = *job->result.document;
        if (controlled_storage) {
            if (identities.size() >= 8 && !identities.count(request.relative_path)) identities.erase(identities.begin());
            identities[request.relative_path] = {job->result.file_size, job->result.file_modified,
                job->result.file_inode, loaded.metadata()};
        }
        if (request.kind == ContentJob::Kind::Inspect) {
            detail_metadata = loaded.metadata(); detail_progress = job->result.progress;
            detail_size = job->result.file_size; detail_cover = std::move(job->result.image);
            if (detail_progress.record) {
                for (const auto& entry : loaded.toc())
                    if (entry.resource == detail_progress.record->anchor.resource) { detail_position = entry.label; break; }
                if (detail_position.empty()) detail_position = "已保存阅读位置";
            }
            draw_details(); overlay(); return;
        }
        document = std::move(*job->result.document);
        if (request.chapter_session == chapter_session) chapter_stream_bytes = job->result.chapter_stream_bytes;
        finish_open(request.relative_path, &job->result);
        return;
    }
    if (request.kind == ContentJob::Kind::Chapter) {
        finish_chapter(*job);
        return;
    }
    if (request.chapter != chapter_index || request.part != chapter.part) return;
    content_loading = false;
    image_prepared = true;
    prepared_image = std::move(job->result.image);
    prepared_image_failures = std::move(job->result.failed_images);
    prepared_image_error = job->result.error;
#ifdef ESP_PLATFORM
    if (!prepared_image_error.empty())
        ESP_LOGW("reader_app", "Image unavailable: resource=%s error=%s", request.image_resource.c_str(), prepared_image_error.c_str());
#endif
    if (request.preview) { draw_typography(); overlay(); }
    else show_page(request.offset);
    image_prepared = false;
    prepared_image.reset();
    prepared_image_failures.clear();
}


void ReaderApp::Impl::finish_chapter(ContentJob& job) {
    if (memory_error(job.result.error) && (recent_chapter_cache || job.request.content_cache || job.request.chapter_session)) {
        // Retry once without optional retained payloads; the completed job releases its references on return.
        auto retry = job.request;
        retry.content_cache.reset(); retry.content_cache_write_needed = false;
        retry.chapter_session.reset(); chapter_session.reset(); chapter_stream_bytes = 0;
        recent_chapter_cache.reset(); recent_cache_write_needed = false;
        pending_content = std::move(retry);
        return;
    }
    chapter_stream_bytes = job.request.chapter_session == chapter_session ? job.result.chapter_stream_bytes : 0;
    content_loading = false; chapter_feedback_after.reset();
    if (!job.result.error.empty()) { error(chapter_error(job.result.error)); return; }
    if (job.result.retained_cache_hit) {
        chapter_cache = job.request.content_cache;
        chapter_cache_write_needed = job.request.content_cache_write_needed;
    } else if (!job.result.content_cache.bytes.empty()) {
        chapter_cache_write_needed = !job.result.content_cache.from_disk;
        chapter_cache = std::make_shared<ContentCacheEntry>(std::move(job.result.content_cache));
    }
    chapter = std::move(job.result.chapter); chapter_index = job.request.chapter;
    offset = job.result.offset;
    for (const auto& span : job.result.history) record_boundary(computed_pages, span.first, span.second);
    show_page(offset, job.request.target == ContentJob::Target::End || job.request.target == ContentJob::Target::Before);
}


void ReaderApp::Impl::poll_counting_job() {
    if (!counting_job || !counting_job->done()) return;
    auto job = std::move(counting_job);
    const auto& request = job->request;
    const auto& result = job->result;
    if (request.kind == ContentJob::Kind::Chapter) {
        if (request.generation != foreground_generation || request.identity.book_id != document.metadata().book_id) return;
        if (request.chapter_session == chapter_session) chapter_stream_bytes = result.chapter_stream_bytes;
        if (result.error == kChapterCancelled) chapter_prefetch_attempted = false;
        if (result.error.empty() && !result.chapter.text.empty()) {
            prepared_chapter_size_bytes = chapter_result_bytes(*job);
            prepared_chapter = std::move(job);
            if (cache_usage().retained > cache_target) prepared_chapter.reset();
        }
        return;
    }
    if (request.generation != counting_generation) return;
    if (request.kind == ContentJob::Kind::ContentCacheWrite) return;
    // The lock screen reads the cover cache itself; a failed or cancelled attempt falls back to the title.
    if (request.kind == ContentJob::Kind::SleepCover) return;
    if (request.kind == ContentJob::Kind::LockScreen) {
#ifdef ESP_PLATFORM
        if (!result.error.empty())
            ESP_LOGW("reader_app", "Lock screen result: prepare=%d overlay=%d cached=%d image=%d retryable=%d error=%s",
                request.lock_screen_prepare, request.sleep_overlay, result.cache_finished,
                !!result.image, result.retryable_failure, result.error.c_str());
#endif
        if (result.cursor_saved && request.lock_screen_shown == custom_rotation_shown) custom_rotation_shown.clear();
        if (settings.sleep_screen != "custom" || request.sleep_image != settings.sleep_image || request.sleep_overlay != settings.sleep_overlay) return;
        if (request.preview && custom_sleep_attempted && sleep_cover_deadline) {
            custom_sleep_image = std::move(job->result.image);
            custom_rotation_name = settings.sleep_image.empty() && custom_sleep_image ? result.lock_screen : "";
            // A prepared image is shown even when its cache commit failed; only a fallback is reported.
            custom_notice = custom_sleep_image ? "" : result.error.empty() ? "休眠图片尚未就绪" : result.error;
        }
        if (request.lock_screen_prepare) {
            custom_prepare_finished = result.cache_finished || !result.retryable_failure;
            custom_retry_at = result.cache_finished ? std::chrono::steady_clock::time_point{} :
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        return;
    }
    if (request.kind == ContentJob::Kind::PageCacheRead) {
        if (result.cache_finished) pages_cache_pending = false;
        if (job->result.page_cache) adopt_pages(std::move(*job->result.page_cache));
        return;
    }
    if (request.kind == ContentJob::Kind::PageCacheWrite) {
        if (result.cache_finished) pages_dirty = false;
        return;
    }
    if (request.kind == ContentJob::Kind::FontIdentity) {
        if (request.font_scan != font_scan) return;
        if (!result.error.empty()) { pages_cache_disabled = true; font_scan.reset(); }
        else if (!result.font_identity.empty()) {
            font_identity = result.font_identity;
            font_scan.reset();
            load_pages_cache();
        }
        return;
    }
    if (request.chapter != counting_index ||
        request.part != counting_part || request.offset != counting_offset) return;
    if (!result.error.empty()) {
        if (memory_error(result.error)) defer_counting();
        else pages_failed = true;
        return;
    }
    auto& starts = chapter_pages[counting_index][counting_part];
    if (!reserve_page_starts(starts, result.starts.size())) return;
    starts.insert(starts.end(), result.starts.begin(), result.starts.end());
    counting_offset = result.offset;
    if (result.part_done) {
        if (result.last_part) finish_counting();
        else { ++counting_part; counting_offset = 0; add_counting_part(); }
    }
}


} // namespace pico_page
