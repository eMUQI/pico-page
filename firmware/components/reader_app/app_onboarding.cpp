#include "app_impl.h"
#include "onboarding_font.h"

namespace pico_page {

void ReaderApp::Impl::start_onboarding(bool from_help) {
    onboarding_from_help = from_help;
    onboarding_step = OnboardingStep::Welcome;
    onboarding_middle_hint = false;
    // The guide must not depend on the card's fonts or an open book's typeface.
    ui.open_fallback();
    draw_onboarding();
}

void ReaderApp::Impl::finish_onboarding() {
    if (onboarding_from_help) {
        help_page = 0;
        draw_help();
    } else {
        if (card_available) {
            const auto snapshot = store.settings_snapshot();
            auto values = snapshot.config.settings;
            values.onboarding_done = true;
            std::string detail;
            // Failed commits remain incomplete, but never trap the user in the guide.
            if (store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok)
                settings.onboarding_done = true;
        }
        draw_library();
    }
    overlay();
}

void ReaderApp::Impl::draw_turn_regions(int x, int y, int width, int height, int selected, bool labels, std::uint8_t divider) {
    const int edges[] = {0, width * 205 / 684, width * 479 / 684, width};
    const char* symbols[] = {"‹", "≡", "›"};
    const char* names[] = {"上一页", "工具", "下一页"};
    for (int i = 0; i < 3; ++i) {
        const int left = x + edges[i], w = edges[i + 1] - edges[i];
        const bool active = selected == HelpPrevious + i;
        canvas.fill_rect(left, y, w, height, active ? 1 : 15);
        if (i) canvas.line(left, y, left, y + height - 1, divider, 2);
        if (labels) {
            center(symbols[i], left, y + height / 2 - 56, w, 40, active ? 15 : 1);
            center(names[i], left, y + height / 2 + 12, w, 26, active ? 15 : i == 1 ? 3 : 1);
        }
    }
}

void ReaderApp::Impl::draw_onboarding() {
    view = View::Onboarding;
    hits.clear(); canvas.clear();
    const bool welcome = onboarding_step == OnboardingStep::Welcome;
    const bool practice = onboarding_step == OnboardingStep::PageTurn;
    const std::string finish = onboarding_from_help ? "返回使用说明" : "进入书库";
    const auto primary = [&](int id, const std::string& label, int y) {
        button(id, label, 36, y, 612, 96, true, 30, true);
    };
    const auto link = [&](const std::string& label, int x, int y, int w) {
        center(label, x, y, w, 26);
        const int width = text_width(label, 26), left = x + (w - width) / 2;
        canvas.line(left, y + 35, left + width, y + 35, 1);
    };
    if (welcome) {
        text("欢迎使用", 56, 260, 26, 572, 3);
        text("Pico Page", 56, 316, 72, 572, 1, true);
        graphics::Font subtitle;
        if (subtitle.open_memory(kOnboardingFont, sizeof(kOnboardingFont)))
            text(subtitle, "花一分钟，学会翻页和打开工具。", 56, 424, 34, 572);
        else text("花一分钟，学会翻页和打开工具。", 56, 424, 34, 572);
        canvas.line(56, 640, 628, 640, 1, 3);
        for (int i = 0; i < 2; ++i) {
            text(std::to_string(i + 1), 56, 668 + i * 96, 28, 64, 1, true);
            text(i == 0 ? "翻页" : "打开工具", 120, 668 + i * 96, 28, 508);
            canvas.line(56, 736 + i * 96, 628, 736 + i * 96, 1, 2);
        }
        primary(OnboardingStart, "开始", 968);
        link(onboarding_from_help ? finish : "跳过，直接进入书库", 36, 1106, 612);
        hits.push_back({36, 1080, 612, 88, OnboardingFinish});
        return;
    }
    text(practice ? "1 / 2" : "2 / 2", 36, 32, 26);
    canvas.line(0, 103, 683, 103, 1, 3);
    if (practice) {
        const auto label = onboarding_from_help ? finish : "跳过";
        const int width = text_width(label, 26);
        link(label, 648 - width, 32, width);
        hits.push_back({342, 0, 342, 104, OnboardingFinish});
        text("翻页", 36, 144, 34, 612, 1, true);
        title_lines("点屏幕左侧或右侧翻页，也可以用按键。", 36, 200, 612, 26, false, 2, 3);
        draw_turn_regions(36, 320, 612, 420, HelpNext, true);
        outline(36, 320, 612, 420, 3);
        for (int i = 0; i < 3; ++i) {
            const int x = 36 + i * 209;
            outline(x, 780, 194, 112);
            center("K" + std::to_string(i + 1), x, 794, 194, 26, 1, true);
            center(i == 0 ? "上一页" : i == 1 ? "全刷" : "下一页", x, 834, 194, 22);
        }
        canvas.line(0, 1064, 683, 1064, 1, 3);
        center(onboarding_middle_hint ? "中间是工具，下一步介绍" : "试试：点屏幕右侧 ›", 0, 1120, 684, 30, 1, true);
        return;
    }
    text("最后一步", 648 - text_width("最后一步", 26), 32, 26, 200, 5);
    text("轻点中间，打开工具", 36, 144, 34, 612, 1, true);
    title_lines("阅读时点屏幕中间，底部会出现工具面板。点面板以外的地方收起。", 36, 200, 612, 26, false, 3, 3);
    const int ids[] = {Contents, Typography, Sync, Refresh, FullRefresh};
    const char* names[] = {"目录", "排版", "同步", "刷新模式", "全刷"};
    outline(36, 360, 612, 136, 3);
    for (int i = 0; i < 5; ++i) {
        const int x = 36 + i * 612 / 5, right = 36 + (i + 1) * 612 / 5;
        if (i) canvas.line(x, 360, x, 495, 1, 2);
        tool_icon(ids[i], (x + right) / 2, 402);
        center(names[i], x, 442, right - x, i == 3 ? 20 : 22);
    }
    const char* labels[] = {"目录", "排版", "同步", "全刷"};
    const char* descriptions[] = {"跳到任意章节", "字号、行距、边距、字体", "在其他设备上接着读", "屏幕有残影时用"};
    for (int i = 0; i < 4; ++i) {
        const int y = 560 + i * 76;
        text(labels[i], 36, y + 18, 26, 96, 1, true);
        text(descriptions[i], 132, y + 18, 26, 516);
        canvas.line(36, y + 75, 648, y + 75, 12);
    }
    center("更多操作见 设置 → 使用说明", 36, 1016, 612, 24, 3);
    primary(OnboardingFinish, finish, 1072);
}

void ReaderApp::Impl::onboarding_input(const AppInput& input) {
    if (input.queued) return;
    const bool old_hint = onboarding_middle_hint;
    onboarding_middle_hint = false;
    if (!input.touch && input.event.input == Input::K2) {
        if (old_hint) draw_onboarding();
        force_refresh(); return;
    }
    if (input.touch) {
        for (const auto& hit : hits) if (hit.contains(input.x, input.y)) {
            if (hit.id == OnboardingFinish) { finish_onboarding(); return; }
            if (hit.id == OnboardingStart) {
                onboarding_step = OnboardingStep::PageTurn;
                draw_onboarding(); overlay(); return;
            }
        }
    }
    const auto action = map_input(Context::Reading, input.event.input);
    const bool next = input.touch ? input.x >= 479 && input.y >= 104 && input.y < 1064 : action == Action::NextPage;
    if (onboarding_step == OnboardingStep::PageTurn) {
        if (next) onboarding_step = OnboardingStep::Tools;
        else if (input.touch && input.y >= 104 && input.y < 1064 && input.x >= 205 && input.x < 479)
            onboarding_middle_hint = true;
        if (next || old_hint != onboarding_middle_hint) { draw_onboarding(); overlay(); }
    } else if (!input.touch && action == Action::NextPage) {
        if (onboarding_step == OnboardingStep::Tools) { finish_onboarding(); return; }
        onboarding_step = OnboardingStep::PageTurn;
        draw_onboarding(); overlay();
    }
}
} // namespace pico_page
