#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace soundtrigger {

struct AudioBlock {
    std::array<float, 512> samples;
    size_t count = 0;
    uint64_t generation = 0;
    uint64_t audio_timestamp = 0;
    double captured_at = 0;
};

// One producer (OBS mixer), one consumer (detector worker).
// Dropping a block increments a generation so no fingerprint can span a gap.
class AudioQueue {
public:
    static constexpr size_t capacity = 64;
    bool push(const float *samples, size_t count, uint64_t timestamp, double captured_at)
    {
        if (count > AudioBlock{}.samples.size()) return false;
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) >= capacity) {
            ++generation_;
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        auto &block = blocks_[write % capacity];
        for (size_t i = 0; i < count; ++i) block.samples[i] = samples[i];
        block.count = count;
        block.generation = generation_;
        block.audio_timestamp = timestamp;
        block.captured_at = captured_at;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }
    bool pop(AudioBlock &block)
    {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        block = blocks_[read % capacity];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::array<AudioBlock, capacity> blocks_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
    std::atomic<uint64_t> dropped_{0};
    uint64_t generation_ = 0;
};

} // namespace soundtrigger
