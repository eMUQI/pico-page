#include "pico_page/provision.h"
#include "pico_page/store.h"
#include "pico_page/network_session.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <unistd.h>

using namespace pico_page;
namespace fs = std::filesystem;

static std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

int main() {
    char directory[] = "/tmp/pico-page-provision-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path card = directory;
    const std::string root = card.string();
    const FileStore store(root);
    std::string error, problem;

    // The written files are read back by the parsers the firmware uses; schema 1 is migrated.
    std::vector<provision::SavedWifi> loaded;
    assert(provision::load_networks(root, loaded, error) && loaded.empty());
    const provision::WifiCredentials wifi{"家 \"2.4G\"\\", "p\"a\\ss 密码"};
    assert(provision::valid_wifi(wifi) && provision::valid_wifi({"open", ""}) && provision::valid_wifi({"psk", std::string(64, 'A')}));
    assert(!provision::valid_wifi({"", "12345678"}) && !provision::valid_wifi({"net", "1234567"}) &&
           !provision::valid_wifi({"net", std::string(65, 'a')}));
    fs::create_directories(card / "PicoPage");
    { std::ofstream file(card / "PicoPage/network.toml", std::ios::binary); file << "schema_version = 1\n[wifi]\nssid = \"old\"\npassword = \"hunter2-long\"\n"; }
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 1 && loaded[0].ssid == "old" &&
           loaded[0].password == "hunter2-long" && loaded[0].last_connected == 0);
    std::vector<provision::SavedWifi> saved;
    const std::int64_t day = 86400, now = provision::clock_floor + 300 * day;
    assert(provision::add_network(saved, wifi, now) && provision::add_network(saved, {"office", ""}, 0));
    assert(saved[0].last_connected == now && saved[1].last_connected == 0);
    assert(store.save_private("network.toml", provision::network_toml(saved), error) == StoreStatus::Ok);
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 2);
    assert(loaded[0].ssid == wifi.ssid && loaded[0].password == wifi.password && loaded[0].last_connected == now);
    assert(loaded[1].ssid == "office" && loaded[1].password.empty() && loaded[1].last_connected == 0);
    const sync::Config account{"https://sync.example.invalid", "读者 \"a\\b\"", "55d2fddf8ba5768bbc44b083846cf439", "pico-page-test", 20240114};
    assert(store.save_private("sync.toml", provision::sync_toml(account), error) == StoreStatus::Ok);
    sync::Config configured;
    assert(sync::load_config(root, configured, error));
    assert(configured.endpoint == account.endpoint && configured.username == account.username &&
           configured.userkey == account.userkey && configured.device_id == account.device_id &&
           configured.dom_version == account.dom_version);

    // At most five networks with distinct names; adding a saved name replaces its password.
    for (const char* name : {"n3", "n4", "n5"}) assert(provision::add_network(saved, {name, "password"}, now));
    assert(saved.size() == provision::saved_limit && !provision::add_network(saved, {"n6", "password"}, now));
    assert(provision::add_network(saved, {"office", "new-password"}, now + day) && saved.size() == 5 &&
           saved.back().ssid == "office" && saved.back().password == "new-password" && saved.back().last_connected == now + day);
    assert(provision::remove_network(saved, "n4") && !provision::remove_network(saved, "n4") && saved.size() == 4);
    assert(provision::add_network(saved, {"n6", "password"}, 0) && saved.back().last_connected == 0);

    assert(provision::last_network(saved)->ssid == "n6" && !provision::last_network({}));
    assert(provision::record_connection(saved, "n3", 100) && provision::last_network(saved)->ssid == "n3");
    assert(!provision::record_connection(saved, "missing", now));
    assert(provision::record_connection(saved, "office", now + day + 60) &&
           provision::last_network(saved)->ssid == "office" && saved.back().last_connected == now + day);
    assert(provision::record_connection(saved, "n3", now + 2 * day) && provision::last_network(saved)->ssid == "n3");
    // A time recorded under a clock that was later set back is replaced.
    assert(provision::record_connection(saved, "n3", now) && saved.back().last_connected == now);
    assert(!provision::record_connection(saved, "n3", now + 60));
    std::vector<provision::SavedWifi> cold{{{"old", ""}, now}};
    assert(provision::add_network(cold, {"new", ""}, 0) && provision::last_network(cold)->ssid == "new");
    assert(provision::record_connection(cold, "old", 100) && provision::last_network(cold)->ssid == "old" &&
           cold.back().last_connected == now);
    assert(store.save_private("network.toml", provision::network_toml(cold), error) == StoreStatus::Ok);
    assert(provision::load_networks(root, loaded, error) && provision::last_network(loaded)->ssid == "old");

    // Automatic connection updates retain comments, formatting and each table's note.
    const auto network_file = card / "PicoPage/network.toml";
    const std::string annotated = "# configured by the reader\nschema_version = 2 # keep\n\n"
                                  "# home note\n[[wifi]] # home\nssid = 'home'\npassword = 'password1'\n"
                                  "last_connected = " + std::to_string(now) + " # visited\n\n"
                                  "# office note\n[[wifi]]\nssid = \"office\"\npassword = \"\"\n";
    assert(store.save_private("network.toml", annotated, error) == StoreStatus::Ok);
    assert(provision::save_connection(root, "home", now + 2 * day, error));
    auto edited = read(network_file);
    assert(edited.find("# configured by the reader\nschema_version = 2 # keep") == 0);
    assert(edited.find("# office note") < edited.find("# home note\n[[wifi]] # home"));
    assert(edited.find("password = 'password1'") != std::string::npos);
    assert(edited.find("last_connected = " + std::to_string(now + 2 * day) + " # visited") != std::string::npos);
    assert(provision::load_networks(root, loaded, error) && provision::last_network(loaded)->ssid == "home");
    assert(provision::save_connection(root, "office", 100, error));
    assert(provision::load_networks(root, loaded, error) && provision::last_network(loaded)->ssid == "office" &&
           read(network_file).find("# office note") != std::string::npos);
    // Phone-side edits retain comments on untouched entries and replace only the chosen value.
    assert(store.save_private("network.toml", annotated, error) == StoreStatus::Ok);
    assert(provision::save_network_edit(root, {"home", "new-password"}, false, now + day, error));
    edited = read(network_file);
    assert(edited.find("# configured by the reader\nschema_version = 2 # keep") == 0 &&
           edited.find("# office note") < edited.find("# home note") &&
           edited.find("password = \"new-password\"") != std::string::npos &&
           edited.find("# visited") != std::string::npos);
    assert(provision::save_network_edit(root, {"office", ""}, true, 0, error));
    edited = read(network_file);
    assert(edited.find("# office note") == std::string::npos && edited.find("# home note") != std::string::npos);
    assert(provision::save_network_edit(root, {"guest", ""}, false, 100, error));
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 2 && loaded.back().ssid == "guest" &&
           read(network_file).find("# home note") != std::string::npos);
    const std::string legacy = "# original\nschema_version = 1 # keep\n[wifi] # keep\n"
                               "ssid = 'legacy'\npassword = 'password2' # keep\n";
    assert(store.save_private("network.toml", legacy, error) == StoreStatus::Ok);
    assert(provision::save_connection(root, "legacy", now, error));
    edited = read(network_file);
    assert(edited.find("# original\nschema_version = 2 # keep") == 0 &&
           edited.find("[[wifi]] # keep") != std::string::npos &&
           edited.find("password = 'password2' # keep") != std::string::npos &&
           edited.find("last_connected = " + std::to_string(now)) != std::string::npos);
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 1 && loaded[0].ssid == "legacy");
    assert(store.save_private("network.toml", legacy, error) == StoreStatus::Ok);
    assert(provision::save_network_edit(root, {"legacy", "new-password"}, false, now, error));
    edited = read(network_file);
    assert(edited.find("# original\nschema_version = 2 # keep") == 0 &&
           edited.find("[[wifi]] # keep") != std::string::npos &&
           edited.find("password = \"new-password\" # keep") != std::string::npos);
    assert(provision::save_network_edit(root, {"legacy", ""}, true, 0, error));
    assert(provision::load_networks(root, loaded, error) && loaded.empty() &&
           read(network_file).find("# original\nschema_version = 2 # keep") == 0);
    fs::remove(network_file);
    assert(provision::save_network_edit(root, {"first", ""}, false, 0, error));
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 1 && loaded[0].ssid == "first");

    // A hotspot can be saved while absent; saving does not invent a connection timestamp.
    const auto account_bytes = read(card / "PicoPage/sync.toml");
    assert(provision::save_network_edit(root, {"phone-hotspot", "password1"}, false, 0, error));
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 2 &&
           loaded.back().ssid == "phone-hotspot" && loaded.back().password == "password1" &&
           loaded.back().last_connected == 0);
    assert(read(network_file).find("last_connected") == std::string::npos);
    // Adding networks leaves the connection automatic; only an explicit choice pins one.
    std::optional<provision::SavedWifi> selected;
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(read(network_file).find("selected_ssid") == std::string::npos);
    assert(provision::save_selected_network(root, "phone-hotspot", error));
    assert(provision::load_selected_network(root, selected, error) && selected && selected->ssid == "phone-hotspot");
    assert(read(card / "PicoPage/sync.toml") == account_bytes);

    // A pinned network survives connection history, additions and stronger alternatives.
    assert(provision::save_connection(root, "first", now, error));
    assert(provision::load_selected_network(root, selected, error) && selected->ssid == "phone-hotspot");
    assert(provision::save_network_edit(root, {"stronger", "password2"}, false, 0, error));
    assert(provision::load_selected_network(root, selected, error) && selected->ssid == "phone-hotspot");
    const auto pinned = read(network_file);
    assert(!provision::save_selected_network(root, "not-saved", error) && read(network_file) == pinned);
    // Forgetting the pinned network returns to automatic connection, and later edits keep it.
    assert(provision::save_network_edit(root, {"phone-hotspot", ""}, true, 0, error));
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(provision::save_network_edit(root, {"phone-hotspot", "password1"}, false, 0, error));
    assert(provision::save_network_edit(root, {"first", "password2"}, false, 0, error));
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(provision::save_selected_network(root, "first", error));
    assert(provision::save_network_edit(root, {"phone-hotspot", "password1"}, false, 0, error));
    assert(provision::load_selected_network(root, selected, error) && selected->ssid == "first");
    // An empty name selects automatic connection again without touching the entries.
    assert(provision::save_selected_network(root, "", error));
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 3);

    // A file without a selection is automatic, also after a connection reorders its entries.
    assert(store.save_private("network.toml", annotated, error) == StoreStatus::Ok);
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(provision::save_connection(root, "home", now + day, error));
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(provision::save_selected_network(root, "home", error));
    assert(read(network_file).find("# office note") != std::string::npos);
    // A pinned name that is no longer saved falls back to automatic connection.
    assert(store.save_private("network.toml", "selected_ssid = \"gone\"\nschema_version=2\n[[wifi]]\nssid=\"home\"\npassword=\"password\"\n", error) == StoreStatus::Ok);
    assert(provision::load_selected_network(root, selected, error) && !selected);
    // Adding a network with that leftover name must not pin it.
    assert(provision::save_network_edit(root, {"gone", "password3"}, false, 0, error));
    assert(provision::load_selected_network(root, selected, error) && !selected);
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 2);
    assert(store.save_private("network.toml", pinned, error) == StoreStatus::Ok);

    // Automatic connection tries the saved networks in range, strongest first, each once.
    const std::vector<provision::WifiCredentials> known{{"home", "password"}, {"office", "password2"}, {"phone", ""}};
    auto order = provision::connection_order(known, {{"cafe", -30, true}, {"phone", -45, true}, {"home", -60, false},
                                                     {"phone", -70, true}});
    assert(order.size() == 2 && order[0].ssid == "phone" && order[1].ssid == "home" && order[1].password == "password");
    assert(provision::connection_order(known, {{"cafe", -30, true}}).empty() && provision::connection_order({}, {{"home", -30, false}}).empty());
    assert(provision::network_absent(201) && provision::network_absent(211) && !provision::network_absent(15));
    assert(provision::password_rejected(15) && provision::password_rejected(202) && !provision::password_rejected(201) &&
           !provision::password_rejected(8));

    // Request completion retains a business transaction; decision timeout never drops a manual hold.
    network::Holds holds;
    auto transaction = std::make_shared<std::atomic_bool>(false);
    holds.await_decision(transaction, 100);
    assert(holds.retained());
    holds.expire(100 + network::Holds::decision_us - 1);
    assert(network::live(transaction));
    auto manual = std::make_shared<std::atomic_bool>(false);
    holds.manual = manual;
    holds.expire(100 + network::Holds::decision_us);
    assert(!network::live(transaction) && network::live(manual) && holds.retained());
    auto next = std::make_shared<std::atomic_bool>(false);
    holds.await_decision(next, 1000);
    network::release(transaction); // A late cancellation belongs only to the old session.
    assert(network::live(next));
    network::release(manual);
    assert(holds.retained());
    network::release(next);
    assert(!holds.retained());
    holds.manual = std::make_shared<std::atomic_bool>(false);
    holds.manual_suspended = true; // AP closure retains intent without restarting STA.
    assert(!holds.retained() && network::live(holds.manual));
    holds.stop(); // Sleep also discards the suspended intent.
    assert(!holds.retained() && !network::live(holds.manual));
    network::Holds reboot;
    assert(!reboot.retained());

    constexpr std::int64_t idle_limit = 600'000'000;
    network::IdleTimer idle;
    idle.update(false, 0);
    assert(!idle.expired(idle_limit - 1, idle_limit));
    assert(idle.expired(idle_limit, idle_limit));
    // A complete short /check or upload between two worker polls resets the deadline.
    idle.update(true, idle_limit - 100'000);
    idle.update(false, idle_limit - 50'000);
    assert(!idle.expired(idle_limit + 100'000, idle_limit));
    assert(!idle.expired(2 * idle_limit - 50'001, idle_limit));
    assert(idle.expired(2 * idle_limit - 50'000, idle_limit));
    idle.update(true, 2 * idle_limit);
    assert(!idle.expired(4 * idle_limit, idle_limit));
    idle.update(false, 4 * idle_limit);
    assert(!idle.expired(5 * idle_limit - 1, idle_limit));
    assert(idle.expired(5 * idle_limit, idle_limit));

    // A failed offline edit preserves both files and can be retried after storage recovers.
    const auto before_failed_save = read(network_file);
    fs::create_directory(card / "PicoPage/network.toml.tmp");
    assert(!provision::save_network_edit(root, {"phone-hotspot", "password2"}, false, 0, error));
    assert(read(network_file) == before_failed_save && read(card / "PicoPage/sync.toml") == account_bytes);
    fs::remove(card / "PicoPage/network.toml.tmp");
    assert(provision::save_network_edit(root, {"phone-hotspot", "password2"}, false, 0, error));
    assert(provision::save_connection(root, "phone-hotspot", now, error));
    assert(provision::save_network_edit(root, {"phone-hotspot", "password3"}, false, 0, error));
    assert(provision::load_networks(root, loaded, error) && loaded.back().password == "password3" &&
           loaded.back().last_connected == now);

    // Replacing a credential file leaves no copy of the previous secret behind.
    assert(store.save_private("network.toml", provision::network_toml({{{"next", ""}, 0}}), error) == StoreStatus::Ok);
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 1 && loaded[0].ssid == "next");
    assert(store.save_private("network.toml", provision::network_toml({}), error) == StoreStatus::Ok);
    assert(provision::load_networks(root, loaded, error) && loaded.empty());
    for (const auto& entry : fs::directory_iterator(card / "PicoPage")) {
        const auto name = entry.path().filename().string();
        assert(name == "network.toml" || name == "sync.toml");
        assert(read(entry.path()).find("密码") == std::string::npos);
    }
    assert(store.save_private("settings.toml", "x", error) == StoreStatus::UnsafePath);
    assert(store.save_private("../sync.toml", "x", error) == StoreStatus::UnsafePath);
    assert(store.save_private("sync.toml", std::string(8193, 'x'), error) != StoreStatus::Ok);
    assert(sync::load_config(root, configured, error));

    // Hand-written files outside the accepted shape are refused without echoing their contents.
    const std::string entry = "[[wifi]]\nssid = \"net\"\npassword = \"hunter2-long\"\n";
    std::string six = "schema_version = 2\n";
    for (int i = 0; i < 6; ++i) six += "[[wifi]]\nssid = \"net" + std::to_string(i) + "\"\npassword = \"hunter2-long\"\n";
    for (const std::string& text : std::initializer_list<std::string>{"schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter2-short\"\nextra = 1\n",
                                   "schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter2-long\"\nlast_connected = 1\n",
                                   "schema_version = 2\n[wifi]\nssid = \"net\"\npassword = \"hunter2-long\"\n",
                                   "schema_version = 3\n" + entry, "schema_version = 2\n" + entry + entry, six,
                                   "schema_version = 2\n" + entry + "last_connected = -1\n",
                                   "schema_version = 2\n" + entry + "last_connected = \"today\"\n",
                                   "schema_version = 2\n[[wifi]]\nssid = \"net\"\npassword = \"hunter\"\n",
                                   "schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter\"\n",
                                   "schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter2-open"}) {
        { std::ofstream file(card / "PicoPage/network.toml", std::ios::binary); file << text; }
        assert(!provision::load_networks(root, loaded, error) && loaded.empty());
        assert(error.find("hunter") == std::string::npos);
    }

    using provision::WifiFailure;
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 201).find("找不到") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 211).find("2.4 GHz") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 15).find("密码可能不正确。") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 8) == "无法连接“home”。");
    assert(provision::wifi_problem("home", WifiFailure::NoClock, 0).find("网络时间") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::NoAddress, 0).find("超时") != std::string::npos);
    assert(provision::wifi_qr({"PicoPage-18B8", "abcd2345"}) == "WIFI:T:WPA;S:PicoPage-18B8;P:abcd2345;;");
    assert(provision::wifi_qr({"a;b", "c:d\\e"}) == R"(WIFI:T:WPA;S:a\;b;P:c\:d\\e;;)");

    sync::HttpRequest request;
    assert(sync::build_auth_request(account, request, error) && request.method == "GET" && request.body.empty());
    assert(request.url == "https://sync.example.invalid/users/auth" && request.headers.size() == 3);
    assert(request.headers[2].first == "x-auth-key" && request.headers[2].second == account.userkey);
    auto insecure = account;
    insecure.endpoint = "http://sync.example.invalid";
    assert(!sync::build_auth_request(insecure, request, error) && request.url.empty());

    fs::remove_all(card);
    std::cout << "saved networks, connection mode and order, and failure sentences passed\n";
}
