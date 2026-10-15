#pragma once

#include "pico_page/core.h"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace pico_page::platform {

inline constexpr int width = 684;
inline constexpr int height = 1216;
inline constexpr std::size_t framebuffer_bytes = width * height / 2;

enum class EventKind { None, K1, K2, K3, Touch, Power, SdRemoved, InputReset, PowerLong };
enum class EventPhase { Begin, End, Cancel };

struct Event {
    EventKind kind = EventKind::None;
    EventPhase phase = EventPhase::Begin;
    std::uint64_t sequence = 0;
    int x = 0;
    int y = 0;
    /** esp_timer time of the controller sample that produced a touch or key event. */
    std::int64_t time_us = 0;
    /** Inclusive extrema of all sampled positions in this contact, carried on release. */
    int min_x = x, min_y = y, max_x = x, max_y = y;
};

struct Battery {
    bool valid = false;
    int percent = 0;
    int millivolts = 0;
    bool charging = false;
};

/**
 * Initializes the single Read Pico instance on the calling application task.
 * Subsequent board calls must use that task. Missing or unreadable factory VCOM
 * prevents panel power-on. SD mount failure is nonfatal; inspect card_root().
 * BSP allocation and board self-test failures may abort inside the BSP.
 */
bool initialize(std::string& error);
bool panel_ready();

/** Returns /sdcard for the currently valid mount, otherwise nullptr. */
const char* card_root();
const std::string& card_error();

/** Closes and remounts the card without formatting. Close all files first. */
bool refresh_card(std::string& error);

/** Releases the application mount after all consumers close their files. */
bool release_card_for_usb(std::string& error);
/** Starts exclusive raw-card MSC access on the application task; failure requires restart. */
bool start_usb_storage(std::string& error);
/** Stops new USB I/O, waits for synchronous writes, and disconnects before restart. */
bool stop_usb_storage(std::string& error);

/**
 * Presents a 684x1216 portrait frame synchronously. Packed grayscale uses
 * even x in the low nibble, odd x in the high nibble, 0 black and 15 white.
 * The caller owns packed and keeps it immutable until this function returns.
 * full_clean always drives physical clearing, including for unchanged pixels.
 * region limits gray updates in portrait coordinates when the baseline is valid.
 * changed_pixels_only holds each equal old/new pixel during gray updates; clearing
 * and underrun recovery always ignore this mask and rebuild the full baseline.
 * A line-queue underrun with healthy power gets one full-clean retry before returning.
 * false means that the physical display state is no longer a valid baseline.
 */
bool present(const std::uint8_t* packed, std::size_t size, bool full_clean,
             std::string& error, std::optional<RefreshRegion> region = std::nullopt,
             bool changed_pixels_only = false);

/**
 * True from the start of a present() call until it returns; callable from any task.
 * Wi-Fi reception during the panel scan delays the driver's line producers past the depth
 * of their queue, so a task that receives bulk data should stop reading while this is set.
 */
bool presenting();

/**
 * Polls one event; EventKind::None means no new event. Touch coordinates use
 * portrait pixels; K1/K2/K3 retain their controller coordinates below the panel.
 * InputReset cancels all prior contacts after queue overflow; input resumes after release.
 * A held input never repeats. Read failures cancel an active touch sequence.
 * A sampler task records touch and key events every 5 ms, including while the
 * application task is blocked; they are returned here in order with their sample time.
 */
bool poll(Event& event, std::string& error);

struct InputDiagnostics { std::uint32_t overflows = 0, stack_free_bytes = 0; };
/** Sampler overflow count and minimum remaining stack since task creation. */
InputDiagnostics input_diagnostics();

/** Blocks the application task until a sampled event is waiting or the timeout expires. */
void wait_for_input(std::uint32_t timeout_ms);

/** True when the next waiting touch or key event was sampled before time_us. */
bool input_waiting_before(std::int64_t time_us);

/**
 * Selects the maximum CPU frequency (true) or allows the idle frequency (false).
 * Call on the application task. present() always restores the maximum because
 * panel line timing depends on it. Without CONFIG_PM_ENABLE this has no effect.
 */
void set_performance(bool high);

/** Page-update waveforms for non-clearing frames; full-clean frames always use GC16. */
enum class PageWaveform {
    Gray16,     // Default GL16, 37 phases.
    Gray8,      // 8-level GL16, 30 phases; faster, with more ghosting between full refreshes.
    Ripple,
    TextTurnA, TextTurnB, TextTurnC, // Diagnostic candidates, 37 phases; optical behavior unverified.
};

/** Selects the waveform for subsequent non-clearing present() calls. */
void set_page_waveform(PageWaveform waveform);

/**
 * Selects a volatile diagnostic override on the application task between presents.
 * Gray16 disables the override; TextTurnA/B/C and Ripple are otherwise accepted.
 * Failure leaves the previous selection intact. Does not refresh or save settings.
 */
