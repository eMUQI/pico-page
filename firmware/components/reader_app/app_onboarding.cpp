#include "app_impl.h"
#include "onboarding_font.h"

namespace pico_page {

namespace {
constexpr const char* kPracticePages[] = {
    "清晨，窗外下起了小雨。她把椅子挪到窗边，翻开那本读了一半的书。雨声很轻，纸上的字也显得安静。",
    "读到一半，她停下来，想起前面某一章的情节。往回翻几页，答案就在那里，和记忆里的稍有不同。",
    "雨停的时候，书也读到了最后一页。她合上书，想了想明天要读什么。",
};
constexpr int kPracticePageCount = sizeof(kPracticePages) / sizeof(kPracticePages[0]);
constexpr const char* kToolPurposes[] = {
    "跳到任意章节。", "调整字号、行距、边距和字体。", "与 KOReader 同步阅读位置。", "设置多久全刷一次屏幕。", "立即全刷，清除残影。",
};
}

void ReaderApp::Impl::start_onboarding(bool from_help) {
    onboarding_from_help = from_help;
    message.clear();
    onboarding_step = OnboardingStep::Language;
    ui.open_fallback();
    draw_onboarding();
}

void ReaderApp::Impl::start_practice() {
    onboarding_step = OnboardingStep::Practice;
    onboarding_task = OnboardingTask::Next;
    onboarding_page = 0;
    onboarding_tool = 0;
    onboarding_tools_open = false;
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
            values.onboarding_in_progress = false;
            std::string detail;
            if (store.apply_settings(snapshot, values, false, detail) == StoreStatus::Ok) {
                settings.onboarding_done = true;
                settings.onboarding_in_progress = false;
            }
        }
        draw_library();
    }
    overlay();
}

void ReaderApp::Impl::draw_turn_regions(int x, int y, int width, int height, int selected, std::uint8_t divider) {
    const int edges[] = {0, width * 205 / 684, width * 479 / 684, width};
    for (int i = 0; i < 3; ++i) {
        const int left = x + edges[i], w = edges[i + 1] - edges[i];
        canvas.fill_rect(left, y, w, height, selected == HelpPrevious + i ? 1 : 15);
        if (i) canvas.line(left, y, left, y + height - 1, divider, 2);
    }
}

