#pragma once

#include "pico_page/record.h"
#include <optional>

namespace pico_page {

enum class Context { Reading, Tools, Typography, Contents, Library, LibraryRoot,
                     Conflict, Confirmation, Transfer, Sleeping };
enum class Input { K1, K2, K3, TouchCenter, Previous, Next };
enum class Action { None, PreviousPage, NextPage, ForceRefresh, OpenTools, Close,
                    CancelPreview, KeepLocal, Cancel, EndTransfer, Wake };
Action MapInput(Context context, Input input);

enum class Phase { Begin, End, Cancel };
struct InputEvent {
    std::uint64_t sequence;
    Phase phase;
    Input input;
};

/** Serial input sequences have increasing nonzero IDs across context changes. */
class InputRouter {
public:
    /** Consume a physical gesture; true only for its first accepted Begin. */
    bool accept(InputEvent event);
    /** Only a board-confirmed wake source may set wake_authorized. */
    Action route(Context context, InputEvent event, bool wake_authorized = false);
private:
    std::uint64_t last_sequence_ = 0;
    std::optional<std::uint64_t> active_;
};

struct OperationContext {
    std::string book_id;
    std::uint64_t session = 0;
    std::uint64_t layout = 0;
    std::uint64_t request = 0;
};
bool operator==(const OperationContext& a, const OperationContext& b);

enum class FramePurpose { Body, Overlay, ManualRefresh };
enum class RefreshIntent { Content, FullClean };
/** Optional portrait-pixel damage bounds; a full clean always ignores them. */
struct RefreshRegion {
    int x = 0, y = 0, width = 0, height = 0;
    bool fits(int screen_width, int screen_height) const {
        return x >= 0 && y >= 0 && width > 0 && height > 0 &&
               x < screen_width && y < screen_height &&
               width <= screen_width - x && height <= screen_height - y;
    }
};
struct Frame {
    OperationContext token;
    Anchor anchor;
    FramePurpose purpose = FramePurpose::Body;
    RefreshIntent refresh = RefreshIntent::Content;
    std::optional<RefreshRegion> region = std::nullopt;
    /** Keep equal old/new pixels electrically idle; ignored by full-clean recovery. */
    bool changed_pixels_only = false;
};
struct FrameResult {
    Frame frame;
    bool success;
};

struct SaveRequest {
    OperationContext token;
    ReadingRecord record;
};

/**
 * Board adapters start a transaction; completion must arrive separately on the
 * application task. The frame remains valid until Display::finish is called.
 * start must not call finish synchronously or retain the reference afterward.
 */
class DisplayBackend {
public:
    virtual ~DisplayBackend() = default;
    virtual bool start(const Frame& frame) = 0;
};

/** Owns the single in-flight frame and physical baseline validity. */
class Display {
public:
    explicit Display(DisplayBackend& backend) : backend_(backend) {}
    bool present(Frame frame);
    std::optional<FrameResult> finish(const OperationContext& token, bool success);
    bool busy() const { return in_flight_.has_value(); }
    bool baseline_valid() const { return baseline_valid_; }
private:
    DisplayBackend& backend_;
    std::optional<Frame> in_flight_;
    bool baseline_valid_ = false;
};

/** All methods run on the application owner task; callbacks must be marshalled to it. */
class Session {
public:
    explicit Session(Display& display) : display_(display) {}
    void open(std::string book_id, std::string content_revision, std::string writer_id,
              std::optional<ReadingRecord> restored = std::nullopt);
    void change_layout();
    OperationContext request_page(Anchor anchor, RefreshIntent refresh = RefreshIntent::Content);
    bool layout_ready(const OperationContext& token);
    bool force_refresh();
    /** Accept only results emitted by Display::finish; returns true for adopted body frames. */
    bool presented(const FrameResult& result);
    const std::optional<ReadingRecord>& visible_record() const { return visible_; }
    const std::optional<ReadingRecord>& saved_record() const { return saved_; }
    /** At most one save is in flight; failure permits retry without another page turn. */
    std::optional<SaveRequest> request_save();
    bool saved(const SaveRequest& request, bool success);
    bool needs_save() const;
private:
    OperationContext next_token();
    bool current(const OperationContext& token) const;
    Display& display_;
    std::string book_id_, content_revision_, writer_id_;
    std::uint64_t session_ = 0, layout_ = 0, request_ = 0, revision_ = 0;
    std::optional<Frame> pending_layout_;
    std::optional<Frame> pending_body_;
    std::optional<ReadingRecord> visible_, saved_;
    std::optional<SaveRequest> pending_save_;
};

}  // namespace pico_page
