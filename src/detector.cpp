#include "soundtrigger/detector.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <utility>

namespace soundtrigger {

void Model::validate() const
{
    if (frames < 4 || frames > 128 || bins.empty() || bins.size() > 64 ||
        templates.empty() || templates.size() > 64)
        throw std::runtime_error("Invalid model dimensions");
    if (!std::isfinite(threshold) || threshold <= 0 || threshold > 1 ||
        !std::isfinite(cooldown_seconds) || cooldown_seconds < 0.1f || cooldown_seconds > 60 ||
        !std::isfinite(minimum_rms_db) || minimum_rms_db < -120 || minimum_rms_db > 0)
        throw std::runtime_error("Invalid detector settings");
    for (size_t i = 0; i < bins.size(); ++i) {
        if (bins[i] < 6 || bins[i] > fft_size / 2 - 6 || (i && bins[i] <= bins[i - 1]))
            throw std::runtime_error("Invalid or unsorted frequency bands");
    }
    for (const auto &t : templates) {
        if (t.name.empty() || t.name.size() > 1024 || t.values.size() != frames * bins.size())
            throw std::runtime_error("Invalid template dimensions");
        double energy = 0;
        for (float value : t.values) {
            if (!std::isfinite(value) || value < 0 || value > 1)
                throw std::runtime_error("Invalid template value");
            energy += value * value;
        }
        if (std::abs(energy - 1.0) > 0.001)
            throw std::runtime_error("Template is not normalized");
    }
}

Model Model::load(const std::filesystem::path &path)
{
    if (std::filesystem::file_size(path) > 8 * 1024 * 1024)
        throw std::runtime_error("Model exceeds 8 MiB limit");
    std::ifstream in(path);
    std::string magic;
    size_t version, rate, fft, hop, bands, count;
    Model model;
    if (!(in >> magic >> version >> rate >> fft >> hop) || magic != "SOUNDTRIGGER" ||
        version != 1 || rate != sample_rate || fft != fft_size || hop != hop_size)
        throw std::runtime_error("Unsupported model format or analysis settings");
    if (!(in >> model.frames >> bands >> count >> model.threshold >> model.cooldown_seconds >>
          model.minimum_rms_db) || model.frames < 4 || model.frames > 128 ||
        bands < 1 || bands > 64 || count < 1 || count > 64)
        throw std::runtime_error("Invalid model header");
    model.bins.resize(bands);
    for (auto &bin : model.bins)
        if (!(in >> bin)) throw std::runtime_error("Truncated model bands");
    model.templates.resize(count);
    for (auto &t : model.templates) {
        if (!(in >> std::quoted(t.name))) throw std::runtime_error("Missing template name");
        t.values.resize(model.frames * bands);
        for (auto &value : t.values)
            if (!(in >> value)) throw std::runtime_error("Truncated template");
    }
    in >> std::ws;
    if (!in.eof()) throw std::runtime_error("Unexpected data after model");
    model.validate();
    return model;
}

Detector::Detector(Model model) : model_(std::move(model))
{
    model_.validate();
    minimum_power_ = std::pow(10.0f, model_.minimum_rms_db / 10.0f);
    cooldown_samples_ = static_cast<uint64_t>(std::ceil(static_cast<double>(model_.cooldown_seconds) * sample_rate));
    constexpr double pi = 3.14159265358979323846;
    for (size_t i = 0; i < fft_size; ++i) {
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * pi * i / (fft_size - 1)));
        size_t reversed = 0;
        for (size_t b = i, j = fft_size; j > 1; j >>= 1, b >>= 1)
            reversed = (reversed << 1) | (b & 1);
        reversed_[i] = reversed;
    }
    for (size_t i = 0; i < fft_size / 2; ++i)
        twiddles_[i] = std::polar(1.0f, static_cast<float>(-2 * pi * i / fft_size));
    history_.resize(model_.frames * model_.bins.size());
    rms_history_.resize(model_.frames);
    reset();
}

void Detector::reset()
{
    audio_.fill(0);
    std::fill(history_.begin(), history_.end(), 0);
    std::fill(rms_history_.begin(), rms_history_.end(), 0);
    audio_pos_ = history_pos_ = available_frames_ = 0;
    samples_ = last_trigger_ = 0;
    has_triggered_ = false;
}

bool Detector::push(float sample, Detection &result)
{
    audio_[audio_pos_] = std::isfinite(sample) ? sample : 0.0f;
    audio_pos_ = (audio_pos_ + 1) % fft_size;
    ++samples_;
    if (samples_ < fft_size || (samples_ - fft_size) % hop_size) return false;
    analyse(result);
    return true;
}

void Detector::analyse(Detection &result)
{
    float rms_energy = 0;
    for (size_t i = 0; i < fft_size; ++i) {
        float value = audio_[(audio_pos_ + i) % fft_size];
        rms_energy += value * value;
        fft_[reversed_[i]] = value * window_[i];
    }
    for (size_t length = 2; length <= fft_size; length *= 2) {
        const size_t half = length / 2;
        const size_t step = fft_size / length;
        for (size_t base = 0; base < fft_size; base += length) {
            for (size_t j = 0; j < half; ++j) {
                const auto even = fft_[base + j];
                const auto odd = fft_[base + j + half] * twiddles_[j * step];
                fft_[base + j] = even + odd;
                fft_[base + j + half] = even - odd;
            }
        }
    }
    for (size_t i = 0; i < power_.size(); ++i) power_[i] = std::norm(fft_[i]);
    const size_t bands = model_.bins.size();
    for (size_t i = 0; i < bands; ++i) {
        const auto b = model_.bins[i];
        const float signal = power_[b - 1] + power_[b] + power_[b + 1];
        const float noise = (power_[b - 6] + power_[b - 5] + power_[b - 4] +
                             power_[b + 4] + power_[b + 5] + power_[b + 6]) * 0.5f;
        history_[history_pos_ * bands + i] = std::sqrt(std::max(0.0f, signal - noise));
    }
    rms_history_[history_pos_] = rms_energy / fft_size;
    history_pos_ = (history_pos_ + 1) % model_.frames;
    available_frames_ = std::min(available_frames_ + 1, model_.frames);
    result = {samples_, 0, 0, false};
    if (available_frames_ < model_.frames) return;

    float energy = 0, total_rms = 0;
    for (float v : history_) energy += v * v;
    for (float v : rms_history_) total_rms += v;
    if (energy < 1e-20f || total_rms / model_.frames < minimum_power_) return;
    const float denominator = std::sqrt(energy);
    // Split the circular history into two contiguous dot products.
    const size_t split = history_pos_ * bands;
    for (size_t t = 0; t < model_.templates.size(); ++t) {
        const auto &values = model_.templates[t].values;
        float dot = 0;
        size_t j = 0;
        for (size_t i = split; i < history_.size(); ++i) dot += history_[i] * values[j++];
        for (size_t i = 0; i < split; ++i) dot += history_[i] * values[j++];
        const float score = std::min(1.0f, dot / denominator);
        if (score > result.score) { result.score = score; result.template_index = t; }
    }
    if (result.score >= model_.threshold &&
        (!has_triggered_ || samples_ - last_trigger_ >= cooldown_samples_)) {
        result.triggered = true;
        last_trigger_ = samples_;
        has_triggered_ = true;
    }
}

} // namespace soundtrigger
