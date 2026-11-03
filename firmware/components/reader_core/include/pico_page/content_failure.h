#pragma once

namespace pico_page {
/** Resource preparation outcome. Diagnostic text never determines cache or retry policy. */
enum class ContentFailureKind {
    None,
    MissingResource,
    InvalidFormat,
    IoError,
    OutOfMemory,
    Cancelled,
    SourceChanged,
    TimedOut,
};

constexpr bool content_failure_is_permanent(ContentFailureKind kind) {
    return kind == ContentFailureKind::MissingResource || kind == ContentFailureKind::InvalidFormat;
}

/** Cancellation permits a later request, but never a negative cache entry. Scheduling bounds belong to the owner. */
constexpr bool content_failure_is_retryable(ContentFailureKind kind) {
    return kind == ContentFailureKind::IoError || kind == ContentFailureKind::OutOfMemory ||
           kind == ContentFailureKind::Cancelled || kind == ContentFailureKind::SourceChanged ||
           kind == ContentFailureKind::TimedOut;
}
} // namespace pico_page
