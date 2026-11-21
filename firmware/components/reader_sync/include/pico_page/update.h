#pragma once

#include "pico_page/build_profile.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pico_page::update {

using build_profile::manifest_url;
using build_profile::board;
inline constexpr std::uint32_t slot_bytes = build_profile::update_slot_bytes;
inline constexpr std::size_t notes_limit = 8 * 1024;
/**
 * Highest schema_version this firmware reads and writes in progress.json and in settings.toml.
 * Progress records use 1 and the marker of cleared progress uses 2. Raise them together with
 * the store formats; a manifest names the versions its firmware writes.
 */
inline constexpr int progress_schema = 2, config_schema = 1;
inline constexpr char state_cache[] = "update-v2.json";
inline constexpr std::size_t state_limit = 24 * 1024;

/**
 * Returns negative, zero or positive for project-version precedence, or nullopt on invalid input.
 * Accepts X.Y.Z, X.Y.Z-beta.N and X.Y.Z-rc.N; numbers have at most nine digits, no leading
 * zeros, and N starts at 1. The first "+" and everything after it are ignored without validation.
 */
std::optional<int> compare_versions(std::string_view a, std::string_view b);

/** True when the version carries build metadata, which marks a build without a release tag. */
bool development_build(std::string_view version);

std::string release_version(std::string_view version);

struct Manifest {
    std::string version, published_at, url, sha256, notes;
    std::uint32_t size = 0;
    /** Schema versions the firmware writes; see progress_schema and config_schema. */
    int progress_schema = update::progress_schema, config_schema = update::config_schema;
    /** True if the manifest reports truncation or the local reader applies notes_limit. */
    bool notes_truncated = false;
};

/**
 * Reads a manifest of format 2 for this board and its partition layout. url must be an https address, size is nonzero,
 * sha256 is 64 hexadecimal digits and version is a release version without build metadata.
 * Whether the image fits a slot is left to evaluate(). notes is cut at notes_limit on a UTF-8
 * boundary. false, with an English diagnostic, for anything else.
 */
bool parse_manifest(const std::string& json, Manifest& manifest, std::string& error);

/** True when either data schema is newer; installation requires confirmation, including development offers. */
bool requires_schema_confirmation(const Manifest& manifest);

enum class Offer {
    Current,
    Newer,         // Installable after the caller's battery and activity checks.
    NewerSchema,   // As Newer, after the user accepts a data format change.
    Development,   // Replaces the running build with the release; schema confirmation is checked separately.
    Skipped,       // This version failed to start earlier and is withheld.
    TooLarge,
    OlderSchema,   // The firmware writes an older data format than this one reads.
};
Offer evaluate(const Manifest& manifest, std::string_view running, std::string_view skipped);

struct NoteSpan {
    std::string text;
    bool bold = false;
};
struct NoteBlock {
    enum class Kind { Heading, Item, Text } kind = Kind::Text;
    std::vector<NoteSpan> spans;
};
/**
 * Reduces Markdown to headings ("#" through "###"), one level of list items ("-" or "*", at
 * any indentation) and plain lines. "**" marks bold; backticks are dropped and "[text](url)"
 * keeps only text. Any other syntax stays as written. Blank lines are skipped.
 */
std::vector<NoteBlock> parse_notes(const std::string& markdown);

struct State {
    std::optional<Manifest> manifest;
    std::int64_t checked_at = 0;  // Unix seconds; 0 when the clock was unset.
    /** Set immediately before restarting into pending_to; cleared when the result was shown. */
    std::string pending_from, pending_to;
    std::string skipped;
};
std::string encode_state(const State& state);
/** false leaves state empty; the caller treats the cache entry as absent. */
bool decode_state(const std::string& json, State& state);

}
