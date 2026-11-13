#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include "soundtrigger/audio_queue.hpp"
#include "soundtrigger/detector.hpp"
#include "soundtrigger/recording.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void) { return "Sound Trigger"; }
MODULE_EXPORT const char *obs_module_description(void)
{
    return "Lightweight sound fingerprints and timed, extendable recording for OBS on macOS";
}

static double monotonic_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Runtime {
public:
    ~Runtime() { stop(); }
    void start(soundtrigger::Model model, size_t track)
    {
        stop();
        if (!obs_get_audio()) throw std::runtime_error("OBS audio is not initialized");
        detector = std::make_unique<soundtrigger::Detector>(std::move(model));
        queue = std::make_unique<soundtrigger::AudioQueue>();
        track_ = track;
        hits.store(0);
        latest_at.store(0);
        latest_score.store(0);
        peak_score.store(0);
        last_audio_at.store(0);
        resets.store(0);
        running.store(true);
        try { worker_ = std::thread([this] { work(); }); }
        catch (...) { running.store(false); throw; }
        audio_convert_info conversion{};
        conversion.samples_per_sec = soundtrigger::sample_rate;
        conversion.format = AUDIO_FORMAT_FLOAT;
        conversion.speakers = SPEAKERS_MONO;
        // OBS does the downmix and anti-aliased sample-rate conversion once.
        // Using audio_output_connect exposes its success result, unlike the void wrapper.
        if (!audio_output_connect(obs_get_audio(), track_, &conversion, audio_callback, this)) {
            stop();
            throw std::runtime_error("Could not connect to the selected OBS audio track");
        }
        connected_ = true;
    }
    void stop()
    {
        if (connected_) {
            // Disconnect waits for the current callback to finish before returning.
            audio_output_disconnect(obs_get_audio(), track_, audio_callback, this);
            connected_ = false;
        }
        running.store(false);
        if (worker_.joinable()) worker_.join();
    }

    std::unique_ptr<soundtrigger::Detector> detector;
    std::unique_ptr<soundtrigger::AudioQueue> queue;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> resets{0};
    std::atomic<double> latest_at{0};
    std::atomic<double> last_audio_at{0};
    std::atomic<float> latest_score{0};
    std::atomic<float> peak_score{0};
    std::atomic<size_t> latest_template{0};

private:
    static void audio_callback(void *data, size_t, audio_data *audio)
    {
        auto &self = *static_cast<Runtime *>(data);
        if (!self.running.load(std::memory_order_relaxed) || !audio->data[0]) return;
        const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
        const double now = monotonic_seconds();
        self.last_audio_at.store(now, std::memory_order_relaxed);
        for (size_t offset = 0; offset < audio->frames; offset += 512) {
            const size_t count = std::min<size_t>(512, audio->frames - offset);
            self.queue->push(samples + offset, count,
                             audio->timestamp + offset * 1000000000ULL / soundtrigger::sample_rate, now);
        }
    }
    void work()
    {
        soundtrigger::AudioBlock block;
        soundtrigger::Detection result;
        uint64_t generation = 0, expected_timestamp = 0;
        while (running.load(std::memory_order_relaxed)) {
            if (!queue->pop(block)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                continue;
            }
            const bool gap = expected_timestamp &&
                std::abs(static_cast<double>(block.audio_timestamp) - expected_timestamp) > 100000000;
            const bool stale = monotonic_seconds() - block.captured_at > 0.25;
            if (block.generation != generation || gap || stale) {
                detector->reset();
                resets.fetch_add(1, std::memory_order_relaxed);
            }
            generation = block.generation;
            expected_timestamp = block.audio_timestamp + block.count * 1000000000ULL / soundtrigger::sample_rate;
            if (stale) continue;
            for (size_t i = 0; i < block.count; ++i) {
                if (!detector->push(block.samples[i], result)) continue;
                const float previous = peak_score.load(std::memory_order_relaxed);
                if (result.score > previous) peak_score.store(result.score, std::memory_order_relaxed);
                if (!result.triggered) continue;
                latest_at.store(block.captured_at, std::memory_order_relaxed);
                latest_score.store(result.score, std::memory_order_relaxed);
                latest_template.store(result.template_index, std::memory_order_relaxed);
                hits.fetch_add(1, std::memory_order_release);
            }
        }
    }
    size_t track_ = 0;
    bool connected_ = false;
    std::thread worker_;
};

