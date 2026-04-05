#pragma once

#include <atomic>

// RT-safe: process() runs in the JACK thread — no allocations, locks, or syscalls.
class Effect {
public:
    std::atomic<bool> enabled{true};

    virtual ~Effect() = default;
    virtual const char *name() const = 0;
    virtual int effect_kind() const { return 0; }
    virtual void process(float *samples, unsigned frames,
                         unsigned channels, unsigned sample_rate) = 0;
    virtual void reset() {}
};
