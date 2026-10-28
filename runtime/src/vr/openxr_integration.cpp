// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "vr/openxr_integration.h"
#include "vr/menu_anchor.h"

#include "runtime_config.h"
#include "gx_thread.h"
#include "runtime_log.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_policy.h"
#include "vr/mkw_vr_instrumentation.h"
#include "vr/openxr_diagnostics.h"
#include <aurora/gfx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>
#include <string>
#include <string_view>
#include <thread>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(MKW_ENABLE_OPENXR)
#include "vr/openxr_backend.h"
#include "vr/openxr_hand_mesh.h"
#include "vr/openxr_input.h"
#include "vr/adaptive_resolution.h"
#include "vr/openxr_runtime.h"
#if defined(_WIN32)
#include "vr/openxr_windows.h"
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#elif defined(__ANDROID__)
#include "vr/openxr_android.h"
#include "vr/openxr_vulkan.h"
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#elif defined(__linux__)
// Desktop Linux: the PC shared-device Vulkan bridge, driven by the native SteamVR OpenXR runtime.
#define MKW_OPENXR_LINUX_DESKTOP 1
#include "vr/openxr_vulkan_win32.h"
#include <time.h>
#include <unistd.h>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>
#define MKW_OPENXR_GRAPHICS_BACKEND 1
#else
#define MKW_OPENXR_GRAPHICS_BACKEND 0
#endif
#else
#define MKW_OPENXR_GRAPHICS_BACKEND 0
#endif

namespace mkw::vr {
namespace {

inline constexpr float kDegreesToRadians = 0.01745329252f;

// This is the VR build, so the headset path is what an unconfigured installation
// starts in, on the headset and on the desktop alike. Nothing is lost by it:
// with vr.required false, a missing runtime or headset falls back to desktop.
inline constexpr bool kVrEnabledDefault = true;

void ConfigurePolicy(bool enabled) noexcept {
    MkwVRPolicyReset();
    MkwVRPolicyConfig config{};
    config.enabled = enabled;
    config.immersive_races = !RuntimeConfigFile::VrFlatScreen();
    config.world_units_per_meter = RuntimeConfigFile::VrWorldUnitsPerMeter(500.0f);
    config.hud_distance_meters = RuntimeConfigFile::VrHudDistanceMeters(2.0f);
    config.hud_width_meters = RuntimeConfigFile::VrHudWidthMeters(2.4f);
    config.first_person_units_per_meter = RuntimeConfigFile::VrFirstPersonUnitsPerMeter();
    MkwVRPolicyConfigure(config);
    MkwVRInstrumentationInitialize();
    MkwVRFirstPersonApplyConfiguredSettings();
}

#if MKW_OPENXR_GRAPHICS_BACKEND

#if defined(_WIN32)
using GraphicsBackend = OpenXRWindowsBackend;

#elif defined(MKW_OPENXR_LINUX_DESKTOP)
using GraphicsBackend = OpenXRWindowsVulkanBackend;
inline constexpr const char* kGraphicsBackendName = "Vulkan";
// Sets rendered ahead (lane K): the default pipelined pacing on the Linux Vulkan backend.
#define KARTFRAME_AHEAD_PACING 1
#else
using GraphicsBackend = OpenXRVulkanBackend;
inline constexpr const char* kGraphicsBackendName = "Vulkan";
#endif
#ifndef KARTFRAME_AHEAD_PACING
#define KARTFRAME_AHEAD_PACING 0
#endif

// Whether the immersive window's eyes can be aimed through the window, so that only the window is
// rendered: the backend has to show just the part of each eye image they fill. The Quest's shared
// buffers and projection layer do; the PC backends copy whole eyes, so there the window's eyes stay
// full size and are only masked.
#if defined(__ANDROID__)
inline constexpr bool kWindowShapedEyesSupported = true;
#else
inline constexpr bool kWindowShapedEyesSupported = false;
#endif

// Whether menus are drawn into the eye images (projection layer) rather than shown on a quad layer.
// On Android always: under Lepton, SteamVR turns a quad layer into a slow submission, so quality 0
// only swaps Aurora's raymarched menu background for a plain one, which the GPU draws for free.
// The Frame (native SteamVR, Linux desktop) takes the PC rule: with menu_shader_quality 0 the game
// screen is a quad layer fixed in the room (the compositor reprojects it), so a menu set costs a
// copy instead of two full eyes. Projected menus there cost 5 to 7 ms a set in the headset (H1)
// and the screen flickered; the introduction stays projected for its controller models.
inline bool OpenXRProjectedMenus() noexcept {
#if defined(__ANDROID__)
    return true;
#else
    return RuntimeConfigFile::Get().vrMenuShaderQuality>0 || OpenXRIntroductionActive();
#endif
}

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

Quaternion Normalize(Quaternion value) noexcept {
    const float length_squared = value.x * value.x + value.y * value.y +
                                 value.z * value.z + value.w * value.w;
    if (!(length_squared > 1.0e-12f)) {
        return {};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    value.x *= inverse_length;
    value.y *= inverse_length;
    value.z *= inverse_length;
    value.w *= inverse_length;
    return value;
}

Quaternion Conjugate(Quaternion value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& left, const Quaternion& right) noexcept {
    return Normalize({
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

std::array<float, 3> Rotate(const Quaternion& q, const std::array<float, 3>& value) noexcept {
    // Expanded q * [v,0] * conjugate(q), avoiding two temporary normalizations.
    const float tx = 2.0f * (q.y * value[2] - q.z * value[1]);
    const float ty = 2.0f * (q.z * value[0] - q.x * value[2]);
    const float tz = 2.0f * (q.x * value[1] - q.y * value[0]);
    return {
        value[0] + q.w * tx + (q.y * tz - q.z * ty),
        value[1] + q.w * ty + (q.z * tx - q.x * tz),
        value[2] + q.w * tz + (q.x * ty - q.y * tx),
    };
}

void RotationMatrix(const Quaternion& value, float matrix[9]) noexcept {
    const Quaternion q = Normalize(value);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    matrix[0] = 1.0f - 2.0f * (yy + zz);
    matrix[1] = 2.0f * (xy - wz);
    matrix[2] = 2.0f * (xz + wy);
    matrix[3] = 2.0f * (xy + wz);
    matrix[4] = 1.0f - 2.0f * (xx + zz);
    matrix[5] = 2.0f * (yz - wx);
    matrix[6] = 2.0f * (xz - wy);
    matrix[7] = 2.0f * (yz + wx);
    matrix[8] = 1.0f - 2.0f * (xx + yy);
}

// Midpoint between the eyes: the head position the tracking origin is latched
// to. Callers check XR_VIEW_STATE_POSITION_VALID_BIT first.
std::array<float, 3> CenterPosition(const OpenXRFrame& frame) noexcept {
    const auto& left = frame.views[0].pose;
    const auto& right = frame.views[1].pose;
    return {
        (left.position.x + right.position.x) * 0.5f,
        (left.position.y + right.position.y) * 0.5f,
        (left.position.z + right.position.z) * 0.5f,
    };
}

// Places an upright screen `distance` metres ahead of the head. Only the
// head's yaw is used, so the screen is never pitched or rolled by whatever the
// player's head happened to be doing when it was anchored.
XrPosef ScreenPoseAhead(const OpenXRFrame& frame, float distance) noexcept {
    const auto& q = frame.views[0].pose.orientation;
    const auto screen=UprightMenuAhead({q.x,q.y,q.z,q.w},CenterPosition(frame),distance);
    XrPosef pose{};
    pose.orientation = {screen.orientation[0],screen.orientation[1],screen.orientation[2],screen.orientation[3]};
    pose.position = {screen.position[0],screen.position[1],screen.position[2]};
    return pose;
}

// ---- Eye offsets (lane M) ----
//
// The eighth headset session: with sets ahead and every frame ended at the wake-up, the image still
// zigzagged on head turns, mostly in the right eye, with the kart standing still too. SteamVR on the
// Frame has gaze-dependent eye positions on (steamvr.gazeDepEyeToHeadEnable): each eye's pose from
// xrLocateViews moves with the tracked gaze, and the eyes counter-rotate during head turns. If SteamVR
// composes an eye with a newer gaze sample than the one it gave us ~42 ms earlier, each eye's image is
// corrected by a different amount each refresh. KARTFRAME_EYE_OFFSETS=fixed renders and submits each eye
// at the head (VIEW space) pose composed with a fixed eye-to-head offset (the median of the first 120
// focused locates), with the field of view captured then too, so gaze motion cannot reach the images;
// =located (default) keeps the runtime's per-locate eye poses. The submitted poses are the rendered
// ones either way (the frame's views are replaced before anything uses them).
//
// Logging: "vr eyes:" once a second (with KARTFRAME_FRAME_LOG=1 or `fixed`): per eye, the change of
// the LOCATED eye-to-head offset from one locate to the next, p50/p99 in mm and degrees, the largest
// field-of-view change, and the head's angular speed. KARTFRAME_FRAME_LOG=1 adds one XRE line per locate:
//   XRE x=<display time, ms since the first locate> m=<L located views used, F fixed, C fixed but still
//   capturing> h=<head position mm> q=<head orientation x,y,z,w> L=<left eye offset in the head's frame,
//   mm>/<its rotation, degrees, as a rotation vector> dL=<offset change since the last locate, mm>/<deg>
//   fL=<largest fov angle change, deg> R=... dR=... fR=...
class EyeOffsets {
public:
    static constexpr size_t kCaptureLocates = 120;

    void Configure() {
        const char* mode = std::getenv("KARTFRAME_EYE_OFFSETS");
        fixed_ = mode != nullptr && std::string_view(mode) == "fixed";
        const char* log = std::getenv("KARTFRAME_FRAME_LOG");
        frame_log_ = log != nullptr && log[0] == '1';
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR eye offsets: "
                               << (fixed_ ? "fixed (head pose with the median eye-to-head offsets of the first "
                                            "120 focused locates)"
                                          : "located (the runtime's per-locate eye poses)")
                               << (Active() ? "; logged per second (vr eyes)" : "") << std::endl;
    }
    bool Active() const { return fixed_ || frame_log_; }

    void OnLocate(XrTime display_time, const XrSpaceLocation& head, bool focused,
                  std::array<XrView, kOpenXREyeCount>& views) {
        std::lock_guard lock(mutex_);
        constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((head.locationFlags & kValid) != kValid) {
            ++second_.fallback;
            return;
        }
        const Q qh = ToQ(head.pose.orientation);
        const V ph = ToV(head.pose.position);
        Eye now[kOpenXREyeCount];
        for (uint32_t e = 0; e < kOpenXREyeCount; ++e) {
            now[e].pos = Rotate(Conj(qh), Sub(ToV(views[e].pose.position), ph));
            now[e].rot = Canon(Mul(Conj(qh), ToQ(views[e].pose.orientation)));
            now[e].fov = views[e].fov;
        }
        // Change since the last locate, per eye; the head's angular speed.
        double dpos[kOpenXREyeCount] = {}, drot[kOpenXREyeCount] = {}, dfov[kOpenXREyeCount] = {};
        if (have_last_) {
            for (uint32_t e = 0; e < kOpenXREyeCount; ++e) {
                dpos[e] = Len(Sub(now[e].pos, last_[e].pos)) * 1000.0;
                drot[e] = AngleDeg(Mul(Conj(last_[e].rot), now[e].rot));
                dfov[e] = FovChangeDeg(now[e].fov, last_[e].fov);
                second_.dpos[e].push_back(dpos[e]);
                second_.drot[e].push_back(drot[e]);
                second_.dfov[e] = std::max(second_.dfov[e], dfov[e]);
            }
            const double dt = static_cast<double>(display_time - last_time_) / 1e9;
            if (dt > 1e-4) second_.head_dps.push_back(AngleDeg(Mul(Conj(last_head_), qh)) / dt);
        }
        for (uint32_t e = 0; e < kOpenXREyeCount; ++e) last_[e] = now[e];
        last_head_ = qh;
        last_time_ = display_time;
        have_last_ = true;
        ++second_.locates;

        char mode = 'L';
        if (fixed_) {
            if (!captured_) {
                mode = 'C';
                if (focused) {
                    for (uint32_t e = 0; e < kOpenXREyeCount; ++e) capture_[e].push_back(now[e]);
                    if (capture_[0].size() >= kCaptureLocates) FinishCapture();
                }
            }
            if (captured_) {
                mode = 'F';
                for (uint32_t e = 0; e < kOpenXREyeCount; ++e) {
                    const Q q = Normalize(Mul(qh, fixed_eye_[e].rot));
                    const V p = Add(ph, Rotate(qh, fixed_eye_[e].pos));
                    views[e].pose.orientation = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z),
                                                 static_cast<float>(q.w)};
                    views[e].pose.position = {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
                    views[e].fov = fixed_eye_[e].fov;
                }
                ++second_.fixed;
            }
        }

        if (frame_log_) {
            if (first_time_ == 0) first_time_ = display_time;
            char line[400];
            const V l = now[0].pos, r = now[1].pos;
            const V lr = RotVecDeg(now[0].rot), rr = RotVecDeg(now[1].rot);
            std::snprintf(line, sizeof(line),
                          "XRE x=%.3f m=%c h=%.1f,%.1f,%.1f q=%.5f,%.5f,%.5f,%.5f L=%.2f,%.2f,%.2f/%.3f,%.3f,%.3f "
                          "dL=%.3f/%.4f fL=%.4f R=%.2f,%.2f,%.2f/%.3f,%.3f,%.3f dR=%.3f/%.4f fR=%.4f",
                          static_cast<double>(display_time - first_time_) / 1e6, mode, ph.x * 1000.0, ph.y * 1000.0,
                          ph.z * 1000.0, qh.x, qh.y, qh.z, qh.w, l.x * 1000.0, l.y * 1000.0, l.z * 1000.0, lr.x, lr.y,
                          lr.z, dpos[0], drot[0], dfov[0], r.x * 1000.0, r.y * 1000.0, r.z * 1000.0, rr.x, rr.y, rr.z,
                          dpos[1], drot[1], dfov[1]);
            RT_LOG(RT_TAG_RUNTIME) << line << std::endl;
        }

        const auto wall = std::chrono::steady_clock::now();
        if (second_start_ == std::chrono::steady_clock::time_point{}) second_start_ = wall;
        if (wall - second_start_ >= std::chrono::seconds(1)) {
            LogSecond(wall);
            second_ = {};
            second_start_ = wall;
        }
    }

private:
    struct V { double x = 0, y = 0, z = 0; };
    struct Q { double x = 0, y = 0, z = 0, w = 1; };
    struct Eye { V pos; Q rot; XrFovf fov{}; };
    struct Second {
        std::vector<double> dpos[kOpenXREyeCount], drot[kOpenXREyeCount], head_dps;
        double dfov[kOpenXREyeCount] = {};
        uint32_t locates = 0, fixed = 0, fallback = 0;
    };

    static V ToV(const XrVector3f& v) { return {v.x, v.y, v.z}; }
    static Q ToQ(const XrQuaternionf& q) { return {q.x, q.y, q.z, q.w}; }
    static V Sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    static V Add(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    static double Len(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
    static Q Conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
    static Q Mul(Q a, Q b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    static Q Normalize(Q q) {
        const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return n > 0 ? Q{q.x / n, q.y / n, q.z / n, q.w / n} : Q{};
    }
    static Q Canon(Q q) { q = Normalize(q); return q.w < 0 ? Q{-q.x, -q.y, -q.z, -q.w} : q; }
    static V Rotate(Q q, V v) {
        // v + 2w(u x v) + 2 u x (u x v), u = q's vector part
        const V u{q.x, q.y, q.z};
        const V c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        const V cc{u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x};
        return {v.x + 2.0 * (q.w * c.x + cc.x), v.y + 2.0 * (q.w * c.y + cc.y), v.z + 2.0 * (q.w * c.z + cc.z)};
    }
    static double AngleDeg(Q q) {
        q = Canon(q);
        return 2.0 * std::atan2(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z), q.w) * 180.0 / 3.14159265358979323846;
    }
    static V RotVecDeg(Q q) {
        q = Canon(q);
        const double s = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        if (s < 1e-12) return {};
        const double k = 2.0 * std::atan2(s, q.w) * 180.0 / 3.14159265358979323846 / s;
        return {q.x * k, q.y * k, q.z * k};
    }
    static double FovChangeDeg(const XrFovf& a, const XrFovf& b) {
        const double m = std::max({std::fabs(a.angleLeft - b.angleLeft), std::fabs(a.angleRight - b.angleRight),
                                   std::fabs(a.angleUp - b.angleUp), std::fabs(a.angleDown - b.angleDown)});
        return m * 180.0 / 3.14159265358979323846;
    }
    static double Percentile(std::vector<double>& v, double p) {
        if (v.empty()) return 0.0;
        const size_t k = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
        return v[k];
    }
    static double Median(std::vector<double> v) { return Percentile(v, 0.5); }

    void FinishCapture() {
        for (uint32_t e = 0; e < kOpenXREyeCount; ++e) {
            auto& s = capture_[e];
            auto med = [&](auto get) {
                std::vector<double> v;
                v.reserve(s.size());
                for (const auto& x : s) v.push_back(get(x));
                return Median(std::move(v));
            };
            Eye f;
            f.pos = {med([](const Eye& x) { return x.pos.x; }), med([](const Eye& x) { return x.pos.y; }),
                     med([](const Eye& x) { return x.pos.z; })};
            f.rot = Canon({med([](const Eye& x) { return x.rot.x; }), med([](const Eye& x) { return x.rot.y; }),
                           med([](const Eye& x) { return x.rot.z; }), med([](const Eye& x) { return x.rot.w; })});
            f.fov.angleLeft = static_cast<float>(med([](const Eye& x) { return static_cast<double>(x.fov.angleLeft); }));
            f.fov.angleRight = static_cast<float>(med([](const Eye& x) { return static_cast<double>(x.fov.angleRight); }));
            f.fov.angleUp = static_cast<float>(med([](const Eye& x) { return static_cast<double>(x.fov.angleUp); }));
            f.fov.angleDown = static_cast<float>(med([](const Eye& x) { return static_cast<double>(x.fov.angleDown); }));
            // How far the captured locates strayed from the median (gaze, noise): p50/max, mm and deg.
            std::vector<double> dp, dr;
            for (const auto& x : s) {
                dp.push_back(Len(Sub(x.pos, f.pos)) * 1000.0);
                dr.push_back(AngleDeg(Mul(Conj(f.rot), x.rot)));
            }
            spread_[e][0] = Median(dp);
            spread_[e][1] = *std::max_element(dp.begin(), dp.end());
            spread_[e][2] = Median(dr);
            spread_[e][3] = *std::max_element(dr.begin(), dr.end());
            fixed_eye_[e] = f;
            s.clear();
            s.shrink_to_fit();
        }
        captured_ = true;
        constexpr double kDeg = 180.0 / 3.14159265358979323846;
        const V l = fixed_eye_[0].pos, r = fixed_eye_[1].pos;
        const V mid{(l.x + r.x) * 500.0, (l.y + r.y) * 500.0, (l.z + r.z) * 500.0};
        char line[900];
        int n = std::snprintf(line, sizeof(line), "OpenXR eye offsets fixed (median of %zu focused locates): IPD %.2f mm, "
                              "eye midpoint from the head %.2f,%.2f,%.2f mm, |left x| - |right x| %.2f mm",
                              kCaptureLocates, Len(Sub(r, l)) * 1000.0, mid.x, mid.y, mid.z,
                              (std::fabs(l.x) - std::fabs(r.x)) * 1000.0);
        for (uint32_t e = 0; e < kOpenXREyeCount && n > 0 && static_cast<size_t>(n) < sizeof(line); ++e) {
            const auto& f = fixed_eye_[e];
            const V rv = RotVecDeg(f.rot);
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                               "; %s: %.2f,%.2f,%.2f mm, rotation %.3f,%.3f,%.3f deg, fov l/r/u/d %.2f/%.2f/%.2f/%.2f deg, "
                               "captured spread mm p50/max %.2f/%.2f deg %.3f/%.3f",
                               e == 0 ? "left" : "right", f.pos.x * 1000.0, f.pos.y * 1000.0, f.pos.z * 1000.0, rv.x, rv.y,
                               rv.z, f.fov.angleLeft * kDeg, f.fov.angleRight * kDeg, f.fov.angleUp * kDeg,
                               f.fov.angleDown * kDeg, spread_[e][0], spread_[e][1], spread_[e][2], spread_[e][3]);
        }
        RT_LOG(RT_TAG_RUNTIME) << line << std::endl;
    }

    void LogSecond(std::chrono::steady_clock::time_point wall) {
        char line[400];
        const double mono = std::chrono::duration<double>(wall.time_since_epoch()).count();
        std::snprintf(line, sizeof(line),
                      "vr eyes: mono=%.3f mode=%s locates=%u fixed=%u nohead=%u dpos_mm L=%.3f/%.3f R=%.3f/%.3f "
                      "drot_deg L=%.4f/%.4f R=%.4f/%.4f dfov_deg L=%.4f R=%.4f head_dps=%.1f/%.1f",
                      mono, fixed_ ? (captured_ ? "fixed" : "fixed-capturing") : "located", second_.locates, second_.fixed,
                      second_.fallback, Percentile(second_.dpos[0], 0.5), Percentile(second_.dpos[0], 0.99),
                      Percentile(second_.dpos[1], 0.5), Percentile(second_.dpos[1], 0.99), Percentile(second_.drot[0], 0.5),
                      Percentile(second_.drot[0], 0.99), Percentile(second_.drot[1], 0.5), Percentile(second_.drot[1], 0.99),
                      second_.dfov[0], second_.dfov[1], Percentile(second_.head_dps, 0.5), Percentile(second_.head_dps, 0.99));
        RT_LOG(RT_TAG_RUNTIME) << line << std::endl;
    }

    std::mutex mutex_;
    bool fixed_ = false;
    bool frame_log_ = false;
    bool captured_ = false;
    bool have_last_ = false;
    Eye last_[kOpenXREyeCount];
    Q last_head_;
    XrTime last_time_ = 0;
    XrTime first_time_ = 0;
    std::vector<Eye> capture_[kOpenXREyeCount];
    Eye fixed_eye_[kOpenXREyeCount];
    double spread_[kOpenXREyeCount][4] = {};
    Second second_;
    std::chrono::steady_clock::time_point second_start_{};
};

// Hand steering draws the player's hands, in the runtime's own hand mesh where
// it offers one (XR_FB_hand_tracking_mesh). Only asked for when hand steering
// is on at launch; turning it on later uses the procedural gloves.
void AddHandMeshExtensions(OpenXRConfig& config) {
    for(const char* extension:{"XR_EXT_samsung_odyssey_controller",
        "XR_HTC_vive_cosmos_controller_interaction","XR_HTC_vive_focus3_controller_interaction",
        "XR_FB_touch_controller_pro","XR_META_touch_controller_plus","XR_BD_controller_interaction"})
        config.optional_extensions.emplace_back(extension);
    if (RuntimeConfigFile::VrHandSteering()) {
        config.optional_extensions.push_back("XR_EXT_hand_tracking");
        config.optional_extensions.push_back("XR_FB_hand_tracking_mesh");
    }
}

void IdentityEye(AuroraStereoEye& eye) noexcept {
    std::fill(std::begin(eye.projection), std::end(eye.projection), 0.0f);
    eye.projection[0] = 1.0f;
    eye.projection[5] = 1.0f;
    eye.projection[10] = 1.0f;
    eye.projection[15] = 1.0f;
    std::fill(std::begin(eye.viewFromCenter), std::end(eye.viewFromCenter), 0.0f);
    eye.viewFromCenter[0] = 1.0f;
    eye.viewFromCenter[5] = 1.0f;
    eye.viewFromCenter[10] = 1.0f;
}

void ProjectionFromFov(const XrFovf& fov, float output[16]) noexcept {
    const float left = std::tan(fov.angleLeft);
    const float right = std::tan(fov.angleRight);
    const float down = std::tan(fov.angleDown);
    const float up = std::tan(fov.angleUp);
    const float inverse_width = 1.0f / (right - left);
    const float inverse_height = 1.0f / (up - down);
    std::fill(output, output + 16, 0.0f);
    output[0] = 2.0f * inverse_width;
    output[2] = (right + left) * inverse_width;
    output[5] = 2.0f * inverse_height;
    output[6] = (up + down) * inverse_height;
}

// The base is a position and nothing else. OpenXR keeps its reference spaces
// gravity-aligned, so handing the headset's rotation to the game camera as-is
// leaves the game's horizon level and its forward fixed to the reference space.
// Composing a latched head orientation in here instead would bake that instant's
// pitch and roll into the neutral and tilt the horizon for the rest of the session.
//
// lean_back_radians is the one deliberate exception: a fixed pitch of the game
// camera about the reference space's right axis, for a player sitting reclined.
// It multiplies in on the right, so it turns the world before the head rotation
// rather than after it, which is what makes it cancel a reclined head exactly
// and, when you then look sideways, roll the view the way a real recline would.
void ViewFromBase(const XrPosef& eye_pose, const std::array<float, 3>& base_position,
                  bool position_valid, float units_per_meter, float lean_back_radians,
                  float output[12]) noexcept {
    const Quaternion eye = Normalize({eye_pose.orientation.x, eye_pose.orientation.y,
                                      eye_pose.orientation.z, eye_pose.orientation.w});
    const Quaternion inverse_eye = Conjugate(eye);
    float rotation[9];
    if (lean_back_radians == 0.0f) {
        RotationMatrix(inverse_eye, rotation);
    } else {
        const float half_angle = 0.5f * lean_back_radians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        RotationMatrix(Multiply(inverse_eye, lean), rotation);
    }

    std::array<float, 3> translation{};
    if (position_valid) {
        const std::array<float, 3> base_to_eye{
            base_position[0] - eye_pose.position.x,
            base_position[1] - eye_pose.position.y,
            base_position[2] - eye_pose.position.z,
        };
        translation = Rotate(inverse_eye, base_to_eye);
    }
    output[0] = rotation[0];
    output[1] = rotation[1];
    output[2] = rotation[2];
    output[3] = translation[0] * units_per_meter;
    output[4] = rotation[3];
    output[5] = rotation[4];
    output[6] = rotation[5];
    output[7] = translation[1] * units_per_meter;
    output[8] = rotation[6];
    output[9] = rotation[7];
    output[10] = rotation[8];
    output[11] = translation[2] * units_per_meter;
}

// Distinct names from openxr_runtime.cpp's helpers: both files can share a
// unity-build translation unit and the same anonymous namespace.
const char* DiagnosticSpaceName(XrReferenceSpaceType type) noexcept {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "VIEW";
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "LOCAL";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "STAGE";
    default:
        return "OTHER";
    }
}

const char* DiagnosticBlendModeName(XrEnvironmentBlendMode mode) noexcept {
    switch (mode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return "OPAQUE";
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return "ADDITIVE";
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return "ALPHA_BLEND";
    default:
        return "OTHER";
    }
}

// What the runtime reported about the eyes this frame. The cant is the angle
// between the two eyes' forward axes: zero for parallel displays, and the
// headset's display tilt on canted ones (Pimax) unless the runtime is asked for
// parallel projections.
diagnostics::ViewGeometry DiagnosticViewGeometry(const OpenXRBackendFrame& frame) noexcept {
    constexpr float kRadiansToDegrees = 57.29577951f;
    diagnostics::ViewGeometry geometry{};
    std::array<std::array<float, 3>, kOpenXREyeCount> forward{};
    for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
        const XrView& view = frame.xr_frame.views[eye];
        geometry.fov_degrees[eye] = {view.fov.angleLeft * kRadiansToDegrees, view.fov.angleRight * kRadiansToDegrees,
                                     view.fov.angleUp * kRadiansToDegrees, view.fov.angleDown * kRadiansToDegrees};
        const auto& q = view.pose.orientation;
        forward[eye] = Rotate(Normalize({q.x, q.y, q.z, q.w}), {0.0f, 0.0f, -1.0f});
        geometry.width[eye] = frame.render_width[eye];
        geometry.height[eye] = frame.render_height[eye];
    }
    const float dot = forward[0][0] * forward[1][0] + forward[0][1] * forward[1][1] + forward[0][2] * forward[1][2];
    geometry.cant_degrees = std::acos(std::clamp(dot, -1.0f, 1.0f)) * kRadiansToDegrees;
    if ((frame.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
        const auto& left = frame.xr_frame.views[0].pose.position;
        const auto& right = frame.xr_frame.views[1].pose.position;
        const float dx = right.x - left.x;
        const float dy = right.y - left.y;
        const float dz = right.z - left.z;
        geometry.ipd_millimeters = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f;
    }
    return geometry;
}

// Lane X (test only, KARTFRAME_END_PHASE): the frame's end timed against SteamVR's compositor grid instead
// of the wake-up. frame/tools/vrtiming.py publishes the grid (64 bytes, atomic rename, format there and in
// frame/perf/LANE_X.md); the pacing thread re-reads it at most every 50 ms and aims the end at
//   after[:us]  -> vsync + update end p95 + us (default 300),
//   before[:us] -> vsync + update start p50 - us (default 1500),
// of the vsync at or before the wake-up. Without a fresh grid (missing, older than 0.5 s, period 1 % off) the
// end falls back to KARTFRAME_END_DELAY_US, or 4000 us when that is unset. Log lines start "end phase:".
class EndPhaseLock {
public:
    enum class Mode { Off, After, Before };
    using Clock = std::chrono::steady_clock;

    void Configure(long fallback_delay_us) {
        const char* env = std::getenv("KARTFRAME_END_PHASE");
        if (env == nullptr || *env == '\0') return;
        const std::string v(env);
        const std::string word = v.substr(0, v.find(':'));
        if (word == "after") mode_ = Mode::After, offset_us_ = 300;
        else if (word == "before") mode_ = Mode::Before, offset_us_ = 1500;
        else {
            RT_LOG(RT_TAG_RUNTIME) << "end phase: unknown KARTFRAME_END_PHASE=" << v << " (after or before); lock off"
                                   << std::endl;
            return;
        }
        if (const auto colon = v.find(':'); colon != std::string::npos) {
            offset_us_ = std::clamp(std::strtol(v.c_str() + colon + 1, nullptr, 10), 0l, 4000l);
        }
        fallback_us_ = fallback_delay_us > 0 ? fallback_delay_us : 4000;
        const char* path = std::getenv("KARTFRAME_PHASE_FILE");
        const char* run = std::getenv("XDG_RUNTIME_DIR");
        const char* home = std::getenv("HOME");
        path_ = path != nullptr && *path != '\0' ? std::string(path)
                : run != nullptr && *run != '\0'  ? std::string(run) + "/kartframe-phase.bin"
                                                  : std::string(home != nullptr ? home : ".") + "/.cache/kartframe-phase.bin";
        RT_LOG(RT_TAG_RUNTIME) << "end phase: KARTFRAME_END_PHASE=" << v << ": each frame ended at vsync + "
                               << (mode_ == Mode::After ? "update end p95 + " : "update start p50 - ") << offset_us_
                               << " us from the compositor grid in " << path_ << " (frame/tools/vrtiming.py); without a fresh "
                               << "grid " << fallback_us_ << " us after the wake-up (test only)" << std::endl;
    }
    bool Enabled() const { return mode_ != Mode::Off; }

    // When to end this cycle's frame. wake: xrWaitFrame's return; deadline: the latest end the pacing allows.
    Clock::time_point Target(Clock::time_point wake, Clock::time_point deadline, int64_t period_ns) {
        const Clock::time_point now = Clock::now();
        Refresh(now);
        const int64_t now_ns = now.time_since_epoch().count();
        const bool fresh = have_ && now_ns - written_ns_ < 500000000 && period_ns_ > 0 &&
                           std::llabs(period_ns_ - period_ns) * 100 <= period_ns;
        if (fresh != locked_ || !announced_) {
            RT_LOG(RT_TAG_RUNTIME) << "end phase: " << (fresh ? "locked to the compositor grid" : "fixed delay ")
                                   << (fresh ? std::string() : std::to_string(fallback_us_) + " us after the wake-up (" +
                                                               (have_ ? (now_ns - written_ns_ >= 500000000 ? "grid older than 0.5 s"
                                                                                                          : "grid period differs from xrWaitFrame's")
                                                                      : "no grid at " + path_) + ")")
                                   << std::endl;
            locked_ = fresh;
            announced_ = true;
        }
        Clock::time_point target = wake + std::chrono::microseconds(fallback_us_);
        vsync_ns_ = 0;
        if (locked_) {
            const int64_t wake_ns = wake.time_since_epoch().count();
            // The vsync at or before the wake-up (SteamVR returns about 0.4 ms after it; 1 ms of slack for the
            // published grid's error). A late wake-up keeps its vsync, so its target may already have passed.
            const double k = std::floor(static_cast<double>(wake_ns - vsync_pub_ + 1000000) / static_cast<double>(period_ns_));
            vsync_ns_ = vsync_pub_ + static_cast<int64_t>(k * static_cast<double>(period_ns_));
            const int64_t at = mode_ == Mode::After ? vsync_ns_ + ue95_ + offset_us_ * 1000
                                                    : vsync_ns_ + us50_ - offset_us_ * 1000;
            target = Clock::time_point(Clock::duration(at));
        }
        return std::min(target, deadline);
    }
    // After "before": a set started after the end waits until the update window is over.
    Clock::time_point WindowOver() const {
        return Clock::time_point(Clock::duration(vsync_ns_ + ue95_ + 300000));
    }
    bool Locked() const { return locked_ && vsync_ns_ != 0; }
    Mode GetMode() const { return mode_; }

    // The frame's end (the call that releases both eyes and ends the frame) for the 10 s summary.
    void NoteEnd(Clock::time_point end, Clock::time_point wake) {
        ++frames_;
        e_.push_back(std::chrono::duration<double, std::milli>(end - wake).count());
        if (Locked()) {
            const double d = static_cast<double>(end.time_since_epoch().count() - vsync_ns_) / 1e6;
            vs_.push_back(d);
            us_.push_back(d - us50_ / 1e6);
            ue_.push_back(d - ue50_ / 1e6);
        } else {
            ++fallback_frames_;
        }
        if (summary_at_ == Clock::time_point{}) summary_at_ = end;
        if (end - summary_at_ < std::chrono::seconds(10)) return;
        summary_at_ = end;
        auto q = [](std::vector<double>& v, double p) {
            if (v.empty()) return std::nan("");
            const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())));
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
            return v[i];
        };
        char line[400];
        std::snprintf(line, sizeof line,
                      "end phase: 10 s, %u frames (%u on the fixed delay); end after vsync p50 %.2f p95 %.2f ms, minus "
                      "update start p50 %+.2f p95 %+.2f, minus update end p50 %+.2f p95 %+.2f (published start %.2f end "
                      "%.2f/%.2f p50/p95); end after wake-up p50 %.2f p95 %.2f",
                      frames_, fallback_frames_, q(vs_, 0.5), q(vs_, 0.95), q(us_, 0.5), q(us_, 0.95), q(ue_, 0.5),
                      q(ue_, 0.95), us50_ / 1e6, ue50_ / 1e6, ue95_ / 1e6, q(e_, 0.5), q(e_, 0.95));
        RT_LOG(RT_TAG_RUNTIME) << line << std::endl;
        frames_ = fallback_frames_ = 0;
        vs_.clear(), us_.clear(), ue_.clear(), e_.clear();
    }

private:
    void Refresh(Clock::time_point now) {
        if (now - read_at_ < std::chrono::milliseconds(50)) return;
        read_at_ = now;
#if defined(__linux__)
        unsigned char b[64];
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        const ssize_t n = ::pread(fd, b, sizeof b, 0);
        ::close(fd);
        if (n != static_cast<ssize_t>(sizeof b) || std::memcmp(b, "KFP1", 4) != 0) return;
        auto i64 = [&](size_t o) { int64_t v; std::memcpy(&v, b + o, 8); return v; };
        auto i32 = [&](size_t o) { int32_t v; std::memcpy(&v, b + o, 4); return static_cast<int64_t>(v); };
        vsync_pub_ = i64(8);
        period_ns_ = i64(16);
        us50_ = i32(28);
        ue50_ = i32(32);
        ue95_ = i32(36);
        written_ns_ = i64(40);
        have_ = true;
#endif
    }

    Mode mode_ = Mode::Off;
    long offset_us_ = 0;
    long fallback_us_ = 4000;
    std::string path_;
    bool have_ = false, locked_ = false, announced_ = false;
    int64_t vsync_pub_ = 0, period_ns_ = 0, us50_ = 0, ue50_ = 0, ue95_ = 0, written_ns_ = 0, vsync_ns_ = 0;
    Clock::time_point read_at_{}, summary_at_{};
    uint32_t frames_ = 0, fallback_frames_ = 0;
    std::vector<double> vs_, us_, ue_, e_;
};

class OpenXRIntegration final {
public:
    static OpenXRIntegration& Get() {
        static OpenXRIntegration integration;
        return integration;
    }

