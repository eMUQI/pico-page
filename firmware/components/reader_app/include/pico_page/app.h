#pragma once
#include "pico_page/core.h"
#include "pico_page/graphics.h"
#include "pico_page/sync.h"
#include "pico_page/provision.h"
#include "pico_page/network_session.h"
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
    WifiSettings,
    PowerSettings,
    StorageSettings,
    InputSettings,
    BookManagement,
    SyncSettings,
    Portal,
    Loading,
    PowerMenu,
    PowerOff,
    NetworkClosing,
    Help,
    TextEntry,
    PageEffects,
    LockScreens
};
enum class PageEffect { Off, TextTurnA, TextTurnB, TextTurnC, Ripple };
enum class PowerAction { None, Shutdown, Restart };
struct PowerRequest { PowerAction action = PowerAction::None; bool without_save = false; };
enum class UsbTransferAction { None, Start, Exit };
/** Upload runs over the device access point; UploadStation over a saved network. */
enum class PortalAction { None, Stop, Upload, UploadStation };
enum class PortalPhase { Starting, Waiting, Closed, Receiving };
/** One state of a book upload session, reported by the task that owns the radio. */
struct PortalStatus {
    PortalPhase phase = PortalPhase::Starting;
    /** The device's own access point and upload page address; empty until the access point is up. */
    std::string access_ssid, access_password, url;
    /** Closed: a sentence for the user, or empty. */
    std::string detail;
    /** Upload over a saved network: its name; the access point fields are then empty. */
    std::string network;
    /** Upload sessions: books saved so far and the title of the latest one. */
    int saved_books = 0;
    std::string latest_book;
    /** Nonzero request/session generation; stale events are ignored. */
    std::uint64_t generation = 0;
};
struct BookPages {
    std::uint32_t page;   // 1-based, counted from the first linear chapter.
    std::uint32_t total;
};
enum class WifiPhase { Offline, Connecting, Connected, Clock, Request, Hotspot, Closing, Failed };
struct WifiStatus {
    std::uint64_t sequence = 0;
    WifiPhase phase = WifiPhase::Offline;
    std::string ssid, detail;
    bool manual = false;
};
/** Why a station link was not established; None also covers failures after the link is up. */
enum class LinkFailure { None, NotNearby, Password, Other };
/**
 * Networks a worker may join. With automatic it scans and tries the listed networks that are in
 * range, strongest first, until one connects; otherwise it joins only the single listed network.
 * A station link that is already up is used as it is.
 */
