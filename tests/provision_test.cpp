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

    // Form decoding, the two independent sections and the KOReader userkey.
    provision::Submission submission;
    assert(provision::parse_form("ssid=%E5%AE%B6+%222.4G%22%5C&wifi_password=p%22a%5Css+%E5%AF%86&endpoint=+https%3A%2F%2Fsync.example.invalid%2F%2F+"
                                 "&username=+%E8%AF%BB%E8%80%85+&password=correct+horse+%E7%94%B5%E6%B1%A0&other=1",
                                 submission, problem));
    assert(submission.wifi && submission.account);
    assert(submission.network.ssid == "家 \"2.4G\"\\" && submission.network.password == "p\"a\\ss 密");
    assert(submission.endpoint == "https://sync.example.invalid" && submission.username == "读者");
    assert(submission.userkey == "55d2fddf8ba5768bbc44b083846cf439");
    assert(provision::parse_form("ssid=open&username=kept&endpoint=", submission, problem));
    assert(submission.wifi && !submission.account && submission.network.password.empty());
    assert(provision::parse_form("ssid=&username=reader&password=secret", submission, problem));
    assert(!submission.wifi && submission.account && submission.endpoint == provision::default_endpoint);
    assert(submission.userkey == "5ebe2294ecd0e0f08eab7690d2a6ee69");
    for (const std::string& body : std::initializer_list<std::string>{"", "ssid=&password=", "ssid=%4", "ssid=%zz", "ssid=net&wifi_password=short",
                                   "ssid=" + std::string(33, 'x'), "ssid=a%09b", "ssid=net&wifi_password=" + std::string(64, 'g'),
                                   "username=reader&password=x&endpoint=http://sync.example",
                                   "username=&password=x", "username=a%0Ab&password=x",
                                   "ssid=" + std::string(provision::form_limit, 'x')}) {
        problem.clear();
        assert(!provision::parse_form(body, submission, problem) && !problem.empty());
        assert(problem.find("short") == std::string::npos);
    }

    // The written files are read back by the parsers the firmware uses.
    assert(!provision::load_wifi(root, submission.network, error));
    const provision::WifiCredentials wifi{"家 \"2.4G\"\\", "p\"a\\ss 密码"};
    assert(provision::valid_wifi(wifi) && provision::valid_wifi({"open", ""}) && provision::valid_wifi({"psk", std::string(64, 'A')}));
    assert(!provision::valid_wifi({"", "12345678"}) && !provision::valid_wifi({"net", "1234567"}) &&
           !provision::valid_wifi({"net", std::string(65, 'a')}));
    assert(store.save_private("network.toml", provision::network_toml(wifi), error) == StoreStatus::Ok);
    provision::WifiCredentials loaded;
    assert(provision::load_wifi(root, loaded, error) && loaded.ssid == wifi.ssid && loaded.password == wifi.password);
    const sync::Config account{"https://sync.example.invalid", "读者 \"a\\b\"", "55d2fddf8ba5768bbc44b083846cf439", "pico-page-test", 20240114};
    assert(store.save_private("sync.toml", provision::sync_toml(account), error) == StoreStatus::Ok);
    sync::Config configured;
    assert(sync::load_config(root, configured, error));
    assert(configured.endpoint == account.endpoint && configured.username == account.username &&
           configured.userkey == account.userkey && configured.device_id == account.device_id &&
           configured.dom_version == account.dom_version);

    // Replacing a credential file leaves no copy of the previous secret behind.
    assert(store.save_private("network.toml", provision::network_toml({"next", ""}), error) == StoreStatus::Ok);
    assert(provision::load_wifi(root, loaded, error) && loaded.ssid == "next" && loaded.password.empty());
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
    for (const std::string& text : std::initializer_list<std::string>{"schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter2-short\"\nextra = 1\n",
                                   "schema_version = 2\n[wifi]\nssid = \"net\"\npassword = \"hunter2-long\"\n",
                                   "schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter\"\n",
                                   "schema_version = 1\n[wifi]\nssid = \"net\"\npassword = \"hunter2-open"}) {
        { std::ofstream file(card / "PicoPage/network.toml", std::ios::binary); file << text; }
        assert(!provision::load_wifi(root, loaded, error) && loaded.ssid.empty());
        assert(error.find("hunter") == std::string::npos);
    }

    // The page escapes scanned names and saved values, and withholds the form while busy.
    provision::PageState state;
    state.networks = {"<script>alert(1)</script>", "a\"b'c&d", std::string("bad\x01name"), "", std::string(33, 'x')};
    for (int i = 0; i < 30; ++i) state.networks.push_back("net-" + std::to_string(i));
    state.current_ssid = "home<1>";
    state.username = "u\"><b>";
    state.notice = "<i>done</i>";
    auto html = provision::page(state);
    assert(html.find("<script>") == std::string::npos && html.find("&lt;script&gt;alert(1)&lt;/script&gt;") != std::string::npos);
    assert(html.find("a&quot;b&#39;c&amp;d") != std::string::npos && html.find("bad") == std::string::npos);
    assert(html.find("home&lt;1&gt;") != std::string::npos && html.find("u&quot;&gt;&lt;b&gt;") != std::string::npos);
    assert(html.find("&lt;i&gt;done&lt;/i&gt;") != std::string::npos && html.find("<i>") == std::string::npos);
    assert(html.find("net-17") != std::string::npos && html.find("net-18") == std::string::npos);
    assert(html.find(provision::default_endpoint) != std::string::npos && html.find("action=\"/save\"") != std::string::npos);
    state.busy = true;
    html = provision::page(state);
    assert(html.find("<form") == std::string::npos && html.find("content=\"5;url=/\"") != std::string::npos);

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
    submission.wifi = true; submission.network.ssid = "home";
    assert(provision::saved_summary(submission) == "已保存 Wi-Fi “home”。");
    submission.account = true; submission.username = "reader";
    assert(provision::saved_summary(submission) == "已保存 Wi-Fi “home”和同步账户“reader”。");
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
    std::cout << "phone setup form, credential files, page escaping and result sentences passed\n";
}