    OpenXRStartupResult Prepare(AuroraConfig& aurora_config) {
        Shutdown();
        {
            std::lock_guard lock(error_mutex_);
            last_error_.clear();
        }
        if (graphics_retained_) {
            SetError(std::string("OpenXR cannot be restarted after an unfenceable ") +
                     kGraphicsBackendName + " submission");
            return OpenXRStartupResult::Unavailable;
        }
        // The pacing thread is not running here (Shutdown above joined it), so
        // the sink may be replaced.
        diagnostics::SetLogSink(
            [](std::string_view line) { RT_LOG(RT_TAG_RUNTIME) << line << std::endl; });
        pacing::SetSink([](std::string_view line) { RT_LOG(RT_TAG_RUNTIME) << line << std::endl; });
        diagnostics::SetEnabled(RuntimeConfigFile::DiagnosticsOpenXRLogging(false));
        requested_ = RuntimeConfigFile::VrEnabled(kVrEnabledDefault);
        ConfigurePolicy(requested_);
        if (!requested_) {
            return OpenXRStartupResult::Disabled;
        }
        if (!BackendMatchesConfiguredGraphicsApi(aurora_config)) {
            return OpenXRStartupResult::Unavailable;
        }

        logger_ = [](OpenXRLogLevel level, std::string_view message) {
            const char* name = level == OpenXRLogLevel::Error ? "error" :
                               level == OpenXRLogLevel::Warning ? "warning" : "info";
            RT_LOG(RT_TAG_RUNTIME) << "[openxr::" << name << "] " << message << std::endl;
        };
#if defined(__ANDROID__)
        {
            std::string loader_error;
            if (!OpenXRAndroidInitializeLoader(logger_, &loader_error)) {
                SetError("OpenXR Android loader initialization failed: " + loader_error);
                return OpenXRStartupResult::Unavailable;
            }
        }
#endif
        runtime_ = std::make_unique<OpenXRRuntime>(logger_);
        eye_offsets_.Configure();
        if (eye_offsets_.Active()) {
            runtime_->SetViewsHook([this](XrTime time, const XrSpaceLocation& head, bool focused,
                                          std::array<XrView, kOpenXREyeCount>& views) {
                eye_offsets_.OnLocate(time, head, focused, views);
            });
        }
#if defined(_WIN32)
        backend_ = std::make_unique<GraphicsBackend>(logger_, kRequiredAuroraBackend == BACKEND_VULKAN);
#else
        backend_ = std::make_unique<GraphicsBackend>(logger_);
#endif

        OpenXRConfig config{};
        config.application_name = aurora_config.appName != nullptr ? aurora_config.appName
                                                                    : "KartFrame";
        config.engine_name = "Aurora";
        config.resolution_scale = RuntimeConfigFile::VrRenderScale();
        // KARTFRAME_RENDER_SCALE (lane AG): one launch's render_scale (session.env), same range as the config's.
        if (const char* env = std::getenv("KARTFRAME_RENDER_SCALE"); env != nullptr && *env != '\0') {
            const float scale = std::strtof(env, nullptr);
            if (scale > 0.0f) config.resolution_scale = std::clamp(scale, 0.25f, 2.0f);
        }
#if defined(_WIN32)
        config.required_extensions = {kRequiredAuroraBackend == BACKEND_VULKAN ? "XR_KHR_vulkan_enable2" : "XR_KHR_D3D12_enable"};
        config.optional_extensions = {"XR_KHR_win32_convert_performance_counter_time",
                                      "XR_FB_display_refresh_rate", "XR_EXT_performance_settings"};
        AddHandMeshExtensions(config);
#elif defined(MKW_OPENXR_LINUX_DESKTOP)
        config.required_extensions = {"XR_KHR_vulkan_enable2"};
        // Lane O (H9.md): KARTFRAME_XR_EXTENSIONS=galaxy asks for exactly GalaxyFrame's list (SteamVR on the
        // Frame shows GalaxyFrame clean): its GLES binding extensions beside Vulkan's, no
        // XR_KHR_convert_timespec_time (display times then read XrTime as CLOCK_MONOTONIC, which is
        // SteamVR's clock), no other controllers' profiles and no hand tracking (hand steering is off).
        static const bool galaxy_extensions = [] {
            const char* env = std::getenv("KARTFRAME_XR_EXTENSIONS");
            return env != nullptr && std::string_view(env) == "galaxy";
        }();
        if (galaxy_extensions) {
            config.optional_extensions = {"XR_KHR_opengl_es_enable", "XR_MNDX_egl_enable",
                                          "XR_VALVE_frame_controller_interaction", "XR_FB_display_refresh_rate",
                                          "XR_FB_space_warp", "XR_META_recommended_layer_resolution",
                                          "XR_META_performance_metrics", "XR_EXT_eye_gaze_interaction"};
        } else {
            config.optional_extensions = {"XR_KHR_convert_timespec_time", "XR_VALVE_frame_controller_interaction",
                                          "XR_FB_display_refresh_rate", "XR_EXT_performance_settings"};
            AddHandMeshExtensions(config);
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR extensions asked for: "
                               << (galaxy_extensions ? "GalaxyFrame's list (KARTFRAME_XR_EXTENSIONS=galaxy)"
                                                     : "KartFrame's list (KARTFRAME_XR_EXTENSIONS=galaxy: GalaxyFrame's)")
                               << std::endl;
#else
        // Either Vulkan binding extension is acceptable; the backend picks
        // whichever the runtime enabled, preferring enable2.
        config.required_extensions = {"XR_KHR_android_create_instance"};
        // XR_FB_passthrough: the room around the virtual screen (OpenXRPassthrough), asked for
        // whatever [vr] passthrough says, since the setting is live.
        config.optional_extensions = {"XR_KHR_vulkan_enable2", "XR_KHR_vulkan_enable",
                                      "XR_KHR_convert_timespec_time",
                                      "XR_KHR_android_thread_settings",
                                      "XR_FB_display_refresh_rate", "XR_EXT_performance_settings",
                                      "XR_FB_passthrough"};
        AddHandMeshExtensions(config);
        config.instance_create_next = OpenXRAndroidInstanceCreateNext();
#endif
        if (!runtime_->Initialize(config)) {
            SetError("OpenXR instance initialization failed: " + runtime_->LastError().message);
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }
        const auto& extensions = runtime_->EnabledExtensions();
        const auto has_extension = [&](const char* name) {
            return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
        };
#if defined(_WIN32)
        if (has_extension("XR_KHR_win32_convert_performance_counter_time")) {
            runtime_->LoadFunction("xrConvertTimeToWin32PerformanceCounterKHR", &convert_display_time_);
        }
#else
        if (has_extension("XR_KHR_convert_timespec_time")) {
            runtime_->LoadFunction("xrConvertTimeToTimespecTimeKHR", &convert_display_time_);
        }
#endif
        if (has_extension("XR_FB_display_refresh_rate")) {
            runtime_->LoadFunction("xrGetDisplayRefreshRateFB", &get_display_refresh_rate_);
            runtime_->LoadFunction("xrEnumerateDisplayRefreshRatesFB", &enumerate_display_refresh_rates_);
            runtime_->LoadFunction("xrRequestDisplayRefreshRateFB", &request_display_refresh_rate_);
        }
        if (has_extension("XR_EXT_performance_settings")) {
            runtime_->LoadFunction("xrPerfSettingsSetPerformanceLevelEXT", &set_performance_level_);
        }
#if defined(__ANDROID__) || defined(MKW_OPENXR_LINUX_DESKTOP)
        // Lepton's SteamVR bridge has no XR_KHR_convert_timespec_time (native SteamVR on the
        // Frame has it); without it DisplayTimeNanos falls back to reading XrTime as
        // CLOCK_MONOTONIC, or anchors it to the first frame (see MonotonicDisplayDelta).
        interpolation_available_.store(true, std::memory_order_release);
        clock_fallback_logged_ = false;
        clock_offset_valid_ = false;
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR display times: "
                               << (convert_display_time_ != nullptr ? "XR_KHR_convert_timespec_time"
                                                                     : "no conversion offered; CLOCK_MONOTONIC fallback")
                               << std::endl;
#else
        interpolation_available_.store(convert_display_time_ != nullptr, std::memory_order_release);
#endif
        if (!backend_->QueryGraphicsRequirements(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return OpenXRStartupResult::Unavailable;
        }

        ApplyGraphicsRequirements(aurora_config);
        prepared_ = true;
        return OpenXRStartupResult::Prepared;
    }

    bool Start(AuroraBackend active_backend) {
        if (!prepared_ || runtime_ == nullptr || backend_ == nullptr) {
            return !requested_;
        }
        if (active_backend != kRequiredAuroraBackend) {
            SetError(std::string("Aurora could not create the OpenXR-required ") +
                     kGraphicsBackendName + " backend");
            ResetPreparedObjects();
            return false;
        }
        if (!backend_->BindAurora(*runtime_)) {
            SetError(backend_->LastError());
            ResetPreparedObjects();
            return false;
        }
        input_ = std::make_unique<OpenXRInput>(logger_);
        if (!input_->Create(*runtime_)) {
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR controller input unavailable: " << input_->LastError()
                                   << std::endl;
            input_.reset();
        }

#if defined(__linux__)
        // The producer: SDL's main thread, which also carries every guest fiber.
        game_thread_id_ = static_cast<uint32_t>(gettid());
#endif
        stop_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = false;
        }
        teardown_requested_.store(false, std::memory_order_release);
        WithdrawPublishedFrame();
        aurora_set_stereo_frame_provider(&OpenXRIntegration::ProvideStereoFrame, this);
        provider_registered_ = true;
        if (convert_display_time_ != nullptr || interpolation_available_.load(std::memory_order_acquire)) {
            diagnostics::SetDisplayTimeConverter(
                [this](int64_t xr_time) { return static_cast<int64_t>(DisplayTimeNanos(xr_time)); });
        }
        ReadPacingMode();
        set_pacing_ = {};
        ahead_sets_.clear();
        ahead_shown_time_ = 0;
#if KARTFRAME_AHEAD_PACING
        ahead_ = backend_->EnableAhead(ahead_wanted_);
#else
        ahead_ = false;
#endif
        if (pacing_mode_ == PacingMode::Pipelined) {
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR pipelined pacing: "
                                   << (ahead_ ? (ahead_lead_ == 1 ? "sets rendered one refresh ahead (KARTFRAME_AHEAD_REFRESHES=1), "
                                                                    "each started at the wake-up before the frame is ended, "
                                                                    "every frame ended at its wake-up with the newest finished set"
                                                                  : "sets rendered two refreshes ahead, every frame ended at its wake-up "
                                                                    "with the newest finished set (KARTFRAME_PIPELINED=wait: the old loop)")
                                              : ahead_wanted_ ? "one set at a time, frames ended when it finishes "
                                                                "(sets ahead need GPU completion polling)"
                                                              : "one set at a time, frames ended when it finishes "
                                                                "(KARTFRAME_PIPELINED=wait)")
                                   << (ahead_ && ahead_start_delay_.count() > 0
                                           ? "; each set starts " + std::to_string(ahead_start_delay_.count()) +
                                                 " us after the wake-up (KARTFRAME_AHEAD_START_US)"
                                           : std::string())
                                   << (ahead_ && ahead_end_delay_.count() > 0 && !end_phase_.Enabled()
                                           ? "; each frame is ended " + std::to_string(ahead_end_delay_.count()) +
                                                 " us after the wake-up (KARTFRAME_END_DELAY_US, test only)"
                                           : std::string())
                                   << std::endl;
        }
        running_.store(true, std::memory_order_release);
        try {
            pacing_thread_ = std::thread([this] { PacingThread(); });
        } catch (const std::exception& exception) {
            running_.store(false, std::memory_order_release);
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            SetError(std::string("could not start the OpenXR pacing thread: ") + exception.what());
            ResetPreparedObjects();
            return false;
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR asynchronous " << kGraphicsBackendName
                               << " presentation started" << std::endl;
        return true;
    }

    void Shutdown() noexcept {
        teardown_requested_.store(false, std::memory_order_release);
        // Called on the game thread: aurora's producer (the GX thread) must be
        // idle before the frame worker is quiesced.
        GxThread::Drain();
        // Stop idle replays before draining; no new worker job may race provider removal.
        {
            std::lock_guard lock(interpolation_mutex_);
            interpolation_stopping_ = true;
            aurora_set_stereo_frame_interpolation(false);
        }
        if (pacing_thread_.joinable()) {
            // Registration changes are only safe while no sealed frame is in
            // flight. The caller invokes us before Aurora teardown.
            aurora_quiesce_frame_worker();
            aurora_set_stereo_frame_provider(nullptr, nullptr);
            provider_registered_ = false;
            WithdrawPublishedFrame();
            {
                // Pair the predicate update with the wait mutex. Otherwise a
                // terminal pacing thread can observe false, miss the notify,
                // and make join wait forever.
                std::lock_guard lock(stop_mutex_);
                stop_.store(true, std::memory_order_release);
            }
            stop_cv_.notify_all();
            pacing_thread_.join();
        } else {
            if (provider_registered_) {
                aurora_quiesce_frame_worker();
                aurora_set_stereo_frame_provider(nullptr, nullptr);
                provider_registered_ = false;
            }
            ShutdownOrRetainGraphicsObjects();
        }
        running_.store(false, std::memory_order_release);
        MkwVRPolicySetSessionActive(false);
        // The converter reads runtime_; the pacing thread has stopped using it.
        diagnostics::SetDisplayTimeConverter({});
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
        convert_display_time_ = nullptr;
        get_display_refresh_rate_ = nullptr;
        enumerate_display_refresh_rates_ = nullptr;
        request_display_refresh_rate_ = nullptr;
        logged_refresh_hz_ = 0.0f;
        set_performance_level_ = nullptr;
        headset_hz_.store(0, std::memory_order_relaxed);
        rendered_fps_.store(0, std::memory_order_relaxed);
        interpolation_available_.store(false, std::memory_order_release);
        ResetTrackingOrigin();
        applied_session_run_serial_ = 0;
        session_was_active_ = false;
    }

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    void RequestRecenter() noexcept {
        recenter_requested_.store(true, std::memory_order_release);
    }

    void SetFrameInterpolationFps(uint32_t target) noexcept {
        frame_interpolation_fps_.store(NormalizeFrameInterpolationFps(target), std::memory_order_relaxed);
    }

    OpenXRFrameTiming FrameTiming() const noexcept {
        return {headset_hz_.load(std::memory_order_relaxed), rendered_fps_.load(std::memory_order_relaxed)};
    }

    bool FrameInterpolationAvailable() const noexcept {
        return interpolation_available_.load(std::memory_order_acquire);
    }

    void SetPassthrough(bool enabled) noexcept {
        passthrough_.store(enabled, std::memory_order_relaxed);
    }

    void SetImmersiveWindow(bool enabled) noexcept {
        immersive_window_.store(enabled, std::memory_order_relaxed);
    }

    void SetLeanBackDegrees(float degrees) noexcept {
        lean_back_degrees_.store(
            std::clamp(degrees, -RuntimeConfigFile::kVrLeanBackDegreesLimit,
                       RuntimeConfigFile::kVrLeanBackDegreesLimit),
            std::memory_order_relaxed);
    }

    void ServiceProducerFrameBoundary() noexcept {
        if (teardown_requested_.load(std::memory_order_acquire)) {
            Shutdown();
        }
    }

    std::string LastError() const {
        std::lock_guard lock(error_mutex_);
        return last_error_;
    }

private:
    struct PublishedFrame {
        AuroraStereoFrame frame{};
    };

#if defined(_WIN32)
    AuroraBackend kRequiredAuroraBackend = BACKEND_D3D12;
    const char* kGraphicsBackendName = "D3D12";
#else
    static constexpr AuroraBackend kRequiredAuroraBackend = BACKEND_VULKAN;
#endif
    // Skipped eye copies tolerated back to back before the session is given up: a few seconds
    // at the headset's refresh rate.
    static constexpr uint32_t kMaxConsecutiveSkips = 300;

    bool BackendMatchesConfiguredGraphicsApi(const AuroraConfig& aurora_config) {
#if defined(_WIN32)
        kRequiredAuroraBackend = aurora_config.desiredBackend == BACKEND_VULKAN ? BACKEND_VULKAN : BACKEND_D3D12;
        kGraphicsBackendName = kRequiredAuroraBackend == BACKEND_VULKAN ? "Vulkan" : "D3D12";
#endif
        if (aurora_config.desiredBackend == BACKEND_AUTO ||
            aurora_config.desiredBackend == kRequiredAuroraBackend) {
            return true;
        }
        SetError(std::string("OpenXR requires the ") + kGraphicsBackendName +
                 " graphics backend on this platform");
        return false;
    }

    void ApplyGraphicsRequirements(AuroraConfig& aurora_config) {
        aurora_config.desiredBackend = kRequiredAuroraBackend;
        aurora_config.xrInterop = true;
#if defined(__ANDROID__)
        // Foveated rendering: fragment density maps are decided with the device. They put a flag on
        // every render pipeline, so a session launched with foveation off does without them.
        aurora_config.xrFragmentDensityMap = RuntimeConfigFile::VrFoveation() != "off";
#endif
#if defined(_WIN32)
        if (kRequiredAuroraBackend != BACKEND_D3D12) return;
        const auto& requirements = backend_->GraphicsRequirements();
        aurora_config.hasD3D12AdapterLuid = true;
        aurora_config.d3d12AdapterLuidLow = requirements.adapter_luid_low;
        aurora_config.d3d12AdapterLuidHigh = requirements.adapter_luid_high;
#endif
    }

    void ResetPreparedObjects() {
        diagnostics::SetDisplayTimeConverter({});
        ShutdownOrRetainGraphicsObjects();
        input_.reset();
        backend_.reset();
        runtime_.reset();
        prepared_ = false;
    }

    bool ShutdownOrRetainGraphicsObjects() noexcept {
        if (input_ != nullptr) {
            // Actions belong to the session and must go before it does.
            input_->Destroy();
            input_.reset();
        }
        if (backend_ != nullptr && !backend_->Shutdown()) {
            RT_LOG(RT_TAG_RUNTIME)
                << "OpenXR " << kGraphicsBackendName
                << " queue completion is unknown; retaining the backend, "
                   "runtime, session, and graphics resources until process exit"
                << std::endl;
            (void)backend_.release();
            (void)runtime_.release();
            graphics_retained_ = true;
            return false;
        }
        if (runtime_ != nullptr) {
            runtime_->Shutdown();
        }
        return true;
    }

#if defined(__ANDROID__)
    // Aurora's frame worker publishes its native thread id once it runs; until then there is
    // nothing to hint. The hint itself may be refused by the runtime, which is only logged.
    bool RegisterAuroraFrameWorkerThread() {
        const uint32_t thread_id = aurora_get_frame_worker_native_thread_id();
        if (thread_id == 0 || runtime_ == nullptr) {
            return false;
        }
        const bool hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::RendererMain, thread_id);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: Android thread hint for Aurora's frame worker "
                               << (hinted ? "set" : "refused") << std::endl;
        return true;
    }
    bool RegisterGxThread() {
        const uint32_t thread_id = GxThread::NativeThreadId();
        if (thread_id == 0 || runtime_ == nullptr) {
            return false;
        }
        const bool hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::RendererWorker, thread_id);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: Android thread hint for the GX thread "
                               << (hinted ? "set" : "refused") << std::endl;
        return true;
    }
#endif

