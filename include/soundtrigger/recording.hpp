#pragma once

#include <algorithm>

namespace soundtrigger {

// All calls belong on OBS's UI thread. Time is monotonic seconds.
class RecordingController {
public:
    enum class Action { none, start, stop };
    enum class State { idle, starting, recording, stopping };

    Action hit(double now, double duration, bool externally_active)
    {
        if (state_ == State::idle) {
            // Never take ownership of a recording started by the user.
            if (externally_active) return Action::none;
            state_ = State::starting;
            start_requested_ = now;
            deadline_ = now + duration;
            return Action::start;
        }
        if (state_ == State::starting || state_ == State::recording)
            deadline_ = std::max(deadline_, now + duration);
        return Action::none;
    }

    void started() { if (state_ == State::starting) state_ = State::recording; }
    void stopped() { state_ = State::idle; deadline_ = 0; }
    Action tick(double now, bool active, bool start_in_progress = false)
    {
        if (state_ == State::starting) {
            if (active) state_ = State::recording;
            else if (!start_in_progress && now - start_requested_ >= 5.0) stopped(); // Failed request; allow retry.
        }
        if (state_ == State::recording) {
            if (!active) { stopped(); return Action::none; }
            if (now >= deadline_) { state_ = State::stopping; return Action::stop; }
        }
        return Action::none;
    }
    void relinquish() { stopped(); } // Disarming leaves the file recording under manual control.
    State state() const { return state_; }
    double deadline() const { return deadline_; }

private:
    State state_ = State::idle;
    double deadline_ = 0;
    double start_requested_ = 0;
};

} // namespace soundtrigger
