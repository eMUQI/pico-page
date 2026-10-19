#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Firmware update decisions that need no hardware: version order, the update manifest, the
 * release notes it carries and the small state kept between a check, an installation and the
 * first start of the installed firmware. Downloading and slot handling belong to the platform.
 */
namespace pico_page::update {

inline constexpr char manifest_url[] = "https://picopage.wulu.zone/firmware/manifest.json";
inline constexpr char board[] = "read_pico";
/** Size of one application slot in the measured device partition table. */
inline constexpr std::uint32_t slot_bytes = 4u * 1024 * 1024;
/** Release notes beyond this many bytes are not kept or shown. */
inline constexpr std::size_t notes_limit = 8 * 1024;
/**
 * Highest schema_version this firmware reads and writes in progress.json and in settings.toml.
 * Progress records use 1 and the marker of cleared progress uses 2. Raise them together with
 * the store formats; a manifest names the versions its firmware writes.
 */
inline constexpr int progress_schema = 2, config_schema = 1;
/** Rebuildable cache entry that holds the encoded State. */
inline constexpr char state_cache[] = "update.json";
inline constexpr std::size_t state_limit = 24 * 1024;

/**
 * SemVer precedence of two project versions: negative, zero or positive as a sorts before, with
 * or after b. Accepts X.Y.Z, X.Y.Z-beta.N and X.Y.Z-rc.N without leading zeros, N from 1, each
 * with optional "+" build metadata that takes no part in the order. nullopt when either version
 * has another form.
 */
std::optional<int> compare_versions(std::string_view a, std::string_view b);

/** True when the version carries build metadata, which marks a build without a release tag. */
bool development_build(std::string_view version);

/** The version without its build metadata. */
std::string release_version(std::string_view version);

struct Manifest {
    std::string version, published_at, url, sha256, notes;
    std::uint32_t size = 0;
    /** Schema versions the firmware writes; see progress_schema and config_schema. */
    int progress_schema = update::progress_schema, config_schema = update::config_schema;
    /** notes was cut at notes_limit. */
    bool notes_truncated = false;
};

/**
 * Reads a manifest of format 1 for this board. url must be an https address, size is nonzero,
 * sha256 is 64 hexadecimal digits and version is a release version without build metadata.
 * Whether the image fits a slot is left to evaluate(). notes is cut at notes_limit on a UTF-8
 * boundary. false, with an English diagnostic, for anything else.
 */
bool parse_manifest(const std::string& json, Manifest& manifest, std::string& error);

/** True when either data schema is newer; installation requires confirmation, including development offers. */
bool requires_schema_confirmation(const Manifest& manifest);

/** How a manifest relates to the running firmware. */
enum class Offer {
    Current,       // The manifest is not newer; nothing to install.
    Newer,         // Installable after the caller's battery and activity checks.
    NewerSchema,   // As Newer, after the user accepts a data format change.
    Development,   // Replaces the running build with the release; schema confirmation is checked separately.
    Skipped,       // This version failed to start earlier and is withheld.
    TooLarge,      // The image does not fit an application slot.
    OlderSchema,   // The firmware writes an older data format than this one reads.
};
Offer evaluate(const Manifest& manifest, std::string_view running, std::string_view skipped);

/** One run of release-note text; bold marks a "**" span. */
struct NoteSpan {
    std::string text;
    bool bold = false;
};
/** A heading, a list item or a plain line of the release notes. */
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

/** What survives between a check, a restart into new firmware and the report of its result. */
struct State {
    std::optional<Manifest> manifest;
    std::int64_t checked_at = 0;  // Unix seconds; 0 when the clock was unset.
    /** Set immediately before restarting into pending_to; cleared when the result was shown. */
    std::string pending_from, pending_to;
    /** A version that did not start; withheld until the manifest names a later one. */
    std::string skipped;
};
std::string encode_state(const State& state);
/** false leaves state empty; the caller treats the cache entry as absent. */
bool decode_state(const std::string& json, State& state);

}  // namespace pico_page::update