    // Asks the runtime for the configured performance level in both domains. Standalone
    // headsets clock their cores by this: a Quest 3 held the game thread at CPU level 4
    // (2.2 GHz of a possible 2.36) and the GPU at level 3 with the runtime's own choice. A
    // refusal is logged and changes nothing; desktop runtimes rarely offer the extension.
    void ApplyPerformanceLevel() {
        if (runtime_ == nullptr || set_performance_level_ == nullptr || !runtime_->HasSession()) {
            return;
        }
        const std::string requested = RuntimeConfigFile::VrPerformanceLevel();
        XrPerfSettingsLevelEXT level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
        if (requested == "default") {
            return;
        } else if (requested == "power_savings") {
            level = XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
        } else if (requested == "sustained_low") {
            level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
        } else if (requested == "boost") {
            level = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
        }
        const XrResult cpu = set_performance_level_(runtime_->Session(), XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
        const XrResult gpu = set_performance_level_(runtime_->Session(), XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: performance level \"" << requested << "\" CPU "
                               << (XR_SUCCEEDED(cpu) ? "set" : "refused") << " (" << cpu << "), GPU "
                               << (XR_SUCCEEDED(gpu) ? "set" : "refused") << " (" << gpu << ")" << std::endl;
    }

    static bool ProvideStereoFrame(uint32_t, AuroraStereoFrame* output, void* userdata) {
        auto* self = static_cast<OpenXRIntegration*>(userdata);
        if (self == nullptr || output == nullptr) {
            return false;
        }
        // The packet storage is reused by the XR thread. Claim and copy it
        // under one short lock so cancellation cannot begin the next packet
        // while this callback is preempted between exchange and copy.
        std::lock_guard lock(self->published_mutex_);
        PublishedFrame* frame = self->published_.exchange(nullptr, std::memory_order_acq_rel);
        if (frame == nullptr) {
            return false;
        }
        diagnostics::NotePacketConsumed();
        pacing::NoteSetConsumed();
        *output = frame->frame;
        return true;
    }

    // Asks for the display refresh rate in-app as well (the wrapper sets SteamVR's per-app
    // 120 Hz too): KARTFRAME_REFRESH_HZ, else [vr] refresh_rate, else 120, if the runtime offers
    // it. 0 leaves the runtime's choice.
    void RequestDisplayRefreshRate() {
        if (runtime_ == nullptr || !runtime_->HasSession() || enumerate_display_refresh_rates_ == nullptr ||
            request_display_refresh_rate_ == nullptr) {
            return;
        }
        std::string wanted_text;
        if (const char* env = std::getenv("KARTFRAME_REFRESH_HZ"); env != nullptr && *env != '\0') {
            wanted_text = env;
        } else {
            wanted_text = ReadVrConfigText("refresh_rate");
        }
        const float wanted = wanted_text.empty() ? 120.0f : std::strtof(wanted_text.c_str(), nullptr);
        uint32_t count = 0;
        if (XR_FAILED(enumerate_display_refresh_rates_(runtime_->Session(), 0, &count, nullptr)) || count == 0) return;
        std::vector<float> rates(count);
        if (XR_FAILED(enumerate_display_refresh_rates_(runtime_->Session(), count, &count, rates.data()))) return;
        rates.resize(count);
        std::ostringstream offered;
        for (size_t i = 0; i < rates.size(); ++i) offered << (i ? ", " : "") << rates[i];
        if (!(wanted > 0.0f)) {
            RT_LOG(RT_TAG_RUNTIME) << "display refresh rates: " << offered.str() << "; requested none (runtime's choice)" << std::endl;
            return;
        }
        // Only a rate the runtime offers. SteamVR on the Frame offered only 90 in H1 while the
        // per-app setting ran the display at 120: asking for the nearest (90) switched it to 90
        // until the per-app key put 120 back. Not offered: leave it to SteamVR's setting.
        float best = 0.0f;
        for (float rate : rates) {
            if (std::fabs(rate - wanted) < 0.5f) best = rate;
        }
        if (best == 0.0f) {
            RT_LOG(RT_TAG_RUNTIME) << "display refresh rates: " << offered.str() << "; " << wanted
                                   << " Hz not offered, requested none (SteamVR's per-app setting decides)" << std::endl;
            return;
        }
        const XrResult result = request_display_refresh_rate_(runtime_->Session(), best);
        RT_LOG(RT_TAG_RUNTIME) << "display refresh rates: " << offered.str() << "; requested " << best << " Hz"
                               << (XR_SUCCEEDED(result) ? "" : " (refused)") << std::endl;
    }

    void PacingThread() noexcept {
#if defined(__ANDROID__)
        // The runtime schedules hinted threads onto the fast cores. The game thread and Aurora's
        // frame worker, which submits the GPU work, are the ones that matter; this thread only
        // paces.
        bool worker_registered = false;
        bool gx_registered = false;
        if (runtime_ != nullptr) {
            const bool pacing_hinted =
                OpenXRAndroidRegisterThread(*runtime_, OpenXRAndroidThreadType::RendererWorker);
            bool game_hinted = false;
            if (game_thread_id_ != 0) {
                game_hinted = OpenXRAndroidRegisterThreadId(*runtime_, OpenXRAndroidThreadType::ApplicationMain,
                                                            game_thread_id_);
            }
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR: Android thread hints: game " << (game_hinted ? "set" : "refused")
                                   << ", pacing " << (pacing_hinted ? "set" : "refused") << std::endl;
            worker_registered = RegisterAuroraFrameWorkerThread();
            gx_registered = RegisterGxThread();
        }
#endif
#if defined(__linux__) && !defined(__ANDROID__)
        // Raised priority for the threads a headset frame waits on, so other work on the machine
        // (a build, SteamVR's own threads) cannot starve them. Aurora's frame worker and the GPU
        // watcher may start later: raised once they publish their ids.
        bool worker_raised = false;
        bool watcher_raised = false;
        {
            std::ostringstream raised;
            raised << "thread priority: pacing " << RaiseThreadPriority(0);
            // The game thread wants the 1024 core, the frame worker (2 to 3 ms of encode a set) a big one.
            if (game_thread_id_ != 0) raised << "; game " << RaiseThreadPriority(game_thread_id_, 1024);
            if (const uint32_t gx = GxThread::NativeThreadId(); gx > 1) raised << "; GX " << RaiseThreadPriority(gx);
            RT_LOG(RT_TAG_RUNTIME) << raised.str() << std::endl;
        }
        const auto raise_late_threads = [&] {
            if (!worker_raised) {
                if (const uint32_t tid = aurora_get_frame_worker_native_thread_id(); tid != 0) {
                    worker_raised = true;
                    const std::string result = RaiseThreadPriority(tid, 800);
                    RT_LOG(RT_TAG_RUNTIME) << "thread priority: Aurora frame worker (tid " << tid << ") " << result << std::endl;
                }
            }
            if (!watcher_raised) {
                if (const uint32_t tid = OpenXRVulkanGpuWatcherThreadId(); tid != 0) {
                    watcher_raised = true;
                    const std::string result = RaiseThreadPriority(tid);
                    RT_LOG(RT_TAG_RUNTIME) << "thread priority: GPU watcher (tid " << tid << ") " << result << std::endl;
                }
            }
        };
#endif
        ApplyPerformanceLevel();
        RequestDisplayRefreshRate();
        bool fatal = false;
        uint32_t consecutive_skips = 0;
        bool store_gate_set = false;
        bool store_gate_racing = false;
        bool presentation_logged = false;
        VRPresentationMode logged_presentation = VRPresentationMode::Desktop;
        uint32_t presentation_log_count = 0;
        bool immersive_submission_logged = false;
        int last_pacing_mode = -1;
        while (!stop_.load(std::memory_order_acquire) && !fatal) {
#if defined(__linux__) && !defined(__ANDROID__)
            if (!worker_raised || !watcher_raised) raise_late_threads();
#endif
#if defined(__ANDROID__)
            if (!worker_registered) {
                worker_registered = RegisterAuroraFrameWorkerThread();
            }
            if (!gx_registered) {
                gx_registered = RegisterGxThread();
            }
#endif
            const OpenXREventStatus events = diagnostics::Measure(diagnostics::Stage::PollEvents, [&] {
                return runtime_->PollEvents();
            });
            const bool session_active = runtime_->IsSessionRunning();
            MkwVRPolicySetSessionActive(session_active);
            const uint64_t session_run_serial = runtime_->SessionRunSerial();
            if (session_run_serial != applied_session_run_serial_) {
                applied_session_run_serial_ = session_run_serial;
                ResetTrackingOrigin();
                diagnostics::OnSessionStarted();
                pacing::Reset();
                set_pacing_.half_rate = false;
                set_pacing_.results.clear();
                set_pacing_.last_display_time = 0;
                set_pacing_.first_set = {};
            }
            if (session_active != session_was_active_) {
                session_was_active_ = session_active;
                if (!session_active) {
                    pacing::Totals();
                    ResetTrackingOrigin();
                    // Nothing is displayed while the session is not running (the system menu,
                    // the headset taken off), so the stall of a cache store is invisible here.
                    // Waiting for it is deliberate: the process may be ended next.
                    aurora_store_pipeline_caches();
                }
            }
            if (events == OpenXREventStatus::ExitRequested) {
                SetError("OpenXR runtime requested session exit; continuing on the mirror output");
                break;
            }
            if (events == OpenXREventStatus::Error) {
                SetError("OpenXR event processing failed: " + runtime_->LastError().message);
                break;
            }
            if (!session_active) {
                if (!DropPendingSet()) {
                    fatal = true;
                    break;
                }
                if (input_ != nullptr) {
                    input_->Idle();
                }
                SetInterpolationActive(false);
                interpolation_pacing_.Reset();
                rendered_fps_.store(0, std::memory_order_relaxed);
                WaitForStopOrDelay(std::chrono::milliseconds(5));
                continue;
            }

            // Reference spaces can only be replaced between XR frames. The
            // startup pose check schedules this after the previous submission,
            // so eyes, menu and controller rays all use the new space together.
            if (startup_origin_pending_ && !SetsInFlight()) {
                startup_origin_pending_=false;
                if(runtime_->ResetAppSpace(startup_origin_pose_)) {
                    const auto& q=startup_origin_pose_.orientation;
                    const auto& p=startup_origin_pose_.position;
                    startup_origin_applied_=q.x!=0 || q.y!=0 || q.z!=0 || q.w!=1 ||
                        p.x!=0 || p.y!=0 || p.z!=0;
                    ResetTrackingOrigin();
                    RT_LOG(RT_TAG_RUNTIME) << (startup_origin_applied_ ?
                        "OpenXR: initialized seated startup reference space" :
                        "OpenXR: cleared startup correction after runtime recenter") << std::endl;
                }
            }
            const MkwVRPolicySnapshot policy = MkwVRPolicyGetSnapshot();
            // Diagnostics lift the cap: a presentation flickering between the
            // race and the virtual screen is exactly what a report needs to show.
            if ((!presentation_logged || policy.presentation != logged_presentation) &&
                (presentation_log_count < 16 || diagnostics::Enabled())) {
                presentation_logged = true;
                logged_presentation = policy.presentation;
                ++presentation_log_count;
                RT_LOG(RT_TAG_RUNTIME)
                    << "[mkw-vr] presentation="
                    << (policy.presentation == VRPresentationMode::ImmersiveRace
                            ? "immersive-race"
                            : policy.presentation == VRPresentationMode::VirtualScreen
                                  ? "virtual-screen"
                                  : "desktop")
                    << ", scene=" << static_cast<unsigned>(policy.scene.mode)
                    << ", screens=" << policy.scene.local_player_count
                    << ", camera-valid=" << policy.camera.valid
                    << ", scene-frame=" << policy.scene.guest_frame_index
                    << ", camera-frame=" << policy.camera.guest_frame_index
                    << ", bindings=0x" << std::hex << policy.available_bindings
                    << std::dec << std::endl;
            }
            OpenXRPresentation presentation{};
            const bool immersive = policy.presentation == VRPresentationMode::ImmersiveRace;
            presentation.mode = (immersive || OpenXRProjectedMenus()) ? OpenXRFrameMode::ImmersiveProjection
                                           : OpenXRFrameMode::VirtualScreen;
            // The Frame's menus: the controllers and the pointer around the screen's quad.
            presentation.menu_environment =
                presentation.mode == OpenXRFrameMode::VirtualScreen && MenuEnvironmentAvailable();
            presentation.quad_distance_meters = policy.config.hud_distance_meters;
            presentation.quad_width_meters = policy.config.hud_width_meters;
            if (float picture_aspect = 0.0f, snapshot_aspect = 0.0f;
                aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
                presentation.quad_content_aspect = snapshot_aspect;
            }
            // The immersive window: the race's stereo view seen through its 2D layer's screen.
            // The flag travels with the packet, so the eyes Aurora masks and the layer the
            // backend blends always belong to the same frame.
            presentation.immersive_window = immersive && immersive_window_.load(std::memory_order_relaxed);
            // The room around the menu screen and every other virtual screen, a Flat Screen
            // race included, and around the immersive window; a fully immersive race is
            // virtual all round, and the cameras are paused for it.
            presentation.passthrough =
                (!immersive || presentation.immersive_window) && passthrough_.load(std::memory_order_relaxed);
            // The settings panel gets a compositor layer of its own while it is
            // open, and Aurora leaves it out of the eyes. A backend that could
            // not make that layer has the panel drawn into the eyes instead.
            const bool panel_layer = false; // Keep VR settings in the same stereo eyes as the menu/tutorial.
            aurora_set_stereo_panel_layer(panel_layer);
            PollEyePassesOverride();
            PollFoveationOverride();
            PollWindowEyesOverride();
            presentation.panel.requested = panel_layer && (OpenXRSettingsPanelOpen() || OpenXRIntroductionActive());

            // Pipeline caches are stored where their stall is least visible: once when a race
            // ends, and by the compiler itself while the headset shows the virtual screen. Never
            // mid-race, and a race on the virtual screen (Flat Screen mode) is still a race. The
            // race-exit store runs on Aurora's thread: this one keeps submitting frames while
            // Dawn holds its device to serialize, which is still a brief game stall.
            const bool racing = immersive || (!policy.config.immersive_races &&
                                              policy.scene.mode == VRSceneMode::Race);
            if (!store_gate_set || racing != store_gate_racing) {
                const bool left_race = store_gate_set && store_gate_racing && !racing;
                store_gate_set = true;
                store_gate_racing = racing;
                aurora_set_pipeline_cache_idle_store(!racing);
                if (left_race) {
                    aurora_request_pipeline_cache_store();
                }
            }

            // Updating this on the owner thread also confines retained replay to
            // validated race content. The provider checks policy tags again.
            const uint32_t interpolation_target = frame_interpolation_fps_.load(std::memory_order_relaxed);
            SetInterpolationActive(FrameInterpolationAvailable() && interpolation_target != 0);

            // With interpolation off, the eyes are rendered before
            // the compositor frame that shows them is begun, so that frame never waits for a
            // game frame. Interpolation keeps the frame-first order below: it renders for the
            // frame's own predicted display time.
            const bool render_first = !aurora_get_stereo_frame_interpolation();
            if (last_pacing_mode != static_cast<int>(render_first)) {
                last_pacing_mode = static_cast<int>(render_first);
                // One write: std::cerr is unbuffered, and another thread's log output landed inside
                // this line when it was written piece by piece ("pacing: 0per-refresh").
                const std::string banner = std::string("OpenXR ") + kGraphicsBackendName + " pacing: " +
                    (render_first ? std::string("render-first")
                                  : std::string("per-refresh eye sets (VR interpolation), ") +
                                        CurrentModeName() + " (frame_pacing " + pacing_mode_source_ + ")") +
                    "\n";
                RT_LOG(RT_TAG_RUNTIME) << banner << std::flush;
                pacing::SetMode(CurrentModeName());
            }
            // A set still in flight is shown (or dropped) by the set loop before switching.
            if (render_first && !SetsInFlight()) {
                if (!RenderFirstCycle(presentation, policy, immersive, consecutive_skips,
                                      immersive_submission_logged)) {
                    fatal = true;
                }
                continue;
            }
            const CycleResult result =
                ahead_ ? AheadCycle(presentation, policy, immersive, interpolation_target, !render_first,
                                    immersive_submission_logged)
                       : SetCycle(presentation, policy, immersive, interpolation_target, immersive_submission_logged);
            if (result == CycleResult::Fatal) {
                fatal = true;
            } else if (result == CycleResult::Break) {
                break;
            }
        }

        pacing::Totals();
        SetInterpolationActive(false);
        aurora_set_pipeline_cache_idle_store(false);
        aurora_set_stereo_panel_layer(false);
        running_.store(false, std::memory_order_release);
        MkwVRPolicySetSessionActive(false);
        if (!stop_.load(std::memory_order_acquire)) {
            // A runtime/backend failure can happen while Aurora is submitting.
            // Ask the producer to reach a safe frame boundary, drain Aurora,
            // and unregister the provider before this XR owner destroys state.
            teardown_requested_.store(true, std::memory_order_release);
            std::unique_lock lock(stop_mutex_);
            stop_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire); });
        }
        ShutdownOrRetainGraphicsObjects();
    }

