#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::scan_library() {
    entries.clear();
    folder_root = false;
    library_home_root = settings.library_roots.size() == 1 ? settings.library_roots.front() : "";
    library_directory_unavailable = false;
    if (!card_available)
        return;
    const auto cfg = store.load_settings();
    library_sort = cfg.settings.library_sort;
    folders_first = cfg.settings.library_folders == "first";
    const auto &roots = cfg.settings.library_roots;
    library_home_root = roots.size() == 1 ? roots.front() : "";
    folder_root = std::find(roots.begin(), roots.end(), folder) != roots.end();
    const auto listing = folder.empty() ? library_home_root : folder;
    if (listing.empty()) {
        for (const auto &path : roots) {
            struct stat st{};
            if (safe_existing(root, path) && stat((root + "/" + path).c_str(), &st) == 0 &&
                S_ISDIR(st.st_mode))
                entries.push_back({path, basename(path), true});
        }
    } else {
        DIR *dir = safe_existing(root, listing) ? opendir((root + "/" + listing).c_str()) : nullptr;
        if (!dir) {
            library_directory_unavailable = true;
            message = "无法读取目录，请检查 TF 卡";
            return;
        }
        while (const auto *item = readdir(dir)) {
            const std::string name = item->d_name;
            if (name.empty() || name[0] == '.' || name.size() > 240 ||
                name.find(".picopage") != std::string::npos)
                continue;
            const auto path = listing + "/" + name;
            struct stat st{};
#ifdef ESP_PLATFORM
            const int result = stat((root + "/" + path).c_str(), &st);
#else
            const int result = lstat((root + "/" + path).c_str(), &st);
#endif
            if (result != 0)
                continue;
            if (S_ISDIR(st.st_mode) || (S_ISREG(st.st_mode) && book_name(name)))
                entries.push_back({path, name, S_ISDIR(st.st_mode)});
            if (entries.size() >= 512) {
                message = "当前目录仅显示前 512 项，请分目录存放";
                break;
            }
        }
        closedir(dir);
    }
    std::string history_error;
    const auto recent = library_sort == "recent" ? store.load_recent_books(history_error) : std::vector<RecentBook>{};
    library_recent = library_sort == "recent" && history_error.empty();
    if (library_sort == "recent" && !history_error.empty()) message = "最近阅读记录无法读取，暂按书名排序";
    for (auto& entry : entries) {
        for (std::size_t i = 0; i < recent.size(); ++i) {
            if (recent[i].path == entry.path || (entry.directory && recent[i].path.compare(0, entry.path.size() + 1, entry.path + "/") == 0)) {
                entry.recent_rank = i; entry.opened_at = recent[i].opened_at; break;
            }
        }

    }
    library_resume_omitted = false;
    const auto resume = has_book() ? book_path : last_book;
    resume_opened_at = 0;
    for (const auto& item : recent) if (item.path == resume) { resume_opened_at = item.opened_at; break; }
    if (library_recent && (folder.empty() || folder_root) && !resume.empty()) {
        const auto end = std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return !entry.directory && entry.path == resume;
        });
        library_resume_omitted = end != entries.end(); entries.erase(end, entries.end());
    }
    std::sort(entries.begin(), entries.end(), [this](const Entry &a, const Entry &b) {
        if ((folders_first || !library_recent) && a.directory != b.directory) return a.directory > b.directory;
        if (library_recent && a.recent_rank != b.recent_rank) return a.recent_rank < b.recent_rank;
        if (library_recent && a.directory != b.directory) return a.directory > b.directory;
        return library_sort == "name_desc" ? a.name > b.name : a.name < b.name;
    });
    library_page = std::min(library_page, library_pages() - 1);
}


