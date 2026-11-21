#include "app_impl.h"

namespace pico_page {

bool ReaderApp::Impl::open(std::string path) {
    if (display.busy() || !card_available) return false;
    abandon_content();
    invalidate_counting();
    if (counting_job) counting_job->cancel();
    remote_anchor.reset();
    refresh_badge_config();
    clear_body_cache();
    clear_prefetch();
    if (!safe_existing(root, path)) {
        error(tr("书籍路径无效或包含符号链接"));
        return false;
    }
    if (has_book()) {
        if (async_content) { remember_position(); schedule_save(); }
        else if (!persist()) { error(message); return false; }
        else remember_position();
    }
    save_pages();
    if (async_content) return begin_open(path);
    std::string detail;
    if (!open_document(path, detail)) {
        book_path.clear();
        session.open("", "", writer);
        open_failed(detail, path);
        return false;
    }
    return finish_open(path);
}

bool ReaderApp::Impl::finish_open(const std::string& path, ContentJob::Result* loaded) {
    std::string detail;
    cancel_sync();
    link_return.reset();
    confirmed_link_return.reset();
    sync_return = confirmed_sync_return = false;
    book_path = path;
    author_fonts.clear(); author_fonts_released = false; author_fonts_stable = true;
    last_book = path;
    last_recorded.clear();
    recent_save_failed = false;
    const auto configuration = loaded ? loaded->configuration : store.load_settings(path);
    settings = configuration.settings;
    load_fonts();
    clear_page_history();
    reset_pages();
    load_bookmarks(loaded ? &loaded->bookmarks : nullptr);
    contents_bookmarks = false;
    const auto restored =
        loaded ? loaded->progress : store.load_progress(path, document.metadata().book_id, revision_for(path, document.metadata()));
    session.open(document.metadata().book_id, revision_for(path, document.metadata()), writer, restored.record);
    if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing && restored.status != StoreStatus::Cleared) {
        error(tr("原进度读取失败。为保护阅读记录，已停止打开本书。原书和进度文件都没有被修改。"), path);
        return false;
    }
    chapter_index = 0;
    offset = 0;
    message.clear();
    if (!configuration.diagnostics.empty())
        message = trf("{problem}，本次使用默认值；原文件未修改", {{"problem", settings_file_problem(configuration.diagnostics)}});
    else if (!bookmarks_usable())
        message = bookmarks.status == StoreStatus::IdentityMismatch ? tr("书签与当前原书不符，未使用；原文件已保留")
                                                                    : tr("书签文件无法读取，已保留原文件；阅读进度不受影响");
    else if (font_fallback)
        message = builtin_body ? tr(kBuiltinFontNotice) : tr(kFallbackFontNotice);
    else if (weight_unavailable)
        message = tr(kWeightUnavailableNotice);
    if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing && restored.status != StoreStatus::Cleared)
        message = tr("原进度无法读取，未覆盖原记录");
    if (loaded) {
        adopt_chapter_cache(std::move(loaded->content_cache));
        chapter = std::move(loaded->chapter);
        author_fonts = std::move(loaded->author_fonts); author_fonts_stable = loaded->author_fonts_stable;
        chapter_index = loaded->chapter_index;
        offset = loaded->offset;
    } else if (restored.record) {
        if (!document.chapter_index_for(restored.record->anchor.resource, chapter_index) ||
            !load_anchored_content(store, document, chapter_index, chapter, detail, reading_detail(),
                                   restored.record->anchor, restored_offset)) {
            error(memory_error(detail) ? chapter_error(detail) : tr("保存的位置无法映射，请保留进度文件并检查原书"));
            return false;
        }
        offset = restored_offset;
        author_fonts = prepare_embedded_fonts(store, document, chapter, author_fonts_stable);
    } else {
        std::size_t first = 0;
        while (first + 1 < document.spine().size() && !document.spine()[first].linear)
            ++first;
        if (!load_chapter(first))
            return false;
    }
    return show_page(offset);
}