    // ---- VR interpolation: an eye set every refresh (plan 1.3) ----
    //
    // Every compositor cycle: xrWaitFrame and xrBeginFrame; one xrLocateViews for the time the
    // set will be displayed; both eyes, the menu/HUD screen, the panel and the pointer from those
    // poses; xrEndFrame with exactly those poses, the images released only after the GPU
    // finished them. Inline: the set is located for this cycle's display time, rendered, waited
    // for and shown in the same cycle. Pipelined: located for the next cycle's display time,
    // published after this cycle ended, and shown by the next cycle, so the GPU and the CPU
    // overlap by one refresh (one refresh more latency). A set not finished by its deadline is
    // never released early: the cycle repeats the retained layer with its own poses (late), and
    // the set is shown by a later cycle. The loop never drops to a set every second refresh on
    // its own (KARTFRAME_HALF_RATE, see ReadPacingMode).
    enum class CycleResult { Continue, Break, Fatal };
    enum class PacingMode { Inline, Pipelined };
    enum class HalfRatePolicy { Auto, Never, Always };
    static constexpr size_t kHalfRateWindow = 3600;  // sets: 30 s at 120 Hz

    struct SetPacingState {
        OpenXRBackendFrame set{};
        bool pending = false;
        uint32_t timeouts = 0;
        std::chrono::steady_clock::time_point published_at{};
        bool half_rate = false;
        bool odd_tick = false;
        std::chrono::steady_clock::time_point half_since{};
        std::deque<uint8_t> results;  // 1: the set was late (not finished by its deadline)
        XrTime last_display_time = 0;
        std::chrono::steady_clock::time_point first_set{};
    };

    static const char* PacingModeName(PacingMode mode) noexcept {
        return mode == PacingMode::Pipelined ? "pipelined" : "inline";
    }
    // As the pacing line names it: "pipelined" renders sets ahead, "pipelined-wait" is the old loop.
    const char* CurrentModeName() const noexcept {
        return pacing_mode_ == PacingMode::Pipelined && !ahead_ ? "pipelined-wait" : PacingModeName(pacing_mode_);
    }