static std::unique_ptr<Runtime> runtime;

@interface SoundTriggerUI : NSObject {
    soundtrigger::RecordingController recording;
    uint64_t seenHits;
    BOOL externalStarting;
    BOOL outputStopping;
    double durationSeconds;
    BOOL dryRun;
    NSString *settingsPath;
    NSString *notice;
    NSWindow *window;
    NSTextField *modelField;
    NSTextField *durationField;
    NSTextField *thresholdField;
    NSPopUpButton *trackField;
    NSButton *enabledButton;
    NSButton *dryRunButton;
    NSTextField *statusLabel;
    NSTimer *timer;
}
- (void)show;
- (void)apply:(id)sender;
- (void)browse:(id)sender;
- (void)poll:(NSTimer *)sender;
- (void)event:(enum obs_frontend_event)event;
- (void)shutdown;
@end

static SoundTriggerUI *ui;

static NSTextField *label(NSView *view, NSString *text, CGFloat x, CGFloat y, CGFloat width)
{
    NSTextField *field = [NSTextField labelWithString:text];
    field.frame = NSMakeRect(x, y, width, 22);
    [view addSubview:field];
    return field;
}

static NSTextField *input(NSView *view, CGFloat x, CGFloat y, CGFloat width)
{
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(x, y, width, 24)];
    [view addSubview:field];
    return field;
}

static NSButton *checkbox(NSView *view, NSString *text, CGFloat y)
{
    NSButton *button = [NSButton checkboxWithTitle:text target:nil action:nil];
    button.frame = NSMakeRect(24, y, 560, 24);
    [view addSubview:button];
    return button;
}

@implementation SoundTriggerUI
- (instancetype)init
{
    self = [super init];
    if (!self) return nil;
    durationSeconds = 30;
    dryRun = YES;
    notice = @"Choose a model, enable detection, then Apply. Dry run logs hits without recording.";
    char *config = obs_module_config_path("settings.json");
    if (config) { settingsPath = @(config); bfree(config); }

    window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 470)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    window.title = @"OBS Sound Trigger";
    window.releasedWhenClosed = NO;
    [window center];
    NSView *view = window.contentView;
    label(view, @"Sound fingerprint model", 24, 423, 400);
    modelField = input(view, 24, 392, 494);
    NSButton *browse = [NSButton buttonWithTitle:@"Choose…" target:self action:@selector(browse:)];
    browse.frame = NSMakeRect(526, 391, 90, 26);
    [view addSubview:browse];
    label(view, @"Audio track to monitor", 24, 343, 260);
    trackField = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(290, 341, 120, 26) pullsDown:NO];
    for (int i = 1; i <= 6; ++i) [trackField addItemWithTitle:[NSString stringWithFormat:@"Track %d", i]];
    [view addSubview:trackField];
    label(view, @"Record for seconds after the latest hit", 24, 302, 310);
    durationField = input(view, 360, 300, 100);
    label(view, @"Match threshold (0 = model default)", 24, 261, 310);
    thresholdField = input(view, 360, 259, 100);
    enabledButton = checkbox(view, @"Enable detection", 212);
    dryRunButton = checkbox(view, @"Dry run — log detections without starting a recording", 179);
    NSTextField *hint = label(view, @"A new hit keeps recording until X seconds after that hit.\nDisarming leaves an active recording under manual control.", 24, 122, 590);
    hint.frame = NSMakeRect(24, 115, 590, 48);
    hint.lineBreakMode = NSLineBreakByWordWrapping;
    NSButton *apply = [NSButton buttonWithTitle:@"Apply" target:self action:@selector(apply:)];
    apply.frame = NSMakeRect(516, 73, 100, 30);
    [view addSubview:apply];
    statusLabel = label(view, notice, 24, 12, 590);
    statusLabel.frame = NSMakeRect(24, 12, 590, 57);
    statusLabel.lineBreakMode = NSLineBreakByWordWrapping;
    statusLabel.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];

    obs_data_t *settings = settingsPath ? obs_data_create_from_json_file(settingsPath.fileSystemRepresentation) : nullptr;
    if (!settings) settings = obs_data_create();
    obs_data_set_default_double(settings, "duration_seconds", 30);
    obs_data_set_default_bool(settings, "dry_run", true);
    obs_data_set_default_int(settings, "track", 1);
    const char *savedModel = obs_data_get_string(settings, "model");
    if (*savedModel) modelField.stringValue = @(savedModel);
    else {
        char *bundled = obs_module_file("acnh.stmodel");
        if (bundled) { modelField.stringValue = @(bundled); bfree(bundled); }
    }
    durationField.doubleValue = obs_data_get_double(settings, "duration_seconds");
    thresholdField.doubleValue = obs_data_get_double(settings, "threshold");
    dryRunButton.state = obs_data_get_bool(settings, "dry_run") ? NSControlStateValueOn : NSControlStateValueOff;
    [trackField selectItemAtIndex:std::clamp<int64_t>(obs_data_get_int(settings, "track") - 1, 0, 5)];
    // Always begin disarmed. Restarts cannot unexpectedly begin recordings.
    enabledButton.state = NSControlStateValueOff;
    obs_data_release(settings);
    timer = [NSTimer timerWithTimeInterval:0.05 target:self selector:@selector(poll:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    return self;
}

- (void)show
{
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)browse:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"stmodel"] ?: UTTypeData];
    [panel beginSheetModalForWindow:window completionHandler:^(NSModalResponse response) {
        if (response == NSModalResponseOK) modelField.stringValue = panel.URL.path;
    }];
}