bool set_page_effect_waveform(PageWaveform waveform, std::string& error);
/** Application-task query; Gray16 means use the configured refresh profile. */
PageWaveform page_effect_waveform();

/** Durations of the most recent present(), in microseconds. */
struct PresentTiming {
    std::int64_t copy_us = 0;   // Portrait conversion into the driver frame.
    std::int64_t power_us = 0;  // Rail power-on and the first power-good check.
    std::int64_t draw_us = 0;   // Driver update, including any clearing pulses.
};
PresentTiming present_timing();

/** Returns the latest validated PMU snapshot, refreshed by poll(). */
Battery battery();

/**
 * Enters light sleep and returns after a PMU power-key wake or an error.
 * The caller must finish display work, persist progress and stop network work
 * before calling. Touch and the three capacitive keys do not wake this mode.
 * Pending contacts are discarded under the sampler lock, including on aborted
 * preparation; only power-key events cancel the handoff. Contacts remain
 * suppressed until release after the controller returns from standby.
 * The wake input is consumed; this function does not advance reading state.
 * Periodic PMU events and rejected sleep requests are handled without returning.
 * entered_sleep remains true after a sleep request even if wake cleanup fails.
 * An aborted preparation preserves a cancelling long press for the next poll.
 * USB storage must be stopped. After an actual sleep, the USB console is briefly
 * disconnected to request host enumeration; existing host handles must reopen.
 */
bool sleep(std::string& error, bool& entered_sleep);

/** Drops panel rails and attempts TF release after all file consumers stop. Failure does not resume I/O. */
bool finish_power(std::string& error);
/** Requests PMU power-off after a final-frame and finish_power attempt, including release failure. */
bool power_off(std::string& error);

/** Attempts deferred RTC persistence on the application task at an idle boundary.
 * May block for BSP command retries; failures are logged without changing system time. */
void persist_clock();

/** Where connect_network() or synchronize_clock() stopped. Link carries the Wi-Fi driver's disconnect reason. */
struct ConnectFailure {
    enum class Stage { None, Setup, Link, Address, Clock } stage = Stage::None;
    int reason = 0;
};

/** A network found by a scan; rssi in dBm. ssid holds the raw broadcast bytes. open means no password. */
struct NearbyNetwork {
    std::string ssid;
    int rssi = 0;
    bool open = false;
};

/**
 * Joins an open or WPA2-or-later personal network. With clock it also obtains a usable TLS
 * clock, as synchronize_clock() does. Call only after an explicit user network action. The
 * credentials remain in RAM inside the Wi-Fi driver; this function never writes them to NVS or
 * logs. It blocks its worker task for up to 20 s for an address and 10 s for the time. The
 * first call establishes a session owner independent of the display/input task. While Wi-Fi is
 * already running for this owner (an access point from start_access_point() or a station from
 * scan_networks()), only the station link is replaced; a failure then leaves an access point
 * running and releases a station-only session.
 */
bool connect_network(const std::string& ssid, const std::string& password, std::string& error,
                     ConnectFailure* failure = nullptr, bool clock = true, const std::atomic_bool* cancelled = nullptr);

/**
 * Waits up to 10 s for SNTP on a connection until its first successful sync, even with RTC time.
 * Timeout may use an already valid system clock. Returns false on cancellation, missing link
 * or a clock before 2024; the link stays up either way. With fresh, restarts SNTP and requires a
 * new synchronization even if the system clock is valid. Actual SNTP updates queue RTC persistence.
 */
bool synchronize_clock(std::string& error, ConnectFailure* failure = nullptr, const std::atomic_bool* cancelled = nullptr,
                       bool fresh = false);

/**
 * Starts the station interface when Wi-Fi is not running, then scans once. networks receives up
 * to 20 distinct names, strongest first. The calling task becomes the session owner until
 * disconnect_network(); the station stays started so connect_network() can follow directly.
 */
bool scan_networks(std::vector<NearbyNetwork>& networks, std::string& error, const std::atomic_bool* cancelled = nullptr);

/** Dotted IPv4 address of the joined station link, or empty without one. */
std::string station_address();

/** True while the station still has an IP address on its current link. */
bool station_connected();

/**
 * Starts a WPA2 access point at 192.168.4.1 for at most two stations, next to an idle station
 * interface, and scans once for nearby networks. networks receives up to 20 distinct names,
 * strongest first, as raw broadcast bytes. password needs 8 to 63 bytes. The calling task
 * becomes the session owner until disconnect_network(). The access point may change channel,
 * and briefly drop its stations, when connect_network() joins a network on another channel.
 */
bool start_access_point(const std::string& ssid, const std::string& password,
                        std::vector<NearbyNetwork>& networks, std::string& error, const std::atomic_bool* cancelled = nullptr);