struct WifiTargets {
    std::vector<provision::WifiCredentials> networks;
    bool automatic = false;
};
struct WifiOperation {
    /** Connect keeps the link until cancelled is set; Join verifies new credentials and releases the link. */
    enum class Kind { Connect, Scan, Join };
    Kind kind = Kind::Connect;
    std::uint64_t id = 0;
    WifiTargets targets;
    network::Hold cancelled;
};
struct NetworkOperation {
    std::uint64_t id;
    sync::HttpRequest request;
    std::shared_ptr<std::atomic_bool> cancelled;
    network::Hold session = {};
    WifiTargets targets = {};
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
    /** The touch was held before release. Book rows and reading images have long-press actions. */
    bool long_press = false;
    struct TouchBounds { int min_x, min_y, max_x, max_y; };
    /** Sampled contact extent, including press and release; omitted for synthetic point taps. */
    std::optional<TouchBounds> touch_bounds = std::nullopt;
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
    /** Refuses during active sync or portal work; cancelled HTTP may drain while local reading resumes. */
    bool open_book(const std::string &relative_path);
    /** Updates the latest battery reading for the next render; nullopt means unknown. */
    void set_battery(std::optional<int> percent);
    void input(const AppInput &event);
    /** True when a held touch can open an image or a library book's details at this screen point. */
    bool can_long_press_at(int x, int y) const;
    /** The canvas must remain immutable until complete_frame is called. */
    const graphics::Canvas &canvas() const;
    const std::optional<Frame> &pending_frame() const;
    void complete_frame(bool success);
    View view() const;
    const std::string &status() const;
    const std::optional<ReadingRecord> &saved_record() const;
    /** True when the fast refresh preset selects the shorter page-update waveform. */
    bool fast_page_updates() const;
    /** Volatile experimental selection; shared by the UI and diagnostic console. */
    PageEffect page_effect() const;
    /** Selects an effect without saving configuration; redraws an open selection page. */
    void set_page_effect(PageEffect effect);
    /** Idle minutes before sleep from the global settings loaded at start or book open; 0 disables. */
    int auto_sleep_minutes() const;
    /** Reloads global settings when idle; uses the admitted network state, and rejects access during a bookmark commit. */
    bool wifi_enabled() const;
    /** Cancels network work on the application owner without waiting for the worker.
     * for_sleep retains the manual target for a cancelled sleep attempt. */
    void stop_wifi(bool for_sleep = false);
    /** Consumes a Wi-Fi request when display and power-menu work permit it; credentials are owned values. */
    std::optional<WifiOperation> take_wifi_request();
    void complete_wifi(std::uint64_t id, const std::vector<provision::NearbyWifi>& nearby, const std::string& error,
                       LinkFailure failure = LinkFailure::None);
    void show_wifi(const WifiStatus& status);
    /** True while the portal or its read-only Wi-Fi status page is visible. */
    bool portal_visible() const;
    /** Consumes one explicit UI request; the main loop arbitrates device ownership. */
    UsbTransferAction take_usb_transfer_action();
    /** Saves confirmed progress and releases card files; false forbids USB handoff. */
    bool prepare_usb_transfer();
    /** Draws a transfer/error page from internal fonts, without accessing the card. */
    void show_usb_transfer(bool active, const std::string& error);
    /** Consumes one explicit UI request; the main loop owns the radio and the setup page server. */
    PortalAction take_portal_action();
    /** Generation attached to worker status and completion for the current portal request. */
    std::uint64_t portal_generation() const;
    /**
     * Saves confirmed progress, ends sync work and shows the starting state of a book upload
     * session; station selects upload over a saved network instead of the device access point.
     * Stops background card jobs before returning. false when the display or network is busy,
     * the card is absent or the save fails.
     */
    bool prepare_portal(bool station = false);
    /** Networks for an upload session over a saved network, read before the session owns the card. */
    WifiTargets upload_targets() const;
    /**
     * Draws a state tagged with the request generation. Closed confirms that the worker has
     * stopped accessing the card and radio; only then can the requested next page read settings.
     * Closed leaves that page, or the page
     * that requested the session, or the refreshed library after upload. A session that closes
     * before its first Waiting state returns to the network page with the closing sentence.
     */
    void show_portal(const PortalStatus &status);
    /**
     * Saves progress and prepares a sleep frame. false with sleep_pending() means the cover
     * worker is still handing back resources; call again from the event loop. Other false
     * results reject sleep. Present a successful frame before suspending storage or hardware.
     */
    bool prepare_sleep();
    bool sleep_pending() const;
    /** Cancels sleep and restores its manual Wi-Fi intent without destroying a running cover worker. */
    void cancel_sleep();
    /**
     * Requests a modal menu without changing the underlying page or draft. A canceled cover
     * worker is polled before opening; true means opened or queued. Other busy tasks reject it.
     */
    bool show_power_menu();
    /** Displays why the menu is unavailable while preserving the current task and controls. */
    void power_unavailable(const std::string& message);
    /** Consumes an explicit menu choice; the device loop performs the hardware operation. */
    PowerRequest take_power_request();
    /** Saves local progress and starts irreversible teardown. Only save failure allows returning. */
    bool prepare_power(PowerRequest request);
    /** Closes card consumers after the final frame attempt, even if presentation failed. */
    bool close_power_files();
    /** Draws terminal hardware-off instructions using fallback fonts; never resumes reading. */
    void show_power_off_help();
    /** Resumes reading or the library; slept=false also restores the cancelled manual Wi-Fi intent. */
    void wake(bool slept = true);
    void card_removed();
    /**
     * True when a presented position awaits its first save attempt. Saving is left to the
     * caller's idle time because a card commit blocks input sampling.
     */
    bool save_due() const;
    /** Commits the presented position now; a failure replaces the page with the error view. */
    void save_progress();
    /** True while an immutable displayed-position snapshot or a bookmark list is being committed. */
    bool save_busy() const;
    /**
     * Page number of the visible page and the page total of the open book under the applied
     * typography; nullopt until every chapter has been counted or outside the reading order.
     */
    std::optional<BookPages> book_pages() const;
    /** True while the reading page is shown and next-page preparation or counting has work left. */
    bool background_due() const;
    /** True while foreground content is pending, including the inline-image dwell interval and cancellation. */
    bool content_busy() const;
    /**
     * Diagnostic state: "opening" during book preparation, "image" while an image is prepared, "prefetch" while the next page is being prepared, "idle" without
     * a book or when all work is complete, "counting"
     * while chapters remain, "waiting" while memory is too low to load the next chapter, and
     * "failed" after a chapter could not be read or laid out. The string is static.
     */
    const char *background_state() const;
    /**
     * Diagnostic name of the sync panel's last result: "unchecked", "no-record", "same", "different",
     * "unmapped", "uploaded", "uncertain", "unauthorized", "failed", "link-failed" or "config-changed".
     * Starting a check resets it to "unchecked" until the answer is applied. The string is static.
     * Defined for host builds and for firmware with the diagnostic console.
     */
    const char *sync_state() const;
    /**
     * Diagnostic order of the "different" result: 1 when the mapped remote position lies after the
     * visible page, -1 when it lies before it, 0 on the page or in any other state. Defined where
     * sync_state() is.
     */
    int sync_order() const;
    /** Effective body font size in pixels for the open book. Defined where sync_state() is. */
    int font_size_px() const;
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
    /** Marshal completion after worker resource release; matching cancelled work unlocks deferred navigation, old IDs are discarded. */
    void complete_network(std::uint64_t id, int status, const std::string &body,
                          const std::string &error, LinkFailure failure = LinkFailure::None);
    /** True while the on-screen keyboard is shown; every tap made during a refresh is then kept, in order. */
    bool keyboard_visible() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pico_page
