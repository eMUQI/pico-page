#include "pico_page/app.h"
#include <SDL.h>
#include <curl/curl.h>
#include <future>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace pico_page;
namespace {
void usage() {
    std::cout
        << "Pico Page --card DIRECTORY [--book Books/file.epub] [--script FILE] [--headless]\n"
           "Keys: Left/1=K1, R/2=K2 full refresh, Right/3=K3, Escape=K2, Space=sleep/wake.\n"
           "Script: k1 | k2 | k3 | tap X Y | capture FILE.bmp | sleep | wake | open PATH | count\n";
}
struct Response {
    std::uint64_t id = 0;
    int status = 0;
    std::string body, error;
};
Response perform(NetworkOperation operation) {
    Response out;
    out.id = operation.id;
    if (operation.cancelled->load()) {
        out.error = "已取消";
        return out;
    }
    auto *curl = curl_easy_init();
    if (!curl) {
        out.error = "HTTP 初始化失败";
        return out;
    }
    const auto &request = operation.request;
    curl_slist *headers = nullptr;
    for (const auto &item : request.headers)
        headers = curl_slist_append(headers, (item.first + ": " + item.second).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(
        curl, CURLOPT_XFERINFOFUNCTION,
        +[](void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
            return static_cast<std::atomic_bool *>(data)->load() ? 1 : 0;
        });
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, operation.cancelled.get());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    if (!request.body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char *data, size_t size, size_t count, void *opaque) -> size_t {
            auto &body = *static_cast<std::string *>(opaque);
            const size_t bytes = size * count;
            if (bytes > 32768 - body.size())
                return 0;
            body.append(data, bytes);
            return bytes;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        out.error = "连接失败或响应超出限制；上传结果可能尚未确认";
        out.body.clear();
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.status = static_cast<int>(status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return out;
}
struct Host {
    ReaderApp app;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    std::vector<std::uint32_t> rgba = std::vector<std::uint32_t>(684 * 1216);
    std::uint64_t sequence = 0;
    std::size_t frames = 0;
    std::future<Response> network;
    explicit Host(std::string root) : app(std::move(root)) {}
    ~Host() {
        if (texture)
            SDL_DestroyTexture(texture);
        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }
    bool init(bool headless) {
        if (!headless) {
            if (SDL_Init(SDL_INIT_VIDEO) != 0)
                return false;
            window = SDL_CreateWindow("Pico Page", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      456, 811, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!window)
                return false;
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            if (!renderer)
                return false;
            SDL_RenderSetLogicalSize(renderer, 684, 1216);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, 684, 1216);
            if (!texture)
                return false;
        }
        if (!app.initialize())
            return false;
        return flush();
    }
    void pixels() {
        const auto &c = app.canvas();
        for (int y = 0; y < 1216; ++y)
            for (int x = 0; x < 684; ++x) {
                const auto gray = c.pixel(x, y) * 17U;
                rgba[y * 684 + x] = 0xff000000U | (gray << 16) | (gray << 8) | gray;
            }
    }
    bool flush() {
        unsigned loops = 0;
        while (app.pending_frame()) {
            if (++loops > 4) {
                std::cerr << "Repeated presentation failure\n";
                return false;
            }
            const auto frame = *app.pending_frame();
            pixels();
            bool okay = true;
            if (renderer) {
                okay = SDL_UpdateTexture(texture, nullptr, rgba.data(), 684 * 4) == 0 &&
                       SDL_RenderClear(renderer) == 0 &&
                       SDL_RenderCopy(renderer, texture, nullptr, nullptr) == 0;
                if (okay)
                    SDL_RenderPresent(renderer);
            }
            app.complete_frame(okay);
            ++frames;
            std::cout << "frame " << frames << " "
                      << (frame.refresh == RefreshIntent::FullClean ? "full" : "content")
                      << " view=" << static_cast<int>(app.view())
                      << " saved=" << (app.saved_record() ? app.saved_record()->local_revision : 0)
                      << "\n";
            if (!okay)
                return false;
            // The desktop preview has no input sampling to protect and commits at once.
            if (!app.pending_frame() && app.save_due())
                app.save_progress();
        }
        return true;
    }
    void poll_network() {
        if (network.valid() &&
            network.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            auto response = network.get();
            app.complete_network(response.id, response.status, response.body, response.error);
            flush();
        }
        if (!network.valid()) {
            auto request = app.take_network_request();
            if (request)
                network = std::async(std::launch::async, perform, std::move(*request));
        }
    }
    // The desktop preview has no radio. It shows the setup page with placeholder access values
    // and never reports a saved or failed submission.
    bool poll_portal() {
        const auto action = app.take_portal_action();
        if (action == PortalAction::Start) {
            if (!app.prepare_portal())
                app.show_portal({PortalPhase::Closed, "", "", "", "无法开启手机配置，请稍后重试。"});
            else if (flush())
                app.show_portal({PortalPhase::Waiting, "PicoPage-PREVIEW", "preview0", "http://192.168.4.1", ""});
        } else if (action == PortalAction::Stop)
            app.show_portal({PortalPhase::Closed, "", "", "", "桌面预览不提供设备热点。"});
        return flush();
    }
    bool gesture(Input key, bool touch = false, int x = 0, int y = 0) {
        ++sequence;
        app.input({{sequence, Phase::Begin, key}, touch, x, y});
        app.input({{sequence, Phase::End, key}, touch, x, y});
        return flush() && poll_portal();
    }
    bool capture(const std::string &path) {
        pixels();
        auto *surface = SDL_CreateRGBSurfaceWithFormatFrom(rgba.data(), 684, 1216, 32, 684 * 4,
                                                           SDL_PIXELFORMAT_ARGB8888);
        if (!surface)
            return false;
        const bool okay = SDL_SaveBMP(surface, path.c_str()) == 0;
        SDL_FreeSurface(surface);
        return okay;
    }
    bool command(const std::string &line) {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if (command.empty() || command[0] == '#')
            return true;
        if (command == "k1")
            return gesture(Input::K1);
        if (command == "k2")
            return gesture(Input::K2);
        if (command == "k3")
            return gesture(Input::K3);
        if (command == "tap") {
            int x, y;
            if (!(input >> x >> y))
                return false;
            return gesture(Input::TouchCenter, true, x, y);
        }
        if (command == "capture") {
            std::string path;
            std::getline(input >> std::ws, path);
            return capture(path);
        }
        if (command == "open") {
            std::string path;
            std::getline(input >> std::ws, path);
            return app.open_book(path) && flush();
        }
        if (command == "count") {
            // Finishes whole-book page counting, which the device spreads over idle time.
            while (app.background_due())
                app.background_step();
            return true;
        }
        if (command == "sleep")
            return app.prepare_sleep() && flush();
        if (command == "wake") {
            app.wake();
            return flush();
        }
        std::cerr << "Unknown script command: " << command << "\n";
        return false;
    }
};
} // namespace
int main(int argc, char **argv) {
    std::string root = ".local/card", book, script;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            usage();
            return 0;
        }
        if (arg == "--headless")
            headless = true;
        else if ((arg == "--card" || arg == "--book" || arg == "--script") && i + 1 < argc) {
            auto value = argv[++i];
            if (arg == "--card")
                root = value;
            else if (arg == "--book")
                book = value;
            else
                script = value;
        } else {
            usage();
            return 2;
        }
    }
    if (headless && script.empty() && book.empty()) {
        std::cerr << "Headless run requires a book or script\n";
        return 2;
    }
    std::error_code path_error;
    root = std::filesystem::canonical(root, path_error).string();
    if (path_error) {
        std::cerr << "Cannot access card directory\n";
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    Host host(root);
    if (!host.init(headless)) {
        std::cerr << "Initialization failed: " << SDL_GetError() << "\n";
        return 1;
    }
    if (!book.empty() && (!host.app.open_book(book) || !host.flush())) {
        std::cerr << host.app.status() << "\n";
        return 1;
    }
    if (!script.empty()) {
        std::ifstream file(script);
        if (!file) {
            std::cerr << "Cannot open script\n";
            return 1;
        }
        std::string line;
        while (std::getline(file, line))
            if (!host.command(line)) {
                std::cerr << "Failed: " << line << "; " << host.app.status() << "\n";
                return 1;
            }
    }
    if (headless)
        return 0;
    bool running = true;
    while (running) {
        host.poll_network();
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, host.app.background_due() ? 1 : 50)) {
            host.app.background_step();
            continue;
        }
        if (event.type == SDL_QUIT) {
            host.app.save_progress();
            running = false;
        } else if (event.type == SDL_WINDOWEVENT &&
                   (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
            SDL_RenderClear(host.renderer);
            SDL_RenderCopy(host.renderer, host.texture, nullptr, nullptr);
            SDL_RenderPresent(host.renderer);
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
            host.gesture(Input::TouchCenter, true, event.button.x, event.button.y);
        else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
            switch (event.key.keysym.sym) {
            case SDLK_LEFT:
            case SDLK_1:
                host.gesture(Input::K1);
                break;
            case SDLK_r:
            case SDLK_2:
            case SDLK_ESCAPE:
                host.gesture(Input::K2);
                break;
            case SDLK_RIGHT:
            case SDLK_3:
                host.gesture(Input::K3);
                break;
            case SDLK_SPACE:
                if (host.app.view() == View::Sleeping)
                    host.app.wake();
                else
                    host.app.prepare_sleep();
                host.flush();
                break;
            default:
                break;
            }
        }
    }
    return 0;
}