void ReaderApp::Impl::draw_onboarding() {
    view = View::Onboarding;
    hits.clear(); canvas.clear();
    const bool welcome = onboarding_step == OnboardingStep::Welcome;
    const bool practice = onboarding_step == OnboardingStep::Practice;
    const std::string finish = onboarding_from_help ? tr("返回使用说明") : tr("进入书库");
    const auto primary = [&](int id, const std::string& label, int y) {
        button(id, label, 36, y, 612, 96, true, 30, true);
    };
    const auto link = [&](const std::string& label, int x, int y, int w) {
        center(label, x, y, w, 26);
        const int width = text_width(label, 26), left = x + (w - width) / 2;
        canvas.line(left, y + 35, left + width, y + 35, 1);
    };
    if (onboarding_step == OnboardingStep::Language) {
        header(tr("选择语言 / Language"), onboarding_from_help ? Back : 0, tr("使用说明"));
        const int description_bottom = draw_language_choices();
        if (!message.empty()) title_lines(message, 36, description_bottom + 16, 612, 24, false, 3, 3);
        primary(OnboardingNext, tr("继续"), 1080);
        return;
    }
    if (welcome) {
        text("‹ " + std::string(tr("语言")), 28, 32, 28, 200);
        hits.push_back({0, 0, 228, 104, Back});
        text(tr("欢迎使用"), 36, 260, 26, 612, 3);
        text("Pico Page", 36, 316, 72, 612, 1, true);
        graphics::Font subtitle;
        if (subtitle.open_memory(kOnboardingFont, sizeof(kOnboardingFont)))
            title_lines(subtitle, tr("花一分钟，学会翻页和打开工具。"), 36, 424, 612, 34, false, 2);
        else title_lines(tr("花一分钟，学会翻页和打开工具。"), 36, 424, 612, 34, false, 2);
        canvas.fill_rect(36, 640, 612, 3, 1);
        for (int i = 0; i < 2; ++i) {
            text(std::to_string(i + 1), 36, 668 + i * 96, 28, 64, 1, true);
            text(i == 0 ? tr("翻页") : tr("打开工具"), 100, 668 + i * 96, 28, 548);
            canvas.fill_rect(36, 736 + i * 96, 612, 2, 1);
        }
        primary(OnboardingStart, tr("开始"), 968);
        link(onboarding_from_help ? finish : tr("跳过，直接进入书库"), 36, 1106, 612);
        hits.push_back({36, 1080, 612, 88, OnboardingFinish});
        return;
    }
    text(practice ? "1 / 2" : "2 / 2", 36, 32, 26);
    canvas.line(0, 103, 683, 103, 1, 3);
    if (practice) {
        draw_practice(finish);
        return;
    }
    text(tr("最后一步"), 648 - text_width(tr("最后一步"), 26), 32, 26, 200, 5);
    text(tr("工具栏里有什么"), 36, 144, 34, 612, 1, true);
    title_lines(tr("阅读时点屏幕中间打开工具栏，点工具栏以外的地方收起。"), 36, 200, 612, 26, false, 3, 3);
    const int ids[] = {Contents, Typography, Sync, Refresh, FullRefresh};
    const char* names[] = {tr("目录"), tr("排版"), tr("同步"), tr("刷新模式"), tr("全刷")};
    outline(36, 360, 612, 136, 3);
    for (int i = 0; i < 5; ++i) {
        const int x = 36 + i * 612 / 5, right = 36 + (i + 1) * 612 / 5;
        if (i) canvas.line(x, 360, x, 495, 1, 2);
        tool_icon(ids[i], (x + right) / 2, 402);
        const auto label = ids[i] == Refresh && text_width(names[i], 20) > right - x - 16 ? tr("模式") : names[i];
        center(label, x, 442, right - x, i == 3 ? 20 : 22);
    }
    const char* labels[] = {tr("目录"), tr("排版"), tr("同步"), tr("全刷")};
    const char* descriptions[] = {tr("跳到任意章节"), tr("字号、行距、边距、字体"), tr("在其他设备上接着读"), tr("屏幕有残影时用")};
    int label_width = 96;
    for (const auto* label : labels) label_width = std::max(label_width, text_width(label, 26) + 20);
    for (int i = 0; i < 4; ++i) {
        const int y = 560 + i * 76;
        text(labels[i], 36, y + 18, 26, label_width - 12, 1, true);
        const int text_height = title_lines(descriptions[i], 36 + label_width, y, 612 - label_width, 26, false, 2, 1, false);
        title_lines(descriptions[i], 36 + label_width, y + (76 - text_height) / 2, 612 - label_width, 26);
        canvas.line(36, y + 75, 648, y + 75, 12);
    }
    center(tr("更多操作见 设置 → 使用说明"), 36, 1016, 612, 24, 3);
    primary(OnboardingFinish, finish, 1072);
}

void ReaderApp::Impl::draw_practice(const std::string& finish) {
    const auto label = onboarding_from_help ? finish : tr("跳过");
    const int label_width = text_width(label, 26);
    text(label, 648 - label_width, 32, 26);
    canvas.line(648 - label_width, 67, 648, 67, 1);
    hits.push_back({342, 0, 342, 104, OnboardingFinish});
    std::string title, hint;
    if (onboarding_tools_open && onboarding_tool) {
        const auto index = std::find(std::begin(kToolbarIds), std::end(kToolbarIds), onboarding_tool) - std::begin(kToolbarIds);
        title = tr(kToolbarLabels[index]);
        hint = trf("{purpose}练习中不会打开。", {{"purpose", tr(kToolPurposes[index])}});
    } else if (onboarding_tools_open) {
        title = tr("收起工具栏"); hint = tr("点工具栏以外的任意位置。");
    } else switch (onboarding_task) {
    case OnboardingTask::Next: title = tr("翻到下一页"); hint = tr("点屏幕右侧、向左滑，或按 K3。"); break;
    case OnboardingTask::Previous: title = tr("翻回上一页"); hint = tr("点屏幕左侧、向右滑，或按 K1。"); break;
    case OnboardingTask::OpenTools: title = tr("打开工具栏"); hint = tr("点屏幕中间。目录、排版和同步都在工具栏里。"); break;
    default: title = tr("练习完成"); hint = tr("可以继续练习，或点「下一步」。"); break;
    }
    text(title, 36, 128, 34, 612, 1, true);
    title_lines(hint, 36, 184, 612, 26, false, 2, 3);
    canvas.line(0, 264, 683, 264, 1, 2);
    wrapped("\u3000\u3000" + std::string(tr(kPracticePages[onboarding_page])), 36, 304, 612, 30);
    if (onboarding_task <= OnboardingTask::OpenTools && !onboarding_tools_open) {
        const int edges[] = {0, 205, 479, 684};
        const char* names[] = {tr("‹ 上一页"), tr("工具栏"), tr("下一页 ›")};
        const int active = onboarding_task == OnboardingTask::Next ? 2 : onboarding_task == OnboardingTask::Previous ? 0 : 1;
        for (int i = 0; i < 3; ++i) {
            if (i) canvas.line(edges[i], 760, edges[i], 1063, 12, 2);
            center(names[i], edges[i], 980, edges[i + 1] - edges[i], 24, i == active ? 1 : 5, i == active);
        }
    }
    canvas.line(0, 1064, 683, 1064, 1, 3);
    const bool last = onboarding_page + 1 == kPracticePageCount;
    if (onboarding_tools_open) draw_toolbar();
    else if (onboarding_task == OnboardingTask::Done) button(OnboardingNext, tr("下一步"), 36, 1088, 612, 96, true, 30, true);
    else {
        center(last ? tr("最后一页，再往后翻进入下一步") : trf("第 {page} / {total} 页", {{"page", std::to_string(onboarding_page + 1)}, {"total", std::to_string(kPracticePageCount)}}), 0, 1092, 684, 24, 3);
        center(tr("K1 上一页　K2 全刷　K3 下一页"), 0, 1144, 684, 22, 3);
    }
}

