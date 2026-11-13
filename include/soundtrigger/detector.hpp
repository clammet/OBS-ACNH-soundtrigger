#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace soundtrigger {

constexpr size_t sample_rate = 16000;
constexpr size_t fft_size = 512;
constexpr size_t hop_size = 256;

struct Template {
    std::string name;
    std::vector<float> values; // Chronological frames, then frequency bands; unit L2 norm.
};

struct Model {
    size_t frames = 0;
    float threshold = 0.9f;
    float cooldown_seconds = 1.0f;
    float minimum_rms_db = -75.0f;
    std::vector<size_t> bins;
    std::vector<Template> templates;

    static Model load(const std::filesystem::path &path);
    void validate() const;
};

struct Detection {
    uint64_t end_sample = 0;
    float score = 0.0f;
    size_t template_index = 0;
    bool triggered = false;
};

// Single-threaded, causal, fixed-memory processing after construction.
class Detector {
public:
    explicit Detector(Model model);
    void reset();
    // Returns true on analysis frames (every hop_size samples).
    bool push(float sample, Detection &result);
    const Model &model() const { return model_; }

private:
    void analyse(Detection &result);
    Model model_;
    std::array<float, fft_size> audio_{};
    std::array<float, fft_size> window_{};
    std::array<size_t, fft_size> reversed_{};
    std::array<std::complex<float>, fft_size / 2> twiddles_{};
    std::array<std::complex<float>, fft_size> fft_{};
    std::array<float, fft_size / 2 + 1> power_{};
    std::vector<float> history_;
    std::vector<float> rms_history_;
    size_t audio_pos_ = 0;
    size_t history_pos_ = 0;
    size_t available_frames_ = 0;
    uint64_t samples_ = 0;
    uint64_t last_trigger_ = 0;
    uint64_t cooldown_samples_ = 0;
    float minimum_power_ = 0;
    bool has_triggered_ = false;
};

} // namespace soundtrigger