void ReaderApp::Impl::footer() {
    if (settings.fullscreen_reading) {
        canvas.fill_rect(0, fullscreen_notice_top, 684, 1216 - fullscreen_notice_top, 15);
        const auto& status = notice();
        if (!status.empty()) text(status, 36, top_for_middle(ui, 18, 1204), 18, 612, 3);
        return;
    }
    canvas.fill_rect(0, footer_top, 684, 88, 15);
    text(notice().empty() && footer_position == reading_footer::Position::ChapterName ? body : ui,
         notice().empty() ? footer_position_text() : message, 36, 1156, 24, 476, 3);
    footer_charging = battery_charging;
    if (footer_device == reading_footer::Device::Clock) {
        center(reading_footer::clock(std::time(nullptr), 8 * 60), reading_footer::device_left, 1156,
               684 - reading_footer::device_left, 24, 3);
    } else if (footer_device == reading_footer::Device::Battery) {
        draw_battery_icon(623, 1175);
    }
}

void ReaderApp::Impl::draw_battery_icon(int center_x, int center_y) {
    if (!battery_charging && !battery_percent) return;
    constexpr int width = 46, height = 22, terminal_width = 3;
    const int x = center_x - (width + terminal_width) / 2, y = center_y - height / 2;
    outline(x, y, width, height, 2, 3);
    canvas.fill_rect(x + width, center_y - 4, terminal_width, 8, 3);
    if (battery_charging) {
        static constexpr std::uint16_t bolt[] = {12, 28, 56, 112, 224, 510, 255, 14, 28, 56, 112, 224, 192};
        for (int row = 0; row < 13; ++row)
            for (int col = 0; col < 9; ++col)
                if (bolt[row] & (1U << (8 - col))) canvas.pixel(x + (width - 9) / 2 + col, y + 4 + row, 3);
    } else {
        const int filled_cells = (*battery_percent + 19) / 20;
        for (int cell = 0; cell < filled_cells; ++cell)
            canvas.fill_rect(x + 4 + cell * 8, y + 4, 6, 14, 5);
    }
}

void ReaderApp::Impl::reclaim_caches() {
    if (view != View::Reading || display.busy() || content_loading ||
        (counting_job && (counting_job->request.kind == ContentJob::Kind::SleepCover || counting_job->request.kind == ContentJob::Kind::LockScreen))) return;
    auto page_bytes = page_index_bytes();
    const auto pressure = [&] {
        if (cache_usage(page_bytes).retained > cache_target) return true;
        PP_HOST_WORK(if (host_work::memory_pressure) return true;)
#ifdef ESP_PLATFORM
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1536U * 1024 ||
               heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 512U * 1024;
#else
        return false;
#endif
    };
    if (!pressure()) return;
    if (chapter_session) {
        release_chapter_session(); ++cache_evictions;
        if (!pressure()) return;
    }
    if (prepared_chapter) {
        prepared_chapter.reset(); ++cache_evictions;
        if (!pressure()) return;
    }
    if (recent_chapter_cache) {
        recent_chapter_cache.reset(); recent_cache_write_needed = false; ++cache_evictions;
        if (!pressure()) return;
    }
    if (chapter_cache) {
        chapter_cache.reset(); chapter_cache_write_needed = false; ++cache_evictions;
        if (!pressure()) return;
    }
    if (yield_counting_session()) { defer_counting(); return; }
    const StageTimer timer{"cache_reclaim"};
    if (!covers.empty()) { covers.clear(); ++cache_evictions; }
    bool dropped = false;
    constexpr std::size_t pressure_check_bytes = 16 * 1024;
    std::size_t reclaimed_since_check = pressure_check_bytes;
    for (std::size_t i = chapter_pages.size(); i-- > 0;) {
        PP_HOST_WORK(host_work::add(host_work::ReclaimVisits);)
        if (i == chapter_index || chapter_pages[i].empty()) continue;
        // ESP-IDF walks the heap to find the largest free block.
        if (reclaimed_since_check >= pressure_check_bytes) {
            if (!pressure()) break;
            reclaimed_since_check = 0;
        }
        const auto bytes = chapter_page_bytes(chapter_pages[i]);
        reclaimed_since_check += bytes;
        page_bytes -= bytes;
        decltype(chapter_pages)::value_type().swap(chapter_pages[i]);
        counted[i] = !document.spine()[i].linear; counted_live[i] = false;
        dropped = true; ++cache_evictions;
    }
    if (dropped) {
        invalidate_counting(); release_counting(); counting_index = not_counting;
        defer_counting(); pages_cache_pending = pages_dirty = false;
        pages_complete = std::find(counted.begin(), counted.end(), false) == counted.end();
    }
    if (pressure() && (next_page || next_bitmap)) {
        clear_prefetch(); prefetch_done = true; ++cache_evictions;
    }
    if (pressure()) {
        if (counting_job && counting_job->request.generation == counting_generation) {
            invalidate_counting(); release_counting(); counting_index = not_counting;
        }
        const auto before = ui.cached_glyph_bytes() + body.cached_glyph_bytes() + library_font.cached_glyph_bytes();
        library_font.trim_glyph_cache(0); ui.trim_glyph_cache(128U * 1024); body.trim_glyph_cache(256U * 1024);
        if (ui.cached_glyph_bytes() + body.cached_glyph_bytes() + library_font.cached_glyph_bytes() < before) ++cache_evictions;
        defer_counting();
    }
}