void ReaderApp::Impl::describe(Entry &entry) {
    if (entry.described) return;
    entry.described = true;
    if (entry.directory) {
        int books = 0;
        if (DIR *dir = opendir((root + "/" + entry.path).c_str())) {
            while (const auto *item = readdir(dir))
                if (item->d_name[0] != '.' && book_name(item->d_name)) ++books;
            closedir(dir);
        }
        entry.status = "文件夹 · " + std::to_string(books) + " 本";
        return;
    }
    entry.status = recalled_position(entry.path).second;
    if (!entry.status.empty()) return;
    // Without a cache entry only the existence of a position is known. A cleared record is
    // a small file that names its state.
    char record[512] = {};
    std::size_t length = 0;
    bool unreadable = false;
    for (const auto* suffix : {"/progress.json", "/progress.json.bak"}) {
        if (std::FILE *file = std::fopen((root + "/" + entry.path + ".picopage" + suffix).c_str(), "rb")) {
            length = std::fread(record, 1, sizeof(record) - 1, file);
            unreadable = unreadable || std::ferror(file);
            std::fclose(file);
            if (length) break;
        } else if (errno != ENOENT) unreadable = true;
    }
    entry.status = unreadable ? "位置未知" : length && !std::strstr(record, "\"cleared\"") ? "在读" :
                   entry.recent_rank < 128 ? "已打开" : "未读";
}


ReaderApp::Impl::Cover ReaderApp::Impl::draw_cover(const Entry &entry, int x, int y, bool decode,
                 int width, int height) {
    struct stat info{};
    const auto path = root + "/" + entry.path;
    if (entry.directory || !safe_existing(root, entry.path) || stat(path.c_str(), &info) != 0)
        return Cover::None;
    auto found = std::find_if(covers.begin(), covers.end(), [&](const auto &cover) {
        return cover.path == entry.path;
    });
    if (found != covers.end() && (found->size != info.st_size || found->modified != info.st_mtime))
    {
        covers.erase(found);
        found = covers.end();
    }
    if (found == covers.end()) {
        const auto cached = cover_entry(entry.path, info, kLibraryCoverWidth, kLibraryCoverHeight, true);
        std::vector<std::uint8_t> pixels;
        if (!cached_cover(cached, pixels)) {
            if (!decode || async_content) {
                if (async_content && std::none_of(missing_covers.begin(), missing_covers.end(),
                    [&](const auto &source) { return source.path == entry.path; }))
                    missing_covers.push_back({entry.path, cached.name, cached.header, info.st_size, info.st_mtime});
                return Cover::Undecoded;
            }
            pixels = decode_cover(entry.path, cached, kLibraryCoverWidth, kLibraryCoverHeight, true);
        }
        if (covers.size() == kCoverSlots) covers.erase(covers.begin());
        covers.push_back({entry.path, info.st_size, info.st_mtime, std::move(pixels)});
        found = covers.end() - 1;
    }
    // An entirely white thumbnail identifies nothing; the title is drawn in its place.
    if (std::all_of(found->pixels.begin(), found->pixels.end(), [](std::uint8_t packed) { return packed == 0xff; }))
        return Cover::None;
    if (width == kLibraryCoverWidth && height == kLibraryCoverHeight)
        blit(found->pixels, x, y, width, height);
    else blit_reduced(found->pixels, kLibraryCoverWidth, kLibraryCoverHeight, x, y, width, height);
    return Cover::Drawn;
}


