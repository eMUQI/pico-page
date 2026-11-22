#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace pico_page::platform {

enum class FirmwareFailure {
    None,
    Cancelled,
    Network,     // No link, a failed connection or an interrupted transfer.
    Server,
    Mismatch,
    TooLarge,
    WrongImage,
    WrongLayout,
    Flash,
};

struct FirmwareImage {
    std::string url;      // https address; redirects are not followed.
    std::uint32_t size;   // Exact length in bytes.
    std::string sha256;   // 64 lowercase hexadecimal digits of the whole file.
    std::string version;  // Version that the image's application description must carry.
};

/** Callbacks on the downloading task; each may be null. */
struct FirmwareHooks {
    /** After each written block, with the number of bytes written so far. */
    void (*progress)(void* context, std::uint32_t written) = nullptr;
    /**
     * true before and false after each erase block (at most 64 KiB), write (at most 4 KiB),
     * or final image verification. The caller keeps these operations apart from panel updates.
     */
    void (*flash)(void* context, bool begin) = nullptr;
    void* context = nullptr;
};

/** Checks whether the running application and actual flash partitions support application OTA for this build. */
bool firmware_layout_supported();

/**
 * Streams one application image over HTTPS into the inactive OTA slot after connect_network(),
 * with certificate and server-name verification. Nothing is written until the active flash layout
 * is supported and the image header and application description match the chip, running project name
 * and requested version. The slot is pre-erased in bounded blocks before streaming writes.
 * true means that the length and SHA-256 match and that the image in
 * the slot passed its own digest check; the boot slot is not changed. On any failure the
 * running firmware and the boot slot are untouched; an incomplete inactive image is never selected.
 * Reads tolerate 30 s without data. Up to three reconnects resume from bytes accepted by the OTA
 * handle, with a shared 20 min retry budget; no state survives cancellation or a new call.
 * The client uses a 10 s connection timeout, but synchronous DNS, in-flight SDK calls and cleanup
 * can extend the call beyond these budgets. cancelled is sampled at explicit checkpoints between
 * SDK steps and writes; it must outlive the call, including mandatory resource cleanup.
 */
bool download_firmware(const FirmwareImage& image, const FirmwareHooks& hooks, FirmwareFailure& failure,
                       std::string& error, const std::atomic_bool* cancelled = nullptr);

/**
 * Selects the slot verified by the last successful download_firmware() for the next start and
 * marks it for one unconfirmed start. Validates the image again. The caller restarts afterwards.
 */
bool activate_firmware(std::string& error);

/** True on the first start of an activated image, until confirm_firmware(). A reset in this state returns to the previous slot. */
bool firmware_unconfirmed();
/** Keeps the running image for later starts. false when the state could not be written; the image then stays unconfirmed. */
bool confirm_firmware();
/** Marks the running image invalid and restarts into the previous slot; returns only on failure. */
void reject_firmware();

}