- (void)apply:(id)sender
{
    (void)sender;
    NSNumberFormatter *formatter = [[NSNumberFormatter alloc] init];
    formatter.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSNumber *duration = [formatter numberFromString:durationField.stringValue];
    NSNumber *threshold = [formatter numberFromString:thresholdField.stringValue];
    if (!duration || !threshold || !std::isfinite(duration.doubleValue) || duration.doubleValue < 1 ||
        duration.doubleValue > 3600 || !std::isfinite(threshold.doubleValue) || threshold.doubleValue < 0 ||
        threshold.doubleValue > 1) {
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Check the recording duration and threshold";
        alert.informativeText = @"Duration must be 1–3600 seconds. Threshold must be 0–1; use 0 for the model default.";
        [alert beginSheetModalForWindow:window completionHandler:nil];
        return;
    }
    BOOL enable = enabledButton.state == NSControlStateValueOn;
    try {
        // Validate before disturbing the current detector or automatic recording timer.
        soundtrigger::Model model;
        if (enable) {
            model = soundtrigger::Model::load(modelField.stringValue.fileSystemRepresentation);
            if (threshold.doubleValue > 0) model.threshold = threshold.doubleValue;
        }
        runtime->stop();
        recording.relinquish();
        seenHits = 0;
        durationSeconds = duration.doubleValue;
        dryRun = dryRunButton.state == NSControlStateValueOn;
        if (enable) runtime->start(std::move(model), trackField.indexOfSelectedItem);
        notice = enable ? @"Listening" : @"Detection disabled";
        if (enable) blog(LOG_INFO, "[soundtrigger] Listening on track %ld (%s), X=%.1fs", trackField.indexOfSelectedItem + 1,
                         dryRun ? "dry run" : "automatic recording", durationSeconds);

        obs_data_t *settings = obs_data_create();
        obs_data_set_string(settings, "model", modelField.stringValue.fileSystemRepresentation);
        obs_data_set_double(settings, "duration_seconds", durationSeconds);
        obs_data_set_double(settings, "threshold", threshold.doubleValue);
        obs_data_set_int(settings, "track", trackField.indexOfSelectedItem + 1);
        obs_data_set_bool(settings, "dry_run", dryRun);
        char *directory = obs_module_config_path("");
        bool saved = false;
        if (directory && settingsPath) {
            os_mkdirs(directory);
            saved = obs_data_save_json_safe(settings, settingsPath.fileSystemRepresentation, "tmp", "bak");
        }
        if (directory) bfree(directory);
        obs_data_release(settings);
        if (!saved) blog(LOG_WARNING, "[soundtrigger] Could not save settings");
    } catch (const std::exception &error) {
        if (!runtime->running.load()) enabledButton.state = NSControlStateValueOff;
        notice = @(error.what());
        blog(LOG_ERROR, "[soundtrigger] %s", error.what());
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Could not enable Sound Trigger";
        alert.informativeText = notice;
        [alert beginSheetModalForWindow:window completionHandler:nil];
    }
    [self poll:nil];
}