    // A [vr] key of Config.toml that runtime_config.h does not know (read here so that header,
    // which every translated file includes, stays untouched). Strings, or numbers as text.
    static std::string ReadVrConfigText(const char* key) {
        try {
            std::ifstream file(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
            if (!file) return {};
            const auto document = toml::parse(file, "Config.toml");
            if (auto text = RuntimeConfigFile::FindConfigValue<std::string>(document, "vr", key)) return *text;
            if (auto number = RuntimeConfigFile::FindConfigFloat(document, "vr", key)) {
                std::ostringstream out;
                out << *number;
                return out.str();
            }
        } catch (const std::exception&) {
        }
        return {};
    }

    // [vr] frame_pacing = auto | inline | pipelined in Config.toml, KARTFRAME_PACING in the
    // environment (wins). Auto is pipelined: in the mock at 120 Hz with 1728 eyes, inline held
    // 96 % of refreshes in menus and none in a race (Aurora's encode, ~3 ms, plus the GPU,
    // ~6.5 ms, do not fit one refresh), pipelined 99.9 % in menus and ~93 % in a race.
    void ReadPacingMode() {
        std::string value;
        if (const char* env = std::getenv("KARTFRAME_PACING"); env != nullptr && *env != '\0') {
            value = env;
            pacing_mode_source_ = std::string("KARTFRAME_PACING=") + env;
        } else {
            value = ReadVrConfigText("frame_pacing");
            pacing_mode_source_ = value.empty() ? std::string("auto") : value;
        }
        pacing_mode_ = value == "inline" ? PacingMode::Inline : PacingMode::Pipelined;
        if (!value.empty() && value != "auto" && value != "inline" && value != "pipelined") {
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR: unknown frame_pacing \"" << value << "\"; using pipelined" << std::endl;
        }
        // Time kept free between the deadline and the next xrWaitFrame wake-up for the release
        // and xrEndFrame (KARTFRAME_PACING_MARGIN_US, default 1000).
        long margin_us = 1000;
        if (const char* env = std::getenv("KARTFRAME_PACING_MARGIN_US"); env != nullptr && *env != '\0') {
            margin_us = std::clamp(std::strtol(env, nullptr, 10), 0l, 8000l);
        }
        pacing_margin_ = std::chrono::microseconds(margin_us);
        // Pipelined pacing renders sets ahead and ends every frame at its wake-up (lane K, H7.md);
        // KARTFRAME_PIPELINED=wait restores the loop of lanes A to J (one set at a time, each frame
        // ended when its set finished, up to the deadline). KARTFRAME_AHEAD_START_US delays each set's
        // start after the wake-up (default 0; less pose prediction, less margin).
        const char* pipelined_env = std::getenv("KARTFRAME_PIPELINED");
        ahead_wanted_ = pacing_mode_ == PacingMode::Pipelined &&
                        !(pipelined_env != nullptr && std::string_view(pipelined_env) == "wait");
        long start_us = 0;
        if (const char* env = std::getenv("KARTFRAME_AHEAD_START_US"); env != nullptr && *env != '\0') {
            start_us = std::clamp(std::strtol(env, nullptr, 10), 0l, 8000l);
        }
        ahead_start_delay_ = std::chrono::microseconds(start_us);
        // KARTFRAME_END_DELAY_US (test only, default 0): after the wake-up the pacing thread sleeps this
        // long before it ends the frame (same set as without; the next set still starts at the wake-up).
        long end_delay_us = 0;
        if (const char* env = std::getenv("KARTFRAME_END_DELAY_US"); env != nullptr && *env != '\0') {
            end_delay_us = std::clamp(std::strtol(env, nullptr, 10), 0l, 8000l);
        }
        ahead_end_delay_ = std::chrono::microseconds(end_delay_us);
        end_phase_.Configure(end_delay_us);
        // KARTFRAME_AHEAD_REFRESHES=1 (lane M): each set is located for the next refresh's display time
        // instead of two refreshes ahead: one refresh less pose prediction (about 33 ms instead of 42),
        // one refresh less for its encode and GPU work. Up to two sets stay in flight either way.
        const char* lead_env = std::getenv("KARTFRAME_AHEAD_REFRESHES");
        ahead_lead_ = lead_env != nullptr && std::string_view(lead_env) == "1" ? 1 : 2;
        // Every refresh by default: a late set repeats the retained layer with its own poses
        // (the compositor reprojects it). Half rate shows 60 Hz content twice on a 120 Hz panel,
        // the judder and head-turn ghosting the owner rejected; in H1 the old auto rule flapped
        // (16 drops, 15 returns in 4 minutes). KARTFRAME_HALF_RATE=1 always renders at half rate
        // (measurements), =auto drops after 30 s of sustained lateness and stays there for the
        // session, =0 (or unset) never.
        half_rate_policy_ = HalfRatePolicy::Never;
        if (const char* env = std::getenv("KARTFRAME_HALF_RATE"); env != nullptr && *env != '\0') {
            const std::string value(env);
            half_rate_policy_ = value == "auto" ? HalfRatePolicy::Auto
                                : (value == "1" || value == "always") ? HalfRatePolicy::Always
                                                                      : HalfRatePolicy::Never;
        }
    }

    // The session stopped or the loop is leaving the set path: a set still acquired is
    // released once the GPU finished it, never shown.
    bool DropPendingSet() {
        auto& st = set_pacing_;
#if KARTFRAME_AHEAD_PACING
        if (!ahead_sets_.empty()) {
            WithdrawPublishedFrame();
            bool ok = true;
            for (auto& a : ahead_sets_) {
                if (!backend_->DropAheadSet(a.set, std::chrono::seconds(1))) {
                    SetError(backend_->LastError());
                    ok = false;
                }
            }
            ahead_sets_.clear();
            ahead_shown_time_ = 0;
            if (!ok) return false;
        }
#endif
        if (!st.pending) return true;
        WithdrawPublishedFrame();
        st.pending = false;
        if (!backend_->DropSet(st.set)) {
            SetError(backend_->LastError());
            return false;
        }
        return true;
    }

    void NoteSetResult(bool late) {
        auto& st = set_pacing_;
        // The first seconds of a session (loading, pipeline compiles) say nothing about
        // whether the loop holds the rate.
        const auto now = std::chrono::steady_clock::now();
        if (st.first_set == std::chrono::steady_clock::time_point{}) st.first_set = now;
        if (now - st.first_set < std::chrono::seconds(5)) return;
        st.results.push_back(late ? 1 : 0);
        while (st.results.size() > kHalfRateWindow) st.results.pop_front();
    }

    void UpdateHalfRate(XrDuration period) {
        auto& st = set_pacing_;
        const auto late_in_last = [&](size_t count) {
            const size_t n = std::min(count, st.results.size());
            return static_cast<size_t>(std::count(st.results.end() - static_cast<std::ptrdiff_t>(n), st.results.end(), 1));
        };
        const auto now = std::chrono::steady_clock::now();
        if (half_rate_policy_ != HalfRatePolicy::Auto) {
            const bool half = half_rate_policy_ == HalfRatePolicy::Always;
            if (st.half_rate != half) {
                st.half_rate = half;
                pacing::SetMode(std::string(CurrentModeName()) + (half ? "/2" : ""));
            }
            return;
        }
        // Auto (opt-in): more than 5 % of the sets of the last 30 s (at 120 Hz) late drops to every
        // second refresh for the rest of the session; it never returns, so it cannot flap.
        if (!st.half_rate && st.results.size() >= kHalfRateWindow) {
            const size_t late = late_in_last(kHalfRateWindow);
            if (late * 20 > kHalfRateWindow) {
                st.half_rate = true;
                st.half_since = now;
                RT_LOG(RT_TAG_RUNTIME) << "vr pacing: " << late << " of the last " << kHalfRateWindow
                                       << " eye sets late; rendering every second refresh for the rest of the session"
                                       << std::endl;
                pacing::SetMode(std::string(CurrentModeName()) + "/2");
            }
        }
        (void)period;
    }

    // Locates the set for display_time, places the screens and the pointer from its poses,
    // and publishes it to Aurora.
    CycleResult StartSet(const OpenXRPresentation& presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                         XrTime display_time, const OpenXRFrame& cycle) {
        auto& st = set_pacing_;
        OpenXRBackendFrame set{};
        const auto locate_at = std::chrono::steady_clock::now();
        const OpenXRBeginStatus prepared = backend_->PrepareSet(presentation, display_time, cycle, set);
        if (prepared == OpenXRBeginStatus::SessionNotRunning) {
            return CycleResult::Continue;
        }
        if (prepared == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return CycleResult::Break;
        }
        if (prepared == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return CycleResult::Fatal;
        }
        if (diagnostics::Enabled()) {
            NoteFrameDiagnostics(set, immersive);
        }
        if (set.xr_frame.views_valid && !fov_logged_) {
            fov_logged_ = true;
            constexpr double kDeg = 57.29577951308232;
            std::ostringstream fov;
            fov.setf(std::ios::fixed);
            fov.precision(2);
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                const auto& f = set.xr_frame.views[eye].fov;
                fov << (eye ? "; right " : "left ") << f.angleLeft * kDeg << ' ' << f.angleRight * kDeg << ' '
                    << f.angleUp * kDeg << ' ' << f.angleDown * kDeg;
            }
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR eye fields of view (deg, left right up down): " << fov.str() << std::endl;
        }
        // All of these read this set's located head pose.
        ServiceRecenterRequest();
        ApplyPendingReferenceSpaceChange(set.xr_frame);
        UpdateVirtualScreenPose(set);
        const OpenXRPointerScreen panel_screen = SettingsPanelScreen(set, policy, immersive);
        PlacePanelLayer(set, panel_screen);
        if (input_ != nullptr) {
            const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
            input_->Sync(set.xr_frame.predicted_display_time, PointerScreen(set, policy, immersive), panel_screen,
                         InputSeatFrame(immersive));
        }
        if (!set.expects_gpu_submission) {
            return CycleResult::Continue;  // nothing to render: no tracking, or should_render off
        }
        NotePrediction(set.xr_frame.predicted_display_time, locate_at, presentation.mode != OpenXRFrameMode::VirtualScreen);
        st.set = set;
        st.pending = true;
        st.timeouts = 0;
        st.published_at = std::chrono::steady_clock::now();
        {
            const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
            std::lock_guard lock(published_mutex_);
            BuildPublishedFrame(st.set, immersive, policy);
            diagnostics::OnPacketPublished();
            published_.store(&published_frame_, std::memory_order_release);
        }
        aurora_notify_stereo_frame();
        return CycleResult::Continue;
    }

    // Lane R: how far ahead each set's poses are predicted: its display time minus the moment its
    // xrLocateViews was made (steady clock, through the display-time conversion), logged every 5 s
    // (and when the scene changes between menus and race). Sets rendered ahead: about two refreshes
    // plus SteamVR's display lead; GalaxyFrame: about one refresh plus the lead.
    void NotePrediction(XrTime display_time, std::chrono::steady_clock::time_point locate_at, bool race) {
        const uint64_t display_ns = DisplayTimeNanos(display_time);
        if (display_ns == 0) return;
        const auto now = std::chrono::steady_clock::now();
        if (!prediction_ms_.empty() && (prediction_race_ != race || now - prediction_since_ >= std::chrono::seconds(5))) {
            std::sort(prediction_ms_.begin(), prediction_ms_.end());
            char line[200];
            std::snprintf(line, sizeof(line),
                          "vr: poses predicted ahead (%zu sets, %.1f s, %s): display time minus the set's locate p50 %.1f ms, max %.1f",
                          prediction_ms_.size(), std::chrono::duration<double>(now - prediction_since_).count(),
                          prediction_race_ ? "race" : "menus", prediction_ms_[prediction_ms_.size() / 2], prediction_ms_.back());
            RT_LOG(RT_TAG_RUNTIME) << line << std::endl;
            prediction_ms_.clear();
        }
        if (prediction_ms_.empty()) {
            prediction_since_ = now;
            prediction_race_ = race;
        }
        const int64_t locate_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(locate_at.time_since_epoch()).count();
        if (prediction_ms_.size() < 4096)
            prediction_ms_.push_back(static_cast<double>(static_cast<int64_t>(display_ns) - locate_ns) / 1e6);
    }

    // Every compositor cycle: frame timing, the pacing line's tick, missed refreshes. Returns the period.
    XrDuration NoteCycle(const OpenXRFrame& cycle) {
        auto& st = set_pacing_;
        UpdateFrameTiming(cycle);
        const XrDuration period = cycle.predicted_display_period > 0 ? cycle.predicted_display_period : 8'333'333;
        uint32_t missed = 0;
        int64_t step_error = 0;
        if (st.last_display_time != 0 && cycle.predicted_display_time > st.last_display_time) {
            const double steps = static_cast<double>(cycle.predicted_display_time - st.last_display_time) /
                                 static_cast<double>(period);
            if (steps > 1.5) missed = static_cast<uint32_t>(std::lround(steps)) - 1;
            else step_error = cycle.predicted_display_time - st.last_display_time - period;
        }
        {
            const uint64_t display_ns = DisplayTimeNanos(cycle.predicted_display_time);
            const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            pacing::OnCycleTimes(step_error, display_ns != 0 ? static_cast<int64_t>(display_ns) - now_ns : 0);
        }
        st.last_display_time = cycle.predicted_display_time;
        pacing::OnTick(headset_hz_.load(std::memory_order_relaxed), missed);
        return period;
    }

    CycleResult SetCycle(const OpenXRPresentation& presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                         uint32_t interpolation_target, bool& immersive_submission_logged) {
        auto& st = set_pacing_;
        OpenXRFrame cycle{};
        const OpenXRBeginStatus begin = backend_->BeginCycle(cycle);
        if (begin == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return DropPendingSet() ? CycleResult::Continue : CycleResult::Fatal;
        }
        if (begin == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return CycleResult::Break;
        }
        if (begin == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return CycleResult::Fatal;
        }
        const XrDuration period = NoteCycle(cycle);
        const auto deadline = backend_->CycleWakeTime() + std::chrono::nanoseconds(period) - pacing_margin_;
        const bool pipelined = pacing_mode_ == PacingMode::Pipelined;
        st.odd_tick = !st.odd_tick;
        const XrTime set_time = cycle.predicted_display_time + (pipelined ? period : 0);
        const bool render_tick = (!st.half_rate || st.odd_tick) &&
                                 interpolation_pacing_.ShouldRender(set_time, interpolation_target);
        if (!pipelined && !st.pending && render_tick) {
            const CycleResult started = StartSet(presentation, policy, immersive, set_time, cycle);
            if (started != CycleResult::Continue) return started;
        }
        bool shown = false;
        if (st.pending) {
            const OpenXRSubmissionStatus status = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                return backend_->WaitForSubmissionUntil(st.set, deadline);
            });
            if (status == OpenXRSubmissionStatus::Success) {
                pacing::OnSetShown(st.set.xr_frame.predicted_display_time - cycle.predicted_display_time);
                if (!backend_->EndCycleWithSet(st.set)) {
                    SetError(backend_->LastError());
                    return CycleResult::Fatal;
                }
                shown = true;
                st.pending = false;
                if (pacing_debug_ > 0 && st.timeouts > 0) {
                    --pacing_debug_;
                    int64_t consumed = 0, submitted = 0, complete = 0;
                    pacing::LastSetTimes(consumed, submitted, complete);
                    const int64_t published = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        st.published_at.time_since_epoch()).count();
                    const auto ms = [&](int64_t t) { return t ? static_cast<double>(t - published) / 1e6 : -1.0; };
                    RT_LOG(RT_TAG_RUNTIME) << "vr pacing debug: late set shown; after publish: consumed " << ms(consumed)
                        << " ms, submitted " << ms(submitted) << " ms, GPU done " << ms(complete) << " ms" << std::endl;
                }
                WithdrawPublishedFrame();
                // On time: by its refresh (half rate: by the one after).
                if (st.timeouts < (st.half_rate ? 2u : 1u)) NoteSetResult(false);
                pacing::OnRendered();
                diagnostics::OnSubmission(true);
                ++timing_submissions_;
                if (immersive && !immersive_submission_logged) {
                    immersive_submission_logged = true;
                    RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first immersive packet consumed and submitted as "
                                              "an OpenXR projection layer"
                                           << std::endl;
                }
            } else if (status == OpenXRSubmissionStatus::Timeout) {
                ++st.timeouts;
                if (st.timeouts == (st.half_rate ? 2u : 1u)) NoteSetResult(true);
                if (pacing_debug_ > 0 && st.timeouts == 1) {
                    --pacing_debug_;
                    RT_LOG(RT_TAG_RUNTIME) << "vr pacing debug: late set " << st.set.xr_frame.serial << " published "
                        << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - st.published_at).count()
                        << " ms ago, " << (published_.load(std::memory_order_acquire) == nullptr ? "consumed" : "NOT consumed")
                        << ", deadline was " << std::chrono::duration<double, std::milli>(deadline - st.published_at).count()
                        << " ms after publish" << std::endl;
                }
                // Late: due this cycle (half rate: by the cycle after). Half rate is judged by
                // its own deadline: the GPU clocks down at the lower load, so sets that would
                // fit at full rate miss the first refresh there.
                if (st.timeouts == (st.half_rate ? 2u : 1u)) pacing::OnLate();
                // A stalled game (nothing to replay) leaves the packet unconsumed: withdraw it
                // after 50 ms and cancel the set if Aurora has not started encoding it.
                if (std::chrono::steady_clock::now() - st.published_at >= std::chrono::milliseconds(50)) {
                    diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                    if (diagnostics::Measure(diagnostics::Stage::Cancel, [&] { return backend_->CancelSet(st.set); })) {
                        st.pending = false;
                        diagnostics::OnPacketCanceled();
                    }
                }
            } else if (status == OpenXRSubmissionStatus::ShuttingDown) {
                backend_->EndCycleRetained();
                return CycleResult::Break;
            } else {
                // Aurora failed after GPU work may have been queued: those images stay
                // acquired (never shown unfinished) and the session ends.
                diagnostics::OnSubmission(false);
                backend_->EndCycleRetained();
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed; continuing on the mirror output");
                return CycleResult::Fatal;
            }
        }
        if (!shown) {
            if (!backend_->EndCycleRetained()) {
                SetError(backend_->LastError());
                return CycleResult::Fatal;
            }
            pacing::OnRepeat();
            diagnostics::OnKeepaliveRepeat();
        }
        if (pipelined && !st.pending && render_tick) {
            const CycleResult started = StartSet(presentation, policy, immersive, set_time, cycle);
            if (started != CycleResult::Continue) return started;
        }
        UpdateHalfRate(period);
        pacing::Flush();
        return CycleResult::Continue;
    }