void ReaderApp::Impl::count_pages(const std::function<bool()>& stop) {
    const StageTimer timer{"count_slice"};
    if (stop && stop()) return;
    const auto &spine = document.spine();
    counting_blocked = not_counting;
    if (async_content && font_identity.empty() && !pages_cache_disabled && (font_scan_turn || pages_complete)) {
        if (!counting_job) {
            if (!font_scan) font_scan = std::make_shared<FileDigest>(body_font_path());
            ContentJob::Request request;
            request.font = body_font_path(); request.kind = ContentJob::Kind::FontIdentity;
            request.font_scan = font_scan;
            request.generation = counting_generation;
            counting_job = std::make_unique<ContentJob>(std::move(request));
            if (!counting_job->start()) { counting_job.reset(); defer_counting(); }
            else font_scan_turn = false;
        }
        return;
    }
    if (async_content && !font_identity.empty() && !pages_cache_disabled && (pages_cache_pending || pages_dirty)) {
        if (counting_job) return;
#ifdef ESP_PLATFORM
        if (!pages_cache_pending && !chapter_memory_available(2 * page_index_bytes() + 64U * 1024)) {
            if (yield_counting_session()) return;
            pages_cache_disabled = true; pages_cache_pending = pages_dirty = false;
            return;
        }
#endif
        ContentJob::Request request;
        request.kind = pages_cache_pending ? ContentJob::Kind::PageCacheRead : ContentJob::Kind::PageCacheWrite;
        request.root = root; request.cache_name = pages_name; request.cache_header = pages_header;
        request.generation = counting_generation;
        if (pages_cache_pending) request.page_linear = page_linearity(spine);
        else request.page_cache = PageCache{chapter_pages, counted};
        counting_job = std::make_unique<ContentJob>(std::move(request));
        if (!counting_job->start()) {
            counting_job.reset(); pages_cache_disabled = true; pages_cache_pending = pages_dirty = false;
        }
        return;
    }
    if (pages_complete) return;
    if (counting_index == not_counting) {
        auto next = static_cast<std::size_t>(std::find(counted.begin(), counted.end(), false) - counted.begin());
        if (chapter_index < spine.size() && !counted[chapter_index] && chapter.resource == spine[chapter_index].resource)
            next = chapter_index;
        counting_index = next;
        counting_part = 0;
        counting_offset = 0;
        chapter_pages[next].clear();
        if (!add_counting_part()) return;
    }
    if (async_content) {
        if (counting_job) return;
        font_scan_turn = true;
        const bool part_loaded = counting_session && counting_session->loaded &&
            counting_session->chapter_index == counting_index && counting_session->part == counting_part;
        if (!part_loaded) {
            release_counting();
            if (!counting_memory(spine[counting_index].bytes)) { counting_session.reset(); defer_counting(); return; }
        }
        ContentJob::Request request;
        request.root = root; request.path = root + "/" + book_path;
        request.font = body_font_path();
        request.identity = document.metadata(); request.layout = reading_layout(settings);
        request.book_index = verified_reader_index();
        request.generation = counting_generation; request.chapter = counting_index;
        request.part = counting_part; request.offset = counting_offset;
        if (!counting_session) counting_session = std::make_shared<PaginationSession>();
        request.session = counting_session;
        counting_job = std::make_unique<ContentJob>(std::move(request));
        if (!counting_job->start()) { counting_job.reset(); defer_counting(); }
        return;
    }
    const bool in_place = counting_index == chapter_index && counting_part == chapter.part &&
                          chapter.resource == spine[counting_index].resource;
    if (in_place) release_counting();
    else if (!counting_loaded || counting_chapter.part != counting_part ||
             counting_chapter.resource != spine[counting_index].resource) {
        release_counting();
        if (!counting_memory(spine[counting_index].bytes)) {
            defer_counting();
            return;
        }
        std::string detail;
        const StageTimer load_timer{"chapter_counting"};
        if (load_content(store, document, counting_index, counting_chapter, detail, reading_detail() == ChapterLoad::Original ? ChapterLoad::Original : ChapterLoad::Layout, counting_part, stop)) {
            bool stable;
            counting_fonts = prepare_embedded_fonts(store, document, counting_chapter, stable, stop);
            if (stop && stop()) return;
            if (!stable) { pages_failed = true; return; }
            counting_loaded = true;
        } else if (detail == kChapterCancelled)
            return;
        else if (memory_error(detail))
            defer_counting();
        else if (detail.find("limit") != std::string::npos) {
            chapter_pages[counting_index].assign(1, {});
            finish_counting();
        } else
            pages_failed = true;
        return;
    }
    const Chapter &source = in_place ? chapter : counting_chapter;
    const auto spec = reading_layout(settings);
    if (source.author_style_fallback) { pages_failed = true; return; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
    do {
        if (stop && stop()) return;
        const auto start = pico_page::readable_offset(source, counting_offset, spec.original_epub);
        if (start >= source.text.size()) {
            if (source.last_part) {
                finish_counting();
                return;
            }
            ++counting_part;
            counting_offset = 0;
            if (!add_counting_part()) return;
            release_counting();
            return;
        }
        const auto planned = plan_page(source, start, spec, false, stop);
        if (planned.error == "Layout cancelled") return;
        if (!planned.error.empty() || planned.end <= planned.start) {
            pages_failed = true;
            release_counting();
            return;
        }
        if (!reserve_page_starts(chapter_pages[counting_index][counting_part], 1)) return;
        chapter_pages[counting_index][counting_part].push_back(static_cast<std::uint32_t>(planned.start));
        counting_offset = planned.end;
    } while (std::chrono::steady_clock::now() < deadline);
}

bool ReaderApp::Impl::draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec, bool progressive,
                                         const graphics::AuthorFonts* fonts) {
    const StageTimer timer{"page_draw", 0};
    if (!planned.images.empty()) {
        if (image_prepared) {
            if (prepared_image && prepared_image_error.empty()) canvas = *prepared_image;
            for (const auto& box : planned.images)
                if (!prepared_image || !prepared_image_error.empty() ||
                    std::find(prepared_image_failures.begin(), prepared_image_failures.end(), box.begin) != prepared_image_failures.end())
                    draw_image_error(box);
            return graphics::render_page(canvas, body, planned, 1, {}, fonts);
        }
        if (async_content && progressive) {
            body_images_pending = true;
            for (const auto& box : planned.images) {
                outline(box.x, box.y + 12, box.width, box.height - 24, 2);
                center(tr("正在准备图片…"), box.x + 8, box.y + box.height / 2 - 14, box.width - 16, 24, 3);
            }
            return graphics::render_page(canvas, body, planned, 1, {}, fonts);
        }
        const auto cache_name = spec.original_epub && view == View::Typography ? std::string{} :
            page_image_cache_key(document.metadata().book_id, planned, spec);
        const bool cacheable = !cache_name.empty();
        if (cacheable && body_cache_room()) {
            std::string cached;
            if (store.load_cache(cache_name, canvas.pixels().size() + 32, cached) == StoreStatus::Ok &&
                cached.size() == canvas.pixels().size() + 32) {
                const auto digest = cached.substr(cached.size() - 32);
                cached.resize(cached.size() - 32);
                if (md5_hex(cached) == digest) {
                    std::memcpy(canvas.data(), cached.data(), cached.size());
                    return graphics::render_page(canvas, body, planned, 1, {}, fonts);
                }
            }
        }
        bool all_drawn = true;
        for (const auto& box : planned.images) {
            if (async_content) { draw_image_error(box); all_drawn = false; continue; }
            std::vector<std::uint8_t> encoded;
            std::string detail;
            if (box.resource.empty() || !document.read_image(box.resource, encoded, detail) ||
                !graphics::render_image(canvas, encoded, box.x, box.y, box.width, box.height, detail)) {
                draw_image_error(box);
                all_drawn = false;
            }
        }
        if (cacheable && all_drawn && body_cache_room()) {
                std::string pixels(reinterpret_cast<const char*>(canvas.data()), canvas.pixels().size());
                pixels += md5_hex(pixels);
                store.save_cache(cache_name, pixels);
        }
    }
    return graphics::render_page(canvas, body, planned, 1, {}, fonts);
}

