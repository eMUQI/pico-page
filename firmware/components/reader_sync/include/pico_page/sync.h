#pragma once

#include "pico_page/document.h"
#include "pico_page/record.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pico_page::sync {

struct Config {
    std::string endpoint;
    std::string username;
    std::string userkey;
    std::string device_id;
    int dom_version = 20260812;
};

/** Read only PicoPage/sync.toml. Diagnostics never contain credentials or source lines. */
bool load_config(const std::string& card_root, Config& config, std::string& error);
bool valid_config(const Config& config);
std::string account_scope(const Config& config);

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string scope;
    std::optional<ReadingRecord> uploaded_record;
};

/**
 * Builds a progress query without network I/O.
 * @param config Account configuration; must satisfy valid_config().
 * @param partial_md5 KOReader document identity as 32 lowercase hexadecimal digits.
 * @param[out] request Owned request data; cleared on failure.
 * @param[out] error Failure description; cleared on success.
 * @return True when the request is ready; false for invalid configuration or identity.
 */
bool build_get_request(const Config& config, const std::string& partial_md5,
                       HttpRequest& request, std::string& error);
/**
 * Builds an upload without network I/O. The request owns a saved-record snapshot;
 * a confirmed server write applies only to that snapshot.
 * @param config Account configuration; must satisfy valid_config().
 * @param partial_md5 KOReader document identity as 32 lowercase hexadecimal digits.
 * @param xpointer Exact mapped position of saved_record, starting with '/', at most 4096 bytes.
 * @param progress_fraction Finite display estimate in [0, 1]; never used as a locator.
 * @param saved_record Successfully committed record with a book ID, revision and anchor.
 * @param[out] request Owned request data; cleared on failure.
 * @param[out] error Failure description; cleared on success.
 * @return True when the request is ready; false for invalid input or allocation/encoding failure.
 */
bool build_put_request(const Config& config, const std::string& partial_md5,
                       const std::string& xpointer, double progress_fraction, const ReadingRecord& saved_record,
                       HttpRequest& request, std::string& error);

/**
 * Builds an account-verification request without network I/O or progress-record access.
 * The caller sends it and interprets HTTP 200 as acceptance and 401 as rejection.
 * @param config Account configuration; must satisfy valid_config().
 * @param[out] request Owned request data; cleared on failure.
 * @param[out] error Failure description; cleared on success.
 * @return True when the request is ready, not when the account has been verified.
 */
bool build_auth_request(const Config& config, HttpRequest& request, std::string& error);

enum class RemoteStatus { Found, NoRecord, Unauthorized, Error };
struct RemoteResult {
    RemoteStatus status = RemoteStatus::Error;
    std::string progress;
    double progress_fraction = 0;  ///< Server's "percentage" field in [0, 1], valid when status is Found.
    std::string device;
    std::string device_id;
    std::string error;
};
RemoteResult parse_remote(int http_status, const std::string& body);
/** A 202 response is accepted for processing, but is not a confirmed write. */
bool upload_confirmed(int http_status, std::string& error);

/**
 * Error of to_xpointer and from_xpointer for a chapter part outside the verified plain XHTML
 * profile, for example one that holds a table. No position in that part is mapped.
 */
inline constexpr char kUnsupportedStructure[] = "This chapter contains an unsupported KOReader DOM structure";
/** The XHTML resource contains source text at CREngine's split threshold; both directions refuse exact mapping. */
inline constexpr char kLongSourceNode[] = "This chapter requires KOReader source text splitting";
/**
 * Error of to_xpointer and from_xpointer for an image position in a chapter whose CSS holds a
 * "display: none" declaration. Selectors are not resolved, so the image may have no position in
 * the engine's layout. Text positions of the chapter are not affected.
 */
inline constexpr char kUnresolvedVisibility[] = "Image visibility requires a resolved CSS display value";

/**
 * Exact mapping for the verified plain XHTML profile and configured CREngine
 * DOM version. Unsupported content returns false; percentage is never a locator.
 * A text anchor maps to a text node and offset; the anchor of an img element maps to that
 * element at offset zero. Other media anchors are refused.
 */
bool to_xpointer(const EpubDocument& document, const Chapter& chapter, const Anchor& anchor,
                 int dom_version, std::string& xpointer, std::string& error);
/**
 * Maps an upload anchor by reading its source part from an already opened document.
 * The caller exclusively owns document during this call; no cache or progress is written.
 * progress_fraction is a display estimate in [0, 1], never a locator. On failure or cancellation,
 * xpointer is empty, progress_fraction is zero, and error describes the cause. stop is checked while reading
 * and after mapping; an individual file operation or mapping call may still block.
 */
bool map_upload_position(EpubDocument& document, const Anchor& anchor, int dom_version,
                         std::string& xpointer, double& progress_fraction, std::string& error,
                         const std::function<bool()>& stop = {});
/**
 * A reading-text position inside one spine item. Unlike an Anchor it can be ordered against
 * another position of the same item without loading the chapter.
 */
struct PartPosition {
    std::uint32_t part = 0;
    std::uint32_t offset = 0;        // Unicode scalar index within that part.
    std::uint32_t part_scalars = 0;  // Length of that part in Unicode scalars.
    bool last_part = true;
};

/** As to_xpointer in reverse. place, when given, receives where the anchor lies in its chapter. */
bool from_xpointer(EpubDocument& document, const std::string& xpointer, int dom_version,
                   Anchor& anchor, std::string& error, const std::function<bool()>& stop = {},
                   PartPosition* place = nullptr);

}
