#pragma once

#include "pico_page/sync.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Saved Wi-Fi networks and the KOSync account file: reading and editing network.toml, the
 * connection order and the sentences that report a connection failure. Card access is limited
 * to the network file readers and compared commits; this module does not operate the radio.
 */
namespace pico_page::provision {

inline constexpr char default_endpoint[] = "https://sync.koreader.rocks";
/** Most Wi-Fi networks network.toml holds. */
inline constexpr std::size_t saved_limit = 5;
/** Earliest Unix time accepted as a set clock (2024-01-01T00:00:00Z). */
inline constexpr std::int64_t clock_floor = 1704067200;

struct WifiCredentials {
    std::string ssid;
    std::string password;  // Empty for an open network.
};

/** One network.toml entry. last_connected is Unix seconds, 0 when never recorded. */
struct SavedWifi : WifiCredentials {
    std::int64_t last_connected = 0;
};

/** A scanned network; rssi in dBm. open is true when it accepts a connection without a password. */
struct NearbyWifi {
    std::string ssid;
    int rssi = 0;
    bool open = false;
};

/**
 * True for a name of 1 to 32 bytes and a password that is empty, 8 to 63 bytes, or 64
 * hexadecimal digits, both without control characters.
 */
bool valid_wifi(const WifiCredentials& credentials);

/**
 * Reads PicoPage/network.toml, at most 4 KiB. A missing file yields an empty list. Schema 2
 * holds up to saved_limit [[wifi]] entries with distinct names, each with ssid, password and an
 * optional last_connected; schema 1 holds one [wifi] table and is read as one entry without a
 * connection time. Optional root selected_ssid pins one network; without it the device connects
 * automatically. false for any other content; diagnostics never contain the file contents.
 */
bool load_networks(const std::string& card_root, std::vector<SavedWifi>& networks, std::string& error);

/**
 * Loads the pinned network named by root selected_ssid. selected stays empty for automatic
 * connection: a missing or empty key, or a name that is no longer saved.
 */
bool load_selected_network(const std::string& root, std::optional<SavedWifi>& selected, std::string& error);
/** Pins a saved network, or selects automatic connection with an empty ssid, using a compared, lossless edit of network.toml. */
bool save_selected_network(const std::string& root, const std::string& ssid, std::string& error);

/**
 * Attempt order for automatic connection: the saved networks that the scan found, strongest
 * first. nearby must be ordered strongest first, as a scan returns it.
 */
std::vector<WifiCredentials> connection_order(const std::vector<WifiCredentials>& saved,
                                              const std::vector<NearbyWifi>& nearby);

/** Schema 2 file contents for at most saved_limit valid entries with distinct names; the final entry is the fallback network. */
std::string network_toml(const std::vector<SavedWifi>& networks);
std::string sync_toml(const sync::Config& config);

/** The final entry is the last added or connected network; never use it as live connection state. */
const SavedWifi* last_network(const std::vector<SavedWifi>& saved);

/**
 * Moves the connected network to the fallback position. Updates its Unix time only when now is
 * valid and differs by at least an hour. true when the list changed and should be written.
 */
bool record_connection(std::vector<SavedWifi>& saved, const std::string& ssid, std::int64_t now);

/**
 * Records a successful connection in network.toml while retaining comments and untouched text.
 * A missing, invalid, externally changed or unsupported file is left unchanged and returns
 * false with an error. true also covers an unchanged fallback and connection time.
 */
bool save_connection(const std::string& card_root, const std::string& ssid, std::int64_t now,
                     std::string& error);

/**
 * Adds or removes one network without operating the radio while retaining other TOML text.
 * now is a successful connection time in Unix seconds; pass 0 for an untested save.
 * remove ignores the password and time. Unsupported syntax, an external edit, or a failed
 * commit leaves the file unchanged and returns false with an error.
 */
bool save_network_edit(const std::string& card_root, const WifiCredentials& wifi, bool remove,
                       std::int64_t now, std::string& error);

/**
 * Adds a network, or replaces the password of a saved network with the same name, and
 * moves it to the fallback position. false, with the list unchanged, when the name is new and
 * the list already holds saved_limit entries.
 */
bool add_network(std::vector<SavedWifi>& saved, const WifiCredentials& credentials, std::int64_t now);

/** Removes the named network; false when it is not saved. */
bool remove_network(std::vector<SavedWifi>& saved, const std::string& ssid);

/** Why joining a network failed; Rejected carries the Wi-Fi driver's disconnect reason. */
enum class WifiFailure { Setup, Rejected, NoAddress, NoClock };
std::string wifi_problem(const std::string& ssid, WifiFailure failure, int reason);
/** Wi-Fi driver disconnect reasons for a network that is not in range, and for a refused password. */
bool network_absent(int reason);
bool password_rejected(int reason);

/** Payload of a QR code that joins a WPA network. */
std::string wifi_qr(const WifiCredentials& access);

}  // namespace pico_page::provision