#if KARTFRAME_AHEAD_PACING
    // ---- Sets rendered ahead (lane K; the default pipelined pacing) ----
    //
    // The seventh headset session (frame/perf/H7.md): the old pipelined loop started a set only after
    // the last one was shown and ended each frame when its set finished, so in races (encode about
    // 2.5 ms plus GPU about 5 ms, more than a refresh) every frame ended about 1.7 ms later after
    // SteamVR's wake-up than the one before, up to the deadline, then a repeat, then again: a
    // sawtooth from 0 to 10 ms. On head turns the image zigzagged between two positions from refresh
    // to refresh. Frames ended right at the wake-up (menus, GalaxyFrame, PrimeFrame's pipelined loop)
    // looked clean. So now every cycle ends its frame at once: with the newest set due by now whose
    // GPU work is finished (its own poses, this cycle's display time), else with the retained layer;
    // it never waits inside the cycle. Then it starts the set for the display time two refreshes
    // ahead, so each set has the rest of this refresh and all of the next (about 16 ms) for its encode
    // and GPU work while the set before it is shown; one set is on the GPU while the next is encoded.
    // Up to two sets in flight, each on an eye pair of its own (three pairs at least).
    static constexpr size_t kAheadInFlight = 2;

    // Starts the set for ahead_lead_ refreshes after this cycle's display time when one is due and Aurora is
    // free. may_wait: wait for the last set to reach Aurora's queue (bounded by this cycle's end; only after
    // the frame was ended); otherwise only start when it already has. started: a set was published.
    CycleResult TryStartAhead(const OpenXRPresentation& presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                              bool allow_start, const OpenXRFrame& cycle, XrDuration period, XrTime target, bool render_tick,
                              size_t in_flight, bool may_wait, bool& started) {
        auto& st = set_pacing_;
        started = false;
        const XrDuration half = period / 2;
        const bool covered = !ahead_sets_.empty() &&
                             ahead_sets_.back().set.xr_frame.predicted_display_time >= target - half;
        if (!(allow_start && !startup_origin_pending_ && render_tick && !covered && in_flight < kAheadInFlight))
            return CycleResult::Continue;
        // Aurora takes one set at a time: the last one must have reached its queue first. Waiting
        // for that is bounded by this cycle's end (the next xrWaitFrame must not start late).
        const auto deadline = may_wait ? backend_->CycleWakeTime() + std::chrono::nanoseconds(period) - pacing_margin_
                                       : std::chrono::steady_clock::now();
        const bool aurora_free =
            ahead_sets_.empty() ||
            diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                return backend_->PollAheadSet(ahead_sets_.back().set, deadline, false);
            }) == OpenXRSubmissionStatus::Success;
        if (!aurora_free || !backend_->AheadCanPrepare()) return CycleResult::Continue;
        if (may_wait && ahead_start_delay_.count() > 0) {
            std::this_thread::sleep_until(std::min(backend_->CycleWakeTime() + ahead_start_delay_, deadline));
        }
        const CycleResult result = StartSet(presentation, policy, immersive, target, cycle);
        if (st.pending) {
            ahead_sets_.push_back({st.set, st.published_at, false});
            st.pending = false;
            st.set = {};
        }
        started = true;
        return result;
    }

    CycleResult AheadCycle(const OpenXRPresentation& presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                           uint32_t interpolation_target, bool allow_start, bool& immersive_submission_logged) {
        auto& st = set_pacing_;
        OpenXRFrame cycle{};
        const OpenXRBeginStatus begin = backend_->BeginCycle(cycle);
        if (begin == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return DropPendingSet() ? CycleResult::Continue : CycleResult::Fatal;
        }
        if (begin == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return CycleResult::Break;
        }
        if (begin == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return CycleResult::Fatal;
        }
        const XrDuration period = NoteCycle(cycle);
        const XrTime now_display = cycle.predicted_display_time;
        const XrDuration half = period / 2;
        const auto now = std::chrono::steady_clock::now();

        // The newest set due by this refresh that is finished; never wait for one.
        std::ptrdiff_t chosen = -1;
        for (size_t i = 0; i < ahead_sets_.size(); ++i) {
            auto& a = ahead_sets_[i];
            const XrTime t = a.set.xr_frame.predicted_display_time;
            if (t > now_display + half) break;
            const OpenXRSubmissionStatus status = backend_->PollAheadSet(a.set, now);
            if (status == OpenXRSubmissionStatus::Success) {
                if (t > ahead_shown_time_ && backend_->AheadSetCurrent(a.set)) chosen = static_cast<std::ptrdiff_t>(i);
            } else if (status == OpenXRSubmissionStatus::Timeout) {
                if (!a.late_counted) {
                    // Not finished by its own refresh: late (shown later if nothing newer is ready first).
                    a.late_counted = true;
                    pacing::OnLate();
                    NoteSetResult(true);
                }
            } else if (status == OpenXRSubmissionStatus::ShuttingDown) {
                backend_->EndCycleRetained();
                return CycleResult::Break;
            } else {
                // Aurora failed after GPU work may have been queued: those images stay acquired.
                diagnostics::OnSubmission(false);
                backend_->EndCycleRetained();
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed; continuing on the mirror output");
                return CycleResult::Fatal;
            }
        }
        // KARTFRAME_AHEAD_REFRESHES=1 (lane R): GalaxyFrame's order. The set for the next refresh starts
        // at once, before this frame is ended (GalaxyFrame records the next set before releasing the
        // current one), so its encode and GPU work get the whole refresh; it never waits for Aurora (a set
        // still recording is late anyway: then the start below, after the end, waits as before). The
        // set shown now and sets it overtakes leave this cycle and do not count as in flight.
        // The set for two refreshes ahead (KARTFRAME_AHEAD_REFRESHES=1: the next refresh); one tick a cycle.
        const XrTime target = now_display + static_cast<XrDuration>(ahead_lead_) * period;
        st.odd_tick = !st.odd_tick;
        const bool render_tick = (!st.half_rate || st.odd_tick) &&
                                 interpolation_pacing_.ShouldRender(target, interpolation_target);
        bool started_early = false;
        if (ahead_lead_ == 1 || ahead_end_delay_.count() > 0 || end_phase_.Enabled()) {
            const XrTime gone = chosen >= 0 ? ahead_sets_[static_cast<size_t>(chosen)].set.xr_frame.predicted_display_time
                                            : ahead_shown_time_;
            size_t in_flight = 0;
            for (const auto& a : ahead_sets_) in_flight += a.set.xr_frame.predicted_display_time > gone;
            const CycleResult started = TryStartAhead(presentation, policy, immersive, allow_start, cycle, period, target,
                                                      render_tick, in_flight, false, started_early);
            if (started != CycleResult::Continue) {
                backend_->EndCycleRetained();
                return started;
            }
        }
        // KARTFRAME_END_DELAY_US (test only): the next set has started above (or waits for the start after the
        // end if Aurora was busy); the frame is ended this long after the wake-up, bounded by the cycle's end.
        if (end_phase_.Enabled()) {
            // Lane X (KARTFRAME_END_PHASE, test only): the end at a phase of the compositor's grid.
            const auto wake = backend_->CycleWakeTime();
            std::this_thread::sleep_until(
                end_phase_.Target(wake, wake + std::chrono::nanoseconds(period) - pacing_margin_, period));
            end_phase_.NoteEnd(std::chrono::steady_clock::now(), wake);
        } else if (ahead_end_delay_.count() > 0) {
            std::this_thread::sleep_until(std::min(backend_->CycleWakeTime() + ahead_end_delay_,
                                                   backend_->CycleWakeTime() + std::chrono::nanoseconds(period) - pacing_margin_));
        }
        if (chosen >= 0) {
            auto& a = ahead_sets_[static_cast<size_t>(chosen)];
            const XrTime shown_time = a.set.xr_frame.predicted_display_time;
            pacing::OnSetShown(shown_time - now_display);
            if (!a.late_counted) NoteSetResult(false);
            if (!backend_->EndCycleWithAheadSet(a.set)) {
                SetError(backend_->LastError());
                return CycleResult::Fatal;
            }
            ahead_shown_time_ = shown_time;
            ahead_sets_.erase(ahead_sets_.begin() + chosen);
            pacing::OnRendered();
            diagnostics::OnSubmission(true);
            ++timing_submissions_;
            if (immersive && !immersive_submission_logged) {
                immersive_submission_logged = true;
                RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first immersive packet consumed and submitted as "
                                          "an OpenXR projection layer"
                                       << std::endl;
            }
        } else {
            if (!backend_->EndCycleRetained()) {
                SetError(backend_->LastError());
                return CycleResult::Fatal;
            }
            pacing::OnRepeat();
            diagnostics::OnKeepaliveRepeat();
        }

        // Sets that can no longer be shown (overtaken by the one shown, or located in a reference
        // space no longer in use) are released unshown once the GPU is done with them.
        for (size_t i = 0; i < ahead_sets_.size();) {
            auto& a = ahead_sets_[i];
            const bool overtaken = a.set.xr_frame.predicted_display_time <= ahead_shown_time_;
            if ((overtaken || !backend_->AheadSetCurrent(a.set)) &&
                backend_->PollAheadSet(a.set, now) == OpenXRSubmissionStatus::Success) {
                if (!backend_->DropAheadSet(a.set, std::chrono::milliseconds(0))) {
                    SetError(backend_->LastError());
                    return CycleResult::Fatal;
                }
                pacing::OnSetDropped();
                ahead_sets_.erase(ahead_sets_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            ++i;
        }

        // A stalled game (nothing to replay) leaves the newest packet unconsumed: withdraw it after
        // 50 ms and cancel the set if Aurora has not started encoding it.
        if (!ahead_sets_.empty()) {
            auto& last = ahead_sets_.back();
            if (now - last.published_at >= std::chrono::milliseconds(50) &&
                backend_->PollAheadSet(last.set, now, false) == OpenXRSubmissionStatus::Timeout) {
                diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                if (diagnostics::Measure(diagnostics::Stage::Cancel, [&] { return backend_->CancelAheadSet(last.set); })) {
                    ahead_sets_.pop_back();
                    diagnostics::OnPacketCanceled();
                }
            }
        }

        // The set (KARTFRAME_AHEAD_REFRESHES=1: unless it started above).
        if (!started_early) {
            // Lane X: with the end before the compositor's update, a set that starts after the end (Aurora was
            // busy at the wake-up) waits until the update is over: no acquire or locate inside the window.
            if (end_phase_.GetMode() == EndPhaseLock::Mode::Before && end_phase_.Locked()) {
                std::this_thread::sleep_until(std::min(end_phase_.WindowOver(), backend_->CycleWakeTime() +
                                                                                     std::chrono::nanoseconds(period) -
                                                                                     pacing_margin_));
            }
            bool started_now = false;
            const CycleResult started = TryStartAhead(presentation, policy, immersive, allow_start, cycle, period, target,
                                                      render_tick, ahead_sets_.size(), true, started_now);
            if (started != CycleResult::Continue) return started;
        }
        UpdateHalfRate(period);
        pacing::Flush();
        return CycleResult::Continue;
    }
#else
    CycleResult AheadCycle(const OpenXRPresentation& presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                           uint32_t interpolation_target, bool, bool& immersive_submission_logged) {
        return SetCycle(presentation, policy, immersive, interpolation_target, immersive_submission_logged);
    }
#endif

    // One compositor cycle on the retained layer, with no frame left active. False on a fatal
    // backend or runtime failure (the error is recorded).
    bool KeepAlive() {
        const OpenXRBeginStatus status = backend_->KeepAliveCycle();
        if (status == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return true;
        }
        if (status == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return false;
        }
        if (status == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        return true;
    }

    // Render-first pacing (see each backend's PreparePacket). Returns false on a fatal
    // failure; a cycle that ends without a layer returns true and the loop tries again.
    bool RenderFirstCycle(OpenXRPresentation presentation, const MkwVRPolicySnapshot& policy, bool immersive,
                          uint32_t& consecutive_skips, bool& immersive_submission_logged) {
        OpenXRBackendFrame packet{};
        const OpenXRBeginStatus prepared = backend_->PreparePacket(presentation, packet);
        if (prepared == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return true;
        }
        if (prepared == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return false;
        }
        if (prepared == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        // The head pose this packet was located with places the screens and aims the pointer.
        ServiceRecenterRequest();
        ApplyPendingReferenceSpaceChange(packet.xr_frame);
        UpdateVirtualScreenPose(packet);
        const OpenXRPointerScreen panel_screen = SettingsPanelScreen(packet, policy, immersive);
        PlacePanelLayer(packet, panel_screen);
        if (input_ != nullptr) {
            const diagnostics::ScopedStage input_timer(diagnostics::Stage::InputSync);
            input_->Sync(packet.xr_frame.predicted_display_time, PointerScreen(packet, policy, immersive),
                         panel_screen, InputSeatFrame(immersive));
        }
        if (!packet.expects_gpu_submission) {
            // Nothing to render (no rendering requested or no tracking): keep the compositor fed.
            return KeepAlive();
        }
        {
            const diagnostics::ScopedStage publish_timer(diagnostics::Stage::Publish);
            std::lock_guard lock(published_mutex_);
            BuildPublishedFrame(packet, immersive, policy);
            diagnostics::OnPacketPublished();
            published_.store(&published_frame_, std::memory_order_release);
        }
        aurora_notify_stereo_frame();

        // Aurora renders the eyes at its next seal. Meanwhile the compositor keeps showing the
        // retained layer; a 50 ms stall repeats it explicitly and withdraws the packet.
        OpenXRSubmissionStatus submission = OpenXRSubmissionStatus::Timeout;
        bool canceled_before_encode = false;
        const auto cancel_after = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (!stop_.load(std::memory_order_acquire) && submission == OpenXRSubmissionStatus::Timeout) {
            submission = diagnostics::Measure(diagnostics::Stage::SubmissionWait, [&] {
                return backend_->WaitForSubmission(packet, 50);
            });
            if (submission == OpenXRSubmissionStatus::Timeout) {
                if (std::chrono::steady_clock::now() >= cancel_after) {
                    diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
                    canceled_before_encode = diagnostics::Measure(diagnostics::Stage::Cancel, [&] {
                        return backend_->TryCancelPendingPacket(packet);
                    });
                    if (canceled_before_encode) {
                        diagnostics::OnPacketCanceled();
                        break;
                    }
                }
                diagnostics::OnKeepaliveRepeat();
                if (!KeepAlive()) {
                    return false;
                }
            }
        }
        diagnostics::Measure(diagnostics::Stage::Withdraw, [&] { WithdrawPublishedFrame(); });
        if (stop_.load(std::memory_order_acquire) ||
            submission == OpenXRSubmissionStatus::ShuttingDown) {
            return true;
        }
        if (canceled_before_encode) {
            // Refresh display timing after a canceled packet as well, so the
            // next estimate cannot remain stuck in the past during a game stall.
            return KeepAlive();
        }
        if (submission != OpenXRSubmissionStatus::Success) {
            // Nothing reached the shared buffers (Skipped) or Aurora failed after queuing GPU
            // work (Failed): same accounting as the frame-first path, on a keep-alive cycle.
            diagnostics::OnSubmission(false);
            if (submission == OpenXRSubmissionStatus::Failed) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy failed; continuing on the mirror output");
                return false;
            }
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eye copy skipped (" << consecutive_skips
                                       << " in a row): " << backend_->LastError() << std::endl;
            }
            if (consecutive_skips >= kMaxConsecutiveSkips) {
                SetError(std::string("Aurora's ") + kGraphicsBackendName +
                         " stereo copy keeps failing; continuing on the mirror output");
                return false;
            }
            return KeepAlive();
        }

        // The eyes are rendered: begin the compositor frame, complete the backend copy, end.
        OpenXRBackendFrame frame{};
        const OpenXRBeginStatus begin = backend_->BeginFrameForPacket(packet, frame);
        if (begin == OpenXRBeginStatus::SessionNotRunning) {
            MkwVRPolicySetSessionActive(false);
            return true;
        }
        if (begin == OpenXRBeginStatus::ExitRequested) {
            SetError("OpenXR runtime requested session exit; continuing on the mirror output");
            return false;
        }
        if (begin == OpenXRBeginStatus::Error) {
            SetError(backend_->LastError());
            return false;
        }
        UpdateFrameTiming(frame.xr_frame);
        if (diagnostics::Enabled()) {
            NoteFrameDiagnostics(frame, immersive);
        }
        const OpenXRSubmissionStatus copy =
            frame.expects_gpu_submission ? backend_->CopyRenderedEyes(frame) : OpenXRSubmissionStatus::Skipped;
        const bool submit = copy == OpenXRSubmissionStatus::Success;
        diagnostics::OnSubmission(submit);
        if (!backend_->FinishFrame(frame, submit)) {
            SetError(backend_->LastError());
            return false;
        }
        if (copy == OpenXRSubmissionStatus::Failed) {
            SetError(std::string("Aurora's ") + kGraphicsBackendName +
                     " stereo copy failed; continuing on the mirror output");
            return false;
        }
        if (!submit) {
            ++consecutive_skips;
            if (consecutive_skips == 1 || consecutive_skips % 60 == 0) {
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eye copy skipped (" << consecutive_skips
                                       << " in a row): " << backend_->LastError() << std::endl;
            }
            return consecutive_skips < kMaxConsecutiveSkips;
        }
        consecutive_skips = 0;
        ++timing_submissions_;
        if (immersive && !immersive_submission_logged) {
            immersive_submission_logged = true;
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] first immersive packet consumed and submitted as "
                                      "an OpenXR projection layer"
                                   << std::endl;
        }
        return true;
    }

    // Also aims the immersive window's eyes through the window, in `source` itself, so that the layer
    // later built from it shows the eyes as they were rendered.
    void BuildPublishedFrame(OpenXRBackendFrame& source, bool immersive, const MkwVRPolicySnapshot& policy) noexcept {
        const float units_per_meter = policy.EffectiveUnitsPerMeter();
        const uint64_t content_tag = policy.content_tag;
        auto& destination = published_frame_.frame;
        destination = {};
        destination.frameToken = source.xr_frame.serial;
        destination.contentTag = content_tag;
        destination.displayTimeNanos = DisplayTimeNanos(source.xr_frame.predicted_display_time);
        destination.mode = immersive ? AURORA_STEREO_FRAME_IMMERSIVE_REPLAY
                                      : AURORA_STEREO_FRAME_VIRTUAL_SCREEN;
        destination.window = immersive && source.presentation.immersive_window;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const float scale=RuntimeConfigFile::Get().vrAdaptiveResolution?adaptive_scale_:1.f;
            destination.eyes[eye].width = std::max(2u,uint32_t(source.render_width[eye]*scale));
            destination.eyes[eye].height = std::max(2u,uint32_t(source.render_height[eye]*scale));
            IdentityEye(destination.eyes[eye]);
        }
        if (!immersive) {
            last_immersive_ = false;
            BuildMenuUi(source, destination);
            return;
        }

        const bool position_valid =
            (source.xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
        // Latch on the first immersive frame, after a recenter or an origin
        // change, and on re-entry from the virtual screen so a race start
        // recenters a player who shifted during the menus. Position only: the
        // heading and the horizon belong to the reference space, so no
        // transition here can tilt the view or redefine forward.
        if (position_valid && (!base_position_valid_ || !last_immersive_)) {
            base_position_ = CenterPosition(source.xr_frame);
            base_position_valid_ = true;
        }
        last_immersive_ = true;
        if (source.presentation.immersive_window && WindowShapedEyes()) {
            source.presentation.window_eyes = AimEyesThroughWindow(source, policy);
            for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
                destination.eyes[eye].width = source.render_width[eye];
                destination.eyes[eye].height = source.render_height[eye];
            }
        }
        // Read once so both eyes are built from the same angle even if the
        // settings slider moves between them.
        const float lean_back_radians =
            lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ProjectionFromFov(source.xr_frame.views[eye].fov,
                              destination.eyes[eye].projection);
            ViewFromBase(source.xr_frame.views[eye].pose, base_position_,
                         position_valid && base_position_valid_, units_per_meter,
                         lean_back_radians, destination.eyes[eye].viewFromCenter);
        }
        BuildCockpit(source, position_valid, units_per_meter, lean_back_radians, destination.cockpit);
        BuildHandHud(source, destination);
    }

    static void ViewFromPose(const XrPosef& eye, const XrPosef& panel, float out[12]) noexcept {
        const Quaternion q=Conjugate(Normalize({eye.orientation.x,eye.orientation.y,eye.orientation.z,eye.orientation.w}));
        const Quaternion p=Normalize({panel.orientation.x,panel.orientation.y,panel.orientation.z,panel.orientation.w});
        float r[9]; RotationMatrix(Multiply(q,p),r);
        const auto t=Rotate(q,{panel.position.x-eye.position.x,panel.position.y-eye.position.y,panel.position.z-eye.position.z});
        for(int row=0;row<3;++row) { for(int col=0;col<3;++col) out[row*4+col]=r[row*3+col]; out[row*4+3]=t[row]; }
    }
    void BuildMenuUi(const OpenXRBackendFrame& source, AuroraStereoFrame& destination) noexcept {
        XrPosef panel{};
        const bool environment = source.presentation.mode == OpenXRFrameMode::VirtualScreen &&
                                 source.presentation.menu_environment;
        if (!(OpenXRProjectedMenus() || environment) || !MenuScreenPose(source,panel)) {
            OpenXRPublishHandWorkshopTracking({});return;
        }
        // A virtual-screen packet with `window` set asks Aurora for the menu environment: target 0
        // the screen's image as usual, target 1 both eyes side by side with the controllers and the
        // pointer around the screen (the flag has no other meaning outside immersive replay).
        destination.window = environment;
        destination.ui.anchored=true;
        destination.ui.distance=source.presentation.quad_distance_meters;
        destination.ui.width=source.presentation.quad_width_meters;
        // vrui's origin is behind the visible panel at z=-distance.
        const Quaternion rotation{panel.orientation.x,panel.orientation.y,panel.orientation.z,panel.orientation.w};
        const auto ahead=Rotate(rotation,{0,0,destination.ui.distance});
        panel.position.x+=ahead[0];panel.position.y+=ahead[1];panel.position.z+=ahead[2];
        const auto input=OpenXRReadUiSnapshot();
        const Quaternion inverse=Conjugate(rotation);
        for(int hand=0;hand<2;++hand) {
            const auto& h=input.hands[hand]; const auto& aim=input.aims[hand];
            destination.ui.tracked[hand]=input.active && h.valid;
            destination.ui.pointerTracked[hand]=input.active && aim.valid;
            const auto translate=[&](const std::array<float,3>& position) {
                return Rotate(inverse,{position[0]-panel.position.x,position[1]-panel.position.y,position[2]-panel.position.z});
            };
            const auto pos=translate(h.position),right=Rotate(inverse,h.right),up=Rotate(inverse,h.up),forward=Rotate(inverse,h.forward);
            auto* m=destination.ui.panelFromGrip[hand];
            for(int row=0;row<3;++row) {m[row*4]=right[row];m[row*4+1]=up[row];m[row*4+2]=-forward[row];m[row*4+3]=pos[row];}
            const auto a=translate(aim.position),dir=Rotate(inverse,aim.forward);
            for(int axis=0;axis<3;++axis) { destination.ui.pointerRay[hand][axis]=a[axis];destination.ui.pointerRay[hand][axis+3]=dir[axis]; }
        }
        for(int eye=0;eye<2;++eye) {
            ProjectionFromFov(source.xr_frame.views[eye].fov,destination.eyes[eye].projection);
            ViewFromPose(source.xr_frame.views[eye].pose,panel,destination.ui.eyeFromPanel[eye]);
        }
        HandWorkshopTracking workshop{};
        for(unsigned hand=0;hand<2;++hand) {
            workshop.tracked[hand]=destination.ui.tracked[hand];
            std::copy_n(destination.ui.panelFromGrip[hand],12,workshop.panel_from_grip[hand].begin());
        }
        OpenXRPublishHandWorkshopTracking(workshop);
    }
    void BuildHandHud(const OpenXRBackendFrame& source, AuroraStereoFrame& destination) noexcept {
        const auto input=OpenXRReadUiSnapshot();
        if (!RuntimeConfigFile::Get().vrHandHud || MkwVRGetCameraMode()==CameraMode::FirstPerson ||
            !input.active || !input.hands[0].valid) return;
        const auto& h=input.hands[0];
        const auto head=CenterPosition(source.xr_frame);
        const std::array<float,3> center{h.position[0]+h.up[0]*.06f,h.position[1]+h.up[1]*.06f,h.position[2]+h.up[2]*.06f};
        const float dx=head[0]-center[0],dy=head[1]-center[1],dz=head[2]-center[2];
        const float yaw=std::atan2(dx,dz),pitch=-std::atan2(dy,std::sqrt(dx*dx+dz*dz));
        const auto rotation=Multiply({0,std::sin(yaw/2),0,std::cos(yaw/2)},{std::sin(pitch/2),0,0,std::cos(pitch/2)});
        const auto normal=Rotate(rotation,{0,0,1});
        const XrPosef panel{{rotation.x,rotation.y,rotation.z,rotation.w},
            {center[0]+normal[0],center[1]+normal[1],center[2]+normal[2]}};
        destination.handHud=true;
        for(int eye=0;eye<2;++eye) ViewFromPose(source.xr_frame.views[eye].pose,panel,destination.handHudViewFromPanel[eye]);
    }

    // The first-person cockpit's hands and separate wheel, in the seated frame
    // the eye transforms place at base + lean * seat (metres). Always carries
    // the packet's world scale, which Aurora rescales to the sealed frame's.
    void BuildCockpit(const OpenXRBackendFrame& source, bool position_valid, float units_per_meter,
                      float lean_back_radians, AuroraCockpit& cockpit) noexcept {
        cockpit.unitsPerMeter = units_per_meter;
        const DrivingSnapshot driving = input_ != nullptr ? input_->Driving() : DrivingSnapshot{};
        const auto calibration=OpenXRReadBodyHandCalibration();
        if ((driving.hand_steering || RuntimeConfigFile::VrBodyIk()) && !hand_meshes_loaded_ && runtime_ != nullptr) {
            hand_meshes_loaded_ = true;
            const bool loaded = LoadRuntimeHandMeshes(*runtime_);
            RT_LOG(RT_TAG_RUNTIME) << "[mkw-vr] cockpit hands: "
                                   << (loaded ? "the runtime's hand mesh" : "procedural gloves (no runtime hand mesh)")
                                   << std::endl;
        }
        cockpit.active = driving.cockpit_active && position_valid && base_position_valid_ &&
                         (driving.synthetic_control || driving.hand_steering || RuntimeConfigFile::VrBodyIk());
        if (!cockpit.active) {
            return;
        }
        cockpit.wheelAngle = driving.visual_angle;
        cockpit.nativeWheel = calibration.active || !driving.synthetic_control;
        cockpit.bike = driving.bike;
        cockpit.handlebarRadius = driving.control.radius;
        for (int row = 0; row < 3; ++row) {
            cockpit.seatFromHandlebar[row * 4 + 0] = driving.control.right[row];
            cockpit.seatFromHandlebar[row * 4 + 1] = driving.control.up[row];
            cockpit.seatFromHandlebar[row * 4 + 2] = driving.control.normal[row];
            cockpit.seatFromHandlebar[row * 4 + 3] = driving.control.center[row];
        }
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            ViewFromBase(source.xr_frame.views[eye].pose, base_position_, true, 1.0f, lean_back_radians,
                         cockpit.eyeFromSeat[eye]);
        }
        for (size_t hand = 0; hand < 2; ++hand) {
            auto& target = cockpit.hands[hand];
            const auto& from = driving.hands[hand];
            // A hand the character model draws is still located: it carries the held item
            // (Aurora draws no hand of its own there).
            target.tracked = from.tracked;
            target.characterHand = !calibration.active && MkwVRCharacterHandActive(hand);
            target.calibrationMarker=calibration.active;
            target.calibrationCaptured=calibration.captured[hand];
            target.held = from.held;
            target.squeeze = from.squeeze;
            std::copy(from.seat_from_grip.begin(), from.seat_from_grip.end(), target.seatFromGrip);
        }
    }

    // The immersive window's eyes, aimed through the window itself: each keeps its position but looks
    // square-on at the window's plane, through an off-axis frustum just around the window, so its
    // image is the window and nothing outside it is rendered. The image keeps the display's pixel
    // density at the window's size seen from the race origin (fixed while the window's geometry is),
    // plus a two-pixel border that Aurora's mask leaves transparent, so the compositor finds nothing
    // at the image's edge. The frame's views carry this pose and field of view to the projection
    // layer, which the compositor reprojects like any other. False, with nothing changed, when the
    // window cannot be placed or an eye is not in front of it.
    bool AimEyesThroughWindow(OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy) const noexcept {
        const float distance = policy.config.hud_distance_meters;
        const float half_width = 0.5f * policy.config.hud_width_meters;
        XrPosef window{};
        if (!(half_width > 0.0f) || !(distance > 0.0f) || !RaceScreenPose(frame, policy, window)) {
            return false;
        }
        float picture_aspect = 0.0f;
        float snapshot_aspect = 0.0f;
        if (!aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect) || !(picture_aspect > 0.0f)) {
            picture_aspect = 16.0f / 9.0f; // stereo_hud_screen's own fallback
        }
        const float half_height = half_width / picture_aspect;
        const Quaternion to_window =
            Conjugate(Normalize({window.orientation.x, window.orientation.y, window.orientation.z, window.orientation.w}));
        constexpr uint32_t kBorder = 2;
        std::array<XrFovf, kOpenXREyeCount> fov{};
        std::array<uint32_t, kOpenXREyeCount> width{};
        std::array<uint32_t, kOpenXREyeCount> height{};
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            const XrView& view = frame.xr_frame.views[eye];
            // The eye in the window's frame, whose +Z faces the viewer.
            const std::array<float, 3> at = Rotate(
                to_window, {view.pose.position.x - window.position.x, view.pose.position.y - window.position.y,
                            view.pose.position.z - window.position.z});
            const float located_x = std::tan(view.fov.angleRight) - std::tan(view.fov.angleLeft);
            const float located_y = std::tan(view.fov.angleUp) - std::tan(view.fov.angleDown);
            if (!(at[2] > 0.05f) || !(located_x > 0.0f) || !(located_y > 0.0f) || frame.render_width[eye] <= 2 * kBorder ||
                frame.render_height[eye] <= 2 * kBorder) {
                return false;
            }
            // The display's pixels per unit of tangent, as the eye was located, across the window's
            // tangent extent seen straight on from the race origin.
            const auto pixels = [&](uint32_t full, float located, float half_extent) {
                const float content = std::floor(static_cast<float>(full) / located * (2.0f * half_extent / distance));
                return std::clamp<uint32_t>(static_cast<uint32_t>(std::max(content, 1.0f)) + 2 * kBorder, 2 * kBorder + 1,
                                            full);
            };
            width[eye] = pixels(frame.render_width[eye], located_x, half_width);
            height[eye] = pixels(frame.render_height[eye], located_y, half_height);
            // This frame's frustum: the window's edges seen from where the eye is, widened by the border.
            const float left = (-half_width - at[0]) / at[2];
            const float right = (half_width - at[0]) / at[2];
            const float down = (-half_height - at[1]) / at[2];
            const float up = (half_height - at[1]) / at[2];
            const float border_x = (right - left) * kBorder / static_cast<float>(width[eye] - 2 * kBorder);
            const float border_y = (up - down) * kBorder / static_cast<float>(height[eye] - 2 * kBorder);
            fov[eye].angleLeft = std::atan(left - border_x);
            fov[eye].angleRight = std::atan(right + border_x);
            fov[eye].angleUp = std::atan(up + border_y);
            fov[eye].angleDown = std::atan(down - border_y);
        }
        for (uint32_t eye = 0; eye < kOpenXREyeCount; ++eye) {
            frame.xr_frame.views[eye].pose.orientation = window.orientation;
            frame.xr_frame.views[eye].fov = fov[eye];
            frame.render_width[eye] = width[eye];
            frame.render_height[eye] = height[eye];
        }
        return true;
    }

    bool WindowShapedEyes() const noexcept {
#if defined(__ANDROID__)
        return kWindowShapedEyesSupported && !window_eyes_forced_off_;
#else
        return kWindowShapedEyesSupported;
#endif
    }

    // The seated frame the controllers are located in for hand steering: the
    // immersive base the eye transforms use, from the previous frame (this
    // frame's is latched after input).
    driving::SeatFrame InputSeatFrame(bool immersive) const noexcept {
        driving::SeatFrame seat;
        seat.valid = immersive && base_position_valid_ && last_immersive_;
        seat.base = base_position_;
        seat.lean_back_radians = lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        return seat;
    }

    // Runs once per located frame, before the virtual screen is placed and
    // before the immersive origin is latched, so a recenter reaches both from
    // this frame's head pose rather than the next one's.
    void ServiceRecenterRequest() noexcept {
        if (recenter_requested_.exchange(false, std::memory_order_acq_rel)) {
            ResetTrackingOrigin();
        }
    }

    // Anchors the menu screen in the application space and holds it there. The
    // pose is captured once, from the first frame whose head pose is good enough
    // to place it, and released again by a recenter or an origin change.
    void UpdateVirtualScreenPose(OpenXRBackendFrame& frame) noexcept {
        if (frame.presentation.mode != OpenXRFrameMode::VirtualScreen && !OpenXRProjectedMenus()) {
            return;
        }
        constexpr XrViewStateFlags kPoseUsable =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        // Initial valid poses may precede focus and positional tracking. Do
        // not retain those startup/floor-origin samples as the user's seat.
        const bool tracked = FocusHeld() && frame.xr_frame.views_valid &&
            (frame.xr_frame.view_state_flags & (kPoseUsable | XR_VIEW_STATE_POSITION_TRACKED_BIT |
             XR_VIEW_STATE_ORIENTATION_TRACKED_BIT)) == (kPoseUsable | XR_VIEW_STATE_POSITION_TRACKED_BIT |
             XR_VIEW_STATE_ORIENTATION_TRACKED_BIT);
        const auto now=std::chrono::steady_clock::now();
        if (!tracked) menu_tracking_started_={};
        else if (menu_tracking_started_==std::chrono::steady_clock::time_point{}) menu_tracking_started_=now;
        if (!virtual_screen_pose_valid_ && tracked &&
            now-menu_tracking_started_ >= std::chrono::milliseconds(500)) {
            if(!startup_origin_checked_) {
                startup_origin_checked_=true;
                const auto& q=frame.xr_frame.views[0].pose.orientation;
                bool initializeSeat=StartupReferenceInverted({q.x,q.y,q.z,q.w});
#if defined(_WIN32) || defined(MKW_OPENXR_LINUX_DESKTOP)
                // PC runtimes may start with the previous standing-space origin.
                initializeSeat=true;
#endif
                if(initializeSeat) {
                    const auto center=CenterPosition(frame.xr_frame);
                    const auto origin=StartupMenuOrigin({q.x,q.y,q.z,q.w},center);
                    startup_origin_pose_={{origin.orientation[0],origin.orientation[1],origin.orientation[2],origin.orientation[3]},
                        {origin.position[0],origin.position[1],origin.position[2]}};
                    startup_origin_pending_=true;
                    frame.presentation.quad_anchored=false;
                    return;
                }
            }
            virtual_screen_pose_ = ScreenPoseAhead(
                frame.xr_frame, std::max(0.25f, frame.presentation.quad_distance_meters));
            virtual_screen_pose_valid_ = true;
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR upright menu anchor latched after tracking: "
                << virtual_screen_pose_.position.x << ", " << virtual_screen_pose_.position.y
                << ", " << virtual_screen_pose_.position.z << std::endl;
        }
        frame.presentation.quad_anchored = virtual_screen_pose_valid_;
        frame.presentation.quad_pose = virtual_screen_pose_;
    }

    // The rectangle the game picture covers on the screen this frame shows, in
    // the application space, for the Wii Remote pointer to aim at.
    //
    // Menus: the quad layer UpdateVirtualScreenPose placed (or its head-locked
    // fallback), sized like the backends size it: hud_width_meters across with
    // the eye texture's aspect, the desktop snapshot letterboxed into it and the
    // picture into the snapshot.
    //
    // Races: the 2D layer's screen, which Aurora hangs hud_distance_meters
    // ahead in the recorded centre-eye space. ViewFromBase maps a point p of
    // that space (in metres) to base + lean * p in the application space, so the
    // screen sits at base + lean * (0, 0, -distance), turned by the lean, its
    // height following the picture aspect as stereo_hud_screen's does. With the
    // 2D layer stretched across the eyes there is no screen to point at, except
    // in the immersive window, which is that screen and always carries the layer.
    OpenXRPointerScreen PointerScreen(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                                      bool immersive) const noexcept {
        OpenXRPointerScreen screen{};
        float picture_aspect = 0.0f;
        float snapshot_aspect = 0.0f;
        if (!aurora_get_stereo_screen_aspects(&picture_aspect, &snapshot_aspect)) {
            return screen;
        }

        if (immersive) {
            // The immersive window always carries the 2D layer.
            const bool on_screen = aurora_get_stereo_hud_screen_enabled() || frame.presentation.immersive_window;
            if (!on_screen || !(policy.config.hud_width_meters > 0.0f) || !RaceScreenPose(frame, policy, screen.pose)) {
                return screen;
            }
            screen.half_width_meters = 0.5f * policy.config.hud_width_meters;
            screen.half_height_meters = screen.half_width_meters / picture_aspect;
            screen.valid = true;
            return screen;
        }

        if (frame.render_width[0] == 0 || frame.render_height[0] == 0 || !MenuScreenPose(frame, screen.pose)) {
            return screen;
        }
        if (OpenXRProjectedMenus()) {
            screen.half_width_meters=.5f*frame.presentation.quad_width_meters;
            screen.half_height_meters=screen.half_width_meters/snapshot_aspect;
            screen.valid=true;return screen;
        }
        const float eye_aspect =
            static_cast<float>(frame.render_width[0]) / static_cast<float>(frame.render_height[0]);
        const std::array<float, 2> extents = wii_remote::MenuPictureHalfExtents(
            std::max(0.25f, frame.presentation.quad_width_meters), eye_aspect, snapshot_aspect, picture_aspect);
        screen.half_width_meters = extents[0];
        screen.half_height_meters = extents[1];
        screen.valid = true;
        return screen;
    }

    // The settings panel's rectangle: centred on the same screen, a fixed
    // fraction of its width, the way Aurora lays it over the eyes
    // (aurora_imgui_set_stereo_overlay). Unlike the pointer's picture it is
    // there in a race even when the 2D layer is not on the screen.
    OpenXRPointerScreen SettingsPanelScreen(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                                            bool immersive) const noexcept {
        OpenXRPointerScreen screen{};
        const float screen_width =
            immersive ? policy.config.hud_width_meters : std::max(0.25f, frame.presentation.quad_width_meters);
        if (!(screen_width > 0.0f) ||
            !(immersive ? RaceScreenPose(frame, policy, screen.pose) : MenuScreenPose(frame, screen.pose))) {
            return screen;
        }
        const std::array<float, 2> extents = settings_panel::HalfExtents(screen_width);
        screen.half_width_meters = extents[0];
        screen.half_height_meters = extents[1];
        screen.valid = true;
        return screen;
    }

    // Android: `adb shell setprop debug.wiicompiled.panel_layer 0` draws the settings panel into
    // the eyes again, to compare the two ways or to rule out a runtime's quad layers. Read about
    // once a second.
    // The menus' environment layer (controllers and pointer around the screen's quad): the Frame's
    // Vulkan backend only.
    bool MenuEnvironmentAvailable() const noexcept {
#if defined(MKW_OPENXR_LINUX_DESKTOP)
        return backend_ != nullptr && backend_->MenuEnvironmentAvailable();
#else
        return false;
#endif
    }

    bool PanelLayerForcedOff() noexcept {
#if defined(__ANDROID__)
        if (panel_layer_poll_ == 0) {
            panel_layer_poll_ = 72;
            char value[PROP_VALUE_MAX] = {};
            const bool off =
                __system_property_get("debug.wiicompiled.panel_layer", value) > 0 && value[0] == '0';
            if (off != panel_layer_forced_off_) {
                panel_layer_forced_off_ = off;
                RT_LOG(RT_TAG_RUNTIME) << "OpenXR: settings panel "
                                       << (off ? "drawn into the eyes" : "on its own layer")
                                       << " (debug.wiicompiled.panel_layer)" << std::endl;
            }
        }
        --panel_layer_poll_;
        return panel_layer_forced_off_;
#else
        return false;
#endif
    }

    // Android: `adb shell setprop debug.wiicompiled.eye_passes 0` replays each eye in one render
    // pass per recorded pass again, and 1 forces the single pass, to compare the two within one
    // session. An empty value hands the switch back to the settings. Read about once a second.
    void PollEyePassesOverride() noexcept {
#if defined(__ANDROID__)
        if (eye_passes_poll_ != 0) {
            --eye_passes_poll_;
            return;
        }
        eye_passes_poll_ = 72;
        char value[PROP_VALUE_MAX] = {};
        int override_value = -1;
        if (__system_property_get("debug.wiicompiled.eye_passes", value) > 0 &&
            (value[0] == '0' || value[0] == '1')) {
            override_value = value[0] - '0';
        }
        if (override_value == eye_passes_override_) {
            return;
        }
        eye_passes_override_ = override_value;
        const bool single =
            override_value >= 0 ? override_value == 1 : RuntimeConfigFile::VrSinglePassEyes();
        aurora_set_stereo_single_pass_eyes(single);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: eyes replayed in "
                               << (single ? "one render pass" : "one render pass per recorded pass")
                               << (override_value >= 0 ? " (debug.wiicompiled.eye_passes)"
                                                       : " (debug.wiicompiled.eye_passes cleared)")
                               << std::endl;
#endif
    }

    // Android: `adb shell setprop debug.wiicompiled.foveation <0-3>` overrides the foveation level
    // (off, low, medium, high) within one session, for A/B timing; an empty value hands it back to
    // the settings. Levels need a session launched with foveation on (see VrFoveation). Read about
    // once a second.
    void PollFoveationOverride() noexcept {
#if defined(__ANDROID__)
        if (foveation_poll_ != 0) {
            --foveation_poll_;
            return;
        }
        foveation_poll_ = 72;
        char value[PROP_VALUE_MAX] = {};
        int override_value = -1;
        if (__system_property_get("debug.wiicompiled.foveation", value) > 0 && value[0] >= '0' &&
            value[0] <= '3' && value[1] == '\0') {
            override_value = value[0] - '0';
        }
        if (override_value == foveation_override_) {
            return;
        }
        foveation_override_ = override_value;
        const uint32_t level = override_value >= 0
                                   ? static_cast<uint32_t>(override_value)
                                   : RuntimeConfigFile::VrFoveationLevelIndex(RuntimeConfigFile::VrFoveation());
        aurora_set_stereo_foveation(level);
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: foveation " << RuntimeConfigFile::kVrFoveationLevels[level]
                               << (override_value >= 0 ? " (debug.wiicompiled.foveation)"
                                                       : " (debug.wiicompiled.foveation cleared)")
                               << (aurora_stereo_foveation_available() ? "" : "; unavailable in this session")
                               << std::endl;
#endif
    }

    // Android: `adb shell setprop debug.wiicompiled.window_eyes 0` renders the immersive window's eyes
    // whole and only masks them, as the PC does, to compare the cost within one session; an empty
    // value aims them through the window again. Read about once a second.
    void PollWindowEyesOverride() noexcept {
#if defined(__ANDROID__)
        if (window_eyes_poll_ != 0) {
            --window_eyes_poll_;
            return;
        }
        window_eyes_poll_ = 72;
        char value[PROP_VALUE_MAX] = {};
        const bool off = __system_property_get("debug.wiicompiled.window_eyes", value) > 0 && value[0] == '0';
        if (off != window_eyes_forced_off_) {
            window_eyes_forced_off_ = off;
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR: immersive window eyes "
                                   << (off ? "rendered whole and masked" : "aimed through the window")
                                   << " (debug.wiicompiled.window_eyes)" << std::endl;
        }
#endif
    }

    // The settings panel's layer hangs exactly where its pointer hits are
    // tested, the rectangle it used to cover in the eyes.
    static void PlacePanelLayer(OpenXRBackendFrame& frame, const OpenXRPointerScreen& screen) noexcept {
        OpenXRPanelLayer& panel = frame.presentation.panel;
        panel.placed = panel.requested && screen.valid;
        if (panel.placed) {
            panel.pose = screen.pose;
            panel.width_meters = 2.0f * screen.half_width_meters;
            panel.height_meters = 2.0f * screen.half_height_meters;
        }
    }

    // Centre of the race's 2D screen. ViewFromBase maps a point p of the
    // recorded centre-eye space (in metres) to base + lean * p in the
    // application space, so the screen Aurora hangs hud_distance_meters ahead
    // sits at base + lean * (0, 0, -distance), turned by the lean.
    bool RaceScreenPose(const OpenXRBackendFrame& frame, const MkwVRPolicySnapshot& policy,
                        XrPosef& pose) const noexcept {
        const OpenXRFrame& xr_frame = frame.xr_frame;
        const float distance = policy.config.hud_distance_meters;
        if (!(distance > 0.0f)) {
            return false;
        }
        std::array<float, 3> base{};
        if (base_position_valid_ && last_immersive_) {
            base = base_position_;
        } else if (xr_frame.views_valid &&
                   (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                   (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            // BuildPublishedFrame latches exactly this for the frame.
            base = CenterPosition(xr_frame);
        } else {
            return false;
        }
        const float half_angle = 0.5f * lean_back_degrees_.load(std::memory_order_relaxed) * kDegreesToRadians;
        const Quaternion lean{std::sin(half_angle), 0.0f, 0.0f, std::cos(half_angle)};
        const std::array<float, 3> ahead = Rotate(lean, {0.0f, 0.0f, -distance});
        pose.orientation = {lean.x, lean.y, lean.z, lean.w};
        pose.position = {base[0] + ahead[0], base[1] + ahead[1], base[2] + ahead[2]};
        return true;
    }

    // Centre of the menu quad: where UpdateVirtualScreenPose anchored it, or
    // its head-locked fallback straight ahead of the head.
    bool MenuScreenPose(const OpenXRBackendFrame& frame, XrPosef& pose) const noexcept {
        if (frame.presentation.immersive_window) {
            return false;
        }
        if (frame.presentation.quad_anchored) {
            pose = frame.presentation.quad_pose;
            return true;
        }
        const OpenXRFrame& xr_frame = frame.xr_frame;
        if (!xr_frame.views_valid || (xr_frame.view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0 ||
            (xr_frame.view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
            return false;
        }
        pose = ScreenPoseAhead(xr_frame, std::max(0.25f, frame.presentation.quad_distance_meters));
        return true;
    }

    void ApplyPendingReferenceSpaceChange(const OpenXRFrame& frame) noexcept {
        bool external=false;
        if (runtime_->ConsumeAppSpaceChangesThrough(frame.predicted_display_time, &external)) {
            if (external && startup_origin_applied_) {
                // Runtime recenter replaced both the room's height and axes.
                // Remove our entire startup offset before anchoring again.
                startup_origin_pose_={{0,0,0,1},{0,0,0}};
                startup_origin_pending_=true;
            }
            ResetTrackingOrigin();
        }
    }

    // SteamVR takes focus for a few milliseconds now and then (VISIBLE, then FOCUSED again).
    // A loss shorter than a second is not a pause: the screen placement and the frame loop
    // carry on as if focused.
    bool FocusHeld() noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (runtime_->IsSessionFocused()) {
            last_focused_ = now;
            return true;
        }
        return runtime_->IsSessionVisible() && last_focused_ != std::chrono::steady_clock::time_point{} &&
               now - last_focused_ < std::chrono::seconds(1);
    }

    void ResetTrackingOrigin() noexcept {
        base_position_ = {};
        base_position_valid_ = false;
        last_immersive_ = false;
        // The anchored menu screen is placed in the same space, so it is stale
        // for exactly the same reasons and is re-placed on the next frame.
        virtual_screen_pose_valid_ = false;
        menu_tracking_started_ = {};
    }

    void SetInterpolationActive(bool active) noexcept {
        std::lock_guard lock(interpolation_mutex_);
        aurora_set_stereo_frame_interpolation(active && !interpolation_stopping_);
    }

    void UpdateFrameTiming(const OpenXRFrame& frame) noexcept {
        float hz = 0;
        if (get_display_refresh_rate_ == nullptr ||
            XR_FAILED(get_display_refresh_rate_(runtime_->Session(), &hz)) || !(hz > 0)) {
            if (frame.predicted_display_period > 0)
                hz = static_cast<float>(1.0e9 / static_cast<double>(frame.predicted_display_period));
        }
        headset_hz_.store(hz, std::memory_order_relaxed);
        if (hz > 0.0f && std::fabs(hz - logged_refresh_hz_) > 0.5f) {
            logged_refresh_hz_ = hz;
            RT_LOG(RT_TAG_RUNTIME) << "display refresh rate now " << hz << " Hz" << std::endl;
        }
        const auto now = std::chrono::steady_clock::now();
        const float elapsed = std::chrono::duration<float>(now - timing_start_).count();
        if (elapsed >= 1.0f) {
            rendered_fps_.store(static_cast<float>(timing_submissions_) / elapsed, std::memory_order_relaxed);
            adaptive_scale_=adaptive_resolution_.Observe(static_cast<float>(timing_submissions_)/elapsed,
                aurora_get_stereo_frame_interpolation()?hz:std::min(hz,60.f),RuntimeConfigFile::Get().vrAdaptiveResolution);
            timing_start_ = now;
            timing_submissions_ = 0;
        }
    }

    // Pacing thread, only while diagnostics are on.
    void NoteFrameDiagnostics(const OpenXRBackendFrame& frame, bool immersive) {
        const XrViewStateFlags flags = frame.xr_frame.view_state_flags;
        diagnostics::OnFrameBegun(immersive, frame.xr_frame.should_render, frame.xr_frame.views_valid,
                                  (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0,
                                  (flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
        if (diagnostics::ConsumeSessionInfoRequest()) {
            LogDiagnosticSession(frame);
        }
        // The immersive window's eyes are aimed through the window, so their fields of view follow
        // the head and are no longer the headset's.
        if (frame.xr_frame.should_render && frame.xr_frame.views_valid && !frame.presentation.window_eyes) {
            diagnostics::OnViewGeometry(DiagnosticViewGeometry(frame));
        }
    }

    // Everything about the headset and runtime that a pacing report is read
    // against. Written when logging starts and again for every new session.
    void LogDiagnosticSession(const OpenXRBackendFrame& frame) const {
        const auto& info = runtime_->RuntimeInfo();
        std::ostringstream line;
        line << "session: runtime '" << info.runtime_name << "' " << XR_VERSION_MAJOR(info.runtime_version) << '.'
             << XR_VERSION_MINOR(info.runtime_version) << '.' << XR_VERSION_PATCH(info.runtime_version)
             << ", system '" << info.system_name << "', vendor 0x" << std::hex << info.vendor_id << std::dec
             << ", orientation tracking " << info.supports_orientation_tracking << ", position tracking "
             << info.supports_position_tracking << ", max layers " << info.max_layer_count;
        diagnostics::Info(line.str());

        line.str({});
        line << "session: " << kGraphicsBackendName << " backend, reference space "
             << DiagnosticSpaceName(runtime_->AppSpaceType()) << ", blend mode "
             << DiagnosticBlendModeName(runtime_->EnvironmentBlendMode()) << ", extensions";
        for (const std::string& extension : runtime_->EnabledExtensions()) {
            line << ' ' << extension;
        }
        diagnostics::Info(line.str());

        line.str({});
        const auto& views = runtime_->ViewConfiguration();
        line << "session: recommended eye size " << views[0].properties.recommendedImageRectWidth << 'x'
             << views[0].properties.recommendedImageRectHeight << " / "
             << views[1].properties.recommendedImageRectWidth << 'x'
             << views[1].properties.recommendedImageRectHeight << ", max "
             << views[0].properties.maxImageRectWidth << 'x' << views[0].properties.maxImageRectHeight
             << ", render_scale " << runtime_->Config().resolution_scale << ", swapchains "
             << views[0].render_width << 'x' << views[0].render_height << " / " << views[1].render_width << 'x'
             << views[1].render_height;
        diagnostics::Info(line.str());

        line.str({});
        const uint32_t interpolation = frame_interpolation_fps_.load(std::memory_order_relaxed);
        line << "session: display period ";
        if (frame.xr_frame.predicted_display_period > 0) {
            const double period_ms = static_cast<double>(frame.xr_frame.predicted_display_period) / 1.0e6;
            line << period_ms << " ms (" << 1000.0 / period_ms << " Hz)";
        } else {
            line << "unknown";
        }
        line << ", refresh-rate extension " << (get_display_refresh_rate_ != nullptr ? "yes" : "no")
             << ", display-time conversion " << (convert_display_time_ != nullptr ? "yes" : "no")
             << ", VR frame interpolation "
             << (interpolation == 0 ? std::string("off")
                 : interpolation == 1 ? std::string("auto")
                                      : std::to_string(interpolation))
             << (FrameInterpolationAvailable() ? "" : " (unavailable)");
        diagnostics::Info(line.str());
    }

    // Converts the compositor's predicted display time onto the runtime's
    // steady clock, which is what Aurora's interpolation deadlines are paced by.
    uint64_t DisplayTimeNanos(XrTime display_time) noexcept {
#if defined(__ANDROID__) || defined(MKW_OPENXR_LINUX_DESKTOP)
        if (convert_display_time_ == nullptr) {
            timespec now_spec{};
            if (clock_gettime(CLOCK_MONOTONIC, &now_spec) != 0) return 0;
            const int64_t monotonic_ns = static_cast<int64_t>(now_spec.tv_sec) * 1'000'000'000ll + now_spec.tv_nsec;
            const auto steady_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t delta = MonotonicDisplayDelta(display_time, monotonic_ns);
            return steady_ns + delta > 0 ? static_cast<uint64_t>(steady_ns + delta) : 0;
        }
#endif
        if (convert_display_time_ == nullptr) return 0;
        const auto now = std::chrono::steady_clock::now();
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
#if defined(_WIN32)
        LARGE_INTEGER display_counter{}, counter{}, frequency{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_counter)) ||
            !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
            !QueryPerformanceCounter(&counter)) return 0;
        const auto delta = static_cast<int64_t>(
            (static_cast<double>(display_counter.QuadPart) - static_cast<double>(counter.QuadPart)) *
            1.0e9 / static_cast<double>(frequency.QuadPart));
        return now_ns + delta > 0 ? static_cast<uint64_t>(now_ns + delta) : 0;
#else
        // XR_KHR_convert_timespec_time yields CLOCK_MONOTONIC, which on Linux is the clock behind
        // steady_clock (libstdc++ and libc++ alike), so the converted time is the answer as it is. It
        // used to be carried over as a delta between two clock reads (steady_clock above, then
        // CLOCK_MONOTONIC after the conversion), off by however long the thread was preempted between
        // them: the mock's stamp check caught two eye sets stamped 0.24 ms early in a 10-minute soak.
        timespec display_spec{};
        if (XR_FAILED(convert_display_time_(runtime_->Instance(), display_time, &display_spec))) return 0;
        (void)now_ns;
        const int64_t display_ns = static_cast<int64_t>(display_spec.tv_sec) * 1'000'000'000ll + display_spec.tv_nsec;
        return display_ns > 0 ? static_cast<uint64_t>(display_ns) : 0;
#endif
    }

#if defined(__ANDROID__) || defined(MKW_OPENXR_LINUX_DESKTOP)
    // SteamVR's XrTime is CLOCK_MONOTONIC in nanoseconds and Waydroid shares the host's clock, so a
    // predicted display time normally lies a few frames ahead of now. If it does not, the runtime
    // uses another epoch: anchor it to the first frame seen, assuming that frame shows ~2 frames out.
    int64_t MonotonicDisplayDelta(XrTime display_time, int64_t monotonic_ns) noexcept {
        const int64_t raw = static_cast<int64_t>(display_time) - monotonic_ns;
        // Decided once, at the first frame (lane O): a stalled frame later (a display time already
        // 100 ms in the past) must not switch a CLOCK_MONOTONIC runtime to the anchored fallback.
        if (!clock_fallback_logged_) clock_same_ = raw > -100'000'000ll && raw < 500'000'000ll;
        const bool same_clock = clock_same_;
        if (!clock_fallback_logged_) {
            clock_fallback_logged_ = true;
            RT_LOG(RT_TAG_RUNTIME) << "OpenXR: no clock conversion; XrTime - CLOCK_MONOTONIC = "
                                   << raw / 1000 << " us, "
                                   << (same_clock ? "reading XrTime as CLOCK_MONOTONIC" : "anchoring to the first frame")
                                   << std::endl;
        }
        if (same_clock) return raw;
        if (!clock_offset_valid_) {
            clock_offset_valid_ = true;
            clock_offset_ = raw - 22'000'000ll;
        }
        return raw - clock_offset_;
    }
#endif

    void WaitForStopOrDelay(std::chrono::milliseconds delay) {
        std::unique_lock lock(stop_mutex_);
        stop_cv_.wait_for(lock, delay,
                          [this] { return stop_.load(std::memory_order_acquire); });
    }

    void WithdrawPublishedFrame() noexcept {
        std::lock_guard lock(published_mutex_);
        published_.store(nullptr, std::memory_order_release);
    }

    void SetError(std::string message) {
        {
            std::lock_guard lock(error_mutex_);
            last_error_ = std::move(message);
        }
        RT_LOG(RT_TAG_RUNTIME) << "OpenXR: " << LastError() << std::endl;
    }

    OpenXRLogCallback logger_;
    EyeOffsets eye_offsets_; // before runtime_: its views hook calls into it
    std::unique_ptr<OpenXRRuntime> runtime_;
    std::unique_ptr<GraphicsBackend> backend_;
#if defined(__ANDROID__)
    uint32_t panel_layer_poll_ = 0;
    bool panel_layer_forced_off_ = false;
    uint32_t eye_passes_poll_ = 0;
    int eye_passes_override_ = -1;
    uint32_t foveation_poll_ = 0;
    int foveation_override_ = -1;
    uint32_t window_eyes_poll_ = 0;
    bool window_eyes_forced_off_ = false;
#endif
    std::unique_ptr<OpenXRInput> input_;
    std::thread pacing_thread_;
    std::atomic_bool stop_{false};
    std::atomic_bool running_{false};
    std::atomic_bool teardown_requested_{false};
    std::chrono::steady_clock::time_point menu_tracking_started_{};
    bool startup_origin_checked_=false, startup_origin_pending_=false, startup_origin_applied_=false;
    XrPosef startup_origin_pose_{};
    std::atomic_bool recenter_requested_{false};
    std::atomic<float> lean_back_degrees_{RuntimeConfigFile::VrLeanBackDegrees()};
    std::atomic_bool passthrough_{RuntimeConfigFile::VrPassthrough()};
    std::atomic_bool immersive_window_{RuntimeConfigFile::VrImmersiveWindow()};
#if defined(__ANDROID__)
    static uint32_t AndroidFrameInterpolationFps() noexcept {
        // `setprop debug.wiicompiled.vr_interp 0|1|72|90|120` before launch overrides [vr]
        // frame_interpolation_fps; both default to auto (render every display refresh).
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.wiicompiled.vr_interp", value) > 0) {
            return NormalizeFrameInterpolationFps(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)));
        }
        return NormalizeFrameInterpolationFps(RuntimeConfigFile::Get().vrFrameInterpolationFps.value_or(1));
    }
    std::atomic_uint32_t frame_interpolation_fps_{AndroidFrameInterpolationFps()};
#else
    std::atomic_uint32_t frame_interpolation_fps_{RuntimeConfigFile::VrFrameInterpolationFps()};
#endif
#if defined(__ANDROID__) || defined(MKW_OPENXR_LINUX_DESKTOP)
    bool clock_fallback_logged_ = false;
    bool clock_offset_valid_ = false;
    bool clock_same_ = false;  // XrTime read as CLOCK_MONOTONIC (decided at the first frame)
    int64_t clock_offset_ = 0;
#endif
    std::atomic_bool interpolation_available_{false};
    std::mutex interpolation_mutex_;
    bool interpolation_stopping_ = true;
    FrameInterpolationPacing interpolation_pacing_;
    SetPacingState set_pacing_;
    // Sets rendered ahead (lane K), oldest display time first.
    struct AheadSet {
        OpenXRBackendFrame set{};
        std::chrono::steady_clock::time_point published_at{};
        bool late_counted = false;
    };
    std::deque<AheadSet> ahead_sets_;
    XrTime ahead_shown_time_ = 0;  // display time of the last set shown
    bool ahead_ = false;
    bool ahead_wanted_ = false;
    std::chrono::microseconds ahead_start_delay_{0};
    std::chrono::microseconds ahead_end_delay_{0};
    EndPhaseLock end_phase_;  // lane X: KARTFRAME_END_PHASE (test only)
    size_t ahead_lead_ = 2; // refreshes between a set's start and its display time (KARTFRAME_AHEAD_REFRESHES)
    std::vector<double> prediction_ms_;  // lane R: NotePrediction
    std::chrono::steady_clock::time_point prediction_since_{};
    bool prediction_race_ = false;
    bool SetsInFlight() const { return set_pacing_.pending || !ahead_sets_.empty(); }
    std::chrono::steady_clock::time_point last_focused_{};
    PacingMode pacing_mode_ = PacingMode::Pipelined;
    bool fov_logged_ = false;
    HalfRatePolicy half_rate_policy_ = HalfRatePolicy::Auto;
    int pacing_debug_ = std::getenv("KARTFRAME_PACING_DEBUG") ? 60 : 0;
    std::string pacing_mode_source_ = "auto";
    std::chrono::microseconds pacing_margin_{1000};
    std::atomic<float> headset_hz_{0};
    std::atomic<float> rendered_fps_{0};
    std::chrono::steady_clock::time_point timing_start_ = std::chrono::steady_clock::now();
    uint32_t timing_submissions_ = 0;
    AdaptiveResolution adaptive_resolution_;
    float adaptive_scale_=1.f;
    PFN_xrGetDisplayRefreshRateFB get_display_refresh_rate_ = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_display_refresh_rates_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB request_display_refresh_rate_ = nullptr;
    float logged_refresh_hz_ = 0.0f;
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_performance_level_ = nullptr;
#if defined(_WIN32)
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, LARGE_INTEGER*);
#else
    using ConvertDisplayTime = XrResult (XRAPI_PTR*)(XrInstance, XrTime, struct timespec*);
