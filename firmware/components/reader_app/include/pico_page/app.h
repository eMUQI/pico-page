#pragma once
#include "pico_page/core.h"
#include "pico_page/graphics.h"
#include "pico_page/sync.h"
#include <memory>
#include <functional>
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
    BookManagement,
    SyncSettings,
    Portal,
    Loading
};
enum class UsbTransferAction { None, Start, Exit };
enum class PortalAction { None, Start, Stop, Upload };
enum class PortalPhase { Starting, Waiting, JoiningWifi, CheckingAccount, Saved, Failed, Closed };
/** One state of a phone setup session, reported by the task that owns the radio. */
struct PortalStatus {
    PortalPhase phase = PortalPhase::Starting;
    /** The device's own access point and setup page address; empty until the access point is up. */
    std::string access_ssid, access_password, url;
    /** JoiningWifi: the network name. Saved, Failed and Closed: a sentence for the user, or empty. */
    std::string detail;
};
struct BookPages {
    std::uint32_t page;   // 1-based, counted from the first linear chapter.
    std::uint32_t total;
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
    /**
     * The contact began before the previous frame finished, so it was aimed at the earlier
     * screen. Only a page turn that follows a page turn is honoured; anything else is ignored.
     */
    bool queued = false;
};

/** Retained payload capacities; allocator bookkeeping and hash nodes are excluded. */
struct CacheUsage {
    std::size_t body_bitmap = 0, next_layout = 0, next_bitmap = 0, next_content = 0, page_index = 0, cover_pixels = 0;
    std::uint64_t bitmap_hits = 0, evictions = 0;
    std::size_t retained = 0, target = 0;
    std::size_t ui_glyph_bytes = 0, body_glyph_bytes = 0, ui_glyphs = 0, body_glyphs = 0;
    std::size_t ui_advances = 0, body_advances = 0;
};

/** Owns document, UI, display transactions and reading session on one task. */
class ReaderApp {
  public:
    explicit ReaderApp(std::string card_root, std::string writer_id = "pico-page-host",
                       bool controlled_storage = false, bool async_content = false);
    ~ReaderApp();
    ReaderApp(const ReaderApp &) = delete;
    ReaderApp &operator=(const ReaderApp &) = delete;
    /** Drops all verified identities before storage can change outside this application. */
    void invalidate_identity_cache();
    bool initialize();
    /** Accepts an open request; asynchronous content finishes through poll_background and body presentation. */
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
    /** Consumes one explicit UI request; the main loop owns the radio and the setup page server. */
    PortalAction take_portal_action();
    /**
     * Saves confirmed progress, ends sync work and shows the starting state of a phone setup
     * session (or book upload when upload is true). Stops background card jobs before returning.
     * false when the display or network is busy, the card is absent or the save fails.
     */
    bool prepare_portal(bool upload = false);
    /**
     * Draws a session state while the setup page is shown. Closed leaves that page, or the page
     * that requested the session, for settings or the refreshed library after upload.
     */
    void show_portal(const PortalStatus &status);
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
    /** True while an immutable displayed-position snapshot is being committed. */
    bool save_busy() const;
    /**
     * Page number of the visible page and the page total of the open book under the applied
     * typography; nullopt until every chapter has been counted or outside the reading order.
     */
    std::optional<BookPages> book_pages() const;
    /** True while the reading page is shown and next-page preparation or counting has work left. */
    bool background_due() const;
    /** True until a foreground content job has completed or acknowledged cancellation. */
    bool content_busy() const;
    /**
     * Diagnostic state: "opening" during book preparation, "image" while an image is prepared, "prefetch" while the next page is being prepared, "idle" without
     * a book or when all work is complete, "counting"
     * while chapters remain, "waiting" while memory is too low to load the next chapter, and
     * "failed" after a chapter could not be read or laid out. The string is static.
     */
    const char *background_state() const;
    CacheUsage cache_usage() const;
    /**
     * Accepts worker results; image completion may queue a body frame. cancel stops statistics
     * and defers image completion until pending input has been handled.
     */
    void poll_background(bool cancel = false);
    /**
     * Prepares one next-page layout and warms glyphs in 15 ms slices, then starts statistics.
     * With async_content, an independent parser and font return page boundaries; otherwise
     * counting runs here in 15 ms slices or one chapter load. Storage and individual operations
     * can exceed that target. Call only on the application owner task while no input is waiting;
     * stop must not call back into ReaderApp. No frame or reading position is changed.
     */
    void background_step(const std::function<bool()>& stop = {});
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
