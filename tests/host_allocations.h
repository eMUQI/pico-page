#pragma once
#include <cstddef>
#include <cstdint>
namespace host_allocations {
struct Snapshot { std::uint64_t calls = 0, bytes = 0; std::size_t live = 0, peak = 0; };
// Captures are serial; all workers must join before ending or starting the next capture.
void begin();
Snapshot end();
}
