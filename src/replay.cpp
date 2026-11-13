#include "soundtrigger/detector.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: soundtrigger-replay MODEL [SCORES.csv] < mono-16000Hz.f32\n";
        return 2;
    }
    try {
        soundtrigger::Detector detector(soundtrigger::Model::load(argv[1]));
        std::ofstream scores;
        if (argc == 3) {
            scores.open(argv[2]);
            if (!scores) throw std::runtime_error("Cannot open scores CSV");
            scores << "end_seconds,score,template_index,triggered\n";
        }
        std::cout << "end_seconds,score,template_index\n" << std::fixed << std::setprecision(6);
        std::array<float, 4096> buffer;
        uint64_t samples = 0;
        double compute_seconds = 0;
        const auto cpu_start = std::clock();
        soundtrigger::Detection result;
        while (true) {
            const size_t count = std::fread(buffer.data(), sizeof(float), buffer.size(), stdin);
            if (!count) break;
            const auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < count; ++i) {
                if (!detector.push(buffer[i], result)) continue;
                const double time = static_cast<double>(result.end_sample) / soundtrigger::sample_rate;
                if (scores) scores << std::setprecision(9) << time << ',' << result.score << ','
                                   << result.template_index << ',' << result.triggered << '\n';
                if (result.triggered) std::cout << time << ',' << result.score << ','
                                               << result.template_index << '\n';
            }
            compute_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            samples += count;
        }
        if (std::ferror(stdin)) throw std::runtime_error("Error reading audio input");
        const double duration = static_cast<double>(samples) / soundtrigger::sample_rate;
        const double cpu_seconds = static_cast<double>(std::clock() - cpu_start) / CLOCKS_PER_SEC;
        std::cerr << std::fixed << std::setprecision(6)
                  << "{\"audio_seconds\":" << duration << ",\"processing_seconds\":" << compute_seconds
                  << ",\"cpu_seconds\":" << cpu_seconds
                  << ",\"percent_of_one_core\":" << (duration ? cpu_seconds / duration * 100 : 0)
                  << ",\"includes_score_csv_io\":" << (scores.is_open() ? "true" : "false") << "}\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
