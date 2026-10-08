#include "pico_page/provision.h"
#include "pico_page/store.h"
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

    // Form decoding: each action carries only its own fields, and the KOReader userkey.
    provision::Submission submission;
    assert(provision::parse_form("action=wifi_add&ssid=%E5%AE%B6+%222.4G%22%5C&wifi_password=p%22a%5Css+%E5%AF%86"
                                 "&endpoint=https%3A%2F%2Fx&password=ignored&other=1", submission, problem));
    assert(submission.action == provision::Action::WifiAdd && submission.verify_wifi);
    assert(submission.network.ssid == "家 \"2.4G\"\\" && submission.network.password == "p\"a\\ss 密");
    assert(submission.endpoint.empty() && submission.userkey.empty());
    assert(provision::parse_form("action=wifi_add&ssid=&manual=typed&wifi_password=", submission, problem));
    assert(submission.network.ssid == "typed" && submission.network.password.empty() && !submission.verify_wifi);
    assert(provision::parse_form("action=wifi_add&ssid=picked&manual=typed&wifi_password=", submission, problem));
    assert(submission.network.ssid == "picked" && submission.verify_wifi);
    assert(provision::parse_form("action=wifi_delete&ssid=home&wifi_password=x", submission, problem));
    assert(submission.action == provision::Action::WifiDelete && submission.network.ssid == "home" &&
           submission.network.password.empty() && !submission.verify_wifi);
    assert(provision::parse_form("action=account&endpoint=+https%3A%2F%2Fsync.example.invalid%2F%2F+"
                                 "&username=+%E8%AF%BB%E8%80%85+&password=correct+horse+%E7%94%B5%E6%B1%A0&ssid=x",
                                 submission, problem));
    assert(submission.action == provision::Action::Account && submission.network.ssid.empty() && !submission.verify_wifi);
    assert(submission.endpoint == "https://sync.example.invalid" && submission.username == "读者");
    assert(submission.userkey == "55d2fddf8ba5768bbc44b083846cf439");
    assert(provision::parse_form("action=account&endpoint=&username=reader&password=secret", submission, problem));
    assert(submission.endpoint == provision::default_endpoint && submission.userkey == "5ebe2294ecd0e0f08eab7690d2a6ee69");
    for (const std::string& body : std::initializer_list<std::string>{"", "ssid=net", "action=other", "action=wifi_add&ssid=",
                                   "action=wifi_delete&manual=net", "action=wifi_add&ssid=%4", "action=wifi_add&ssid=%zz",
                                   "action=wifi_add&ssid=net&wifi_password=short", "action=wifi_add&ssid=" + std::string(33, 'x'),
                                   "action=wifi_add&ssid=a%09b", "action=wifi_add&ssid=net&wifi_password=" + std::string(64, 'g'),
                                   "action=account&username=reader&password=", "action=account&username=reader&password=x&endpoint=http://sync.example",
                                   "action=account&username=&password=x", "action=account&username=a%0Ab&password=x",
                                   "action=wifi_add&ssid=" + std::string(provision::form_limit, 'x')}) {
        problem.clear();
        assert(!provision::parse_form(body, submission, problem) && !problem.empty());
        assert(problem.find("short") == std::string::npos);
    }

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

    // Connection order: nearby saved networks by signal; without any, the final entry alone.
    const std::vector<provision::NearbyWifi> nearby{{"stranger", -30}, {"n3", -70}, {"office", -50}};
    auto order = provision::connection_order(saved, nearby);
    assert(order.size() == 2 && order[0].ssid == "office" && order[1].ssid == "n3");
    order = provision::connection_order(saved, {});
    assert(order.size() == 1 && order[0].ssid == "n6");
    assert(provision::connection_order({}, nearby).empty());
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
    assert(provision::parse_form("action=wifi_add&ssid=&manual=phone-hotspot&wifi_password=password1",
                                 submission, problem));
    assert(provision::save_network_edit(root, submission.network, false, 0, error));
    assert(provision::load_networks(root, loaded, error) && loaded.size() == 2 &&
           loaded.back().ssid == "phone-hotspot" && loaded.back().password == "password1" &&
           loaded.back().last_connected == 0);
    assert(read(network_file).find("last_connected") == std::string::npos);
    auto offline_order = provision::connection_order(loaded, {});
    assert(offline_order.size() == 1 && offline_order[0].ssid == "phone-hotspot");
    assert(read(card / "PicoPage/sync.toml") == account_bytes);

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

    // The page escapes scanned names and saved values, and withholds the forms while busy.
    provision::PageState state;
    state.device_name = "PicoPage-<7F>";
    state.nearby = {{"<script>alert(1)</script>", -40}, {"a\"b'c&d", -41}, {std::string("bad\x01name"), -42}, {"", -43},
                    {std::string(33, 'x'), -44}};
    for (int i = 0; i < 30; ++i) state.nearby.push_back({"net-" + std::to_string(i), -50 - i});
    auto html = provision::page(state);
    assert(html.find("<script>alert") == std::string::npos && html.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
    assert(html.find("a&quot;b&#39;c&amp;d") != std::string::npos && html.find("bad") == std::string::npos);
    assert(html.find("PicoPage-&lt;7F&gt;") != std::string::npos);
    assert(html.find("net-17") != std::string::npos && html.find("net-18") == std::string::npos);
    // First setup: the add form is open, the account block waits for Wi-Fi.
    assert(html.find("value=\"wifi_add\"") != std::string::npos && html.find("id=\"wifi-add\" method") != std::string::npos);
    assert(html.find("id=\"wifi-add\" method=\"post\" action=\"/save\" class=\"block\" hidden") == std::string::npos);
    assert(html.find("需要先保存 Wi-Fi") != std::string::npos && html.find("value=\"account\"") == std::string::npos);
    assert(html.find("id=\"wifi-view\"") == std::string::npos);
    assert(html.find(">验证并保存 Wi-Fi</button>") != std::string::npos);
    provision::PageState manual_page;
    const auto manual_html = provision::page(manual_page);
    assert(manual_html.find(">保存 Wi-Fi</button>") != std::string::npos);
    assert(manual_html.find("从列表选择的网络先验证，成功后保存；手动填写的网络直接保存") != std::string::npos);
    assert(html.find("未开启的手机热点也可手动填写") != std::string::npos);
    assert(html.find("先保存并在设备上结束配置，再打开手机热点") != std::string::npos);

    state.saved = {{{"home<1>", "secret-wifi"}, now}, {{"office", ""}, 0}};
    state.nearby = {{"office", -40}, {"home<1>", -60}, {"new", -70}};
    state.endpoint = "https://sync.example.invalid";
    state.username = "u\"><b>";
    html = provision::page(state);
    assert(html.find("secret-wifi") == std::string::npos);
    assert(html.find("home&lt;1&gt;") != std::string::npos && html.find("u&quot;&gt;&lt;b&gt;") != std::string::npos);
    // Nearby by signal first, each with its own delete form; saved names cannot be picked again.
    assert(html.find("<b>office</b>") != std::string::npos && html.find("office") < html.find("home&lt;1&gt;"));
    assert(html.find("value=\"wifi_delete\"><input type=\"hidden\" name=\"ssid\" value=\"home&lt;1&gt;\"") != std::string::npos);
    assert(html.find("data-t=\"" + std::to_string(now) + "\"") != std::string::npos);
    assert(html.find("value=\"office\" disabled") != std::string::npos && html.find("value=\"new\" checked") != std::string::npos);
    assert(html.find("id=\"wifi-add\" method=\"post\" action=\"/save\" class=\"block\" hidden") != std::string::npos);
    assert(html.find("id=\"account-form\" method=\"post\" action=\"/save\" class=\"block\" hidden") != std::string::npos);
    assert(html.find("更换同步账户") != std::string::npos && html.find("已保存：u&quot;&gt;&lt;b&gt;") != std::string::npos);

    // A refused account stays open with the attempted values and an empty password.
    state.result = provision::Result{provision::Action::Account, true, "同步服务拒绝了这个账户或密码。"};
    state.retry_endpoint = "https://other.example.invalid";
    state.retry_username = "reader2";
    html = provision::page(state);
    assert(html.find("账户未保存") != std::string::npos && html.find("设备仍使用原账户“u&quot;&gt;&lt;b&gt;”") != std::string::npos);
    assert(html.find("value=\"reader2\"") != std::string::npos && html.find("other.example.invalid") != std::string::npos);
    assert(html.find("id=\"account-form\" method=\"post\" action=\"/save\" class=\"block\">") != std::string::npos);
    assert(html.find("重新验证") != std::string::npos && html.find("Wi-Fi：office · 已保存") != std::string::npos);
    state.result = provision::Result{provision::Action::WifiAdd, true, "无法连接“new”。"};
    html = provision::page(state);
    assert(html.find("Wi-Fi 未保存") != std::string::npos && html.find("同步账户不受影响") != std::string::npos);
    assert(html.find("id=\"wifi-add\" method=\"post\" action=\"/save\" class=\"block\">") != std::string::npos);
    state.result = provision::Result{provision::Action::WifiDelete, false, "已删除 Wi-Fi “x”。"};
    html = provision::page(state);
    assert(html.find("<div class=\"ok\" role=\"status\">已删除 Wi-Fi “x”。</div>") != std::string::npos);

    // Five saved networks: no add form.
    for (const char* name : {"a1", "a2", "a3"}) state.saved.push_back({{name, ""}, 0});
    html = provision::page(state);
    assert(html.find("value=\"wifi_add\"") == std::string::npos && html.find("删除一个后才能添加") != std::string::npos);

    state.result = provision::Result{provision::Action::WifiAdd, false, "已保存 Wi-Fi “phone-hotspot”。"};
    html = provision::page(state);
    assert(html.find("配置已保存，尚未测试连接") != std::string::npos);
    assert(html.find("再打开手机的 2.4 GHz 热点") != std::string::npos);
    state.result = provision::Result{provision::Action::WifiAdd, false, "已验证并保存 Wi-Fi “home”。", true};
    html = provision::page(state);
    assert(html.find("设备已连接该网络并取得 IP 地址") != std::string::npos);
    assert(html.find("配置已保存，尚未测试连接") == std::string::npos);
    state.busy = true;
    state.busy_action = provision::Action::Account;
    state.stage = 2;
    html = provision::page(state);
    assert(html.find("<form") == std::string::npos && html.find("content=\"2;url=/\"") != std::string::npos);
    assert(html.find("<li class=\"done\">取得网络时间</li><li class=\"now\">验证账户…</li><li class=\"next\">保存</li>") != std::string::npos);
    state.busy_action = provision::Action::WifiAdd;
    state.busy_name = "new";
    state.stage = 0;
    html = provision::page(state);
    assert(html.find("<li class=\"now\">保存“new”…</li>") != std::string::npos && html.find("<form") == std::string::npos);
    assert(html.find("连接“new”") == std::string::npos);
    state.busy_verify_wifi = true;
    html = provision::page(state);
    assert(html.find("<li class=\"now\">连接“new”…</li><li class=\"next\">保存</li>") != std::string::npos);
    assert(html.find("请看设备屏幕上的结果") != std::string::npos);
    state.stage = 1;
    html = provision::page(state);
    assert(html.find("<li class=\"done\">连接“new”</li><li class=\"now\">保存…</li>") != std::string::npos);
    state = {};
    state.saved_readable = false;
    html = provision::page(state);
    assert(html.find("network.toml 内容无效") != std::string::npos && html.find("<form") == std::string::npos);

    // Result sentences and the access point code.
    using provision::WifiFailure;
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 201).find("找不到") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 211).find("2.4 GHz") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 15).find("密码可能不正确（驱动原因 15）") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::Rejected, 8) == "无法连接“home”（驱动原因 8）。");
    assert(provision::wifi_problem("home", WifiFailure::NoClock, 0).find("网络时间") != std::string::npos);
    assert(provision::wifi_problem("home", WifiFailure::NoAddress, 0).find("超时") != std::string::npos);
    assert(provision::account_problem(true, 401, "").find("拒绝") != std::string::npos);
    assert(provision::account_problem(true, 502, "").find("HTTP 502") != std::string::npos);
    assert(provision::account_problem(false, 0, "HTTPS connection: ESP_FAIL").find("HTTPS connection") != std::string::npos);
    submission = {};
    submission.action = provision::Action::WifiAdd; submission.network.ssid = "home";
    assert(provision::saved_summary(submission) == "已保存 Wi-Fi “home”，未验证连接。");
    submission.verify_wifi = true;
    assert(provision::saved_summary(submission) == "已验证并保存 Wi-Fi “home”。");
    submission.action = provision::Action::WifiDelete;
    assert(provision::saved_summary(submission) == "已删除 Wi-Fi “home”。");
    submission.action = provision::Action::Account; submission.username = "reader";
    assert(provision::saved_summary(submission) == "已保存同步账户“reader”。");
    assert(provision::wifi_qr({"PicoPage-18B8", "abcd2345"}) == "WIFI:T:WPA;S:PicoPage-18B8;P:abcd2345;;");
    assert(provision::wifi_qr({"a;b", "c:d\\e"}) == R"(WIFI:T:WPA;S:a\;b;P:c\:d\\e;;)");

    sync::HttpRequest request;
    assert(sync::auth_request(account, request, error) && request.method == "GET" && request.body.empty());
    assert(request.url == "https://sync.example.invalid/users/auth" && request.headers.size() == 3);
    assert(request.headers[2].first == "x-auth-key" && request.headers[2].second == account.userkey);
    auto insecure = account;
    insecure.endpoint = "http://sync.example.invalid";
    assert(!sync::auth_request(insecure, request, error) && request.url.empty());

    fs::remove_all(card);
    std::cout << "phone setup actions, saved networks, page states and result sentences passed\n";
}