bool ReaderApp::Impl::render_body(std::size_t start) {
    const StageTimer timer{"body_draw"};
    const auto spec = reading_layout(settings, link_return.has_value());
    const bool cached_page_marked = bookmarked(page.start, page.end);
    if (body_reusable(start, spec) && (!settings.fullscreen_reading || body_bookmarked == cached_page_marked)) {
        canvas = *body_canvas;
        if (body_bookmarked != cached_page_marked) {
            canvas.fill_rect(bookmark_cell_left + 38, 0, 24, 44, 15);
            if (cached_page_marked) bookmark_ribbon(bookmark_cell_left + 50, 0, 44, true);
            *body_canvas = canvas;
            body_bookmarked = cached_page_marked;
        }
        links = body_links;
        offset = page.start;
        footer();
        return true;
    }
    const bool same_spec = same_layout(next_spec, spec);
    const bool prefetched = next_page && same_spec && next_page->start == readable_offset(start);
    auto candidate = prefetched ? std::move(*next_page) : plan_page(start, spec);
    auto bitmap = prefetched ? std::move(next_bitmap) : nullptr;
    clear_prefetch();
    if (!candidate.error.empty() || candidate.end <= candidate.start) {
        error(trf("本页无法排版。{reason}。", {{"reason", reason(candidate.error)}}));
        return false;
    }
    const bool missing = std::any_of(candidate.glyphs.begin(), candidate.glyphs.end(),
        [this](const auto &glyph) { return !glyph.font_slot && body.missing(glyph.scalar); });
    if (missing) message = tr(kMissingNotice);
    else if (message == tr(kMissingNotice)) message.clear();
    canvas.clear();
    body_images_pending = false;
    if (bitmap) {
        canvas = std::move(*bitmap);
        ++bitmap_hits;
    }
    else if (!draw_page_content(candidate, spec, true, &author_fonts)) {
#ifdef ESP_PLATFORM
        ESP_LOGW("reader_app", "Body rendering failed: font=%s size_px=%d weight=%s chapter=%u part=%u start=%u; %s",
                 effective_font.c_str(), body.pixel_size(), settings.font_weight.c_str(),
                 static_cast<unsigned>(chapter_index), static_cast<unsigned>(chapter.part),
                 static_cast<unsigned>(candidate.start), body.error().c_str());
#endif
        error(trf("正文绘制失败。{reason}。", {{"reason", reason(body.error(), tr("绘制失败原因未记录（F00）"))}}));
        return false;
    }
    page = std::move(candidate);
#if defined(ESP_PLATFORM) && defined(CONFIG_PICO_PAGE_DIAGNOSTIC_CONSOLE)
    if (spec.original_epub) {
        const auto embedded = std::count_if(page.glyphs.begin(), page.glyphs.end(),
            [](const auto& glyph) { return glyph.font_slot != 0; });
        ESP_LOGI("epub_font", "page_glyphs=%u embedded_glyphs=%u stable=%d",
            static_cast<unsigned>(page.glyphs.size()), static_cast<unsigned>(embedded), author_fonts_stable);
    }
#endif
    offset = page.start;
    footer();
    links.clear();
    for (std::size_t i = 0; i < chapter.links.size(); ++i) {
        const auto &link = chapter.links[i];
        if (link.end <= page.start || link.begin >= page.end)
            continue;
        for (const auto &line : page.lines) {
            int left = 684, right = 0;
            for (const auto &glyph : page.glyphs)
                if (glyph.baseline == line.baseline && glyph.index >= link.begin &&
                    glyph.index < link.end) {
                    left = std::min(left, glyph.x);
                    right = std::max(right, glyph.x + glyph.advance);
                }
            if (right > left) {
                canvas.line(left, line.bottom, right, line.bottom, 3);
                links.push_back({left, line.top, right - left, line.bottom - line.top + 2,
                                 LinkBase + static_cast<int>(i)});
            }
        }
    }
    if (link_return) {
        canvas.fill_rect(0, 0, 684, 104, 15);
        text(tr("‹ 返回原位置"), 28, 30, 28, 612);
        canvas.line(0, 102, 683, 102, 1, 3);
        links.push_back({0, 0, 684, 104, LinkReturn});
    }
    const bool marked = bookmarked(page.start, page.end);
    if (marked) bookmark_ribbon(bookmark_cell_left + 50, 0, page_bookmark_height(), true);
    if (body_cache_room()) {
        body_canvas = canvas; body_spec = spec; body_links = links;
        body_bookmarked = marked;
    }
    else clear_body_cache();
    return true;
}

