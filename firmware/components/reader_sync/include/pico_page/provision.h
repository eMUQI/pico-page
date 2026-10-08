#pragma once

#include "pico_page/sync.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Wi-Fi and KOSync account setup from a phone: the configuration files, the form the device
 * serves and the sentences that report a result. Card access is limited to the network file
 * readers and compared commits for connection records and phone edits; this module does not
 * operate the radio.
 */
namespace pico_page::provision {

inline constexpr char default_endpoint[] = "https://sync.koreader.rocks";
/** Largest accepted form body in bytes. */
inline constexpr std::size_t form_limit = 4096;
/** Most network names listed on the form. */
inline constexpr std::size_t network_limit = 20;
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

/** A scanned network; rssi in dBm. */
struct NearbyWifi {
    std::string ssid;
    int rssi = 0;
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
 * connection time. false for any other content; diagnostics never contain the file contents.
 */
bool load_networks(const std::string& card_root, std::vector<SavedWifi>& networks, std::string& error);

/** Schema 2 file contents for at most saved_limit valid entries with distinct names; the final entry is the fallback network. */
std::string network_toml(const std::vector<SavedWifi>& networks);
std::string sync_toml(const sync::Config& config);

/**
 * Saved networks in the order to try: those seen nearby, strongest first. When none was seen,
 * only the configured fallback, so a failed scan or a hidden network still gets one try.
 */
std::vector<SavedWifi> connection_order(const std::vector<SavedWifi>& saved,
                                        const std::vector<NearbyWifi>& nearby);

/** The final entry is the configured fallback; successful connections move their entry there. */
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

/** What one form submission asks for; each action carries only its own fields. */
enum class Action { WifiAdd, WifiDelete, Account };

struct Submission {
    Action action = Action::WifiAdd;
    WifiCredentials network;  // WifiAdd: credentials to save; WifiDelete: ssid only.
    bool verify_wifi = false; // WifiAdd: a name selected from the scanned list.
    std::string endpoint;     // Account fields.
    std::string username;
    std::string userkey;      // MD5 of the account password, as KOReader sends it.
};

/**
 * Decodes an application/x-www-form-urlencoded body. action is wifi_add (ssid, or manual when
 * ssid is empty, and wifi_password), wifi_delete (ssid) or account (endpoint, username,
 * password; an empty endpoint selects default_endpoint). On failure problem is a sentence for
 * the user. WifiAdd with a nonempty ssid sets verify_wifi; a manual name leaves it false.
 * The account password is not retained.
 */
bool parse_form(std::string_view body, Submission& submission, std::string& problem);

/** Steps shown while an action is processed, in order. */
std::vector<std::string> stages(Action action, const std::string& ssid, bool verify_wifi = false);

/** Outcome of the previous submission, shown in the block of its action. */
struct Result {
    Action action = Action::WifiAdd;
    bool failed = false;
    std::string text;  // A sentence for the user.
    bool wifi_verified = false; // Only true after a successful WifiAdd connection and save.
};

struct PageState {
    std::string device_name;         // The device access point name, shown under the title.
    std::vector<NearbyWifi> nearby;  // Scanned names; untrusted bytes.
    std::vector<SavedWifi> saved;
    bool saved_readable = true;      // false when network.toml exists but cannot be used.
    std::string endpoint;            // Saved account, empty when none.
    std::string username;
    std::optional<Result> result;
    /** Values of a refused account submission, offered again with an empty password. */
    std::string retry_endpoint, retry_username;
    bool busy = false;               // A submission is being processed; forms are withheld.
    Action busy_action = Action::WifiAdd;
    bool busy_verify_wifi = false;
    std::size_t stage = 0;           // Index into stages(busy_action, busy_name).
    std::string busy_name;           // The network being added or deleted.
};

/** The setup page as UTF-8 HTML. Every state field is escaped; no secret is ever included. */
std::string page(const PageState& state);

/** Why joining a network failed; Rejected carries the Wi-Fi driver's disconnect reason. */
enum class WifiFailure { Setup, Rejected, NoAddress, NoClock };
std::string wifi_problem(const std::string& ssid, WifiFailure failure, int reason);

/**
 * Sentence for an account check that did not return 200. transported is false when the request
 * did not complete; detail is then the transport error.
 */
std::string account_problem(bool transported, int status, const std::string& detail);

std::string saved_summary(const Submission& submission);

/** Payload of a QR code that joins a WPA network. */
std::string wifi_qr(const WifiCredentials& access);

}  // namespace pico_page::provision