void ReaderApp::Impl::draw_library(bool decode) {
    if (async_content) cancel_covers();
    decode = decode || display.busy();
    bool undecoded = false;
    view = View::Library;
    hits.clear();
    canvas.clear();
    if (folder.empty()) header("书库", 0, "", 388);
    else {
        const auto slash = folder.find_last_of('/');
        const auto parent = slash == std::string::npos ? "" : folder.substr(0, slash);
        header(basename(folder), Back, folder_root || parent.empty() || parent == library_home_root ? "书库" : basename(parent), 388);
        hits.back() = {0, 0, 396, 104, Back};
    }
    for (int x : {396, 540}) canvas.line(x, 0, x, 102, 1, 2);
    center("导入", 396, 32, 144, 26);
    center("设置", 540, 32, 144, 26);
    hits.push_back({396, 0, 144, 104, Import});
    hits.push_back({540, 0, 144, 104, Settings});
    // The card appears where reading normally starts and only when there is a book to resume.
    int top = 104;
    if (library_resume()) {
        std::string position;
        if (has_book() && page.end > page.start) position = chapter_label() + " · " + book_percent();
        else if (!has_book()) {
            const auto recalled = recalled_position(last_book);
            if (!recalled.second.empty()) position = recalled.first + " · " + recalled.second;
        }
        outline(24, 128, 636, 206, 3);
        text("继续阅读", 48, 146, 22, 520, 3);
        const auto recent = has_book() ? book_path : last_book;
        const auto cover = draw_cover(Entry{recent, basename(recent), false}, 48, 184, decode,
                                      kContinueCoverWidth, kContinueCoverHeight);
        undecoded = undecoded || cover == Cover::Undecoded;
        if (cover == Cover::Drawn) outline(47, 183, kContinueCoverWidth + 2, kContinueCoverHeight + 2);
        const int left = cover == Cover::Drawn ? 168 : 48;
        const int width = cover == Cover::Drawn ? 420 : 540;
        title_lines(has_book() ? document.metadata().title : basename(last_book), left, 182, width, 30, true);
        const auto date = library_recent ? recent_date(resume_opened_at) : "";
        if (!date.empty()) position += (position.empty() ? "" : " · ") + date;
        text(position.empty() ? "打开后恢复已保存的位置" : position, left, 288, 22, width, 3);
        text("›", 620, 214, 28, 30);
        hits.push_back({24, 128, 636, 206, Tools});
        top = 354;
    }
    if (entries.empty() && !library_resume_omitted) {
        if (card_available && !library_directory_unavailable && folder.empty() && top == 104) {
            draw_first_use();
            footer_bar(notice());
            return;
        }
        const int y = top + 120;
        text(!card_available ? "存储卡不可用" : library_directory_unavailable ? "无法读取书库目录" :
             folder.empty() ? "书库里还没有书" : "这个文件夹是空的", 36, y, 40, 612, 1, true);
        if (!card_available || library_directory_unavailable)
            wrapped(!card_available ? "这不代表书籍已被删除。" : "请检查书库目录和 TF 卡。", 36, y + 76, 612, 28);
        else {
            const int height = wrapped(folder.empty() ? "把 EPUB 或 TXT 放入" + library_location() + "，或使用“导入”。"
                                                      : "用“导入”把 EPUB 或 TXT 复制到存储卡，或返回上一级。", 36, y + 76, 612, 28);
            button(Import, "导入", 24, y + 104 + height, 636, 96);
        }
        footer_bar(notice());
        return;
    }
    const auto folders = static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const Entry &entry) { return entry.directory; }));
    std::string count = folders ? std::to_string(folders) + " 个文件夹" : "";
    if (folders < entries.size()) count += (count.empty() ? "" : " · ") + std::to_string(entries.size() - folders) + " 本书";
    if (folders && folders < entries.size()) {
        text((library_resume_omitted ? "另有 " : "") + std::to_string(entries.size() - folders) + " 本书", 36, top + 8, 22, 254, 3);
        text(std::to_string(folders) + " 个文件夹", 36, top + 38, 20, 254, 3);
    } else {
        if (library_resume_omitted) count = "另有 " + std::to_string(entries.size() - folders) + " 本书";
        text(count, 36, top + 24, 22, 254, 3);
    }
    button(LibrarySort, library_sort == "recent" ? (library_recent ? "最近阅读 ⇅" : "书名（回退）") : library_sort == "name_desc" ? "书名 ↓" : "书名 ↑", 300, top + 4, 164, 60, false, 22);
    button(LibraryList, "列表", 476, top + 4, 92, 60, !library_grid, 22);
    button(LibraryGrid, "卡片", 568, top + 4, 92, 60, library_grid, 22);
    canvas.line(0, top + 76, 683, top + 76, 1, 2);
    const auto size = library_page_size(), begin = library_begin();
    int group_gap = 0;
    for (std::size_t i = begin; i < std::min(begin + size, entries.size()); ++i) {
        auto &entry = entries[i];
        describe(entry);
        const auto label = entry_title(entry);
        if (library_grid) {
            const int x = 36 + static_cast<int>((i - begin) % 3) * 214;
            const int y = top + 100 + static_cast<int>((i - begin) / 3) * 320;
            if (entry.directory) {
                // A tabbed frame distinguishes a folder from a book without a cover.
                canvas.fill_rect(x, y, 84, 2, 1);
                canvas.fill_rect(x, y, 2, 22, 1);
                canvas.fill_rect(x + 82, y, 2, 22, 1);
                outline(x, y + 22, kLibraryCoverWidth, kLibraryCoverHeight - 22);
                title_lines(label, x + 18, y + 162, 148, 28, true);
            } else {
                const auto cover = draw_cover(entry, x, y, decode);
                undecoded = undecoded || cover == Cover::Undecoded;
                if (cover != Cover::Drawn) {
                    title_lines(label, x + 18, y + 20, 148, 28, true, 4);
                    canvas.fill_rect(x + 18, y + 228, 48, 2, 1);
                }
                outline(x, y, kLibraryCoverWidth, kLibraryCoverHeight);
            }
            text(entry.status, x, y + 264, 22, kLibraryCoverWidth, 3);
            hits.push_back({x, y, kLibraryCoverWidth, 296, EntryBase + static_cast<int>(i)});
        } else {
            if (library_recent && !entry.directory && entry.recent_rank == 128 &&
                (i == begin || entries[i - 1].directory || entries[i - 1].recent_rank < 128)) {
                const int divider = top + 78 + static_cast<int>(i - begin) * 126;
                text("其他书籍", 36, divider + 4, 20, 150, 5);
                canvas.line(156, divider + 18, 648, divider + 18, 12);
                group_gap = 32;
            }
            const int y = top + 78 + static_cast<int>(i - begin) * 126 + group_gap;
            // A book without a cover leaves no empty slot; its title starts at the margin.
            const auto cover = entry.directory ? Cover::None : draw_cover(entry, 37, y + 22, decode,
                                                                            kRowCoverWidth, kRowCoverHeight);
            undecoded = undecoded || cover == Cover::Undecoded;
            if (cover == Cover::Drawn) outline(36, y + 21, kRowCoverWidth + 2, kRowCoverHeight + 2);
            const int left = cover == Cover::Drawn ? 118 : 36;
            const auto date = library_recent ? recent_date(entry.opened_at) : "";
            const auto status = entry.directory ? entry.status + " ›" : entry.status;
            const int status_width = std::min(std::max(text_width(status, 22), text_width(date, 22)), 200);
            const int title_width = 628 - left - status_width;
            title_lines(label, left, y + (text_width(label, 28) <= title_width ? 44 : 26), title_width, 28);
            if (!date.empty()) {
                text(date, 648 - text_width(date, 22), y + 30, 22, 200, 3);
                text(status, 648 - status_width, y + 66, 20, status_width, 3);
            } else text(status, 648 - status_width, y + 50, 22, status_width, 3);
            canvas.line(24, y + 125, 660, y + 125, 12, 2);
            hits.push_back({24, y, 636, 126, EntryBase + static_cast<int>(i)});
        }
    }
    footer_bar(undecoded ? "正在生成封面…" : notice(), library_page, library_pages());
    if (undecoded && async_content) {
        ContentJob::Request request;
        request.kind = ContentJob::Kind::LibraryCovers; request.root = root;
        request.generation = cover_generation; request.covers = missing_covers;
        pending_covers = std::move(request);
    } else if (undecoded)
        deferred = [this] {
            if (view != View::Library) return;
            draw_library(true);
            overlay();
        };
}