bool ReaderApp::Impl::show_page(std::size_t start, bool reverse_page_effect, PageTurnOrigin origin) {
    const auto spec = reading_layout(settings, link_return.has_value());
    const bool reused = body_reusable(start, spec);
    const bool hit = next_page && next_bitmap && next_page->start == readable_offset(start) && same_layout(next_spec, spec);
    const StageTimer timer{hit ? "body_prepare_hit" : reused ? "body_prepare_reuse" : "body_prepare_miss", 0};
    if (display.busy())
        return false;
    load_fonts();
    if (inline_image_pending) {
        const auto& request = pending_content ? *pending_content : foreground_job->request;
        if (request.offset != readable_offset(start) || request.chapter != chapter_index ||
            request.part != chapter.part || !same_layout(request.layout, spec)) abandon_content();
    }
    if (settings.original_epub) {
        if (!author_fonts_stable && !pages_failed) {
            reset_pages();
            pages_failed = true;
            clear_page_history();
        }
        if (chapter.author_style_fallback) {
            if (!pages_failed) { reset_pages(); pages_failed = true; }
            message = tr("原书样式不可用，本段使用基本排版");
        }
        start = restore_readable_offset(chapter, start);
    }
    if (!render_body(start))
        return false;
    Anchor anchor;
    std::size_t p = page.start;
    while (p < page.end && !chapter.anchor_at(static_cast<std::uint32_t>(p), anchor))
        ++p;
    if (p == page.end) {
        error(tr("本页没有可保存的来源位置"));
        return false;
    }
    view = View::Reading;
    hits.clear();
    const auto token = session.request_page(
        anchor, (settings.refresh_preset == "clear" ||
                 (refresh_interval(settings) > 0 &&
                  body_updates >= static_cast<unsigned>(refresh_interval(settings))))
                    ? RefreshIntent::FullClean
                    : RefreshIntent::Content, reverse_page_effect, origin);
    inline_image_after = std::chrono::steady_clock::time_point::max();
    if (!session.layout_ready(token))
        return false;
    return true;
}

