#include "app_impl.h"

namespace pico_page {

void StatisticsWork::run(const FileStore& store, const std::function<bool()>& stop) {
#ifdef ESP_PLATFORM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 192U * 1024 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 32U * 1024) return;
#endif
    switch (kind) {
    case Kind::Commit: {
        const StageTimer timer{"reading_statistics_commit", 0};
        if (!writer) {
            status = store.begin_statistics_writer(writer);
            if (status != StoreStatus::Ok) return;
        }
        status = store.commit_statistics(writer, *source, records);
        return;
    }
    case Kind::LoadOptions:
        status = store.load_statistics_options(options);
        return;
    case Kind::Options:
        status = store.save_statistics_options(options, &previous_options);
        return;
    case Kind::Clear:
        for (const auto& binding : bindings) {
            std::uint64_t id = 0;
            status = store.statistics_book_id(binding.second, id, false);
            if (status != StoreStatus::Ok && status != StoreStatus::Missing) return;
            if (id == book_id) cleared_tokens.push_back(binding.first);
        }
        status = store.clear_statistics(book_id, discard_through);
        return;
    case Kind::Query:
        if (source) {
            status = store.statistics_book_id(*source, book_id, false);
            if (status != StoreStatus::Ok && status != StoreStatus::Missing) return;
            query.book_id = book_id;
        } else book_id = query.book_id;
        status = source && !book_id ? StoreStatus::Ok : store.query_statistics(query, report, stop);
        if (status == StoreStatus::Ok && source && report.books.empty()) {
            const auto progress = store.load_progress(source->path, source->content_id, source->content_revision);
            if (progress.record) {
                StatisticsBook book;
                book.progress_percent = store.cached_progress_percent(source->path, *progress.record);
                if (book.progress_percent >= 0) report.books.push_back(std::move(book));
            }
        }
        return;
    }
}

#ifndef ESP_PLATFORM
void ReaderApp::set_statistics_clock_for_test(std::function<StatisticsClock()> source) {
    impl_->statistics_test_clock = std::move(source);
}
#endif

std::uint64_t ReaderApp::Impl::statistics_milliseconds() const {
#ifndef ESP_PLATFORM
    if (statistics_test_clock) return statistics_test_clock().monotonic_ms;
#endif
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

StatisticsClock ReaderApp::Impl::statistics_now() const {
#ifndef ESP_PLATFORM
    if (statistics_test_clock) return statistics_test_clock();
#endif
    const auto utc = std::time(nullptr);
    return {statistics_milliseconds(), utc >= provision::clock_floor
        ? std::optional<std::int64_t>{utc} : std::nullopt};
}

void ReaderApp::Impl::adopt_statistics_options(StatisticsOptions options) {
    if (statistics_options.enabled != options.enabled) {
        statistics_collector.end_visit(statistics_now());
        statistics_running = false;
    }
    statistics_options = settings.statistics = options;
    statistics_collector.configure({static_cast<std::uint16_t>(options.minimum_seconds),
                                    static_cast<std::uint16_t>(options.maximum_seconds)});
    resume_statistics();
}

void ReaderApp::Impl::open_statistics_book() {
    const auto existing = statistics_sources.find(statistics_book);
    if (std::exchange(statistics_resume_open, false) && existing != statistics_sources.end() &&
        existing->second.path == book_path && existing->second.content_id == document.metadata().book_id)
        return;
    statistics_collector.end_visit(statistics_now());
    statistics_session = statistics_layout = statistics_revision = 0;
    statistics_confirmed = false;
    statistics_running = false;
    for (auto it = statistics_sources.begin(); it != statistics_sources.end();) {
        if (!statistics_collector.has_pending(it->first)) it = statistics_sources.erase(it);
        else ++it;
    }
    ++statistics_book;
    if (book_path.size() > 1024 || document.metadata().book_id.empty()) {
        statistics_save_failed = true;
        return;
    }
    auto title = document.metadata().title.substr(0, 240);
    if (document.metadata().title.size() > title.size())
        while (!title.empty() && (static_cast<unsigned char>(document.metadata().title[title.size()]) & 0xc0) == 0x80)
            title.pop_back();
    statistics_sources.emplace(statistics_book, StatisticsBookSource{book_path, document.metadata().book_id,
        std::move(title), -1, revision_for(book_path, document.metadata())});
}

bool ReaderApp::Impl::statistics_body_visible() const {
    return statistics_confirmed && !statistics_serial_transfer && view == View::Reading && !pending && !image_viewing && !content_loading &&
        !sleep_destination_captured && !sleep_wait_footer && !sleep_wait_restore_pending &&
        !power_menu_wait && !dictionary_contact && card_available && !portal_live && !usb_prepared &&
        session.visible_record() && statistics_session && statistics_sources.count(statistics_book);
}

void ReaderApp::Impl::pause_statistics() {
    if (std::exchange(statistics_running, false)) statistics_collector.pause(statistics_now());
}

void ReaderApp::Impl::resume_statistics() {
    if (!statistics_options.enabled || statistics_running || !statistics_body_visible()) return;
    const auto clock = statistics_now();
    statistics_collector.present(statistics_book, false, clock);
    statistics_running = true;
    if (!statistics_next_sample_ms) statistics_next_sample_ms = clock.monotonic_ms + 45000;
}

void ReaderApp::Impl::confirm_statistics_body(const Frame& frame) {
    const auto revision = session.visible_record()->local_revision;
    enum class Presentation { OpenOrResume, Reflow, Redisplay, Navigation };
    const auto reason = statistics_session != frame.token.session ? Presentation::OpenOrResume :
        statistics_layout != frame.token.layout ? Presentation::Reflow :
        statistics_revision == revision ? Presentation::Redisplay : Presentation::Navigation;
    statistics_confirmed = true;
    statistics_session = frame.token.session;
    statistics_layout = frame.token.layout;
    statistics_revision = revision;
    if (!statistics_options.enabled || !statistics_body_visible()) return;
    const auto clock = statistics_now();
    statistics_collector.present(statistics_book, reason == Presentation::Navigation, clock);
    statistics_running = true;
    if (!statistics_next_sample_ms) statistics_next_sample_ms = clock.monotonic_ms + 45000;
}

std::shared_ptr<StatisticsWork> ReaderApp::Impl::statistics_batch() {
    const auto* first = statistics_collector.pending();
    if (!first) return {};
    const auto source = statistics_sources.find(first->book_token);
    if (source == statistics_sources.end()) return {};
    auto work = std::make_shared<StatisticsWork>();
    work->kind = StatisticsWork::Kind::Commit;
    work->mount = statistics_mount;
    work->writer = statistics_writer;
    work->source = source->second;
    if (first->book_token == statistics_book && statistics_confirmed && !content_loading &&
        page.end > page.start && source->second.path == book_path && session.visible_record() &&
        session.saved_record() == session.visible_record())
        work->source->progress_percent = book_percent_value(page.start, page.end);
    for (std::size_t i = 0; i < std::min<std::size_t>(4, statistics_collector.pending_count()); ++i) {
        const auto& delta = *statistics_collector.pending(i);
        if (delta.book_token != first->book_token) break;
        work->records.push_back({delta.id, delta.day, delta.duration_ms});
    }
    return work;
}

void ReaderApp::Impl::request_statistics(StatisticsQuery query, std::optional<StatisticsBookSource> lookup) {
    cancel_statistics_query();
    statistics_last_query = query;
    statistics_lookup = std::move(lookup);
    statistics_pending = std::make_shared<StatisticsWork>();
    if (statistics_page == StatisticsPage::Settings) statistics_pending->kind = StatisticsWork::Kind::LoadOptions;
    statistics_pending->query = query;
    statistics_pending->source = statistics_lookup;
    statistics_pending->mount = statistics_mount;
    statistics_pending->request = statistics_generation;
    statistics_loading = true;
    statistics_status = StoreStatus::Ok;
    statistics_report = {};
    statistics_resolved_id = 0;
    statistics_flush_requested = true;
}

bool ReaderApp::Impl::statistics_mutating() const {
    const auto mutation = [](const std::shared_ptr<StatisticsWork>& work) {
        return work && (work->kind == StatisticsWork::Kind::Clear || work->kind == StatisticsWork::Kind::Options);
    };
    return mutation(statistics_pending) || (counting_job && mutation(counting_job->request.statistics));
}

void ReaderApp::Impl::cancel_statistics_query() {
    ++statistics_generation;
    statistics_loading = false;
    statistics_redraw = false;
    if (statistics_pending && (statistics_pending->kind == StatisticsWork::Kind::Query ||
                               statistics_pending->kind == StatisticsWork::Kind::LoadOptions))
        statistics_pending.reset();
    if (counting_job && counting_job->request.statistics &&
        counting_job->request.statistics->kind == StatisticsWork::Kind::Query) counting_job->cancel();
}

void ReaderApp::Impl::save_statistics_options(StatisticsOptions options) {
    cancel_statistics_query();
    statistics_pending = std::make_shared<StatisticsWork>();
    statistics_pending->kind = StatisticsWork::Kind::Options;
    statistics_pending->options = options;
    statistics_pending->previous_options = statistics_options;
    statistics_pending->mount = statistics_mount;
    statistics_pending->request = statistics_generation;
    statistics_loading = true;
    statistics_status = StoreStatus::Ok;
}

void ReaderApp::Impl::clear_statistics(std::uint64_t book_id) {
    cancel_statistics_query();
    statistics_pending = std::make_shared<StatisticsWork>();
    statistics_pending->kind = StatisticsWork::Kind::Clear;
    statistics_pending->book_id = book_id;
    if (const auto count = statistics_collector.pending_count())
        statistics_pending->discard_through = statistics_collector.pending(count - 1)->id;
    if (book_id)
        for (const auto& binding : statistics_sources) statistics_pending->bindings.push_back(binding);
    statistics_pending->mount = statistics_mount;
    statistics_pending->request = statistics_generation;
    statistics_flush_requested = true;
    statistics_loading = true;
    statistics_status = StoreStatus::Ok;
}

void ReaderApp::Impl::complete_statistics_work(StatisticsWork& work) {
    if (work.mount != statistics_mount) return;
    const bool okay = work.status == StoreStatus::Ok;
    if (work.kind == StatisticsWork::Kind::Commit) {
        if (work.writer) statistics_writer = work.writer;
        statistics_save_failed = !okay || statistics_lost_on_handoff;
        if (okay) {
            for (const auto& record : work.records) statistics_collector.acknowledge(record.id);
            statistics_retry_ms = 0;
            statistics_flush_requested = statistics_collector.pending_count() != 0;
        } else statistics_retry_ms = statistics_now().monotonic_ms + 45000;
        return;
    }
    const bool current = work.request == statistics_generation && view == View::Statistics;
    if (okay && (work.kind == StatisticsWork::Kind::Options ||
                 (current && work.kind == StatisticsWork::Kind::LoadOptions))) {
        adopt_statistics_options(work.options);
        if (current) statistics_draft = work.options;
    }
    if (work.kind == StatisticsWork::Kind::Clear && okay) {
        if (!work.book_id) {
            statistics_writer = 0;
            statistics_collector.discard(0);
            statistics_lost_on_handoff = false;
            statistics_save_failed = false;
            statistics_running = false;
        } else {
            for (auto token : work.cleared_tokens) {
                statistics_collector.discard(token);
                if (token == statistics_book) statistics_running = false;
            }
        }
        statistics_retry_ms = 0;
    }
    if (!current) return;
    statistics_status = work.status;
    statistics_loading = false;
    if (okay && statistics_page != StatisticsPage::Settings &&
        (work.kind == StatisticsWork::Kind::Options || work.kind == StatisticsWork::Kind::Clear))
        request_statistics(statistics_last_query, statistics_lookup);
    else {
        statistics_report = std::move(work.report);
        statistics_resolved_id = work.book_id;
        statistics_redraw = true;
    }
}

void ReaderApp::Impl::redraw_statistics_when_ready() {
    if (!statistics_redraw || view != View::Statistics || display.busy() || sleep_destination_captured) return;
    statistics_redraw = false;
    draw_statistics();
    overlay();
}

void ReaderApp::Impl::run_statistics(bool allow_start) {
    redraw_statistics_when_ready();
    if (!allow_start || !card_available || usb_prepared || portal_live || statistics_serial_transfer ||
        sleep_destination_captured || active_network || network_worker_id || view == View::Sleeping ||
        view == View::PowerOff || view == View::PowerMenu || power_menu_wait || display.busy()) return;
    if (!statistics_pending && !statistics_running && !statistics_collector.pending_count()) return;
    const auto now_ms = statistics_milliseconds();
    if (statistics_running && now_ms >= statistics_next_sample_ms) {
        statistics_collector.sample(statistics_now());
        statistics_next_sample_ms = now_ms + 45000;
        statistics_flush_requested = statistics_collector.pending_count() != 0;
    }
    if (statistics_retry_ms && now_ms >= statistics_retry_ms) statistics_retry_ms = 0;
    if ((!statistics_flush_requested || !statistics_collector.pending_count() || statistics_retry_ms) &&
        !statistics_pending) return;
    if (save_job || bookmark_job || foreground_job || pending_content || cover_job || counting_job || content_loading) return;
    if (session.needs_save() && !save_failed) {
        if (statistics_pending) schedule_save();
        return;
    }
    std::shared_ptr<StatisticsWork> work;
    if (statistics_flush_requested && !statistics_retry_ms && !save_failed &&
        !(statistics_pending && statistics_pending->kind == StatisticsWork::Kind::Clear))
        work = statistics_batch();
    if (!work) work = std::exchange(statistics_pending, {});
    if (!work) return;
    ContentJob::Request request;
    request.kind = ContentJob::Kind::ReadingStatistics;
    request.root = root;
    request.statistics = work;
    counting_job = std::make_unique<ContentJob>(std::move(request));
    if (!counting_job->start()) { counting_job.reset(); complete_statistics_work(*work); }
}

void ReaderApp::Impl::flush_statistics_for_handoff() {
    pause_statistics();
    if (!card_available || usb_prepared || portal_live) return;
    if (counting_job && counting_job->request.statistics) {
        counting_job->cancel();
        counting_job->wait();
        poll_counting_job();
    }
    if (statistics_pending && statistics_mutating()) {
        auto work = std::exchange(statistics_pending, {});
        work->run(store, {});
        complete_statistics_work(*work);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        auto work = statistics_batch();
        if (!work) break;
        work->run(store, {});
        complete_statistics_work(*work);
        if (work->status != StoreStatus::Ok || std::chrono::steady_clock::now() >= deadline) break;
    }
}

void ReaderApp::Impl::retire_statistics_mount() {
    ++statistics_mount;
    statistics_writer = 0;
    statistics_running = false;
    statistics_session = 0;
    statistics_confirmed = false;
    statistics_lost_on_handoff |= statistics_collector.pending_count() != 0 || statistics_collector.dropped_ms() != 0;
    statistics_save_failed |= statistics_lost_on_handoff;
    statistics_collector = {};
    statistics_collector.configure({static_cast<std::uint16_t>(statistics_options.minimum_seconds),
                                    static_cast<std::uint16_t>(statistics_options.maximum_seconds)});
    statistics_sources.clear();
    statistics_retry_ms = 0;
    statistics_next_sample_ms = 0;
    statistics_pending.reset();
    statistics_waiting_control.reset();
    cancel_statistics_query();
}

}
