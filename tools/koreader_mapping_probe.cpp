#include "pico_page/sync.h"
#include "lvdocview.h"
#include "lvdocviewprops.h"
#include "lvfntman.h"
#include "hyphman.h"

#include <cstdlib>
#include <iostream>
#include <map>

namespace {
void require(bool okay, const std::string& detail) {
    if (!okay) { std::cerr << detail << '\n'; std::exit(1); }
}
}

// This host-only verifier links the unmodified external CREngine, never the firmware.
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Usage: koreader_mapping_probe original.epub body-font.ttf\n"; return 2; }
    pico_page::EpubDocument native;
    std::string error;
    require(native.open(argv[1], error), error);
    InitFontManager(lString8(""));
    require(fontMan->RegisterFont(lString8(argv[2])), "Cannot register reference font");
    HyphMan::initDictionaries(lString32(""));
    for (const int version : {20240114, 20260812}) {
        for (const int size : {28, 38, 56}) {
            LVDocView view(-1, true);
            view.Resize(684, 1216);
            auto properties = LVCreatePropsContainer();
            properties->setInt(PROP_REQUESTED_DOM_VERSION, version);
            properties->setString(PROP_FONT_FACE, "LXGW WenKai");
            properties->setInt(PROP_FONT_SIZE, size);
            view.propsApply(properties);
            require(view.LoadDocument(argv[1]), "Reference EPUB load failed");
            view.Render();
            require(view.getDocument()->getDOMVersionRequested() == static_cast<unsigned>(version), "DOM version mismatch");
            std::size_t checked = 0, reversed = 0, bookmarks = 0, retained = 0;
            std::map<std::string, std::size_t> skipped, bookmark_skipped;
            for (std::size_t index = 0; index < native.spine().size(); ++index) {
                pico_page::Chapter chapter;
                require(native.load_chapter(index, chapter, error), error);
                for (std::size_t pos = 0; pos < chapter.text.size(); ++pos) {
                    pico_page::Anchor source;
                    std::string pointer;
                    if (!chapter.anchor_at(pos, source)) continue;
                    ++retained;
                    if (!pico_page::sync::to_xpointer(native, chapter, source, version, pointer, error)) {
                        ++skipped[error]; continue;
                    }
                    auto reference = view.getDocument()->createXPointer(Utf8ToUnicode(pointer.c_str()));
                    require(!reference.isNull() && reference.getNode()->isText(), "Reference cannot resolve " + pointer);
                    const auto text = reference.getNode()->getText();
                    require(reference.getOffset() >= 0 && reference.getOffset() < text.length(), "Reference offset outside node");
                    require(static_cast<char32_t>(text[reference.getOffset()]) == chapter.text[pos], "Reference text mismatch at " + pointer);
                    if (pos % 53 == 0 || pos + 1 == chapter.text.size()) {
                        pico_page::Anchor recovered;
                        const auto actual = UnicodeToUtf8(reference.toString());
                        require(pico_page::sync::from_xpointer(native, actual.c_str(), version, recovered, error), error);
                        require(source == recovered, "Source round trip mismatch");
                        ++reversed;
                        view.goToBookmark(reference);
                        view.SetPos(reference.toPoint().y);
                        auto resumed = view.getBookmark();
                        require(!resumed.isNull(), "Reference cannot resume bookmark");
                        const auto resume_pointer = UnicodeToUtf8(resumed.toString());
                        if (pico_page::sync::from_xpointer(native, resume_pointer.c_str(), version, recovered, error)) {
                            std::uint32_t visible = 0;
                            std::size_t resumed_index = 0;
                            pico_page::Chapter resumed_chapter;
                            require(native.chapter_index_for(recovered.resource, resumed_index), "Engine bookmark chapter missing");
                            require(native.load_chapter(resumed_index, resumed_chapter, error), error);
                            require(resumed_chapter.offset_for(recovered, visible), "Engine bookmark source missing");
                            require(resumed.getNode()->isText(), "Engine bookmark expected text");
                            const auto resumed_text = resumed.getNode()->getText();
                            require(resumed.getOffset() >= 0 && resumed.getOffset() < resumed_text.length(), "Engine bookmark text offset");
                            require(resumed_chapter.text[visible] == static_cast<char32_t>(resumed_text[resumed.getOffset()]), "Engine bookmark reverse text mismatch");
                            ++bookmarks;
                        } else ++bookmark_skipped[error + ": " + resume_pointer.c_str()];
                    }
                    ++checked;
                }
                std::cout << "CHAPTER " << index << " checked=" << checked << " retained=" << retained << std::endl;
            }
            require(checked > 0, "No supported positions compared");
            std::cout << "DOM " << version << ", " << size << " px: " << checked
                      << '/' << retained << " source/CREngine scalars, " << reversed
                      << " reverse positions, " << bookmarks << " engine bookmarks passed" << std::endl;
            for (const auto& [reason, count] : skipped)
                std::cout << "UNSUPPORTED " << count << ' ' << reason << std::endl;
            if (!bookmark_skipped.empty()) {
                std::size_t count = 0;
                for (const auto& [reason, occurrences] : bookmark_skipped) count += occurrences;
                std::cout << "UNSUPPORTED_BOOKMARKS " << count << " example=" << bookmark_skipped.begin()->first << std::endl;
            }
        }
    }
    ShutdownFontManager();
}