void ReaderApp::Impl::turn(bool forward, PageTurnOrigin origin) {
    if (!has_book())
        return;
    turned = true;
    if (message == tr(kBuiltinFontNotice) || message == tr(kFallbackFontNotice) || message == tr(kWeightUnavailableNotice)) message.clear();
    if (forward) {
        const auto next_offset = readable_offset(page.end);
        if (next_offset < chapter.text.size()) {
            expire_sync_return();
            show_page(next_offset, false, origin);
        } else if (!chapter.last_part) {
            expire_sync_return();
            const auto next = chapter.part + 1;
            if (async_content) { begin_chapter(chapter_index, next, ContentJob::Target::Start, 0, origin); return; }
            change_part(chapter_index, [this, next, origin] {
                if (load_chapter(chapter_index, next)) show_page(0, false, origin);
            });
        } else if (chapter_index + 1 < document.spine().size()) {
            auto next = chapter_index + 1;
            while (next < document.spine().size() && !document.spine()[next].linear)
                ++next;
            if (next < document.spine().size()) expire_sync_return();
            if (async_content && next < document.spine().size()) { begin_chapter(next, 0, ContentJob::Target::Start, 0, origin); return; }
            if (next < document.spine().size() && load_chapter(next))
                show_page(0, false, origin);
        } else {
            message = tr("已到全书末尾");
            draw_tools();
            overlay();
        }
    } else {
        if (offset == readable_offset(0)) {
            if (chapter.part > 0) {
                expire_sync_return();
                const auto previous = chapter.part - 1;
                if (async_content) { begin_chapter(chapter_index, previous, ContentJob::Target::End, 0, origin); return; }
                change_part(chapter_index, [this, previous, origin] {
                    if (load_chapter(chapter_index, previous)) show_page_before(chapter.text.size(), origin);
                });
                return;
            }
            if (chapter_index == 0)
                return;
            auto previous_chapter = chapter_index - 1;
            while (previous_chapter > 0 && !document.spine()[previous_chapter].linear)
                --previous_chapter;
            if (!document.spine()[previous_chapter].linear)
                return;
            expire_sync_return();
            if (async_content) { begin_chapter(previous_chapter, kLastChapterPart, ContentJob::Target::End, 0, origin); return; }
            if (document.spine()[previous_chapter].bytes > announced_bytes) {
                defer(tr("正在排版…"), [this, previous_chapter, origin] {
                    if (load_chapter(previous_chapter, kLastChapterPart)) show_page_before(chapter.text.size(), origin);
                });
                return;
            }
            if (!load_chapter(previous_chapter, kLastChapterPart))
                return;
            turn_back_to(chapter.text.size(), origin);
        } else {
            expire_sync_return();
            turn_back_to(offset, origin);
        }
    }
}

}
