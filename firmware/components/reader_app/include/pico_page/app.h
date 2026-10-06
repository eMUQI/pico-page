#pragma once
#include "pico_page/core.h"
#include "pico_page/graphics.h"
#include "pico_page/sync.h"
#include <memory>
#include <atomic>
#include <string>

namespace pico_page {
enum class View {
    Library,
    Reading,
    Tools,
    Contents,
    Typography,
    Refresh,
    Settings,
    Import,
    Sync,
    Error,
    Sleeping,
    Confirmation
};
struct NetworkOperation {
    std::uint64_t id;
    sync::HttpRequest request;
    std::shared_ptr<std::atomic_bool> cancelled;
};
struct AppInput {
    InputEvent event;
    bool touch = false;
    int x = 0;
    int y = 0;
};

/** Owns document, UI, display transactions and reading session on one task. */
class ReaderApp {
  public:
    explicit ReaderApp(std::string card_root, std::string writer_id = "pico-page-host");
    ~ReaderApp();
    ReaderApp(const ReaderApp &) = delete;
    ReaderApp &operator=(const ReaderApp &) = delete;
    bool initialize();
    bool open_book(const std::string &relative_path);
    void input(const AppInput &event);
    /** The canvas must remain immutable until complete_frame is called. */
    const graphics::Canvas &canvas() const;
    const std::optional<Frame> &pending_frame() const;
    void complete_frame(bool success);
    View view() const;
    const std::string &status() const;
    const std::optional<ReadingRecord> &saved_record() const;
    /** True when the fast refresh preset selects the shorter page-update waveform. */
    bool fast_page_updates() const;
    /** Idle minutes before sleep from the global settings loaded at start or book open; 0 disables. */
    int auto_sleep_minutes() const;
    bool prepare_sleep();
    void wake();
    void card_removed();
    void retry_save();
    void show_error(const std::string &message);
    /** Copies one queued request for an external worker; never call from that worker. */
    std::optional<NetworkOperation> take_network_request();
    /** Marshal worker completion to the application task; old IDs are discarded. */
    void complete_network(std::uint64_t id, int status, const std::string &body,
                          const std::string &error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pico_page
