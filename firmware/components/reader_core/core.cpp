#include "pico_page/core.h"
#include <utility>

namespace pico_page {

Action MapInput(Context context, Input input) {
    if (context == Context::Sleeping) return Action::None;
    if (input == Input::K2) return Action::ForceRefresh;
    if (context == Context::Reading && input == Input::TouchCenter) return Action::OpenTools;
    if (context == Context::Reading || context == Context::Contents || context == Context::Library ||
        context == Context::LibraryRoot || context == Context::Transfer) {
        if (input == Input::K1 || input == Input::Previous) return Action::PreviousPage;
        if (input == Input::K3 || input == Input::Next) return Action::NextPage;
    }
    return Action::None;
}

bool InputRouter::accept(InputEvent event) {
    if (event.phase != Phase::Begin) {
        if (active_ == event.sequence) active_.reset();
        return false;
    }
    if (event.sequence == 0 || event.sequence <= last_sequence_) return false;
    last_sequence_ = event.sequence;
    // Concurrent contacts are consumed without replacing the original gesture owner.
    if (active_) return false;
    active_ = event.sequence;
    return true;
}

Action InputRouter::route(Context context, InputEvent event, bool wake_authorized) {
    if (!accept(event)) return Action::None;
    if (context == Context::Sleeping) return wake_authorized ? Action::Wake : Action::None;
    return MapInput(context, event.input);
}

bool operator==(const OperationContext& a, const OperationContext& b) {
    return a.book_id == b.book_id && a.session == b.session && a.layout == b.layout && a.request == b.request;
}

bool Display::present(Frame frame) {
    if (in_flight_) return false;
    if (!baseline_valid_ || frame.purpose == FramePurpose::ManualRefresh) {
        frame.refresh = RefreshIntent::FullClean;
    }
    frame.changed_pixels_only = frame.purpose == FramePurpose::Overlay &&
                                frame.refresh == RefreshIntent::Content;
    if (frame.refresh == RefreshIntent::FullClean) {
        frame.region.reset();
        frame.changed_pixels_only = false;
    }
    in_flight_ = std::move(frame);
    if (!backend_.start(*in_flight_)) {
        in_flight_.reset();
        baseline_valid_ = false;
        return false;
    }
    return true;
}

std::optional<FrameResult> Display::finish(const OperationContext& token, bool success) {
    if (!in_flight_ || !(in_flight_->token == token)) return std::nullopt;
    FrameResult result{std::move(*in_flight_), success};
    in_flight_.reset();
    baseline_valid_ = success;
    return result;
}

void Session::open(std::string book_id, std::string content_revision, std::string writer_id,
                   std::optional<ReadingRecord> restored) {
    book_id_ = std::move(book_id);
    content_revision_ = std::move(content_revision);
    writer_id_ = std::move(writer_id);
    ++session_;
    layout_ = 1;
    revision_ = 0;
    pending_layout_.reset();
    pending_body_.reset();
    pending_save_.reset();
    visible_.reset();
    saved_.reset();
    if (restored && restored->book_id == book_id_ && restored->content_revision == content_revision_) {
        saved_ = std::move(restored);
        if (saved_->writer_id == writer_id_) revision_ = saved_->local_revision;
    }
}

void Session::change_layout() {
    ++layout_;
    pending_layout_.reset();
    pending_body_.reset();
}

OperationContext Session::next_token() { return {book_id_, session_, layout_, ++request_}; }

bool Session::current(const OperationContext& token) const {
    return token.book_id == book_id_ && token.session == session_ && token.layout == layout_;
}

OperationContext Session::request_page(Anchor anchor, RefreshIntent refresh) {
    auto token = next_token();
    pending_body_.reset();
    pending_layout_ = Frame{token, std::move(anchor), FramePurpose::Body, refresh};
    return token;
}

bool Session::layout_ready(const OperationContext& token) {
    if (!pending_layout_ || !(pending_layout_->token == token) || !current(token)) return false;
    if (!display_.present(*pending_layout_)) return false;
    pending_body_ = std::move(pending_layout_);
    pending_layout_.reset();
    return true;
}

bool Session::force_refresh() {
    if (!visible_ || display_.busy()) return false;
    return display_.present({next_token(), visible_->anchor, FramePurpose::ManualRefresh, RefreshIntent::FullClean});
}

bool Session::presented(const FrameResult& result) {
    if (result.frame.purpose != FramePurpose::Body || !pending_body_ ||
        !(result.frame.token == pending_body_->token) || !current(result.frame.token) ||
        result.frame.anchor != pending_body_->anchor) return false;
    pending_body_.reset();
    if (!result.success) return false;
    // Repainting the same source position does not create another reading revision.
    if (!visible_ && saved_ && saved_->anchor == result.frame.anchor) {
        visible_ = saved_;
    } else if (!visible_ || visible_->anchor != result.frame.anchor) {
        visible_ = ReadingRecord{book_id_, content_revision_, writer_id_, ++revision_, result.frame.anchor};
    }
    return true;
}

bool Session::needs_save() const { return visible_ && (!saved_ || !(*visible_ == *saved_)); }

std::optional<SaveRequest> Session::request_save() {
    if (!needs_save() || pending_save_) return std::nullopt;
    pending_save_ = SaveRequest{next_token(), *visible_};
    return pending_save_;
}

bool Session::saved(const SaveRequest& request, bool success) {
    if (!pending_save_ || !(pending_save_->token == request.token) ||
        !(pending_save_->record == request.record)) return false;
    pending_save_.reset();
    if (!success) return false;
    saved_ = request.record;
    return true;
}

}  // namespace pico_page
