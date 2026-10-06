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
    UsbTransfer,
    Sync,
    Error,
    Sleeping,
    Confirmation,
    RefreshConfirm,
    RefreshInterval,
    BookDetails,
    PowerSettings,
    StorageSettings,
    InputSettings,
    BookManagement
};
enum class UsbTransferAction { None, Start, Exit };
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
    /**
     * The contact began before the previous frame finished, so it was aimed at the earlier
     * screen. Only a page turn that follows a page turn is honoured; anything else is ignored.
     */
    bool queued = false;
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
    /** Updates the latest battery reading for the next render; nullopt means unknown. */
    void set_battery(std::optional<int> percent);
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
    /** Consumes one explicit UI request; the main loop arbitrates device ownership. */
    UsbTransferAction take_usb_transfer_action();
    /** Saves confirmed progress and releases card files; false forbids USB handoff. */
    bool prepare_usb_transfer();
    /** Draws a transfer/error page from internal fonts, without accessing the card. */
    void show_usb_transfer(bool active, const std::string& error);
    bool prepare_sleep();
    void wake();
    void card_removed();
    /**
     * True when a presented position awaits its first save attempt. Saving is left to the
     * caller's idle time because a card commit blocks input sampling.
     */
    bool save_due() const;
    /** Commits the presented position now; a failure replaces the page with the error view. */
    void save_progress();
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
