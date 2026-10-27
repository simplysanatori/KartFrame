// SPDX-License-Identifier: GPL-3.0-or-later
//
// Throwing the held item: the swing detector and the stick and item button it
// plays, tested without a headset (vr/openxr_item_throw.h).

#include "vr/openxr_item_throw.h"

#include <cmath>
#include <functional>
#include <utility>
#include <iostream>
#include <vector>

namespace {

using namespace mkw::vr;
using namespace mkw::vr::item_throw;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

struct Fire {
    Direction direction;
    float time;
};

// Plays `duration` seconds of a hand moving with `velocity(t)` (m/s in the seated
// frame) from `start`, at `rate` samples a second, and returns every throw.
std::vector<Fire> Play(ThrowDetector& detector, const std::function<std::array<float, 3>(float)>& velocity,
                       float duration, float rate = 90.0f, std::array<float, 3> start = {-0.2f, -0.3f, -0.35f},
                       const std::function<bool(float)>& held = nullptr, bool bare = false,
                       const std::function<bool(float)>& tracked = nullptr) {
    std::vector<Fire> fires;
    const float dt = 1.0f / rate;
    std::array<float, 3> position = start;
    for (float t = 0.0f; t < duration; t += dt) {
        const auto v = velocity(t);
        for (int axis = 0; axis < 3; ++axis) {
            position[axis] += v[axis] * dt;
        }
        HandSample sample{};
        sample.tracked = tracked ? tracked(t) : true;
        sample.held = held ? held(t) : false;
        sample.bare = bare;
        sample.position = position;
        const Direction direction = detector.Update(sample, dt);
        if (direction != Direction::None) {
            fires.push_back({direction, t});
        }
    }
    return fires;
}

// A flick: speeds up at 30 m/s^2 to `peak`, holds it briefly and stops.
std::function<std::array<float, 3>(float)> Flick(std::array<float, 3> direction, float peak, float start = 0.1f) {
    return [=](float t) {
        const float local = t - start;
        float speed = 0.0f;
        if (local >= 0.0f && local < 0.25f) {
            speed = std::min(30.0f * local, peak);
        }
        return std::array<float, 3>{direction[0] * speed, direction[1] * speed, direction[2] * speed};
    };
}

void TestThrows() {
    for (float rate : {72.0f, 90.0f, 120.0f}) {
        ThrowDetector forward;
        const auto fires = Play(forward, Flick({0.0f, 0.0f, -1.0f}, 3.0f), 0.8f, rate);
        Check(fires.size() == 1 && fires[0].direction == Direction::Forward, "a forward flick throws forward");
        Check(!fires.empty() && fires[0].time < 0.35f, "the throw fires during the swing");
        ThrowDetector backward;
        const auto back = Play(backward, Flick({0.0f, 0.0f, 1.0f}, 2.5f), 0.8f, rate);
        Check(back.size() == 1 && back[0].direction == Direction::Backward, "a backward flick throws backward");
    }
    // An overhand throw ends forward and down; an underhand toss forward and up.
    ThrowDetector overhand;
    Check(Play(overhand, Flick({0.0f, -0.45f, -0.89f}, 3.0f), 0.8f).size() == 1, "an overhand throw counts");
    ThrowDetector underhand;
    Check(Play(underhand, Flick({0.0f, 0.45f, -0.89f}, 3.0f), 0.8f).size() == 1, "an underhand toss counts");
}

void TestNotThrows() {
    ThrowDetector reach;
    // Reaching for the wheel, and bringing a hand back to rest.
    Check(Play(reach, [](float t) { return std::array<float, 3>{0.0f, 0.0f, t < 0.4f ? -0.9f : 0.0f}; }, 1.0f).empty(),
          "reaching for the wheel is no throw");
    ThrowDetector rest;
    Check(Play(rest, [](float t) { return std::array<float, 3>{0.0f, 0.0f, t < 0.35f ? 0.9f : 0.0f}; }, 1.0f).empty(),
          "bringing a hand back is no throw");
    ThrowDetector sideways;
    Check(Play(sideways, Flick({1.0f, 0.0f, 0.0f}, 3.0f), 0.8f).empty(), "a sideways sweep is no throw");
    ThrowDetector upward;
    Check(Play(upward, Flick({0.0f, 1.0f, 0.0f}, 3.0f), 0.8f).empty(), "an upward flick (a trick) is no throw");
    ThrowDetector steep;
    Check(Play(steep, Flick({0.0f, 0.87f, -0.5f}, 3.0f), 0.8f).empty(), "a mostly upward swing is no throw");
    ThrowDetector still;
    Check(Play(still, [](float) { return std::array<float, 3>{}; }, 1.0f).empty(), "a still hand is no throw");
    ThrowDetector untracked;
    HandSample lost{};
    lost.position = {0.0f, 0.0f, -5.0f};
    const bool first = untracked.Update(lost, 0.011f) == Direction::None;
    lost.position = {0.0f, 0.0f, 5.0f};
    Check(first && untracked.Update(lost, 0.011f) == Direction::None, "an untracked hand is no throw");
}

void TestWheel() {
    // A swing while holding the wheel, then right after letting go of it.
    ThrowDetector on_wheel;
    Check(Play(on_wheel, Flick({0.0f, 0.0f, -1.0f}, 3.0f), 0.8f, 90.0f, {-0.2f, -0.3f, -0.35f},
               [](float) { return true; })
              .empty(),
          "a hand on the wheel does not throw");
    ThrowDetector just_free;
    Check(Play(just_free, Flick({0.0f, 0.0f, -1.0f}, 3.0f, 0.1f), 0.8f, 90.0f, {-0.2f, -0.3f, -0.35f},
               [](float t) { return t < 0.05f; })
              .empty(),
          "a hand that just let go of the wheel does not throw");
    ThrowDetector free;
    Check(Play(free, Flick({0.0f, 0.0f, -1.0f}, 3.0f, 0.4f), 1.0f, 90.0f, {-0.2f, -0.3f, -0.35f},
               [](float t) { return t < 0.05f; })
              .size() == 1,
          "a hand free for a moment throws");
}

void TestJumpAndRepeats() {
    ThrowDetector jump;
    HandSample sample{};
    sample.tracked = true;
    sample.position = {-0.2f, -0.3f, -0.35f};
    bool fired = false;
    for (int frame = 0; frame < 60; ++frame) {
        if (frame == 30) {
            sample.position[2] -= 0.5f; // the pose snaps half a metre ahead
        }
        fired = fired || jump.Update(sample, 1.0f / 90.0f) != Direction::None;
    }
    Check(!fired, "a tracking jump is no throw");

    ThrowDetector long_swing;
    Check(Play(long_swing, [](float t) { return std::array<float, 3>{0.0f, 0.0f, t > 0.1f ? -2.0f : 0.0f}; }, 1.2f)
                  .size() == 1,
          "one long swing throws once");
    const auto two = [](float gap) {
        return [gap](float t) {
            const bool first = t >= 0.1f && t < 0.3f, second = t >= 0.3f + gap && t < 0.5f + gap;
            return std::array<float, 3>{0.0f, 0.0f, first || second ? -2.5f : 0.0f};
        };
    };
    ThrowDetector close;
    Check(Play(close, two(0.2f), 1.5f).size() == 1, "a second swing inside the cooldown does not throw");
    ThrowDetector apart;
    Check(Play(apart, two(0.6f), 2.0f).size() == 2, "a second swing after the cooldown throws again");
}

// Bare hands: the cameras lag, smooth and drop a hand in a fast swing.
void TestBareHands() {
    const auto none = [](float) { return false; };
    // A slower push than a controller needs: 1.4 m/s at its peak.
    ThrowDetector controller;
    Check(Play(controller, Flick({0.0f, 0.0f, -1.0f}, 1.4f), 0.8f).empty(), "a controller needs a faster swing");
    ThrowDetector bare;
    const auto push = Play(bare, Flick({0.0f, 0.0f, -1.0f}, 1.4f), 0.8f, 60.0f, {-0.2f, -0.3f, -0.35f}, none, true);
    Check(push.size() == 1 && push[0].direction == Direction::Forward, "a bare hand's slower push throws");
    Check(bare.Last().bare && bare.Last().travel >= kBareHandTuning.forward_travel, "the throw's measure");
    ThrowDetector bare_back;
    const auto back = Play(bare_back, Flick({0.0f, 0.0f, 1.0f}, 1.2f), 0.8f, 60.0f, {-0.2f, -0.3f, -0.35f}, none, true);
    Check(back.size() == 1 && back[0].direction == Direction::Backward, "a bare hand throws backward");

    // Tracking is lost for the fast part of the swing and comes back as the
    // hand stops, so only the positions either side of the gap show the throw:
    // 0.18 s for a bare hand, 0.09 s for a controller.
    for (bool is_bare : {false, true}) {
        const float speed = is_bare ? 2.0f : 3.0f, stop = is_bare ? 0.3f : 0.22f, found = is_bare ? 0.3f : 0.21f;
        ThrowDetector gap;
        const auto fires = Play(
            gap, [=](float t) { return std::array<float, 3>{0.0f, 0.0f, t >= 0.1f && t < stop ? -speed : 0.0f}; },
            0.8f, 72.0f, {-0.2f, -0.3f, -0.35f}, none, is_bare, [=](float t) { return t <= 0.12f || t > found; });
        Check(fires.size() == 1 && fires[0].direction == Direction::Forward, "a swing across a short gap throws");
        Check(gap.Last().bridged, "the throw's measure notes the gap");
    }
    // A hand the cameras lose for 0.3 s reappears 30 cm ahead: no throw.
    ThrowDetector long_gap;
    Check(Play(long_gap, [](float t) { return std::array<float, 3>{0.0f, 0.0f, t > 0.3f && t < 0.6f ? -1.0f : 0.0f}; },
               1.2f, 72.0f, {-0.2f, -0.3f, -0.35f}, none, true, [](float t) { return t < 0.3f || t > 0.6f; })
              .empty(),
          "a hand lost for long is no throw");
    // A slow drift the cameras lose briefly: 0.6 m/s across a 0.18 s gap.
    ThrowDetector drift;
    Check(Play(drift, [](float) { return std::array<float, 3>{0.0f, 0.0f, -0.6f}; }, 1.2f, 72.0f,
               {-0.2f, -0.3f, 0.0f}, none, true, [](float t) { return t < 0.5f || t > 0.68f; })
              .empty(),
          "a slow drift across a gap is no throw");
    // Reaching for the wheel with a bare hand.
    ThrowDetector reach;
    Check(Play(reach, [](float t) { return std::array<float, 3>{0.0f, 0.0f, t < 0.4f ? -0.8f : 0.0f}; }, 1.0f, 60.0f,
               {-0.2f, -0.3f, -0.35f}, none, true)
              .empty(),
          "a bare hand reaching for the wheel is no throw");
}

struct Frame {
    float stick_y;
    float trigger;
};

// Runs the sequence at 90 Hz with the player's own trigger from `player(t)`.
std::vector<Frame> Sequence(Direction direction, const std::function<float(float)>& player, float duration,
                            ThrowSequence& sequence) {
    std::vector<Frame> frames;
    sequence.Start(direction);
    const float dt = 1.0f / 90.0f;
    for (float t = 0.0f; t < duration; t += dt) {
        wii_remote::HandInputs left{};
        left.stick_x = 0.6f;
        left.trigger = player(t);
        sequence.Apply(left, dt);
        Check(left.stick_x == 0.6f, "the throw leaves the steering alone");
        frames.push_back({left.stick_y, left.trigger});
    }
    return frames;
}

void TestSequence() {
    ThrowSequence sequence;
    const auto frames = Sequence(Direction::Forward, [](float) { return 0.0f; }, 0.4f, sequence);
    // 90 Hz: aim for frames 0-4 (0.05 s), press 5-13, release 14-22, then done.
    Check(frames[0].stick_y == 1.0f && frames[0].trigger == 0.0f, "the stick aims before the button");
    Check(frames[4].trigger == 0.0f && frames[5].trigger == 1.0f, "then the button is pressed");
    Check(frames[13].trigger == 1.0f && frames[14].trigger == 0.0f, "then released");
    Check(frames[14].stick_y == 1.0f && frames[21].stick_y == 1.0f, "with the stick still aimed");
    Check(frames[24].stick_y == 0.0f && !sequence.Active(), "then the stick is the player's again");
    ThrowSequence back;
    Check(Sequence(Direction::Backward, [](float) { return 0.0f; }, 0.1f, back)[0].stick_y == -1.0f,
          "a backward throw pulls the stick back");

    // The player was trailing the item with the button held: the throw lets it
    // go, and the button stays released until the player lets go of it.
    ThrowSequence trailed;
    const auto held = Sequence(Direction::Forward, [](float t) { return t < 0.5f ? 1.0f : (t < 0.6f ? 0.0f : 1.0f); },
                               0.8f, trailed);
    Check(held[2].trigger == 1.0f && held[10].trigger == 1.0f, "a held button stays pressed until the release");
    Check(held[14].trigger == 0.0f && held[30].trigger == 0.0f, "the throw releases a held button");
    Check(held[44].trigger == 0.0f, "a button still held after the throw is ignored");
    Check(held[60].trigger == 1.0f, "a new press after letting go uses the next item");
}

// KartFrame: the settable controller tuning, and the throw played on the GameCube port.
void TestTuning() {
    // A 1.4 m/s push is too slow for the Quest's tuning; a lighter one takes it.
    ThrowDetector lighter;
    Tuning light = kControllerTuning;
    light.forward_travel = 0.10f;
    light.backward_travel = 0.09f;
    light.moving_speed = 0.3f;
    lighter.SetTuning(light);
    Check(Play(lighter, Flick({0.0f, 0.0f, -1.0f}, 1.4f), 0.8f).size() == 1, "a lighter tuning takes a slower push");
    lighter.Reset();
    Check(lighter.ControllerTuning().forward_travel == 0.10f, "Reset keeps the tuning");
    Check(Play(lighter, Flick({0.0f, 0.0f, 1.0f}, 1.4f), 0.8f).size() == 1, "and throws backward with it");
    // A heavier tuning ignores the default's forward flick.
    ThrowDetector heavier;
    Tuning heavy = kControllerTuning;
    heavy.forward_travel = 0.40f;
    heavier.SetTuning(heavy);
    Check(Play(heavier, Flick({0.0f, 0.0f, -1.0f}, 3.0f), 0.8f).empty(), "a heavier tuning needs a longer swing");
}

void TestPort() {
    ThrowSequence sequence;
    sequence.Start(Direction::Backward);
    const float dt = 1.0f / 120.0f;
    std::vector<std::pair<float, float>> frames;
    for (float t = 0.0f; t < 0.4f; t += dt) {
        float stick_y = 0.3f, item = 0.0f;
        ApplyToPort(sequence, stick_y, item, dt);
        frames.push_back({stick_y, item});
    }
    // 120 Hz: aim for frames 0-5 (0.05 s), press 6-17, release 18-29, then the player's stick.
    Check(frames[0].first == -1.0f && frames[0].second == 0.0f, "port: the stick aims back before the button");
    Check(frames[5].second == 0.0f && frames[7].second > 0.5f, "port: then the item button (L) is pressed");
    Check(frames[16].second > 0.5f && frames[19].second == 0.0f, "port: then released");
    Check(frames[19].first == -1.0f && frames[27].first == -1.0f, "port: with the stick still aimed");
    Check(frames[32].first == 0.3f && !sequence.Active(), "port: then the stick is the player's again");
    // The player holding the item button through a throw: released, then ignored until let go.
    ThrowSequence held;
    held.Start(Direction::Forward);
    float last_item = 1.0f;
    for (float t = 0.0f; t < 0.4f; t += dt) {
        float stick_y = 0.0f, item = 1.0f;
        ApplyToPort(held, stick_y, item, dt);
        last_item = item;
    }
    Check(last_item == 0.0f, "port: a held item button is ignored after the throw");
    float stick_y = 0.0f, item = 0.0f;
    ApplyToPort(held, stick_y, item, dt);
    item = 1.0f;
    ApplyToPort(held, stick_y, item, dt);
    Check(item == 1.0f && stick_y == 0.0f, "port: a new press after letting go goes through");
}

} // namespace

int main() {
    TestTuning();
    TestPort();
    TestThrows();
    TestNotThrows();
    TestWheel();
    TestJumpAndRepeats();
    TestBareHands();
    TestSequence();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mkw_vr_item_throw_tests passed\n";
    return 0;
}
