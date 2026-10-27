// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GameCube motor on the VR controllers (vr/openxr_gamepad_rumble.h), without a headset.

#include "vr/openxr_gamepad_rumble.h"

#include <cmath>
#include <iostream>

namespace {

using mkw::vr::gamepad_rumble::Shaper;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << what << '\n';
    }
}

void TestSteady() {
    Shaper shaper(0.8f);
    Check(shaper.Update(false).action == Shaper::Action::None, "an idle motor does nothing");
    const auto first = shaper.Update(true);
    Check(first.action == Shaper::Action::Pulse && first.duration_ns == Shaper::kPulseNs, "the motor on pulses");
    Check(first.amplitude > 0.3f && first.amplitude < 0.5f, "a first sample starts near the floor");
    Shaper::Output out;
    for (size_t i = 0; i < Shaper::kWindow; ++i) out = shaper.Update(true);
    Check(out.action == Shaper::Action::Pulse && std::fabs(out.amplitude - 0.8f) < 1e-5f, "a steady rumble at the base");
    // Off: the pulse runs out, then a stop once the window is clear.
    int stops = 0, pulses = 0;
    for (size_t i = 0; i < Shaper::kWindow + 5; ++i) {
        const auto o = shaper.Update(false);
        stops += o.action == Shaper::Action::Stop;
        pulses += o.action == Shaper::Action::Pulse;
    }
    Check(stops == 1 && pulses == 0 && !shaper.Playing(), "one stop after the motor goes off");
}

void TestPattern() {
    // On every other sample: about the floor plus half the rest.
    Shaper shaper(1.0f);
    float amplitude = 0.0f;
    int stops = 0;
    for (int i = 0; i < 48; ++i) {
        const auto o = shaper.Update(i % 2 == 0);
        if (o.action == Shaper::Action::Pulse) amplitude = o.amplitude;
        stops += o.action == Shaper::Action::Stop;
    }
    Check(std::fabs(amplitude - 0.7f) < 0.06f, "a half-duty pattern at about 0.7 of the base");
    Check(stops == 0, "a pattern never stops in between");
}

void TestOff() {
    Shaper shaper(0.0f);
    Check(shaper.Update(true).action == Shaper::Action::None, "base 0 never pulses");
    Shaper reset(0.5f);
    reset.Update(true);
    reset.Reset();
    Check(!reset.Playing() && reset.Update(false).action == Shaper::Action::None, "reset forgets");
    Check(Shaper(3.0f).Base() == 1.0f && Shaper(-1.0f).Base() == 0.0f, "the base is clamped");
}

} // namespace

int main() {
    TestSteady();
    TestPattern();
    TestOff();
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "mkw_vr_gamepad_rumble_tests passed\n";
    return 0;
}