void ReaderApp::Impl::inspect_book(const std::string &path, bool keep_message) {
    if (!keep_message) message.clear();
    detail_path = path; detail_metadata = {}; detail_progress = {}; detail_cover.reset();
    detail_position.clear(); detail_error.clear(); detail_size.reset();
    if (async_content && safe_existing(root, path)) {
        abandon_content(); invalidate_counting(); release_counting();
        begin_open(path, ContentJob::Kind::Inspect); return;
    }
    struct stat st{};
    if (!safe_existing(root, path) || stat((root + "/" + path).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        detail_error = "原文件不可用，请检查 TF 卡。";
    } else {
        detail_size = st.st_size;
        EpubDocument inspected;
        if (inspected.open(root + "/" + path, detail_error)) {
            const auto *source = &inspected;
            detail_metadata = source->metadata();
            detail_progress = store.load_progress(path, detail_metadata.book_id, revision_for(path, detail_metadata));
            if (detail_progress.record) {
                const auto &anchor = detail_progress.record->anchor;
                for (const auto &item : source->toc())
                    if (item.resource == anchor.resource) { detail_position = item.label; break; }
                if (detail_position.empty()) detail_position = "已保存阅读位置";
            }
        }
    }
    draw_details();
}


void ReaderApp::Impl::draw_details() {
    view = View::BookDetails;
    hits.clear(); canvas.clear(); header("书籍详情", Back, "书库");
    const auto title = detail_metadata.title.empty() ? basename(detail_path) : detail_metadata.title;
    struct stat info{};
    std::vector<std::uint8_t> cover;
    if (async_content) { if (detail_cover) cover = detail_cover->pixels(); }
    else if (detail_error.empty() && stat((root + "/" + detail_path).c_str(), &info) == 0)
        cover = cover_pixels(detail_path, info, 120, 160, false);
    const bool covered = !cover.empty();
    if (covered) blit(cover, 36, 140, 120, 160);
    const int title_x = covered ? 180 : 36, title_width = covered ? 468 : 612;
    int size = 32;
    while (size > 22 && wrapped(title, 0, 0, title_width, size, false) > 160) size -= 2;
    title_lines(title, title_x, 140, title_width, size, true, 4);
    text(detail_metadata.author.empty() ? "作者未知" : detail_metadata.author, 36, 324, 24, 612, 3);
    canvas.line(24, 378, 660, 378, 1, 2);
    text("进度", 36, 408, 24, 130, 3);
    text(detail_progress.record ? detail_position : (detail_progress.status == StoreStatus::Missing || detail_progress.status == StoreStatus::Cleared) ? "尚无本地阅读记录" : "阅读记录无法验证", 186, 408, 24, 462);
    text("同步", 36, 478, 24, 130, 3);
    text(txt_name(detail_path) ? "仅本地" : detail_path == book_path && detail_metadata.book_id == document.metadata().book_id ? sync_badge() : "打开书籍后检查", 186, 478, 24, 462);
    text("原书占用", 36, 548, 24, 150, 3);
    text(detail_size ? file_size(*detail_size) : "未知", 210, 548, 24, 438);
    if (detail_error.empty()) button(DetailOpen, detail_progress.record ? "继续阅读" : "打开书籍", 36, 630, 612, 96, true);
    else wrapped("无法打开：" + reason(detail_error, "无法解析文件内容") + "。", 36, 620, 612, 24);
    canvas.line(0, 764, 683, 764, 1, 3);
    button(DetailCache, "清理本书缓存", 36, 788, 612, 76);
    if (detail_error.empty()) button(DetailReset, "清除本地阅读进度", 36, 880, 612, 76);
    button(DetailDelete, "删除原书文件", 36, 972, 612, 76);
    footer_bar(notice());
}


void ReaderApp::Impl::manage_book() {
    if (!bookmark_io_ready()) return;
    if (detail_path == book_path) {
        cancel_sync();
        session.open("", "", writer);
        document.close(); book_path.clear(); release(chapter); page = {};
        links.clear(); clear_page_history(); link_return.reset(); confirmed_link_return.reset();
        sync_return = confirmed_sync_return = false;
        last_recorded.clear();
    }
    abandon_content(); foreground_job.reset(); stop_covers(); clear_prefetch();
    stop_counting_job();
    identities.clear();
    font_identity.clear(); font_scan.reset(); effective_font.clear();
    const auto result = store.manage_book(management_snapshot, delete_original, detail_metadata.book_id, revision_for(detail_path, detail_metadata));
    if (result != StoreStatus::Ok) {
        error(result == StoreStatus::ExternalChange ? "文件已被外部修改，操作未执行。请重新打开书籍详情。" :
              "操作未完成，请检查 TF 卡并重新打开详情核实文件状态。");
        return;
    }
    if (acknowledged_record && acknowledged_record->book_id == detail_metadata.book_id) acknowledged_record.reset();
    if (uncertain_record && uncertain_record->book_id == detail_metadata.book_id) uncertain_record.reset();
    covers.erase(std::remove_if(covers.begin(), covers.end(), [&](const auto& cover) { return cover.path == detail_path; }), covers.end());
    if (delete_original) {
        if (last_book == detail_path) last_book.clear();
        message = "原书已删除，阅读记录保留";
        scan_library(); draw_library();
    } else {
        // The recorded percentage belongs to the cleared position.
        store.save_cache(position_name(detail_path), "");
        message = "本地进度已清除，远端记录未修改";
        inspect_book(detail_path, true);
    }
    overlay();
}


} // namespace pico_page
