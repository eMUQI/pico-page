#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace pico_page::platform {

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
    /** Authenticated upload/check handlers start with true and end with false after cleanup.
     * Called on the server task, including subsequent validation/transfer failures; may be null.
     * Invalid tokens and requests received while stopping do not count as activity.
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

/** Stops the name service and server on the single network owner task.
 * Returns only after handlers have exited. SDK stop failures retain the handle and borrowed
 * session, reject new uploads and retry after one second. Keep callbacks, cancellation flag,
 * context and its mutex alive until return; neither SDK blocking calls nor retries have a
 * total deadline. Do not call from an HTTP handler or while holding a callback's mutex.
 */
void stop_portal_server();

/**
 * Resolves the host of an HTTPS URL over the joined station link, between panel frames. The
 * lwIP lookup itself cannot be interrupted and is bounded by its DNS retries; cancelled is
 * checked before it starts. A successful answer stays in the lwIP cache for the request.
 */
bool resolve_host(const std::string& url, std::string& error, const std::atomic_bool* cancelled = nullptr);

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

} // namespace pico_page::platform