- (void)poll:(NSTimer *)sender
{
    (void)sender;
    if (!runtime) return;
    if (!runtime->running.load() && recording.state() == soundtrigger::RecordingController::State::idle) {
        if (window.visible) statusLabel.stringValue = notice;
        return;
    }
    const double now = monotonic_seconds();
    bool active = obs_frontend_recording_active();
    const uint64_t count = runtime->hits.load(std::memory_order_acquire);
    if (runtime->running.load() && count != seenHits) {
        seenHits = count;
        const double at = runtime->latest_at.load(std::memory_order_relaxed);
        const float score = runtime->latest_score.load(std::memory_order_relaxed);
        const size_t index = runtime->latest_template.load(std::memory_order_relaxed);
        if (now - at < 0.5) {
            const auto &name = runtime->detector->model().templates[index].name;
            blog(LOG_INFO, "[soundtrigger] Hit #%llu: %s score=%.4f%s", static_cast<unsigned long long>(count),
                 name.c_str(), score, dryRun ? " (dry run)" : "");
            if (!dryRun && recording.hit(at, durationSeconds, active || externalStarting || outputStopping) ==
                               soundtrigger::RecordingController::Action::start) {
                obs_frontend_recording_start();
                active = obs_frontend_recording_active();
            }
        }
    }
    if (recording.tick(now, active, externalStarting) == soundtrigger::RecordingController::Action::stop) {
        blog(LOG_INFO, "[soundtrigger] Latest-hit timer elapsed; stopping recording");
        obs_frontend_recording_stop();
    }
    if (runtime->running.load() && window.visible) {
        NSString *state = dryRun ? @"DRY RUN" : @"ARMED";
        if (recording.state() == soundtrigger::RecordingController::State::recording)
            state = [NSString stringWithFormat:@"RECORDING (%.1fs left)", std::max(0.0, recording.deadline() - now)];
        else if (active) state = @"Manual recording";
        const double last = runtime->last_audio_at.load(std::memory_order_relaxed);
        statusLabel.stringValue = [NSString stringWithFormat:@"%@ | hits: %llu | last: %.3f | peak: %.3f\n%@ | dropped: %llu | resets: %llu",
            state, static_cast<unsigned long long>(count), runtime->latest_score.load(), runtime->peak_score.load(),
            (last && now - last < 1) ? @"Audio callback active" : @"Waiting for audio callback",
            static_cast<unsigned long long>(runtime->queue->dropped()), static_cast<unsigned long long>(runtime->resets.load())];
    } else if (window.visible) statusLabel.stringValue = notice;
}

- (void)event:(enum obs_frontend_event)event
{
    switch (event) {
    case OBS_FRONTEND_EVENT_RECORDING_STARTING: externalStarting = YES; break;
    case OBS_FRONTEND_EVENT_RECORDING_STARTED:
        externalStarting = NO;
        recording.started();
        break;
    case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
        outputStopping = YES;
        recording.relinquish();
        break;
    case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
        externalStarting = outputStopping = NO;
        recording.stopped();
        break;
    case OBS_FRONTEND_EVENT_RECORDING_PAUSED: recording.relinquish(); break;
    case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
        runtime->stop();
        recording.relinquish();
        enabledButton.state = NSControlStateValueOff;
        notice = @"Profile changed. Apply to enable detection again.";
        break;
    case OBS_FRONTEND_EVENT_EXIT: [self shutdown]; break;
    default: break;
    }
}

- (void)shutdown
{
    [timer invalidate];
    timer = nil;
    if (runtime) runtime->stop();
    recording.relinquish();
    [window close];
}
@end

static void show_settings(void *) { [ui show]; }
static void frontend_event(enum obs_frontend_event event, void *) { [ui event:event]; }

bool obs_module_load(void)
{
    runtime = std::make_unique<Runtime>();
    ui = [[SoundTriggerUI alloc] init];
    obs_frontend_add_tools_menu_item("Sound Trigger…", show_settings, nullptr);
    obs_frontend_add_event_callback(frontend_event, nullptr);
    blog(LOG_INFO, "[soundtrigger] Loaded (disarmed). Open Tools > Sound Trigger.");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(frontend_event, nullptr);
    [ui shutdown];
    ui = nil;
    runtime.reset();
}
