#include "soundtrigger/audio_queue.hpp"
#include "soundtrigger/detector.hpp"
#include "soundtrigger/recording.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

static void recording_tests()
{
    using R = soundtrigger::RecordingController;
    R r;
    require(r.hit(0, 10, false) == R::Action::start, "First detection must start recording");
    r.started();
    require(r.hit(8, 10, true) == R::Action::none && r.deadline() == 18, "Hit resets deadline to hit plus X");
    require(r.tick(10, true) == R::Action::none, "Original deadline must not stop extended recording");
    require(r.tick(18, true) == R::Action::stop, "Extended deadline must stop recording");
    require(r.tick(19, true) == R::Action::none, "Stop must be emitted only once");
    r.stopped();
    require(r.hit(20, 10, true) == R::Action::none && r.state() == R::State::idle, "Manual recording must stay manual");
    require(r.hit(30, 10, false) == R::Action::start, "A new recording can start after stop");
    require(r.hit(31, 10, false) == R::Action::none && r.deadline() == 41, "Hit during startup extends deadline");
    r.tick(36, false);
    require(r.state() == R::State::idle, "Failed start must release ownership");
    r.hit(37, 10, false);
    r.tick(43, false, true);
    require(r.state() == R::State::starting, "Slow asynchronous start must retain timer ownership");
    r.started();
    require(r.tick(47, true) == R::Action::stop, "Slow-started recording must still stop at its deadline");
    r.stopped();
    r.hit(40, 10, false); r.started(); r.stopped();
    require(r.tick(100, true) == R::Action::none, "Manual stop must cancel automatic timer");
    r.hit(110, 10, false); r.started(); r.relinquish();
    require(r.tick(200, true) == R::Action::none, "Disarming must relinquish ownership");
}

static void queue_tests()
{
    soundtrigger::AudioQueue queue;
    soundtrigger::AudioBlock block;
    float value = 0.25f;
    for (size_t i = 0; i < queue.capacity; ++i)
        require(queue.push(&value, 1, i, 1.5), "Queue must accept its capacity");
    require(!queue.push(&value, 1, 1, 0) && queue.dropped() == 1, "Full queue must drop without blocking");
    for (size_t i = 0; i < queue.capacity; ++i) {
        require(queue.pop(block), "Queue must preserve unread blocks");
        require(block.audio_timestamp == i && block.samples[0] == value && block.generation == 0,
                "Queue must preserve samples, order, and timestamps");
    }
    require(!queue.pop(block), "Empty queue cannot produce data");
    queue.push(&value, 1, 0, 0); queue.pop(block);
    require(block.generation == 1, "First block after overflow must mark a discontinuity");

    soundtrigger::AudioQueue concurrent;
    constexpr size_t count = 20000;
    std::thread producer([&] {
        for (size_t i = 0; i < count; ++i) {
            float sample = static_cast<float>(i);
            while (!concurrent.push(&sample, 1, i, 0)) std::this_thread::yield();
        }
    });
    for (size_t i = 0; i < count;) {
        if (!concurrent.pop(block)) { std::this_thread::yield(); continue; }
        require(block.audio_timestamp == i && block.samples[0] == i, "Concurrent queue corrupted data");
        ++i;
    }
    producer.join();
}

static void detector_tests()
{
    soundtrigger::Model model;
    model.frames = 4;
    model.bins = {64, 128};
    model.threshold = 0.99f;
    model.templates = {{"tone", {0.5f, 0, 0.5f, 0, 0.5f, 0, 0.5f, 0}}};
    soundtrigger::Detector d(model);
    soundtrigger::Detection result;
    unsigned triggers = 0;
    for (unsigned i = 0; i < 16000; ++i) {
        if (d.push(0.1f * std::sin(2 * 3.141592653589793 * 2000 * i / 16000), result) && result.triggered) {
            ++triggers;
            require(result.score > .999f, "Matching tone must have a near-unit score");
            require(result.end_sample >= 1280, "No detection before full history");
        }
    }
    require(triggers == 1, "Cooldown must suppress duplicate detections");
    d.reset();
    for (unsigned i = 0; i < 32000; ++i) {
        if (d.push(i == 1 ? std::numeric_limits<float>::quiet_NaN() : 0, result))
            require(!result.triggered && result.score == 0, "Silence and NaN must not trigger");
    }
    d.reset();
    for (unsigned i = 0; i < 32000; ++i) {
        if (d.push(0.1f * std::sin(2 * 3.141592653589793 * 4000 * i / 16000), result))
            require(!result.triggered, "Wrong-frequency tone must not trigger");
    }
    auto path = std::filesystem::temp_directory_path() / "soundtrigger-model-test.stmodel";
    { std::ofstream f(path); f << "SOUNDTRIGGER 1\n16000 512 256\n4 2 1\n0.9 1 -75\n64 128\n\"tone\"\n.5 0 .5 0 .5 0 .5 0\n"; }
    require(soundtrigger::Model::load(path).templates.front().name == "tone", "Model parser must read valid templates");
    { std::ofstream f(path); f << "SOUNDTRIGGER 1\n16000 512 256\n4 2 1\n0.9 1 -75\n64 128\n\"tone\"\n.5 0"; }
    bool rejected = false;
    try { soundtrigger::Model::load(path); } catch (const std::exception &) { rejected = true; }
    require(rejected, "Truncated models must be rejected");
    std::filesystem::remove(path);
    model.bins[0] = 0;
    rejected = false;
    try { soundtrigger::Detector bad(model); } catch (const std::exception &) { rejected = true; }
    require(rejected, "Unsafe FFT bin indices must be rejected");
}

int main()
{
    try { recording_tests(); queue_tests(); detector_tests(); }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return EXIT_FAILURE; }
    std::cout << "Detector, model parsing, audio queue, and recording controller passed\n";
}