void ReaderApp::Impl::onboarding_input(const AppInput& input) {
    if (input.queued) return;
    if (!input.touch && input.event.input == Input::K2) { force_refresh(); return; }
    int hit_id = 0;
    if (input.touch)
        for (const auto& hit : hits) if (hit.contains(input.x, input.y)) { hit_id = hit.id; break; }
    if (onboarding_step == OnboardingStep::Language && (hit_id == LanguageSimplified || hit_id == LanguageTraditional || hit_id == LanguageEnglish)) {
        const auto language = hit_id == LanguageEnglish ? i18n::Language::English :
            hit_id == LanguageTraditional ? i18n::Language::TraditionalChinese : i18n::Language::SimplifiedChinese;
        const auto snapshot = store.settings_snapshot();
        if (language == settings.language && language == snapshot.config.settings.language && message.empty()) return;
        save_language(snapshot, language, !onboarding_from_help);
        draw_onboarding(); overlay(); return;
    }
    if (hit_id == OnboardingFinish) { finish_onboarding(); return; }
    const auto action = input.touch ? Action::None : map_input(Context::Reading, input.event.input);
    if (onboarding_step == OnboardingStep::Language) {
        if (onboarding_from_help && hit_id == Back) { finish_onboarding(); return; }
        if (hit_id != OnboardingNext && action != Action::NextPage) return;
        message.clear();
        onboarding_step = OnboardingStep::Welcome;
    } else if (onboarding_step == OnboardingStep::Welcome) {
        if (hit_id == Back || action == Action::PreviousPage) onboarding_step = OnboardingStep::Language;
        else if (hit_id == OnboardingStart || action == Action::NextPage) start_practice();
        else return;
    } else if (onboarding_step == OnboardingStep::Tools) {
        if (action == Action::NextPage) finish_onboarding();
        return;
    } else if (hit_id == OnboardingNext) {
        onboarding_step = OnboardingStep::Tools;
    } else if (onboarding_tools_open) {
        if (!input.touch || (!hit_id && input.y < 104) || (hit_id && hit_id == onboarding_tool)) return;
        onboarding_tool = hit_id;
        if (!hit_id) {
            onboarding_tools_open = false;
            if (onboarding_task == OnboardingTask::CloseTools) onboarding_task = OnboardingTask::Done;
        }
    } else {
        auto turn = action;
        if (input.touch && input.y >= 104)
            turn = input.x < 205 ? Action::PreviousPage : input.x >= 479 ? Action::NextPage : Action::OpenTools;
        if (turn == Action::OpenTools) {
            onboarding_tools_open = true;
            if (onboarding_task == OnboardingTask::OpenTools) onboarding_task = OnboardingTask::CloseTools;
        } else if (turn == Action::NextPage) {
            if (onboarding_page + 1 == kPracticePageCount) onboarding_step = OnboardingStep::Tools;
            else ++onboarding_page;
            if (onboarding_task == OnboardingTask::Next) onboarding_task = OnboardingTask::Previous;
        } else if (turn == Action::PreviousPage && onboarding_page > 0) {
            --onboarding_page;
            if (onboarding_task == OnboardingTask::Previous) onboarding_task = OnboardingTask::OpenTools;
        } else return;
    }
    draw_onboarding(); overlay();
}
}
