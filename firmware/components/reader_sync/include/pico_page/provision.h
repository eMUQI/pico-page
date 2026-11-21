#pragma once
#include "pico_page/clock_time.h"
#include "pico_page/i18n.h"

#include "pico_page/sync.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pico_page::provision {

inline constexpr char default_endpoint[] = "https://sync.koreader.rocks";
inline constexpr std::size_t saved_limit = 5;
inline constexpr auto clock_floor = clock_time::floor;

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

/** Serializes schema 2 for at most saved_limit valid entries with distinct names. */
std::string network_toml(const std::vector<SavedWifi>& networks);
std::string sync_toml(const sync::Config& config);

/** The final entry is the last added or connected network; never use it as live connection state. */
const SavedWifi* last_network(const std::vector<SavedWifi>& saved);

/**
 * Moves the connected network to the end. Updates its Unix time only when now is
 * valid and differs by at least an hour. true when the list changed and should be written.
 */
bool record_connection(std::vector<SavedWifi>& saved, const std::string& ssid, std::int64_t now);

/**
 * Records a successful connection in network.toml while retaining comments and untouched text.
 * A missing, invalid, externally changed or unsupported file is left unchanged and returns
 * false with an error. true also covers unchanged order and connection time.
 */
bool save_connection(const std::string& card_root, const std::string& ssid, std::int64_t now,
                     std::string& error);

/**
 * Adds or removes one network without operating the radio while retaining other TOML text.
 * Syntax validation and external-change checks reject the edit without replacing the file.
 * A publication, sync or readback failure can leave the destination changed or missing;
 * reload its state before retrying. Failure does not guarantee rollback.
 *
 * @param card_root Mounted card root.
 * @param wifi Network identity and credentials; removal uses only the SSID.
 * @param remove True to remove the network, false to add or update it.
 * @param now Successful connection time in Unix seconds; 0 for an untested save, ignored on removal.
 * @param[out] error Failure description; cleared on success.
 * @return True after the edited file has been published and verified; false on failure.
 */
bool save_network_edit(const std::string& card_root, const WifiCredentials& wifi, bool remove,
                       std::int64_t now, std::string& error);

/**
 * Adds a network, or replaces the password of a saved network with the same name, and
 * moves it to the end. false, with the list unchanged, when the name is new and
 * the list already holds saved_limit entries.
 */
bool add_network(std::vector<SavedWifi>& saved, const WifiCredentials& credentials, std::int64_t now);

bool remove_network(std::vector<SavedWifi>& saved, const std::string& ssid);

enum class WifiFailure { Setup, Rejected, NoAddress, NoClock };
std::string wifi_problem(const std::string& ssid, WifiFailure failure, int reason,
                         i18n::Language language = i18n::Language::SimplifiedChinese);
/** Wi-Fi driver disconnect reasons for a network that is not in range, and for a refused password. */
bool network_absent(int reason);
bool password_rejected(int reason);

std::string wifi_qr(const WifiCredentials& access);

}
