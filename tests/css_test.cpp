#include "css.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <new>
#include <string>
static bool fail_array_allocation = false;
void* operator new[](size_t bytes, const std::nothrow_t&) noexcept {
    if (fail_array_allocation) return nullptr;
    try { return ::operator new[](bytes); } catch (const std::bad_alloc&) { return nullptr; }
}
using namespace pico_page::document::css;
static ComputedStyle style(StyleSheet& sheet, const Element& element, const ComputedStyle* parent = nullptr,
                           std::string_view inlined = {}, Context context = {}) {
    WorkBudget budget;
    ComputedStyle result;
    assert(sheet.compute(element, parent, context, inlined, budget, result) == Result::Complete);
    return result;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }
int main() {
    StyleSheet sheet;
    assert(sheet.append("p {font-size:20px; text-align:center}") == Result::Complete);
    assert(near(style(sheet, {"p", "", ""}).font_px, 20));
    const auto centered = style(sheet, {"p", "", ""});
    assert(centered.align == Align::Center && centered.align_declared);
    const auto inherited_alignment = style(sheet, {"span", "", ""}, &centered, "text-align: unset");
    assert(inherited_alignment.align == Align::Center && inherited_alignment.align_declared);
    const auto default_alignment = style(sheet, {"body", "", ""});
    assert(default_alignment.align == Align::Start && !default_alignment.align_declared);
    assert(!style(sheet, {"span", "", ""}, &default_alignment, "text-align: inherit").align_declared);
    const auto initial_alignment = style(sheet, {"span", "", ""}, &centered, "text-align: initial");
    assert(initial_alignment.align == Align::Start && initial_alignment.align_declared);
    assert(style(sheet, {"span", "", ""}, &centered, "text-align: start") == initial_alignment);

    {
        StyleSheet hyphens;
        assert(hyphens.append("p { hyphens: auto } em { hyphens: none }") == Result::Complete);
        assert(style(hyphens, {"body", "", ""}).hyphens == Hyphens::Manual);
        const auto paragraph = style(hyphens, {"p", "", ""});
        assert(paragraph.hyphens == Hyphens::Auto);
        assert(style(hyphens, {"span", "", ""}, &paragraph).hyphens == Hyphens::Auto);
        assert(style(hyphens, {"em", "", ""}, &paragraph).hyphens == Hyphens::None);
        assert(style(hyphens, {"em", "", ""}, &paragraph, "hyphens: initial").hyphens == Hyphens::Manual);
        assert(style(hyphens, {"em", "", ""}, &paragraph, "hyphens: unset").hyphens == Hyphens::Auto);
    }
    {
        StyleSheet bleed;
        assert(bleed.append(".a{duokan-bleed:lefttopright} .b{bleed:left top right} .c{qrbleed:bottom}") == Result::Complete);
        const auto a = style(bleed, {"div", "", "a"});
        assert(a.bleed == 0b1011 && style(bleed, {"div", "", "b"}).bleed == 0b1011);
        assert(style(bleed, {"div", "", "c"}).bleed == 0b0100 && style(bleed, {"img", "", ""}, &a).bleed == 0b1011);
        assert(style(bleed, {"div", "", ""}).bleed == 0);
    }
    {
        StyleSheet cascade;
        assert(cascade.append("p{font-size:18px} .x{font-size:22px} #a{font-size:24px} p{font-size:26px!important}") == Result::Complete);
        assert(near(style(cascade, {"p", "a", "x"}, nullptr, "font-size:30px").font_px, 26));
        assert(near(style(cascade, {"p", "a", "x"}, nullptr, "font-size:30px!important").font_px, 30));
        assert(cascade.append("p{font-size:28px!important}") == Result::Complete);
        assert(near(style(cascade, {"p", "a", "x"}).font_px, 28));
    }
    {
        StyleSheet selectors;
        assert(selectors.append("section > .b .x, #selected{font-size:30px} p, :unsupported{font-size:40px}") == Result::Complete);
        Element root{"section", "", ""}, b1{"div", "", "b", &root}, other{"div", "", "", &b1}, b2{"div", "", "b", &other}, leaf{"p", "", "x", &b2};
        assert(near(style(selectors, leaf).font_px, 30));
        assert(near(style(selectors, {"p", "", ""}).font_px, 16));
        assert(near(style(selectors, {"div", "selected", ""}).font_px, 30));
        assert(selectors.report().rejected_rules == 1);
    }
    {
        StyleSheet units;
        assert(units.append("p{font-size:150%;line-height:120%;text-indent:10%;margin:1em 2rem 3pt 4%;padding:1px 2px 3px}") == Result::Complete);
        ComputedStyle parent; parent.font_px = 20;
        Context context; context.root_font_px = 18; context.containing_width_px = 500;
        const auto p = style(units, {"p", "", ""}, &parent, {}, context);
        assert(near(p.font_px, 30) && near(p.line_height_px, 36) && near(p.indent_px, 50));
        assert(near(p.margin_px[0], 30) && near(p.margin_px[1], 36) && near(p.margin_px[2], 4) && near(p.margin_px[3], 20));
        assert(near(p.padding_px[3], 2));
        const auto child = style(units, {"span", "", ""}, &p, "font-size:50%", context);
        assert(near(child.font_px, 15) && near(child.line_height_px, 36));
        const auto multiple = style(units, {"div", "", ""}, nullptr, "line-height:1.5;font-size:20px");
        assert(near(style(units, {"span", "", ""}, &multiple, "font-size:50%").line_height_px, 15));
        const auto reset = style(units, {"p", "", ""}, &p, "font-size:initial;line-height:unset;margin:inherit;text-align:initial");
        assert(near(reset.font_px, 16) && near(reset.line_height_px, 36) && near(reset.margin_px[0], 30));
    }
    {
        StyleSheet recovery;
        assert(recovery.append("@import 'a.css';@media screen {p{font-size:99px}}@font-face{font-size:99px}p{unknown:fn(';}',[x]);bad{a:b};font-size:23px;margin:1px bogus; padding:2px;font:italic 12px serif;text-align:center}") == Result::Complete);
        const auto p = style(recovery, {"p", "", ""});
        assert(near(p.font_px, 23) && near(p.margin_px[0], 0) && near(p.padding_px[0], 2) && p.align == Align::Center);
        assert(recovery.report().skipped_at_rules == 3);
        assert(recovery.report().rejected_declarations == 4);
        assert(recovery.append("@import url('b.css');@import url('a.css');p{font-size:25px}") == Result::Complete);
        assert(near(style(recovery, {"p", "", ""}).font_px, 25));
    }
    {
        for (const std::string skipped : {
                 "@media screen{font-size:99px}",
                 "@unknown url(data:text/x,;}{);",
                 "@supports (x:fn(';}',[x])){@media screen{font-size:99px}}"}) {
            StyleSheet recovery;
            const auto declarations = "font-size:20px;" + skipped + "font-size:30px";
            assert(recovery.append("p{" + declarations + "}div{font-size:31px}") == Result::Complete);
            assert(near(style(recovery, {"p", "", ""}).font_px, 30));
            assert(near(style(recovery, {"div", "", ""}).font_px, 31));
            assert(near(style(recovery, {"span", "", ""}, nullptr, declarations).font_px, 30));
            unsigned fonts = 0;
            assert(recovery.collect_fonts("@font-face{font-family:Example;" + skipped + "src:url(book.ttf)}", {},
                [&](std::string_view family, std::string_view url) {
                    assert(family == "example" && url == "book.ttf");
                    ++fonts;
                    return true;
                }) == Result::Complete);
            assert(fonts == 1);
        }
        StyleSheet boundaries;
        assert(boundaries.append("p{font-size:20px;@unknown x}div{font-size:31px}") == Result::Complete);
        assert(near(style(boundaries, {"p", "", ""}).font_px, 20));
        assert(near(style(boundaries, {"div", "", ""}).font_px, 31));
    }
    {
        StyleSheet escaped;
        assert(escaped.append("/*{}*/p/**/.\\4e2d {f\\6f nt-size:2em} #\\31 abc{font-weight:bold} p.\\+{font-style:italic}") == Result::Complete);
        assert(near(style(escaped, {"p", "", "中"}).font_px, 32));
        assert(style(escaped, {"div", "1abc", ""}).bold);
        assert(style(escaped, {"p", "", "+"}).italic);
        assert(escaped.append("p{font-size:21px; text-align:bad(\"unterminated\n);font-weight:bold}") == Result::Complete);
        assert(near(style(escaped, {"p", "", ""}).font_px, 21));
    }
    {
        Limits limits; limits.rules = 1;
        StyleSheet bounded(limits);
        assert(bounded.append("p{font-size:20px}p{font-size:30px}") == Result::Limit);
        assert(near(style(bounded, {"p", "", ""}).font_px, 20));
        assert(bounded.report().rules == 1);
        WorkBudget budget; budget.maximum_work = 2;
        ComputedStyle result;
        assert(bounded.compute({"p", "", ""}, nullptr, {}, {}, budget, result) == Result::Limit);
        assert(budget.work <= budget.maximum_work && near(result.font_px, 16));
        budget = {}; budget.maximum_styles = 0;
        assert(bounded.compute({"p", "", ""}, nullptr, {}, {}, budget, result) == Result::Limit);
        Limits invalid; invalid.identifier_bytes = 0xffffffff;
        StyleSheet rejected(invalid);
        assert(rejected.append("") == Result::InvalidInput);
        Limits bytes; bytes.source_bytes = 1;
        StyleSheet source_limit(bytes);
        assert(source_limit.append("pp") == Result::Limit);
        Element cycle{"p", "", ""}; cycle.parent = &cycle;
        budget = {};
        assert(bounded.compute(cycle, nullptr, {}, {}, budget, result) == Result::InvalidInput);
        for (unsigned field = 0; field < 5; ++field) {
            Limits small;
            if (field == 0) small.selectors = 0;
            if (field == 1) small.steps = 0;
            if (field == 2) small.atoms = 0;
            if (field == 3) small.declarations = 0;
            if (field == 4) small.identifier_bytes = 0;
            StyleSheet exhausted(small);
            assert(exhausted.append("p{font-size:20px}") == Result::Limit);
            assert(exhausted.report().rules == 0);
        }
        StyleSheet nested;
        std::string deep = "@unknown{" + std::string(33, '{') + "p{font-size:99px}";
        assert(nested.append(deep) == Result::Limit);
        assert(nested.append("p{" + deep + "}") == Result::Limit);
        budget = {};
        assert(nested.compute({"p", "", ""}, nullptr, {}, deep, budget, result) == Result::Limit);
        assert(nested.collect_fonts("@font-face{" + deep + "}", {},
            [](std::string_view, std::string_view) { assert(false); return true; }) == Result::Limit);
        budget = {}; budget.maximum_work = 4;
        assert(nested.compute({"p", "", ""}, nullptr, {}, "@unknown{}font-size:30px", budget, result) == Result::Limit);
        assert(budget.work <= budget.maximum_work && near(result.font_px, 16));
    }
    {
        StyleSheet cancel;
        unsigned calls = 0;
        Checkpoint stop{[](void* value) { return ++*static_cast<unsigned*>(value) >= 2; }, &calls};
        assert(cancel.append("p{font-size:22px}" + std::string(1000, ' '), stop) == Result::Cancelled);
        assert(near(style(cancel, {"p", "", ""}).font_px, 22));
        calls = 1;
        WorkBudget budget;
        ComputedStyle result;
        assert(cancel.compute({"p", "", ""}, nullptr, {}, {}, budget, result, stop) == Result::Cancelled);
        assert(near(result.font_px, 16));
        const std::string long_rule = "@unknown{" + std::string(1000, ' ') + "}";
        calls = 0;
        assert(cancel.append("p{font-size:99px;" + long_rule + "font-size:30px}", stop) == Result::Cancelled);
        assert(near(style(cancel, {"p", "", ""}).font_px, 22));
        calls = 0; budget = {};
        Checkpoint stop_inline{[](void* value) { return ++*static_cast<unsigned*>(value) >= 4; }, &calls};
        assert(cancel.compute({"p", "", ""}, nullptr, {}, long_rule + "font-size:30px", budget, result, stop_inline) == Result::Cancelled);
        assert(near(result.font_px, 16));
        calls = 0;
        assert(cancel.collect_fonts("@font-face{font-family:Example;" + long_rule + "src:url(book.ttf)}", stop,
            [](std::string_view, std::string_view) { assert(false); return true; }) == Result::Cancelled);
        StyleSheet empty;
        assert(empty.append("") == Result::Complete);
        assert(empty.append("p{font-size:20px") == Result::Complete);
        assert(near(style(empty, {"p", "", ""}).font_px, 20));
        assert(empty.append("p[truncated") == Result::Complete);
    }

    {
        StyleSheet newlines;
        assert(newlines.append(".\\61\r\nb{font-size:29px}p{unknown:\"x\\\r\n}{\";font-size:27px}") == Result::Complete);
        assert(near(style(newlines, {"div", "", "ab"}).font_px, 29));
        assert(near(style(newlines, {"p", "", ""}).font_px, 27));
    }
    {
        StyleSheet urls;
        ComputedStyle parent;
        parent.font_px = 32; parent.bold = true; parent.hidden = true;
        WorkBudget budget;
        ComputedStyle result;
        assert(urls.compute({"p", "", ""}, &parent, {}, std::string(4097, ' '), budget, result) == Result::InvalidInput);
        assert(near(result.font_px, 32) && result.bold && result.hidden);
        Context context; context.minimum_font_px = 24;
        assert(near(style(urls, {"p", "", ""}, nullptr, {}, context).font_px, 24));
        parent.font_px = 20;
        const auto inherited = style(urls, {"p", "", ""}, &parent, {}, context);
        assert(near(inherited.font_px, 24) && near(inherited.line_height_px, 28.8f));
        assert(urls.append("p{unknown:url(art/*);font-size:23px} div{unknown:url(a\\)b);font-size:24px} span{unknown:url(\"}/*\");font-size:25px} em{unknown:url(a b);font-size:26px} strong{unknown:u\\72l(art/*);font-size:27px} b{unknown:url(data:text/x,;}{);font-size:28px}") == Result::Complete);
        assert(near(style(urls, {"p", "", ""}).font_px, 23));
        assert(near(style(urls, {"div", "", ""}).font_px, 24));
        assert(near(style(urls, {"span", "", ""}).font_px, 25));
        assert(near(style(urls, {"em", "", ""}).font_px, 26));
        assert(near(style(urls, {"strong", "", ""}).font_px, 27));
        assert(near(style(urls, {"b", "", ""}).font_px, 28));
        unsigned calls = 0;
        Checkpoint stop{[](void* value) { return ++*static_cast<unsigned*>(value) >= 2; }, &calls};
        assert(urls.append("p{unknown:url(" + std::string(1000, ' ') + "\"x\");font-size:99px}", stop) == Result::Cancelled);
        assert(near(style(urls, {"p", "", ""}).font_px, 23));
    }
    {
        fail_array_allocation = true;
        StyleSheet unavailable;
        fail_array_allocation = false;
        assert(unavailable.append("p{font-size:20px}") == Result::NoMemory);
        WorkBudget budget;
        ComputedStyle result;
        assert(unavailable.compute({"p", "", ""}, nullptr, {}, {}, budget, result) == Result::NoMemory);
        assert(near(result.font_px, 16));
    }
    {
        StyleSheet syntax;
        assert(syntax.append("p{margin:10%;font-size:12pt;padding:1em -2px;text-align:right!important bogus;line-height:normal} p + div{font-size:30px} @unknown foo([a]){p{font-size:99px}}") == Result::Complete);
        auto p = style(syntax, {"p", "", ""}, nullptr, "margin:calc(1px + 1em);font-size:bogus;text-indent:-2em");
        assert(near(p.font_px, 16) && near(p.margin_px[0], 60) && near(p.padding_px[0], 0));
        assert(p.align == Align::Start && near(p.indent_px, -32));
        assert(syntax.report().rejected_rules == 1);
        std::string oversized = "." + std::string(257, 'a') + "{font-size:99px}p{font-size:22px}";
        assert(syntax.append(oversized) == Result::Complete);
        assert(near(style(syntax, {"p", "", ""}).font_px, 22));
        assert(syntax.append("p{font-size:1e99px;font-weight:bold}") == Result::Complete);
        assert(style(syntax, {"p", "", ""}).bold);
        unsigned calls = 0;
        Checkpoint stop{[](void* value) { return ++*static_cast<unsigned*>(value) > 2; }, &calls};
        WorkBudget budget; budget.maximum_work = 20000;
        ComputedStyle result;
        Element many{"p", "", ""};
        std::string classes(4000, 'a'); many.classes = classes;
        StyleSheet match;
        assert(match.append(".a{font-size:99px}") == Result::Complete);
        assert(match.compute(many, nullptr, {}, {}, budget, result, stop) == Result::Cancelled);
        assert(near(result.font_px, 16));
    }
    std::cout << "CSS behavior passed\n";
}
