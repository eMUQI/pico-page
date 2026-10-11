#include "pico_page/sync.h"
#include "lvdocview.h"
#include "lvdocviewprops.h"
#include "lvfntman.h"
#include "hyphman.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <algorithm>
#include <vector>

namespace {
void require(bool okay, const std::string& detail) {
    if (!okay) { std::cerr << detail << '\n'; std::exit(1); }
}
// An image anchor names the element; every other anchor names a text node.
bool image_anchor(const pico_page::Anchor& anchor) { return anchor.node.find("text()") == std::string::npos; }
// Resolves a relative reference against the directory of base, without percent-decoding.
std::string resolved(const std::string& base, const std::string& reference) {
    std::vector<std::string> parts;
    const auto path = base.substr(0, base.find_last_of('/') + 1) + reference;
    for (std::size_t begin = 0; begin <= path.size();) {
        const auto end = std::min(path.find('/', begin), path.size());
        const auto part = path.substr(begin, end - begin);
        if (part == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != ".") parts.push_back(part);
        begin = end + 1;
    }
    std::string joined;
    for (const auto& part : parts) joined += (joined.empty() ? "" : "/") + part;
    return joined;
}
// The engine node must be the img element whose src resolves to the resource of the native image.
void require_image(ldomNode* node, const pico_page::Chapter& chapter, const pico_page::Anchor& source, const std::string& pointer) {
    require(node && node->isElement() && node->getNodeName() == "img", "Reference element is not an image at " + pointer);
    const std::string src = UnicodeToUtf8(node->getAttributeValue("src")).c_str();
    for (const auto& image : chapter.images) {
        if (!(image.source == source) || image.resource.empty()) continue;
        require(image.resource == resolved(chapter.resource, src), "Reference image differs at " + pointer);
    }
}
}

// This host-only verifier links the unmodified external CREngine, never the firmware.
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) { std::cerr << "Usage: koreader_mapping_probe original.epub body-font.ttf [sample-step]\n"; return 2; }
    const std::size_t step = argc == 4 ? std::strtoul(argv[3], nullptr, 10) : 1;
    require(step > 0 && step <= 1000000, "Invalid sample step");
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
            std::size_t checked = 0, reversed = 0, bookmarks = 0, retained = 0, images = 0;
            std::map<std::string, std::size_t> skipped, bookmark_skipped;
            for (std::size_t index = 0; index < native.spine().size(); ++index) {
                for (std::uint32_t part = 0;; ++part) {
                    pico_page::Chapter chapter;
                    require(native.load_chapter(index, chapter, error, pico_page::ChapterLoad::Mapping, part), error);
                    std::vector<std::size_t> positions;
                    for (std::size_t pos = 0; pos < chapter.text.size(); pos += step) positions.push_back(pos);
                    if (!chapter.text.empty() && positions.back() + 1 != chapter.text.size()) positions.push_back(chapter.text.size() - 1);
                    for (const auto pos : positions) {
                        pico_page::Anchor source;
                        std::string pointer;
                        if (!chapter.anchor_at(pos, source)) continue;
                        ++retained;
                        if (!pico_page::sync::to_xpointer(native, chapter, source, version, pointer, error)) {
                            ++skipped[error]; continue;
                        }
                        auto reference = view.getDocument()->createXPointer(Utf8ToUnicode(pointer.c_str()));
                        const bool image = image_anchor(source);
                        require(!reference.isNull(), "Reference cannot resolve " + pointer);
                        if (image) {
                            // Every scalar of the placeholder shares the element; its first one counts the image.
                            require_image(reference.getNode(), chapter, source, pointer);
                            pico_page::Anchor before;
                            if (!pos || !chapter.anchor_at(pos - 1, before) || !(before == source)) ++images;
                        } else {
                            require(reference.getNode()->isText(), "Reference is not text at " + pointer);
                            const auto text = reference.getNode()->getText();
                            require(reference.getOffset() >= 0 && reference.getOffset() < text.length(), "Reference offset outside node");
                            require(static_cast<char32_t>(text[reference.getOffset()]) == chapter.text[pos], "Reference text mismatch at " + pointer);
                        }
                        if (image || step > 1 || pos % 53 == 0 || pos + 1 == chapter.text.size()) {
                            pico_page::Anchor recovered;
                            const auto actual = UnicodeToUtf8(reference.toString());
                            require(pico_page::sync::from_xpointer(native, actual.c_str(), version, recovered, error), error + ": " + actual.c_str());
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
                                require(native.find_chapter_part(resumed_index, resumed_chapter, error, pico_page::ChapterLoad::Reading,
                                    [&](const pico_page::Chapter& candidate) { return candidate.offset_for(recovered, visible); }), error);
                                require(resumed_chapter.offset_for(recovered, visible), "Engine bookmark source missing");
                                if (image_anchor(recovered)) require_image(resumed.getNode(), resumed_chapter, recovered, resume_pointer.c_str());
                                else {
                                    require(resumed.getNode()->isText(), "Engine bookmark expected text");
                                    const auto resumed_text = resumed.getNode()->getText();
                                    require(resumed.getOffset() >= 0 && resumed.getOffset() < resumed_text.length(), "Engine bookmark text offset");
                                    require(resumed_chapter.text[visible] == static_cast<char32_t>(resumed_text[resumed.getOffset()]), "Engine bookmark reverse text mismatch");
                                }
                                ++bookmarks;
                            } else ++bookmark_skipped[error + ": " + resume_pointer.c_str()];
                        }
                        ++checked;
                    }
                    std::cout << "CHAPTER " << index << " PART " << part << " checked=" << checked << " retained=" << retained << std::endl;
                    if (chapter.last_part) break;
                }
            }
            require(checked > 0, "No supported positions compared");
            std::cout << "DOM " << version << ", " << size << " px: " << checked
                      << '/' << retained << " source/CREngine scalars (step=" << step << "), " << reversed
                      << " reverse positions, " << bookmarks << " engine bookmarks passed, " << images << " images" << std::endl;
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
