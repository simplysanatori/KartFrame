// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GameCube pad's rumble motor on the VR controllers, for the "gamepad" controller mode
// (KartFrame). The game drives the motor through PADControlMotor as on or off only, and shapes its
// patterns by switching it every few game frames. The controllers take an amplitude and a duration,
// so the pacing thread samples the motor once per XR frame and plays the share of recent samples it
// was on as the amplitude: a steady rumble at full strength, a pattern switching every other frame at
// about half. While the motor is on, a short pulse is sent every frame (it dies out by itself if the
// stop never arrives); while it is off the last pulse runs out; once no sample in the window was on,
// the haptics are stopped. Nothing here depends on OpenXR (tests/vr_gamepad_rumble_tests.cpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace mkw::vr::gamepad_rumble {

class Shaper {
public:
    static constexpr size_t kWindow = 12;          // samples: 100 ms at 120 Hz
    static constexpr int64_t kPulseNs = 50'000'000; // re-sent every frame while on
    static constexpr float kFloor = 0.4f;           // share of the base amplitude for a bare blip

    enum class Action : uint8_t { None, Pulse, Stop };
    struct Output {
        Action action = Action::None;
        float amplitude = 0.0f;
        int64_t duration_ns = 0;
    };

    // `base`: the amplitude of a steady rumble, 0..1.
    explicit Shaper(float base = 0.7f) noexcept { SetBase(base); }
    void SetBase(float base) noexcept { m_base = std::clamp(base, 0.0f, 1.0f); }
    float Base() const noexcept { return m_base; }

    // One XR frame: whether the game has the motor on now.
    Output Update(bool on) noexcept {
        if (m_history[m_next]) --m_on;
        m_history[m_next] = on;
        if (on) ++m_on;
        m_next = (m_next + 1) % kWindow;
        Output out;
        if (m_on == 0 || m_base <= 0.0f) {
            if (m_playing) out.action = Action::Stop;
            m_playing = false;
            return out;
        }
        if (!on) return out; // the last pulse runs out
        const float duty = static_cast<float>(m_on) / static_cast<float>(kWindow);
        out.action = Action::Pulse;
        out.amplitude = m_base * (kFloor + (1.0f - kFloor) * duty);
        out.duration_ns = kPulseNs;
        m_playing = true;
        return out;
    }

    bool Playing() const noexcept { return m_playing; }
    void Reset() noexcept {
        m_history = {};
        m_next = m_on = 0;
        m_playing = false;
    }

private:
    std::array<bool, kWindow> m_history{};
    size_t m_next = 0, m_on = 0;
    bool m_playing = false;
    float m_base = 0.7f;
};

} // namespace mkw::vr::gamepad_rumble
