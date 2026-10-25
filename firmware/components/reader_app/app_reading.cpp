#include "app_impl.h"

namespace pico_page {

bool ReaderApp::Impl::open(std::string path) {
    if (display.busy() || !card_available) return false;
    abandon_content();
    invalidate_counting();
    if (counting_job) counting_job->cancel();
    // A remote position kept as a header reminder belongs to the book being left.
    remote_anchor.reset();
    refresh_badge_config();
    clear_body_cache();
    clear_prefetch();
    if (!safe_existing(root, path)) {
        error("书籍路径无效或包含符号链接");
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
        error(reason(detail, "无法解析文件内容") + "。原书没有被修改。", path);
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
        error("原进度读取失败。为保护阅读记录，已停止打开本书。原书和进度文件都没有被修改。", path);
        return false;
    }
    chapter_index = 0;
    offset = 0;
    message.clear();
    if (!configuration.diagnostics.empty())
        message = "设置文件无效，已回退；原文件未修改";
    else if (!bookmarks_usable())
        message = bookmarks.status == StoreStatus::IdentityMismatch ? "书签与当前原书不符，未使用；原文件已保留"
                                                                    : "书签文件无法读取，已保留原文件；阅读进度不受影响";
    else if (font_fallback)
        message = builtin_body ? kBuiltinFontNotice : kFallbackFontNotice;
    else if (weight_unavailable)
        message = kWeightUnavailableNotice;
    if (restored.status != StoreStatus::Ok && restored.status != StoreStatus::Missing && restored.status != StoreStatus::Cleared)
        message = "原进度无法读取，未覆盖原记录";
    if (loaded) {
        pending_chapter_cache = std::move(loaded->content_cache);
        chapter = std::move(loaded->chapter);
        chapter_index = loaded->chapter_index;
        offset = loaded->offset;
    } else if (restored.record) {
        const auto holds = [&](const Chapter &part) {
            std::uint32_t ignored;
            return part.offset_for(restored.record->anchor, ignored);
        };
        if (!document.chapter_index_for(restored.record->anchor.resource, chapter_index) ||
            !find_content(store, document, chapter_index, chapter, detail, ChapterLoad::Reading, holds) ||
            !chapter.offset_for(restored.record->anchor, restored_offset)) {
            error(memory_error(detail) ? chapter_error(detail) : "保存的位置无法映射，请保留进度文件并检查原书");
            return false;
        }
        offset = restored_offset;
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
    canvas.fill_rect(0, footer_top, 684, 88, 15);
    text(notice().empty() && footer_position == reading_footer::Position::ChapterName ? body : ui,
         notice().empty() ? footer_position_text() : message, 36, 1156, 24, 476, 3);
    footer_charging = battery_charging;
    if (footer_device == reading_footer::Device::Clock) {
        // The system clock is restored from RTC or calibrated by on-demand networking.
        center(reading_footer::clock(std::time(nullptr), 8 * 60), reading_footer::device_left, 1156,
               684 - reading_footer::device_left, 24, 3);
    } else if (footer_device == reading_footer::Device::Battery) {
        const auto level = battery_percent ? std::to_string(*battery_percent) + "%" : "—";
        const int level_width = text_width(level, 24);
        const int level_x = 648 - level_width;
        const int icon_x = level_x - 10 - 35;
        outline(icon_x, 1166, 32, 18, 2, 3);
        canvas.fill_rect(icon_x + 32, 1171, 3, 8, 3);
        if (battery_charging) {
            // Each row stores nine pixels, with the leftmost pixel in bit 8.
            static constexpr std::uint16_t bolt[] = {12, 28, 56, 112, 224, 510, 255, 14, 28, 56, 112, 224, 192};
            for (int y = 0; y < 13; ++y)
                for (int x = 0; x < 9; ++x)
                    if (bolt[y] & (1U << (8 - x))) canvas.pixel(icon_x + 11 + x, 1168 + y, 3);
        } else if (battery_percent) canvas.fill_rect(icon_x + 4, 1170, *battery_percent * 24 / 100, 10, 3);
        text(level, level_x, 1156, 24, level_width, 3);
    }
}


void ReaderApp::Impl::reclaim_caches() {
    if (view != View::Reading || display.busy() || content_loading ||
        (counting_job && (counting_job->request.kind == ContentJob::Kind::SleepCover || counting_job->request.kind == ContentJob::Kind::LockScreen))) return;
    const auto pressure = [&] {
        if (cache_usage().retained > cache_target) return true;
#ifdef ESP_PLATFORM
        return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 1536U * 1024 ||
               heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 512U * 1024;
#else
        return false;
#endif
    };
    if (!pressure()) return;
    if (!pending_chapter_cache.bytes.empty()) { pending_chapter_cache = {}; ++cache_evictions; }
    if (!pressure()) return;
    // The counting session is rebuilt on demand, so it goes before any counted pages.
    if (yield_counting_session()) { defer_counting(); return; }
    const StageTimer timer{"cache_reclaim"};
    if (!covers.empty()) { covers.clear(); ++cache_evictions; }
    bool dropped = false;
    for (std::size_t i = chapter_pages.size(); i-- > 0 && pressure();) {
        if (i == chapter_index || chapter_pages[i].empty()) continue;
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
        // The encoded payload and expanded index are each bounded to kPageIndexBytes.
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 2 * kPageIndexBytes + 256U * 1024 ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < kPageIndexBytes + 64U * 1024) {
            if (yield_counting_session()) return;
            pages_cache_disabled = true; pages_cache_pending = pages_dirty = false;
            return;
        }
#endif
        ContentJob::Request request;
        request.kind = pages_cache_pending ? ContentJob::Kind::PageCacheRead : ContentJob::Kind::PageCacheWrite;
        request.root = root; request.cache_name = pages_name; request.cache_header = pages_header;
        request.generation = counting_generation;
        if (pages_cache_pending) request.spine = spine;
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
        // No job runs here, so the session members may be read. The memory check covers the
        // load of a new part and assumes that the previous part is no longer held.
        const bool part_loaded = counting_session && counting_session->loaded &&
            counting_session->chapter_index == counting_index && counting_session->part == counting_part;
        if (!part_loaded) {
            release_counting();
            if (!counting_memory(spine[counting_index].bytes)) { counting_session.reset(); defer_counting(); return; }
        }
        ContentJob::Request request;
        request.root = root; request.path = root + "/" + book_path;
        request.font = body_font_path();
        request.identity = document.metadata(); request.layout = layout(settings);
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
        if (load_content(store, document, counting_index, counting_chapter, detail, ChapterLoad::Layout, counting_part, stop))
            counting_loaded = true;
        else if (detail == kChapterCancelled)
            return;
        else if (memory_error(detail))
            defer_counting();
        else if (detail.find("limit") != std::string::npos) {
            // An item beyond the parser limits cannot be shown either; it has no pages.
            chapter_pages[counting_index].assign(1, {});
            finish_counting();
        } else
            pages_failed = true;
        return;
    }
    const Chapter &source = in_place ? chapter : counting_chapter;
    const auto spec = layout(settings);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(15);
    do {
        if (stop && stop()) return;
        const auto start = pico_page::readable_offset(source, counting_offset);
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


bool ReaderApp::Impl::draw_page_content(const graphics::Page& planned, const graphics::LayoutSpec& spec, bool progressive) {
    const StageTimer timer{"page_draw"};
    if (!planned.images.empty()) {
        if (image_prepared) {
            if (prepared_image && prepared_image_error.empty()) canvas = *prepared_image;
            for (const auto& box : planned.images)
                if (!prepared_image || !prepared_image_error.empty() ||
                    std::find(prepared_image_failures.begin(), prepared_image_failures.end(), box.begin) != prepared_image_failures.end())
                    draw_image_error(box);
            return graphics::render_page(canvas, body, planned);
        }
        if (async_content && progressive) {
            body_images_pending = true;
            for (const auto& box : planned.images) {
                outline(box.x, box.y + 12, box.width, box.height - 24, 2);
                center("正在准备图片…", box.x + 8, box.y + box.height / 2 - 14, box.width - 16, 24, 3);
            }
            return graphics::render_page(canvas, body, planned);
        }
        const bool cacheable = planned.images.size() == 1 && planned.glyphs.empty() &&
                               planned.images.front().height == spec.height;
        const auto cache_name = cacheable ? image_cache_key(document.metadata().book_id, planned.images.front().resource, spec) : "";
        if (cacheable && body_cache_room()) {
            std::string cached;
            if (store.load_cache(cache_name, canvas.pixels().size() + 32, cached) == StoreStatus::Ok &&
                cached.size() == canvas.pixels().size() + 32) {
                const auto digest = cached.substr(cached.size() - 32);
                cached.resize(cached.size() - 32);
                if (md5_hex(cached) == digest) {
                    std::memcpy(canvas.data(), cached.data(), cached.size());
                    return true;
                }
            }
        }
        bool all_drawn = true;
        for (const auto& box : planned.images) {
            // Foreground images are prepared by a worker; overlays do not start a decoder.
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
    return graphics::render_page(canvas, body, planned);
}


bool ReaderApp::Impl::render_body(std::size_t start) {
    const StageTimer timer{"body_draw"};
    const auto spec = layout(settings, link_return ? 144 : 64);
    if (body_reusable(start, spec)) {
        canvas = *body_canvas;
        const bool marked = bookmarked(page.start, page.end);
        if (body_bookmarked != marked) {
            // The ribbon sits in the blank top margin, above text and image layout.
            canvas.fill_rect(bookmark_cell_left + 38, 0, 24, 44, 15);
            if (marked) bookmark_ribbon(bookmark_cell_left + 50, 0, 44, true);
            *body_canvas = canvas;
            body_bookmarked = marked;
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
        error("本页无法排版。" + reason(candidate.error) + "。");
        return false;
    }
    // No face covers every character; the page stays readable and the missing ones draw
    // as the placeholder box. The notice applies to this page alone.
    const bool missing = std::any_of(candidate.glyphs.begin(), candidate.glyphs.end(),
        [this](const auto &glyph) { return body.missing(glyph.scalar); });
    if (missing) message = kMissingNotice;
    else if (message == kMissingNotice) message.clear();
    canvas.clear();
    body_images_pending = false;
    if (bitmap) {
        canvas = std::move(*bitmap);
        ++bitmap_hits;
    }
    else if (!draw_page_content(candidate, spec, true)) {
        error("正文绘制失败。" + reason(body.error()) + "。");
        return false;
    }
    page = std::move(candidate);
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
                    body.set_pixel_size(glyph.font_px);
                    right = std::max(right, glyph.x + body.advance(glyph.scalar));
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
        text("‹ 返回原位置", 28, 30, 28, 612);
        canvas.line(0, 102, 683, 102, 1, 3);
        links.push_back({0, 0, 684, 104, LinkReturn});
    }
    if (bookmarked(page.start, page.end)) bookmark_ribbon(bookmark_cell_left + 50, 0, 44, true);
    if (body_cache_room()) {
        body_canvas = canvas; body_spec = spec; body_links = links;
        body_bookmarked = bookmarked(page.start, page.end);
    }
    else clear_body_cache();
    return true;
}


bool ReaderApp::Impl::show_page(std::size_t start, bool reverse_page_effect) {
    const auto spec = layout(settings, link_return ? 144 : 64);
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
    if (!render_body(start))
        return false;
    Anchor anchor;
    std::size_t p = page.start;
    while (p < page.end && !chapter.anchor_at(static_cast<std::uint32_t>(p), anchor))
        ++p;
    if (p == page.end) {
        error("本页没有可保存的来源位置");
        return false;
    }
    view = View::Reading;
    hits.clear();
    const auto token = session.request_page(
        anchor, (settings.refresh_preset == "clear" ||
                 body_updates >= static_cast<unsigned>(refresh_interval(settings)))
                    ? RefreshIntent::FullClean
                    : RefreshIntent::Content, reverse_page_effect);
    inline_image_after = std::chrono::steady_clock::time_point::max();
    if (!session.layout_ready(token))
        return false;
    return true;
}


void ReaderApp::Impl::turn(bool forward) {
    if (!has_book())
        return;
    turned = true;
    // The font notice is shown on the page where the book opened; later pages show the position.
    if (message == kBuiltinFontNotice || message == kFallbackFontNotice || message == kWeightUnavailableNotice) message.clear();
    if (forward) {
        const auto next_offset = readable_offset(page.end);
        if (next_offset < chapter.text.size()) {
            expire_sync_return();
            show_page(next_offset);
        } else if (!chapter.last_part) {
            expire_sync_return();
            const auto next = chapter.part + 1;
            if (async_content) { begin_chapter(chapter_index, next, ContentJob::Target::Start); return; }
            change_part(chapter_index, [this, next] {
                if (load_chapter(chapter_index, next)) show_page(0);
            });
        } else if (chapter_index + 1 < document.spine().size()) {
            auto next = chapter_index + 1;
            while (next < document.spine().size() && !document.spine()[next].linear)
                ++next;
            if (next < document.spine().size()) expire_sync_return();
            if (async_content && next < document.spine().size()) { begin_chapter(next, 0, ContentJob::Target::Start); return; }
            if (next < document.spine().size() && load_chapter(next))
                show_page(0);
        } else {
            message = "已到全书末尾";
            draw_tools();
            overlay();
        }
    } else {
        if (offset == readable_offset(0)) {
            if (chapter.part > 0) {
                expire_sync_return();
                const auto previous = chapter.part - 1;
                if (async_content) { begin_chapter(chapter_index, previous, ContentJob::Target::End); return; }
                change_part(chapter_index, [this, previous] {
                    if (load_chapter(chapter_index, previous)) show_page_before(chapter.text.size());
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
            if (async_content) { begin_chapter(previous_chapter, kLastChapterPart, ContentJob::Target::End); return; }
            if (document.spine()[previous_chapter].bytes > announced_bytes) {
                defer("正在排版…", [this, previous_chapter] {
                    if (load_chapter(previous_chapter, kLastChapterPart)) show_page_before(chapter.text.size());
                });
                return;
            }
            if (!load_chapter(previous_chapter, kLastChapterPart))
                return;
            turn_back_to(chapter.text.size());
        } else {
            expire_sync_return();
            turn_back_to(offset);
        }
    }
}


} // namespace pico_page
