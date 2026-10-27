#include "app_impl.h"

namespace pico_page {

void ReaderApp::Impl::draw_import() {
    if (!bookmark_io_ready()) return;
    view = View::Import;
    hits.clear();
    canvas.clear();
    header("网络与传输", Back, import_from_settings ? "设置" : "书库");
    text("USB 文件传输", 36, 140, 24, 612, 3);
    const int usb = wrapped("电脑可直接管理 TF 卡。传输期间暂停阅读和同步，退出时会重启。", 36, 180, 612, 30);
    button(UsbStart, "开启 USB 传输", 24, 204 + usb, 636, 96, false, 30);
    int y = 340 + usb;
    canvas.line(36, y, 647, y, 12, 2);
    text("Wi-Fi", 36, y + 36, 24, 612, 3);
    y += 76;
    std::vector<provision::SavedWifi> networks;
    std::string detail;
    const bool readable = provision::load_networks(root, networks, detail);
    if (!wifi_enabled()) {
        wrapped("Wi-Fi 已关闭。打开后可以传书和同步。", 36, y + 20, 612, 28);
        button(WifiSettings, "Wi-Fi 设置", 24, y + 156, 636, 96);
    } else if (readable && !networks.empty()) {
        entry_row(WifiSettings, "Wi-Fi 设置", "已保存 " + std::to_string(networks.size()) + " 个网络", network_label(), y, 120);
        text("Wi-Fi 传书", 36, y + 156, 24, 612, 3);
        entry_row(WifiUploadStation, "经已保存的 Wi-Fi 传书", "手机连同一网络，传书时仍能上网", "", y + 196, 120);
        entry_row(WifiUpload, "经设备热点传书", "不需要路由器，手机暂时无法访问互联网", "", y + 316, 120);
    } else {
        const int height = readable ? 104 : 120;
        entry_row(WifiSettings, "Wi-Fi 设置", readable ? "" : "PicoPage/network.toml 无效",
                  readable ? "未添加网络" : "", y, height);
        entry_row(WifiUpload, "Wi-Fi 传书", "经设备热点，不需要路由器", "", y + height, 120);
        wrapped("添加 Wi-Fi 后，也可以经已保存的网络传书，手机传书时仍能上网。", 36, y + height + 140, 612, 24, true, 3);
    }
    // Start failures name their cause in several clauses, so the notice wraps.
    if (!notice().empty()) {
        canvas.line(0, 1012, 683, 1012, 1, 3);
        wrapped(message, 36, 1044, 612, 24);
    }
}


void ReaderApp::Impl::draw_portal() {
    view = View::Portal;
    hits.clear();
    canvas.clear();
    header("Wi-Fi 传书", 0, "", 420);
    if (!portal_closing) wifi_link(44);
    const auto scheme = portal.url.find("://");
    const auto address = scheme == std::string::npos ? portal.url : portal.url.substr(scheme + 3);
    if (portal.url.empty()) {
        note(portal_closing ? "正在结束…" : portal_station ? "正在连接已保存的 Wi-Fi…" : "正在开启设备热点…", 200);
    } else if (!portal.network.empty()) {
        text("用手机扫码打开传书页", 36, 140, 30, 612, 1, true);
        wrapped("手机或电脑需连着「" + portal.network + "」。", 36, 188, 612, 24, true, 3);
        graphics::draw_qr(canvas, portal.url, 152, 256, 380);
        center("或在浏览器输入", 0, 660, 684, 24, 3);
        center(address, 0, 700, 684, 34);
        canvas.line(0, portal_summary_line = 780, 683, 780, 1, 3);
        upload_summary(808, 30);
    } else {
        text("1  用手机连接设备热点", 36, 128, 26, 612, 1, true);
        graphics::draw_qr(canvas, provision::wifi_qr({portal.access_ssid, portal.access_password}), 36, 176, 264);
        text("热点名称", 324, 196, 22, 324, 3);
        text(portal.access_ssid, 324, 230, 28, 324);
        text("密码", 324, 300, 22, 324, 3);
        text(portal.access_password, 324, 334, 28, 324);
        text("扫码可直接连接", 324, 400, 22, 324, 3);
        text("2  连接后扫码打开传书页", 36, 472, 26, 612, 1, true);
        graphics::draw_qr(canvas, portal.url, 36, 520, 264);
        text("或在浏览器输入", 324, 540, 22, 324, 3);
        text(address, 324, 574, 28, 324);
        text("书籍、字体、锁屏图片", 324, 660, 22, 324, 3);
        canvas.line(0, portal_summary_line = 816, 683, 816, 1, 3);
        upload_summary(844, 28);
        const int hint_y = portal.saved_books > 0 ? 970 : 936;
        text("iPhone 提示无互联网时，选择「不连接互联网使用」，", 36, hint_y, 22, 612, 3);
        text("再用浏览器打开上述地址。", 36, hint_y + 32, 22, 612, 3);
    }
    if (!portal_closing) button(PortalStop, "结束传书", 24, 1080, 636, 96);
}


} // namespace pico_page
