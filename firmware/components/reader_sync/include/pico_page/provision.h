#pragma once

#include "pico_page/sync.h"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/**
 * Wi-Fi and KOSync account setup from a phone: the configuration files, the form the device
 * serves and the sentences that report a result. Nothing here touches the radio or the card
 * apart from load_wifi().
 */
namespace pico_page::provision {

inline constexpr char default_endpoint[] = "https://sync.koreader.rocks";
/** Largest accepted form body in bytes. */
inline constexpr std::size_t form_limit = 4096;
/** Most network names listed on the form. */
inline constexpr std::size_t network_limit = 20;

struct WifiCredentials {
    std::string ssid;
    std::string password;  // Empty for an open network.
};

/**
 * True for a name of 1 to 32 bytes and a password that is empty, 8 to 63 bytes, or 64
 * hexadecimal digits, both without control characters.
 */
bool valid_wifi(const WifiCredentials& credentials);

/**
 * Reads PicoPage/network.toml: schema 1 with wifi.ssid and wifi.password only, at most 4 KiB.
 * Diagnostics never contain the file contents.
 */
bool load_wifi(const std::string& card_root, WifiCredentials& credentials, std::string& error);

/** File contents for valid credentials and a valid account; load_wifi and load_config accept them. */
std::string network_toml(const WifiCredentials& credentials);
std::string sync_toml(const sync::Config& config);

struct Submission {
    bool wifi = false;     // network holds a Wi-Fi configuration to verify and save.
    bool account = false;  // endpoint, username and userkey hold an account to verify and save.
    WifiCredentials network;
    std::string endpoint;
    std::string username;
    std::string userkey;   // MD5 of the account password, as KOReader sends it.
};

/**
 * Decodes an application/x-www-form-urlencoded body with the fields ssid, wifi_password,
 * endpoint, username and password. An empty ssid leaves Wi-Fi unchanged and an empty password
 * leaves the account unchanged; an empty endpoint selects default_endpoint. On failure problem
 * is a sentence for the user. The account password is not retained.
 */
bool parse_form(std::string_view body, Submission& submission, std::string& problem);

struct PageState {
    std::vector<std::string> networks;  // Scanned names; untrusted bytes.
    std::string current_ssid;           // Saved Wi-Fi name, empty when none.
    std::string endpoint;               // Saved account, empty when none.
    std::string username;
    std::string notice;                 // Result of the previous submission.
    bool busy = false;                  // A submission is being verified; the form is withheld.
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
