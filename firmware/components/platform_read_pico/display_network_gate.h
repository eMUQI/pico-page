#pragma once

#include <atomic>

namespace pico_page::platform {

class DisplayNetworkGate {
public:
    bool try_network() {
        unsigned expected = 0;
        return state_.compare_exchange_strong(expected, network);
    }
    void finish_network() { state_.fetch_and(~network); }
    bool try_display() {
        state_.fetch_or(requested);
        unsigned expected = requested;
        return state_.compare_exchange_strong(expected, requested | display);
    }
    void finish_display() { state_.store(0); }

    void cancel_display() { state_.fetch_and(~requested); }
private:
    static constexpr unsigned requested = 1, network = 2, display = 4;
    std::atomic<unsigned> state_{0};
};

extern DisplayNetworkGate presentation_gate;

}
