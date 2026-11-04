#pragma once

namespace pico_page {
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

constexpr bool content_failure_is_retryable(ContentFailureKind kind) {
    return kind == ContentFailureKind::IoError || kind == ContentFailureKind::OutOfMemory ||
           kind == ContentFailureKind::Cancelled || kind == ContentFailureKind::SourceChanged ||
           kind == ContentFailureKind::TimedOut;
}
}