#endif
    ConvertDisplayTime convert_display_time_ = nullptr;
    std::atomic<PublishedFrame*> published_{nullptr};
    PublishedFrame published_frame_{};
    std::mutex published_mutex_;
    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    mutable std::mutex error_mutex_;
    std::string last_error_;
    std::array<float, 3> base_position_{};
    bool base_position_valid_ = false;
    XrPosef virtual_screen_pose_{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    bool virtual_screen_pose_valid_ = false;
    bool last_immersive_ = false;
    bool hand_meshes_loaded_ = false;
    uint64_t applied_session_run_serial_ = 0;
    bool session_was_active_ = false;
    bool requested_ = false;
    bool prepared_ = false;
    bool provider_registered_ = false;
    bool graphics_retained_ = false;
    uint32_t game_thread_id_ = 0;
};

#endif // MKW_OPENXR_GRAPHICS_BACKEND

} // namespace

OpenXRStartupResult OpenXRPrepareAurora(AuroraConfig& config) {
#if !defined(MKW_ENABLE_OPENXR)
    (void)config;
    ConfigurePolicy(false);
    return RuntimeConfigFile::VrEnabled(kVrEnabledDefault) ? OpenXRStartupResult::Unavailable
                                                           : OpenXRStartupResult::Disabled;
#elif MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Prepare(config);
#else
    (void)config;
    ConfigurePolicy(RuntimeConfigFile::VrEnabled(kVrEnabledDefault));
    if (!RuntimeConfigFile::VrEnabled(kVrEnabledDefault)) {
        return OpenXRStartupResult::Disabled;
    }
    RT_LOG(RT_TAG_RUNTIME) << "OpenXR is not wired to a graphics backend on this platform" << std::endl;
    return OpenXRStartupResult::Unavailable;
#endif
}

