#pragma once

#include <cstdint>
#include <string>

namespace pico_page::platform {

/** K1/K2/K3 are logical auxiliary-key identities; the provider maps available controls to them. */
enum class EventKind { None, K1, K2, K3, Touch, Power, SdRemoved, InputReset, PowerLong };
enum class EventPhase { Begin, End, Cancel, Move };

struct Event {
    EventKind kind = EventKind::None;
    EventPhase phase = EventPhase::Begin;
    std::uint64_t sequence = 0;
    int x = 0;
    int y = 0;
    /** Monotonic microsecond time of the controller sample that produced a touch or key event. */
    std::int64_t time_us = 0;
    /** Inclusive extrema of all sampled positions in this contact, carried on release. */
    int min_x = x, min_y = y, max_x = x, max_y = y;
};

/**
 * Polls one event; EventKind::None means no new event. Touch coordinates use
 * portrait pixels. Auxiliary-key actions are identified by kind, not by touch coordinates.
 * InputReset cancels all prior contacts after queue overflow; input resumes after release.
 * A held input never repeats. Read failures cancel an active touch sequence.
 * Sampling continues while the application task is blocked, except during flash exclusion
 * or sleep. Sampling cadence and controller synchronization belong to the provider.
 * Queued touch and auxiliary-key events carry their sample time. Power and card events
 * take priority over queued input and have no sample timestamp. False leaves event empty.
 */
bool poll(Event& event, std::string& error);

struct InputDiagnostics { std::uint32_t overflows = 0, stack_free_bytes = 0; };
/** Sampler overflow count and minimum remaining stack since task creation. */
InputDiagnostics input_diagnostics();

/** Blocks the application task until a sampled event is waiting or the timeout expires. */
void wait_for_input(std::uint32_t timeout_ms);

/** True when the next waiting touch or key event was sampled before time_us. */
bool input_waiting_before(std::int64_t time_us);

}