/** Stops Wi-Fi, including an access point, and releases the driver, interfaces and session. */
void disconnect_network();

/** File upload over the transfer page; see start_upload_server(). */
struct UploadServer {
    /**
     * Card root. Books go to Books/, fonts to Fonts/ and lock screen images to LockScreens/,
     * chosen by extension; existing files are never replaced.
     */
    std::string root;
    /** Sampled while a file is received; must outlive stop_portal_server(). */
    const std::atomic_bool* cancelled = nullptr;
    /**
     * Empty for the device access point: the page answers at / and unknown paths and host names
     * lead to it. Otherwise the session runs on a joined network and the page answers only at
     * /code; every other path, including /, gets a page that asks for the address on the screen.
     */
    std::string code;
    /** Shown under the page title: the access point name, or the joined network name. */
    std::string network;
    /** Called on the server task after each file is committed and verified; may be null. */
    void (*saved)(void* context, const std::string& name) = nullptr;
    void* context = nullptr;
    /** Every upload/check handler starts with true and ends with false, after file cleanup.
     * Called on the single server task, including failed requests; may be null.
     */
    void (*activity)(void* context, bool active) = nullptr;
};

/**
 * Serves the transfer page on port 80 to at most three connections. Each upload carries
 * this session's random token, which only the transfer page receives. With an empty code, call
 * after start_access_point(); otherwise after connect_network(). The caller must suspend other
 * card writers until stop_portal_server() returns.
 */
bool start_upload_server(const UploadServer& session, std::string& error);

/** Stops the name service and the server, waiting for a running handler to return. */
void stop_portal_server();

/**
 * Performs one HTTPS GET/PUT/POST after connect_network(), without redirects.
 * Certificate chain and server-name verification remain enabled. Request bodies
 * are limited to 16 KiB and responses to 32 KiB. true means complete transport;
 * the caller must interpret status and response before confirming any action.
 * A failed PUT/POST can still have changed the server; its outcome is uncertain.
 * cancelled is sampled between driver operations and must outlive the call.
 * Cancellation never implies that a transmitted write has been rolled back.
 */
bool http_request(const std::string& method, const std::string& url,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, std::string& response, int& status,
                  std::string& error, const std::atomic_bool* cancelled = nullptr);

/** Why download_firmware() stopped. */
enum class FirmwareFailure {
    None,
    Cancelled,
    Network,     // No link, a failed connection or an interrupted transfer.
    Server,      // The server answered with a status other than 200.
    Mismatch,    // Length, SHA-256 or the image's own digest differs from the description.
    TooLarge,    // The image does not fit the update slot.
    WrongImage,  // Not an ESP32-S3 image of this project and version.
    Flash,       // The update slot, a buffer or a flash operation was unavailable.
};

struct FirmwareImage {
    std::string url;      // https address; redirects are not followed.
    std::uint32_t size;   // Exact length in bytes.
    std::string sha256;   // 64 lowercase hexadecimal digits of the whole file.
    std::string version;  // Version that the image's application description must carry.
};

/** Callbacks on the downloading task; each may be null. */
struct FirmwareHooks {
    /** After each written block, with the number of bytes written so far. */
    void (*progress)(void* context, std::uint32_t written) = nullptr;
    /**
     * true before and false after every flash write of at most 4 KiB, including its sector
     * erase. The caller uses the pair to keep flash writes apart from panel updates.
     */
    void (*flash)(void* context, bool begin) = nullptr;
    void* context = nullptr;
};

/**
 * Streams one application image over HTTPS into the inactive OTA slot after connect_network(),
 * with certificate and server-name verification. Nothing is written until the first 288 bytes
 * name an ESP32-S3 image of this project with image.version. The slot is erased sector by
 * sector as data arrives. true means that the length and SHA-256 match and that the image in
 * the slot passed its own digest check; the boot slot is not changed. On any failure the
 * running firmware and the boot slot are untouched and the inactive slot is left unbootable.
 * Blocks its task for up to 10 s per read and 20 min in total; cancelled is sampled between
 * reads and writes and must outlive the call.
 */
bool download_firmware(const FirmwareImage& image, const FirmwareHooks& hooks, FirmwareFailure& failure,
                       std::string& error, const std::atomic_bool* cancelled = nullptr);

/**
 * Selects the slot verified by the last successful download_firmware() for the next start and
 * marks it for one unconfirmed start. Validates the image again. The caller restarts afterwards.
 */
bool activate_firmware(std::string& error);

/** True on the first start of an activated image, until confirm_firmware(). A reset in this state returns to the previous slot. */
bool firmware_unconfirmed();
/** Keeps the running image for later starts. false when the state could not be written; the image then stays unconfirmed. */
bool confirm_firmware();
/** Marks the running image invalid and restarts into the previous slot; returns only on failure. */
void reject_firmware();

} // namespace pico_page::platform