bool OpenXRStartAfterAurora(AuroraBackend active_backend) {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().Start(active_backend);
#else
    (void)active_backend;
    return !RuntimeConfigFile::VrEnabled(kVrEnabledDefault);
#endif
}

void OpenXRShutdownBeforeAurora() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().Shutdown();
#else
    MkwVRPolicySetSessionActive(false);
#endif
}

void OpenXRServiceProducerFrameBoundary() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().ServiceProducerFrameBoundary();
#endif
}

bool OpenXRIsRunning() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().IsRunning();
#else
    return false;
#endif
}

void OpenXRApplyControllerState() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRApplyVirtualGamepad();
#endif
}

void OpenXRRequestRecenter() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().RequestRecenter();
#endif
}

void OpenXRSetLeanBackDegrees(float degrees) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetLeanBackDegrees(degrees);
#else
    (void)degrees;
#endif
}

void OpenXRSetPassthrough(bool enabled) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetPassthrough(enabled);
#else
    (void)enabled;
#endif
}

void OpenXRSetImmersiveWindow(bool enabled) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetImmersiveWindow(enabled);
#else
    (void)enabled;
#endif
}

void OpenXRSetFrameInterpolationFps(uint32_t target) noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    OpenXRIntegration::Get().SetFrameInterpolationFps(target);
#else
    (void)target;
#endif
}

OpenXRFrameTiming OpenXRGetFrameTiming() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameTiming();
#else
    return {};
#endif
}

bool OpenXRFrameInterpolationAvailable() noexcept {
#if MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().FrameInterpolationAvailable();
#else
    return false;
#endif
}

std::string OpenXRLastError() {
#if !defined(MKW_ENABLE_OPENXR)
    return "this build was compiled without OpenXR support";
#elif MKW_OPENXR_GRAPHICS_BACKEND
    return OpenXRIntegration::Get().LastError();
#else
    return "OpenXR is not wired to a graphics backend on this platform";
#endif
}

} // namespace mkw::vr
