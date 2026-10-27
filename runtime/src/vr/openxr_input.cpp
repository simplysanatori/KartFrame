// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(MKW_ENABLE_OPENXR)

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vr/openxr_input.h"
#include "vr/openxr_controller_profiles.h"
#include "vr/mkw_vr_policy.h"
#include "physical_wheel.h"
#include "runtime_config.h"
#include "settings_overlay.h"
#include "vr/mkw_vr_first_person.h"
#include "vr/mkw_vr_item.h"
#include "vr/openxr_diagnostics.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <time.h>
#endif

namespace mkw::vr {
namespace {

// SDL holds its joystick lock for as long as an enumeration takes, and the
// Bluetooth Wii Remote rescan (F10 > Controller settings > Keep scanning)
// closes and reopens every HID device twice per scan: 15 ms on a plain desk,
// over 200 ms on a machine carrying several HID devices, such as a Lighthouse
// setup's base-station dongles. The pacing thread must never wait on that,
// because the OpenXR frame it holds open costs the compositor every display
// slot that passes. So it leaves the gamepad here, and the game thread writes
// it to SDL where it already polls controllers.
//
// m_sdl is the lock the SDL work runs under; the pacing thread only ever takes
// m_state, and only for the copy. Both are taken in that order.
class VirtualGamepadRelay {
public:
    struct Pad {
        std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    };

    void Attach(SDL_Joystick* joystick) {
        std::scoped_lock lock(m_sdl, m_state);
        m_joystick = joystick;
        m_pending = false;
    }

    void Detach() {
        std::scoped_lock lock(m_sdl, m_state);
        m_joystick = nullptr;
        m_pending = false;
    }

    // Pacing thread.
    void Publish(const Pad& pad) {
        std::lock_guard lock(m_state);
        m_pad = pad;
        m_pending = true;
    }

    // Game thread. Holding m_sdl here is what keeps Detach from closing the
    // joystick underneath the writes.
    void Apply() {
        std::lock_guard sdl(m_sdl);
        Pad pad;
        SDL_Joystick* joystick = nullptr;
        {
            std::lock_guard lock(m_state);
            if (!m_pending || m_joystick == nullptr) {
                return;
            }
            pad = m_pad;
            joystick = m_joystick;
            m_pending = false;
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
            SDL_SetJoystickVirtualAxis(joystick, static_cast<SDL_GamepadAxis>(axis),
                                       pad.axes[static_cast<size_t>(axis)]);
        }
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
            SDL_SetJoystickVirtualButton(joystick, static_cast<SDL_GamepadButton>(button),
                                         pad.buttons[static_cast<size_t>(button)]);
        }
    }

private:
    std::mutex m_sdl;
    std::mutex m_state;
    SDL_Joystick* m_joystick = nullptr;
    Pad m_pad;
    bool m_pending = false;
};

VirtualGamepadRelay& Relay() {
    static VirtualGamepadRelay relay;
    return relay;
}

#if defined(__ANDROID__)
// Debug-only remote button presses for headset experiments driven over adb, so
// a menu can be reached without someone wearing the headset:
//   adb shell setprop debug.wiicompiled.inject <sequence>:<button>
// A new sequence number holds the button for kInjectHoldFrames XR frames.
// Buttons: a, b, x, y, start, up, down, left, right, and for the Wii Remote
// presentation also home, c and z (x/y/start press 1/2/+ there, and the
// directions push the Nunchuk stick). `panel` presses the settings panel's
// chord (left X + Y), opening or closing it,
// where `a` then selects. The property is unset in normal use, so this costs
// one property read every few frames.
constexpr uint32_t kInjectHoldFrames = 12;
constexpr uint32_t kInjectPollFrames = 4;

struct InjectedPress {
    long sequence = -1;
    std::string button;
    uint32_t frames_left = 0;
    uint32_t poll_countdown = 0;
};

InjectedPress& Injection() {
    static InjectedPress press;
    return press;
}

void PollInjection() {
    InjectedPress& press = Injection();
    if (press.frames_left > 0) {
        --press.frames_left;
    }
    if (press.poll_countdown > 0) {
        --press.poll_countdown;
        return;
    }
    press.poll_countdown = kInjectPollFrames;
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.wiicompiled.inject", value) <= 0) {
        return;
    }
    char* end = nullptr;
    const long sequence = std::strtol(value, &end, 10);
    if (end == value || *end != ':' || sequence == press.sequence) {
        return;
    }
    const bool first_read = press.sequence < 0;
    press.sequence = sequence;
    if (first_read) {
        return; // A value left over from an earlier run is not a new press.
    }
    press.button = end + 1;
    press.frames_left = kInjectHoldFrames;
}

bool Injected(const char* button) {
    const InjectedPress& press = Injection();
    return press.frames_left > 0 && press.button == button;
}

using ConvertNowToXrTime = XrResult(XRAPI_PTR*)(XrInstance, const struct timespec*, XrTime*);
#else
void PollInjection() {}
bool Injected(const char*) { return false; }

#if defined(_WIN32)
using ConvertNowToXrTime = XrResult(XRAPI_PTR*)(XrInstance, const LARGE_INTEGER*, XrTime*);
#endif
#endif

constexpr uint32_t kHandCount = 2;

// Re-sent every frame while the game holds the motor on, so a rumble whose stop
// never arrives (or a stalled pacing thread) dies out on its own.
constexpr XrDuration kRumblePulseNs = 50'000'000;

struct Binding {
    XrAction* action;
    const char* path;
};

Sint16 ToAxis(float value) noexcept {
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

// Trigger axes are reported by SDL gamepads on the positive half only.
Sint16 ToTrigger(float value) noexcept {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<Sint16>(std::lround(clamped * 32767.0f));
}

wii_remote::Pose ToWiiRemotePose(const XrPosef& pose) noexcept {
    return {{pose.position.x, pose.position.y, pose.position.z},
            {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}};
}

// The adb injection's buttons as the Wii Remote presentation's WPAD bits.
uint32_t InjectedWiiRemoteButtons() {
    uint32_t hold = 0;
    const auto press = [&hold](const char* name, uint32_t bit) {
        if (Injected(name)) {
            hold |= bit;
        }
    };
    press("a", wii_remote::kButtonA);
    press("b", wii_remote::kButtonB);
    press("x", wii_remote::kButtonOne);
    press("y", wii_remote::kButtonTwo);
    press("start", wii_remote::kButtonPlus);
    press("home", wii_remote::kButtonHome);
    press("c", wii_remote::kButtonC);
    press("z", wii_remote::kButtonZ);
    return hold;
}

// A [vr] key of Config.toml that runtime_config.h does not know (read here so that header, which
// every translated file includes, stays untouched), as text; empty when absent.
std::string ItemThrowConfigText(const char* key) {
    try {
        std::ifstream file(RuntimeConfigFile::ResolveConfigPath(), std::ios::binary);
        if (!file) return {};
        const auto document = toml::parse(file, "Config.toml");
        if (auto text = RuntimeConfigFile::FindConfigValue<std::string>(document, "vr", key)) return *text;
        if (auto flag = RuntimeConfigFile::FindConfigValue<bool>(document, "vr", key)) return *flag ? "true" : "false";
        if (auto number = RuntimeConfigFile::FindConfigFloat(document, "vr", key)) {
            std::ostringstream out;
            out << *number;
            return out.str();
        }
    } catch (const std::exception&) {
    }
    return {};
}

// The environment variable when set, else the [vr] key.
std::string ItemThrowSetting(const char* env, const char* key) {
    if (const char* value = std::getenv(env); value != nullptr && *value != '\0') return value;
    return ItemThrowConfigText(key);
}

} // namespace

// LANE-AH-STUB: the held-item read (mkw_vr_item.h's contract) is lane AH's, in mkw_vr_first_person.cpp
// and declared in mkw_vr_first_person.h. Until it is merged this weak fallback reports no item, so a
// swing throws nothing (KARTFRAME_THROW_INJECT still plays throws); the linker drops it for the real
// definition. Remove this block after the merge.
HeldItem MkwVRFirstPersonGetHeldItem() noexcept;
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) HeldItem MkwVRFirstPersonGetHeldItem() noexcept { return {}; }
#endif

OpenXRInput::OpenXRInput(OpenXRLogCallback logger) : m_logger(std::move(logger)) {}

OpenXRInput::~OpenXRInput() {
    Destroy();
}

bool OpenXRInput::Create(OpenXRRuntime& runtime) {
    m_last_error.clear();
    if (m_created) {
        return true;
    }
    if (!runtime.IsInitialized() || !runtime.HasSession()) {
        m_last_error = "OpenXR input needs an initialized runtime with a session";
        return false;
    }
    m_runtime = &runtime;

    if (!Check(xrStringToPath(runtime.Instance(), "/user/hand/left", &m_hand_paths[0]),
               "xrStringToPath(/user/hand/left)") ||
        !Check(xrStringToPath(runtime.Instance(), "/user/hand/right", &m_hand_paths[1]),
               "xrStringToPath(/user/hand/right)")) {
        Destroy();
        return false;
    }
    if (!CreateActions() || !SuggestBindings()) {
        Destroy();
        return false;
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &m_action_set;
    if (!Check(xrAttachSessionActionSets(runtime.Session(), &attach), "xrAttachSessionActionSets")) {
        Destroy();
        return false;
    }
    m_created = true;
    CreatePoseSpaces();
    LoadInputClock();
    LoadItemThrowSettings();
    if (!AttachVirtualGamepad()) {
        Log(OpenXRLogLevel::Warning,
            "SDL refused the virtual gamepad; OpenXR controllers will not reach the game");
    }
    Log(OpenXRLogLevel::Info, OpenXRGetControllerMode() == OpenXRControllerMode::WiiRemote
                                  ? "OpenXR controller actions attached (Wii Remote + Nunchuk)"
                                  : "OpenXR controller actions attached (gamepad)");
    return true;
}

bool OpenXRInput::CreateActions() {
    XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(set_info.actionSetName, "mkw_gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    set_info.priority = 0;
    if (!Check(xrCreateActionSet(m_runtime->Instance(), &set_info, &m_action_set), "xrCreateActionSet")) {
        return false;
    }

    struct Spec {
        XrAction* action;
        const char* name;
        const char* localized;
        XrActionType type;
    };
    const std::array<Spec, 10> specs{{
        {&m_thumbstick, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
        {&m_thumbstick_click, "thumbstick_click", "Thumbstick Click", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_trigger, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_squeeze, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_button_primary, "button_primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_button_secondary, "button_secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_menu, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_aim_pose, "aim_pose", "Pointer", XR_ACTION_TYPE_POSE_INPUT},
        {&m_grip_pose, "grip_pose", "Motion", XR_ACTION_TYPE_POSE_INPUT},
        {&m_haptic, "haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
    }};
    for (const Spec& spec : specs) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = spec.type;
        std::strncpy(info.actionName, spec.name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, spec.localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.countSubactionPaths = kHandCount;
        info.subactionPaths = m_hand_paths;
        if (!Check(xrCreateAction(m_action_set, &info, spec.action), spec.name)) {
            return false;
        }
    }
    // Separate left-hand actions avoid old SteamVR game bindings which may
    // have mapped the shared A/X or B/Y action only to the right controller.
    for (const Spec& spec : std::array<Spec,2>{{
        {&m_options_x,"vr_options_x","VR options X",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_options_y,"vr_options_y","VR options Y",XR_ACTION_TYPE_BOOLEAN_INPUT}}}) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType=spec.type;
        std::strncpy(info.actionName,spec.name,XR_MAX_ACTION_NAME_SIZE-1);
        std::strncpy(info.localizedActionName,spec.localized,XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
        info.countSubactionPaths=1;info.subactionPaths=m_hand_paths;
        if(!Check(xrCreateAction(m_action_set,&info,spec.action),spec.name)) return false;
    }
    // Steam Frame actions carry no hand: the Frame profile binds them in code, and a SteamVR binding file may bind them to either controller.
    for (const Spec& spec : std::array<Spec,12>{{
        {&m_frame_accelerate,"frame_accelerate","Accelerate",XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_frame_reverse,"frame_reverse","Reverse",XR_ACTION_TYPE_FLOAT_INPUT},
        {&m_frame_brake,"frame_brake","Brake / back",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_item,"frame_item","Use item",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_look_back,"frame_look_back","Look back",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_drift,"frame_drift","Drift / hop",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_start,"frame_start","Start / pause",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_back,"frame_back","Back (hold: VR settings)",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_dpad[0],"frame_dpad_up","D-pad up / trick",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_dpad[1],"frame_dpad_down","D-pad down",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_dpad[2],"frame_dpad_left","D-pad left",XR_ACTION_TYPE_BOOLEAN_INPUT},
        {&m_frame_dpad[3],"frame_dpad_right","D-pad right",XR_ACTION_TYPE_BOOLEAN_INPUT}}}) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType=spec.type;
        std::strncpy(info.actionName,spec.name,XR_MAX_ACTION_NAME_SIZE-1);
        std::strncpy(info.localizedActionName,spec.localized,XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
        if(!Check(xrCreateAction(m_action_set,&info,spec.action),spec.name)) return false;
    }
    return true;
}

bool OpenXRInput::SuggestBindings() {
    bool accepted=false;
    const auto suggest = [&](const char* profile, const std::vector<Binding>& bindings, bool required) {
        XrPath profile_path = XR_NULL_PATH;
        if (!Check(xrStringToPath(m_runtime->Instance(), profile, &profile_path), profile)) {
            return false;
        }
        std::vector<XrActionSuggestedBinding> suggested;
        if(std::string_view(profile)=="/interaction_profiles/htc/vive_controller") m_vive_profile=profile_path;
        if(std::string_view(profile)=="/interaction_profiles/valve/frame_controller") m_frame_profile=profile_path;
        suggested.reserve(bindings.size());
        for (const Binding& binding : bindings) {
            XrPath path = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(m_runtime->Instance(), binding.path, &path))) {
                continue;
            }
            suggested.push_back({*binding.action, path});
            if(std::string_view(binding.path).find("/left/")!=std::string_view::npos) {
                if(binding.action==&m_button_primary) suggested.push_back({m_options_x,path});
                if(binding.action==&m_button_secondary) suggested.push_back({m_options_y,path});
            }
        }
        XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        info.interactionProfile = profile_path;
        info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
        info.suggestedBindings = suggested.data();
        const XrResult result = xrSuggestInteractionProfileBindings(m_runtime->Instance(), &info);
        m_runtime->ObserveResult(result);
        if (XR_FAILED(result)) {
            std::ostringstream message;
            message << "xrSuggestInteractionProfileBindings(" << profile << ") failed (" << result << ')';
            if (required) {
                m_last_error = message.str();
                Log(OpenXRLogLevel::Error, m_last_error);
                return false;
            }
            Log(OpenXRLogLevel::Warning, message.str());
            return false;
        }
        accepted=true;
        return true;
    };

    // Steam Frame controllers (XR_VALVE_frame_controller_interaction), bound entirely in code.
    // Paths follow Valve's frame_controller input profile: right hand a, b, x, y, menu, bumper,
    // trigger, squeeze, thumbstick; left hand dpad_*, view, bumper, trigger, squeeze, thumbstick;
    // aim pose on both. One invalid path fails the whole call, so the grip pose and the grip
    // component name (squeeze/value, else grip/value) are tried in turn.
    {
        const auto& extensions = m_runtime->EnabledExtensions();
        if (std::find(extensions.begin(), extensions.end(), std::string("XR_VALVE_frame_controller_interaction")) ==
            extensions.end()) {
            Log(OpenXRLogLevel::Info, "XR_VALVE_frame_controller_interaction not enabled; Frame controller bindings skipped");
        } else {
            const std::string L = "/user/hand/left/input/", R = "/user/hand/right/input/";
            for (const bool grip_pose : {true, false}) {
                for (const char* squeeze : {"squeeze/value", "grip/value"}) {
                    std::vector<std::string> paths; // keeps the c_str()s alive
                    paths.reserve(64);
                    std::vector<Binding> bindings;
                    const auto add = [&](XrAction* action, const std::string& path) {
                        paths.push_back(path);
                        bindings.push_back({action, paths.back().c_str()});
                    };
                    paths.clear();
                    add(&m_thumbstick, L + "thumbstick");
                    add(&m_thumbstick, R + "thumbstick");
                    add(&m_thumbstick_click, L + "thumbstick/click");
                    add(&m_thumbstick_click, R + "thumbstick/click");
                    // Menus and the settings panel: triggers select, A confirms, B backs out.
                    add(&m_trigger, L + "trigger/value");
                    add(&m_trigger, R + "trigger/value");
                    add(&m_squeeze, L + squeeze);
                    add(&m_squeeze, R + squeeze);
                    add(&m_button_primary, R + "a/click");
                    add(&m_button_secondary, R + "b/click");
                    add(&m_aim_pose, L + "aim/pose");
                    add(&m_aim_pose, R + "aim/pose");
                    if (grip_pose) {
                        add(&m_grip_pose, L + "grip/pose");
                        add(&m_grip_pose, R + "grip/pose");
                    }
                    add(&m_haptic, "/user/hand/left/output/haptic");
                    add(&m_haptic, "/user/hand/right/output/haptic");
                    add(&m_frame_accelerate, R + "trigger/value");
                    add(&m_frame_accelerate, R + "a/click");
                    add(&m_frame_reverse, L + "trigger/value");
                    add(&m_frame_brake, R + "b/click");
                    add(&m_frame_item, R + "x/click");
                    add(&m_frame_item, L + "bumper/click");
                    add(&m_frame_look_back, R + "y/click");
                    add(&m_frame_drift, R + "bumper/click");
                    add(&m_frame_start, R + "menu/click");
                    add(&m_frame_back, L + "view/click");
                    add(&m_frame_dpad[0], L + "dpad_up/click");
                    add(&m_frame_dpad[1], L + "dpad_down/click");
                    add(&m_frame_dpad[2], L + "dpad_left/click");
                    add(&m_frame_dpad[3], L + "dpad_right/click");
                    if (suggest("/interaction_profiles/valve/frame_controller", bindings, false)) {
                        m_frame_profile_suggested = true;
                        Log(OpenXRLogLevel::Info, std::string("Steam Frame controller bindings accepted (") + squeeze +
                                                      (grip_pose ? ", with grip pose)" : ", no grip pose)"));
                        break;
                    }
                }
                if (m_frame_profile_suggested) break;
            }
        }
    }
    // Meta Quest Touch controllers (Quest 2 / 3 / Pro all expose this profile).
    const std::vector<Binding> touch{
        {&m_thumbstick, "/user/hand/left/input/thumbstick"},
        {&m_thumbstick, "/user/hand/right/input/thumbstick"},
        {&m_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
        {&m_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
        {&m_trigger, "/user/hand/left/input/trigger/value"},
        {&m_trigger, "/user/hand/right/input/trigger/value"},
        {&m_squeeze, "/user/hand/left/input/squeeze/value"},
        {&m_squeeze, "/user/hand/right/input/squeeze/value"},
        {&m_button_primary, "/user/hand/left/input/x/click"},
        {&m_button_primary, "/user/hand/right/input/a/click"},
        {&m_button_secondary, "/user/hand/left/input/y/click"},
        {&m_button_secondary, "/user/hand/right/input/b/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_aim_pose, "/user/hand/left/input/aim/pose"},
        {&m_aim_pose, "/user/hand/right/input/aim/pose"},
        {&m_grip_pose, "/user/hand/left/input/grip/pose"},
        {&m_grip_pose, "/user/hand/right/input/grip/pose"},
        {&m_haptic, "/user/hand/left/output/haptic"},
        {&m_haptic, "/user/hand/right/output/haptic"},
    };
    suggest("/interaction_profiles/oculus/touch_controller", touch, false);
    // The port's advertised controller families. Paths are explicit; rejected
    // optional profiles do not prevent another runtime's bindings from loading.
    for (const auto& profile : kOpenXRControllerProfiles) {
        const auto id=profile.interaction_profile;
        if(id=="/interaction_profiles/oculus/touch_controller" || id=="/interaction_profiles/khr/simple_controller") continue;
        auto bindings=touch;
        for(auto& binding:bindings) {
            const bool left=std::string(binding.path).find("/left/")!=std::string::npos;
            if(binding.action==&m_thumbstick) binding.path=(left?profile.actions.steering:profile.actions.tricks).data();
            if(binding.action==&m_button_primary) binding.path=(left?profile.actions.trick:profile.actions.confirm).data();
            if(binding.action==&m_button_secondary) binding.path=(left?profile.actions.item[0]:profile.actions.brake).data();
            if(binding.action==&m_thumbstick_click && !left) binding.path=profile.camera_click.data();
            if(binding.action==&m_trigger && !left) binding.path=profile.actions.accelerate.data();
            if(binding.action==&m_squeeze) {
                if(id.find("index")!=std::string_view::npos) binding.path=left?"/user/hand/left/input/squeeze/value":"/user/hand/right/input/squeeze/value";
                else if(id.find("motion")!=std::string_view::npos || id.find("odyssey")!=std::string_view::npos || id=="/interaction_profiles/htc/vive_controller")
                    binding.path=left?"/user/hand/left/input/squeeze/click":"/user/hand/right/input/squeeze/click";
            }
            if(binding.action==&m_menu) binding.path=id.find("index")!=std::string_view::npos?nullptr:profile.actions.pause[0].data();
            const bool wand=id=="/interaction_profiles/htc/vive_controller";
            const bool motion=id.find("motion")!=std::string_view::npos || id.find("odyssey")!=std::string_view::npos;
            if(wand || motion) {
                // These controllers have no X/Y/A/B cluster. Keep left/right
                // actions on their own hand and avoid unsupported Touch paths.
                if(binding.action==&m_button_primary && left) binding.path="/user/hand/left/input/menu/click";
                if(binding.action==&m_button_secondary && left) binding.path="/user/hand/left/input/trackpad/click";
                if(binding.action==&m_button_primary && !left && motion) binding.path="/user/hand/right/input/trackpad/click";
                if(binding.action==&m_thumbstick_click) binding.path=left?nullptr:(wand?"/user/hand/right/input/trackpad/click":"/user/hand/right/input/thumbstick/click");
            }
        }
        bindings.erase(std::remove_if(bindings.begin(),bindings.end(),[](const Binding& b){return !b.path || !b.path[0];}),bindings.end());
        suggest(id.data(),bindings,false);
    }
    // Minimal fallback so an unfamiliar runtime still offers a select, a menu
    // and something to point with.
    const std::vector<Binding> simple{
        {&m_button_primary, "/user/hand/right/input/select/click"},
        {&m_button_secondary, "/user/hand/left/input/select/click"},
        {&m_menu, "/user/hand/left/input/menu/click"},
        {&m_aim_pose, "/user/hand/left/input/aim/pose"},
        {&m_aim_pose, "/user/hand/right/input/aim/pose"},
        {&m_grip_pose, "/user/hand/left/input/grip/pose"},
        {&m_grip_pose, "/user/hand/right/input/grip/pose"},
        {&m_haptic, "/user/hand/left/output/haptic"},
        {&m_haptic, "/user/hand/right/output/haptic"},
    };
    suggest("/interaction_profiles/khr/simple_controller", simple, false);
    if(!accepted) m_last_error="No supported controller interaction profile could be bound.";
    return accepted;
}

void OpenXRInput::CreatePoseSpaces() {
    bool logged = false;
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        for (auto [action, spaces] : {std::pair{m_aim_pose, m_aim_spaces}, std::pair{m_grip_pose, m_grip_spaces}}) {
            XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            info.action = action;
            info.subactionPath = m_hand_paths[hand];
            info.poseInActionSpace.orientation.w = 1.0f;
            const XrResult result = xrCreateActionSpace(m_runtime->Session(), &info, &spaces[hand]);
            m_runtime->ObserveResult(result);
            if (XR_FAILED(result)) {
                spaces[hand] = XR_NULL_HANDLE;
                if (!logged) {
                    logged = true;
                    std::ostringstream message;
                    message << "xrCreateActionSpace failed (" << result
                            << "); the Wii Remote will have no motion or pointer";
                    Log(OpenXRLogLevel::Warning, message.str());
                }
            }
        }
    }
}

void OpenXRInput::DestroyPoseSpaces() {
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        for (XrSpace* space : {&m_aim_spaces[hand], &m_grip_spaces[hand]}) {
            if (*space != XR_NULL_HANDLE) {
                xrDestroySpace(*space);
                *space = XR_NULL_HANDLE;
            }
        }
    }
}

// Poses for input are located at the measured current time, not the frame's
// predicted display time: that lies tens of milliseconds ahead, and the runtime
// extrapolates a fast wrist turn that far past where the hand really is, which
// sprays the pointer and invents acceleration (DolphinXR's fast-motion fix).
void OpenXRInput::LoadInputClock() {
    const auto& extensions = m_runtime->EnabledExtensions();
    const auto enabled = [&](const char* name) {
        return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
    };
    PFN_xrVoidFunction function = nullptr;
#if defined(_WIN32)
    if (enabled("XR_KHR_win32_convert_performance_counter_time")) {
        m_runtime->GetInstanceProcAddress("xrConvertWin32PerformanceCounterToTimeKHR", &function);
    }
#elif defined(__ANDROID__) || defined(__linux__)
    if (enabled("XR_KHR_convert_timespec_time")) {
        m_runtime->GetInstanceProcAddress("xrConvertTimespecTimeToTimeKHR", &function);
    }
#else
    (void)enabled;
#endif
    m_convert_now_to_xr_time = function;
    if (m_convert_now_to_xr_time == nullptr) {
        Log(OpenXRLogLevel::Info,
            "OpenXR offers no clock conversion; controller motion is sampled at display time");
    }
}

XrTime OpenXRInput::InputSampleTime(XrTime predicted_display_time) const {
    if (m_convert_now_to_xr_time == nullptr) {
        return predicted_display_time;
    }
    XrTime now = 0;
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == 0 ||
        XR_FAILED(reinterpret_cast<ConvertNowToXrTime>(m_convert_now_to_xr_time)(m_runtime->Instance(),
                                                                                  &counter, &now))) {
        return predicted_display_time;
    }
#elif defined(__ANDROID__)
    timespec spec{};
    if (clock_gettime(CLOCK_MONOTONIC, &spec) != 0 ||
        XR_FAILED(reinterpret_cast<ConvertNowToXrTime>(m_convert_now_to_xr_time)(m_runtime->Instance(),
                                                                                  &spec, &now))) {
        return predicted_display_time;
    }
#endif
    return now > 0 ? (std::min)(predicted_display_time, now) : predicted_display_time;
}

void OpenXRApplyVirtualGamepad() noexcept {
    Relay().Apply();
}

bool OpenXRInput::AttachVirtualGamepad() {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_SOUTH) | (1u << SDL_GAMEPAD_BUTTON_EAST) |
                       (1u << SDL_GAMEPAD_BUTTON_WEST) | (1u << SDL_GAMEPAD_BUTTON_NORTH) |
                       (1u << SDL_GAMEPAD_BUTTON_START) | (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
                       (1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) |
                       (1u << SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                     (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                     (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) | (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    desc.name = "OpenXR Touch Controllers";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (id == 0) {
        m_last_error = std::string("SDL_AttachVirtualJoystick failed: ") + SDL_GetError();
        Log(OpenXRLogLevel::Warning, m_last_error);
        return false;
    }
    SDL_Joystick* joystick = SDL_OpenJoystick(id);
    if (joystick == nullptr) {
        m_last_error = std::string("SDL_OpenJoystick failed: ") + SDL_GetError();
        Log(OpenXRLogLevel::Warning, m_last_error);
        SDL_DetachVirtualJoystick(id);
        return false;
    }
    m_joystick_id = id;
    m_joystick = joystick;
    Relay().Attach(joystick);
    return true;
}

void OpenXRInput::DetachVirtualGamepad() {
    // Before the handle goes: the game thread may be writing through it.
    Relay().Detach();
    if (m_joystick != nullptr) {
        SDL_CloseJoystick(static_cast<SDL_Joystick*>(m_joystick));
        m_joystick = nullptr;
    }
    if (m_joystick_id != 0) {
        SDL_DetachVirtualJoystick(m_joystick_id);
        m_joystick_id = 0;
    }
}

void OpenXRInput::Destroy() {
    // The game must stop reading a remote whose controllers are going away.
    OpenXRWithdrawWiiRemote();
    ResetDriving();
    if (m_created) {
        StopRumble();
    }
    DetachVirtualGamepad();
    DestroyPoseSpaces();
    if (m_action_set != XR_NULL_HANDLE) {
        // Destroying the set destroys every action created from it.
        xrDestroyActionSet(m_action_set);
        m_action_set = XR_NULL_HANDLE;
    }
    m_thumbstick = m_thumbstick_click = m_trigger = m_squeeze = XR_NULL_HANDLE;
    m_button_primary = m_button_secondary = m_menu = m_haptic = XR_NULL_HANDLE;
    m_options_x = m_options_y = XR_NULL_HANDLE;
    m_frame_item = m_frame_look_back = m_frame_drift = m_frame_start = m_frame_back = XR_NULL_HANDLE;
    m_frame_accelerate = m_frame_reverse = m_frame_brake = XR_NULL_HANDLE;
    m_frame_profile = XR_NULL_PATH;
    m_frame_profile_suggested = false;
    m_frame_view.Reset();
    m_frame_dpad = {};
    m_frame_layout = false;
    m_injected_throw_held = false;
    m_gamepad_rumble.Reset();
    m_options_previous=0;
    m_aim_pose = m_grip_pose = XR_NULL_HANDLE;
    m_hand_paths[0] = m_hand_paths[1] = XR_NULL_PATH;
    m_convert_now_to_xr_time = nullptr;
    for (auto& motion : m_motion) {
        motion.Rest();
    }
    m_pointer.Reset();
    m_horizon = {1.0f, 0.0f};
    OpenXRPublishUiSnapshot({});
    OpenXRPublishPortControls({});
    m_panel_controls.Reset();
    m_last_input_time = 0;
    m_panel_select_held = false;
    m_created = false;
    m_runtime = nullptr;
}

void OpenXRInput::Idle() {
    if (!m_created) {
        return;
    }
    for (auto& motion : m_motion) {
        motion.Rest();
    }
    m_pointer.Reset();
    m_horizon = {1.0f, 0.0f};
    OpenXRPublishPortControls({});
    OpenXRPublishWiiRemote(m_joystick_id, OpenXRWiiRemoteSample{});
    // The panel stays as it was; only what the controllers were holding is forgotten.
    m_panel_controls.Reset();
    m_last_input_time = 0;
    m_panel_select_held = false;
    OpenXRPublishSettingsPanelPointer(false, 0.0f, 0.0f, false, 0.0f);
    m_first_person_click.Reset();
    m_frame_view.Reset();
    ResetDriving();
    StopRumble();
    m_gamepad_rumble.Reset();
    // Nothing stays held on the gamepad either while input is away.
    if (m_joystick != nullptr) {
        Relay().Publish({});
    }
}

void OpenXRInput::Sync(XrTime predicted_display_time, const OpenXRPointerScreen& screen,
                       const OpenXRPointerScreen& settings_panel, const driving::SeatFrame& seat) {
    if (!m_created || m_runtime == nullptr) {
        return;
    }
    if (!m_runtime->IsSessionFocused()) {
        // SteamVR takes focus for a few milliseconds now and then (VISIBLE, then FOCUSED again).
        // Actions read as inactive meanwhile, so keep what was last published instead of
        // releasing every button; only a loss longer than a second idles the controllers.
        const auto now = std::chrono::steady_clock::now();
        if (m_runtime->IsSessionVisible() && m_last_focused != std::chrono::steady_clock::time_point{} &&
            now - m_last_focused < std::chrono::seconds(1)) {
            return;
        }
        Idle();
        return;
    }
    m_last_focused = std::chrono::steady_clock::now();
    XrActiveActionSet active{m_action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    const XrResult result = diagnostics::Measure(diagnostics::Stage::SyncActions, [&] {
        return xrSyncActions(m_runtime->Session(), &sync);
    });
    m_runtime->ObserveResult(result);
    if (XR_FAILED(result)) {
        if (!m_logged_sync_failure) {
            m_logged_sync_failure = true;
            std::ostringstream message;
            message << "xrSyncActions failed (" << result << ')';
            Log(OpenXRLogLevel::Warning, message.str());
        }
        Idle();
        return;
    }

    const auto boolean = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(m_runtime->Session(), &info, &state)) &&
               state.isActive == XR_TRUE && state.currentState == XR_TRUE;
    };
    const auto scalar = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_FAILED(xrGetActionStateFloat(m_runtime->Session(), &info, &state)) ||
            state.isActive != XR_TRUE) {
            return 0.0f;
        }
        return state.currentState;
    };
    const auto vector = [&](XrAction action, uint32_t hand) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        info.subactionPath = m_hand_paths[hand];
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_FAILED(xrGetActionStateVector2f(m_runtime->Session(), &info, &state)) ||
            state.isActive != XR_TRUE) {
            return XrVector2f{0.0f, 0.0f};
        }
        return state.currentState;
    };

    std::array<wii_remote::HandInputs, kHands> hands{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        wii_remote::HandInputs& inputs = hands[hand];
        inputs.primary = boolean(m_button_primary, hand);
        inputs.secondary = boolean(m_button_secondary, hand);
        inputs.menu = boolean(m_menu, hand);
        inputs.thumbstick_click = boolean(m_thumbstick_click, hand);
        inputs.trigger = scalar(m_trigger, hand);
        inputs.squeeze = scalar(m_squeeze, hand);
        const XrVector2f stick = vector(m_thumbstick, hand);
        inputs.stick_x = stick.x;
        inputs.stick_y = stick.y;
    }

    OpenXRUiSnapshot ui{}; ui.active=true; ui.buttons=hands;
    const auto locate=[&](XrSpace space, UiHandPose& out) {
        if (space==XR_NULL_HANDLE) return;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(space,m_runtime->AppSpace(),predicted_display_time,&location)) ||
            (location.locationFlags&valid)!=valid) return;
        const auto& q=location.pose.orientation; const auto& p=location.pose.position;
        out.valid=true; out.position={p.x,p.y,p.z};
        out.right=RotateUiVector(q.x,q.y,q.z,q.w,{1,0,0});
        out.up=RotateUiVector(q.x,q.y,q.z,q.w,{0,1,0});
        out.forward=RotateUiVector(q.x,q.y,q.z,q.w,{0,0,-1});
    };
    for(int hand=0;hand<2;++hand) { locate(m_grip_spaces[hand],ui.hands[hand]); locate(m_aim_spaces[hand],ui.aims[hand]); }
    OpenXRPublishUiSnapshot(ui);

    PollInjection();
    if (Injected("up")) {
        hands[0].stick_y = 1.0f;
    } else if (Injected("down")) {
        hands[0].stick_y = -1.0f;
    } else if (Injected("left")) {
        hands[0].stick_x = -1.0f;
    } else if (Injected("right")) {
        hands[0].stick_x = 1.0f;
    }

    const XrTime input_time = InputSampleTime(predicted_display_time);
    const float dt_seconds =
        m_last_input_time != 0 && input_time > m_last_input_time
            ? static_cast<float>(input_time - m_last_input_time) * 1.0e-9f
            : 0.0f;
    m_last_input_time = input_time;
    // Steam Frame layout (see m_frame_item). The layout latches when the runtime reports the Frame
    // profile on either hand; as a fallback (a SteamVR binding file on another profile) when any
    // frame_* action reports active.
    bool frame_bound=false;
    const auto frame_button=[&](XrAction action) {
        if(action==XR_NULL_HANDLE) return false;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action=action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        if(XR_FAILED(xrGetActionStateBoolean(m_runtime->Session(),&info,&state)) || state.isActive!=XR_TRUE) return false;
        frame_bound=true;
        return state.currentState==XR_TRUE;
    };
    const auto frame_float=[&](XrAction action) {
        if(action==XR_NULL_HANDLE) return 0.f;
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action=action;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        if(XR_FAILED(xrGetActionStateFloat(m_runtime->Session(),&info,&state)) || state.isActive!=XR_TRUE) return 0.f;
        frame_bound=true;
        return state.currentState;
    };
    const bool frame_item=frame_button(m_frame_item), frame_look_back=frame_button(m_frame_look_back);
    const bool frame_drift=frame_button(m_frame_drift), frame_start=frame_button(m_frame_start);
    const bool frame_back=frame_button(m_frame_back), frame_brake=frame_button(m_frame_brake);
    const float frame_accelerate=frame_float(m_frame_accelerate), frame_reverse=frame_float(m_frame_reverse);
    std::array<bool,4> frame_dpad{};
    for(size_t i=0;i<frame_dpad.size();++i) frame_dpad[i]=frame_button(m_frame_dpad[i]);
    // Latched for the session: SteamVR briefly reports the actions inactive (a controller dozing,
    // focus changes), and the layout must not switch under the player mid-race.
    if(!m_frame_layout && m_frame_profile!=XR_NULL_PATH) {
        for(uint32_t hand=0;hand<kHands && !m_frame_layout;++hand) {
            XrInteractionProfileState current{XR_TYPE_INTERACTION_PROFILE_STATE};
            if(XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_runtime->Session(),m_hand_paths[hand],&current)) &&
               current.interactionProfile==m_frame_profile) {
                m_frame_layout=true;
                Log(OpenXRLogLevel::Info,"Steam Frame button layout active (interaction profile)");
            }
        }
    }
    if(frame_bound && !m_frame_layout) {
        m_frame_layout=true;
        Log(OpenXRLogLevel::Info,"Steam Frame button layout active (frame actions bound)");
    }
    // View: a tap is GameCube B (back), a 0.5 s hold opens the VR settings, and with the settings
    // open a press closes them. The tap is sent on release so that a hold never also goes back.
    const auto view=m_frame_view.Update(frame_back,input_time,OpenXRSettingsPanelOpen());
    const bool frame_back_panel=view.open_panel;
    const bool frame_back_pulse=view.back_pulse;

    std::array<wii_remote::HandInputs, kHands> panel_hands = hands;
    panel_hands[0].primary |= boolean(m_options_x,0);
    panel_hands[0].secondary |= boolean(m_options_y,0);
    if(frame_back_panel) panel_hands[0].primary=panel_hands[0].secondary=true;
    if(view.close_press) panel_hands[0].menu=true;
    const auto physical=m_runtime->RuntimeInfo().runtime_name.find("SteamVR")!=std::string::npos ?
        OpenXRReadPhysicalOptionsButtons() : PhysicalOptionsButtons{};
    if(physical.valid) {
        panel_hands[0].primary |= physical.x;
        panel_hands[0].secondary |= physical.y;
    }
    if(physical.valid!=m_native_options_valid) {
        m_native_options_valid=physical.valid;
        Log(OpenXRLogLevel::Info,physical.valid ? "VR options: native SteamVR face buttons available" :
            "VR options: native SteamVR face buttons unavailable; using OpenXR actions");
    }
    const uint8_t options=(panel_hands[0].primary?1:0)|(panel_hands[0].secondary?2:0);
    if(options!=m_options_previous) {
        m_options_previous=options;
        Log(OpenXRLogLevel::Info,"VR options left buttons: X="+
            std::to_string(bool(options&1))+", Y="+std::to_string(bool(options&2))+
            ", native="+std::to_string(physical.valid)+", native-X="+std::to_string(physical.x)+
            ", native-Y="+std::to_string(physical.y));
    }
    if (Injected("panel")) {
        // The panel button in either controller mode.
        panel_hands[0].primary = true;
        panel_hands[0].secondary = true;
    }
    if (Injected("a")) {
        panel_hands[1].primary = true;
    }
    // The game thread may open or close the panel too; only a change made here
    // is written back.
    const bool introduction = OpenXRIntroductionActive() && !OpenXRHandWorkshopActive();
    const bool was_open = OpenXRSettingsPanelOpen();
    bool open = was_open || introduction;
    settings_panel::Frame panel =
        m_panel_controls.Update(panel_hands, open, dt_seconds, OpenXRGetControllerMode(), introduction);
    // Pointer first: the game thread reads it as soon as it sees the panel open.
    PublishSettingsPanel(input_time, settings_panel, panel);
    // Introduction visibility is temporary and must never latch the settings
    // flag, including when the game thread finishes it during this input frame.
    if (!introduction && (!OpenXRIntroductionActive() || OpenXRHandWorkshopActive()) && open != was_open) {
        Log(OpenXRLogLevel::Info, open ? "VR settings opened with controller chord" : "VR settings closed with controller chord");
        OpenXRSetSettingsPanelOpen(open);
    }

    // Calibration owns the controls while the game continues rendering its
    // paused cockpit. Raw UiSnapshot buttons remain available for capture.
    panel.withheld |= OpenXRReadBodyHandCalibration().active;
    // A clean right-thumbstick click toggles the first-person camera. It fires
    // on release, so the two-thumbstick panel chord never toggles it, and
    // never while the panel has the controllers.
    if (m_first_person_click.Update(hands[1].thumbstick_click, hands[0].thumbstick_click,
                                    panel.open || panel.withheld) &&
        RuntimeConfigFile::VrFirstPersonToggleClick()) {
        settings_overlay::RequestFirstPersonToggle();
        constexpr XrDuration kToggleTickNs = 20'000'000;
        ApplyHaptic(1, 0.35f, kToggleTickNs);
    }

    // The cockpit's wheel before the game reads the controllers: a held wheel
    // steers through the left stick and keeps its grips from the game.
    UpdateDriving(predicted_display_time, seat, hands, panel.withheld);

    QuestInput port{};
    port.active=true;
    if (screen.valid) {
        const auto& q=screen.pose.orientation;
        const auto local=[&](const UiHandPose& aim) {
            UiHandPose ray=aim;
            const auto& p=screen.pose.position;
            ray.position=RotateUiVector(-q.x,-q.y,-q.z,q.w,
                {aim.position[0]-p.x,aim.position[1]-p.y,aim.position[2]-p.z});
            ray.position[2]-=RuntimeConfigFile::VrHudDistanceMeters();
            ray.forward=RotateUiVector(-q.x,-q.y,-q.z,q.w,aim.forward);
            return ray;
        };
        port.ui_pointer=local(ui.aims[1]);port.ui_left_pointer=local(ui.aims[0]);
        port.ui_aspect=screen.half_height_meters>0 ? screen.half_width_meters/screen.half_height_meters : 16.f/9.f;
    }
    port.steamvr=m_runtime->RuntimeInfo().runtime_name.find("SteamVR")!=std::string::npos;
    port.cockpit_controls=MkwVRGetCameraMode()==CameraMode::FirstPerson && MkwVRPolicyGetSnapshot().scene.mode==VRSceneMode::Race;
    port.raw_steering_x=ui.buttons[0].stick_x;port.raw_steering_y=ui.buttons[0].stick_y;
    port.steering_x=hands[0].stick_x;port.steering_y=hands[0].stick_y;
    port.tricks_x=hands[1].stick_x;port.tricks_y=hands[1].stick_y;
    port.accelerate=hands[1].trigger;port.reverse=hands[0].trigger>.5f;
    port.confirm=hands[1].primary;port.brake=hands[1].secondary;
    XrInteractionProfileState rightProfile{XR_TYPE_INTERACTION_PROFILE_STATE};
    if(port.cockpit_controls && m_vive_profile!=XR_NULL_PATH &&
       XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_runtime->Session(),m_hand_paths[1],&rightProfile)) &&
       rightProfile.interactionProfile==m_vive_profile) {
        // Vive wands have no face-button cluster. Right Menu drifts in the
        // cockpit; the left trigger remains the brake/reverse control.
        port.confirm=hands[1].secondary;port.brake=false;
    }
    port.item=hands[0].secondary?1.f:0.f;port.trick=hands[0].primary;
    port.drift=ui.buttons[1].squeeze;
    port.wheel_active=m_driving.held[0]||m_driving.held[1];
    port.wheel_steering=m_driving.steering_input;
    const auto pause=m_trick_pause.Update(port.active && !panel.withheld,port.trick,
                                         hands[0].secondary,predicted_display_time);
    if(port.steamvr) {
        port.trick=pause.trick;
        if(pause.pause && !panel.withheld) OpenXRRequestTutorialPause();
    } else port.pause=hands[0].menu;
    if(m_frame_layout) {
        // A modern gamepad layout over the GameCube pad, the same in both views (frame_controls.h).
        // D-pad up goes through the tricks axis, not `trick`, which MapQuestInput reads with the
        // item as the VR settings chord. Grips only hold the wheel in the cockpit with hand
        // steering on; the right bumper drifts everywhere.
        frame::Inputs in;
        in.right_trigger=std::max(frame_accelerate,hands[1].trigger);
        in.left_trigger=std::max(frame_reverse,hands[0].trigger);
        in.right_squeeze=ui.buttons[1].squeeze;in.left_squeeze=ui.buttons[0].squeeze;
        in.a=hands[1].primary;in.b=frame_brake||hands[1].secondary;
        in.x=frame_item;in.left_bumper=false;in.y=frame_look_back;
        in.right_bumper=frame_drift;in.menu=frame_start;in.view=frame_back;
        in.dpad=frame_dpad;
        in.left_stick_x=hands[0].stick_x;in.left_stick_y=hands[0].stick_y;
        in.right_stick_x=hands[1].stick_x;in.right_stick_y=hands[1].stick_y;
        frame::Context ctx;
        ctx.grips_hold_wheel=port.cockpit_controls && RuntimeConfigFile::VrHandSteering();
        ctx.view_back_pulse=frame_back_pulse;
        port=frame::Map(in,ctx,port);
    }
    // Throwing the held item: a quick swing of the item hand forward or back plays an aimed throw.
    UpdateItemThrow(dt_seconds, hands, port, panel.withheld);
    if(panel.withheld) {
        const auto rawX=port.raw_steering_x,rawY=port.raw_steering_y;
        port=QuestInput{.steamvr=port.steamvr,.active=true};port.raw_steering_x=rawX;port.raw_steering_y=rawY;
    }
    OpenXRPublishPortControls(port);
    // While the panel has the controllers, the game sees them idle.
    static const std::array<wii_remote::HandInputs, kHands> kIdleHands{};
    const auto& game_hands = panel.withheld ? kIdleHands : hands;
    const wii_remote::HandInputs& left = game_hands[0];
    const wii_remote::HandInputs& right = game_hands[1];
    const auto injected = [&panel](const char* button) { return !panel.withheld && Injected(button); };

    if (m_joystick != nullptr) {
        VirtualGamepadRelay::Pad pad;
        // OpenXR thumbsticks report +Y up; SDL gamepads report +Y down.
        pad.axes[SDL_GAMEPAD_AXIS_LEFTX] = ToAxis(left.stick_x);
        pad.axes[SDL_GAMEPAD_AXIS_LEFTY] = ToAxis(-left.stick_y);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHTX] = ToAxis(right.stick_x);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHTY] = ToAxis(-right.stick_y);
        pad.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = ToTrigger(left.trigger);
        pad.axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = ToTrigger(right.trigger);

        pad.buttons[SDL_GAMEPAD_BUTTON_SOUTH] = right.primary || injected("a");
        pad.buttons[SDL_GAMEPAD_BUTTON_EAST] = right.secondary || injected("b");
        pad.buttons[SDL_GAMEPAD_BUTTON_WEST] = left.primary || injected("x");
        pad.buttons[SDL_GAMEPAD_BUTTON_NORTH] = left.secondary || injected("y");
        pad.buttons[SDL_GAMEPAD_BUTTON_START] = left.menu || injected("start");
        pad.buttons[SDL_GAMEPAD_BUTTON_LEFT_STICK] = left.thumbstick_click;
        pad.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = right.thumbstick_click;
        pad.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = left.squeeze > 0.5f;
        pad.buttons[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = right.squeeze > 0.5f;
        Relay().Publish(pad);
    }

    PublishWiiRemote(input_time, screen, game_hands, panel.withheld ? 0u : InjectedWiiRemoteButtons(),
                     panel.withheld);
    UpdateRumble();
}

// The pointing hand's aim ray against the whole panel, in canvas pixels, with a
// short tick in that hand when a selection starts.
void OpenXRInput::PublishSettingsPanel(XrTime input_time, const OpenXRPointerScreen& panel,
                                       const settings_panel::Frame& frame) {
    if (!frame.open) {
        // Nothing may still read as held when the panel next opens.
        m_panel_select_held = false;
        OpenXRPublishSettingsPanelPointer(false, 0.0f, 0.0f, false, 0.0f);
        return;
    }
    constexpr XrSpaceLocationFlags kPoseValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool valid = false;
    std::array<float, 2> point{};
    const XrSpace aim_space = m_aim_spaces[frame.pointing_hand];
    if (panel.valid && aim_space != XR_NULL_HANDLE) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(aim_space, m_runtime->AppSpace(), input_time, &location)) &&
            (location.locationFlags & kPoseValid) == kPoseValid) {
            wii_remote::Screen target{};
            target.pose = ToWiiRemotePose(panel.pose);
            target.half_width = panel.half_width_meters;
            target.half_height = panel.half_height_meters;
            const wii_remote::ScreenHit hit = wii_remote::RaycastScreen(ToWiiRemotePose(location.pose), target);
            if (hit.valid) {
                valid = true;
                point = settings_panel::CanvasPoint(hit);
            }
        }
    }
    if (frame.select && !m_panel_select_held) {
        constexpr XrDuration kTickNs = 15'000'000;
        ApplyHaptic(frame.pointing_hand, 0.35f, kTickNs);
    }
    m_panel_select_held = frame.select;
    OpenXRPublishSettingsPanelPointer(valid, point[0], point[1], frame.select, frame.wheel,frame.pointing_hand);
}

void OpenXRInput::PublishWiiRemote(XrTime input_time, const OpenXRPointerScreen& screen,
                                   const std::array<wii_remote::HandInputs, kHands>& hands,
                                   uint32_t injected_buttons, bool withheld) {
    constexpr XrSpaceLocationFlags kPoseValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;

    OpenXRWiiRemoteSample sample{};
    sample.hold = wii_remote::RemoteButtons(hands[0], hands[1]) | injected_buttons;
    sample.stick = wii_remote::NunchukStick(hands[0]);

    // Left is the Nunchuk, right is the remote.
    std::array<wii_remote::Pose, kHands> aims{};
    std::array<bool, kHands> aim_valid{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        if (m_aim_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(m_aim_spaces[hand], m_runtime->AppSpace(), input_time, &location)) &&
                (location.locationFlags & kPoseValid) == kPoseValid) {
                aims[hand] = ToWiiRemotePose(location.pose);
                aim_valid[hand] = true;
            }
        }
        wii_remote::Vec3 grip_position{};
        wii_remote::Vec3 grip_velocity{};
        bool position_valid = false;
        bool velocity_valid = false;
        if (m_grip_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            location.next = &velocity;
            if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], m_runtime->AppSpace(), input_time, &location))) {
                position_valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                velocity_valid = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
                grip_position = {location.pose.position.x, location.pose.position.y, location.pose.position.z};
                grip_velocity = {velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z};
            }
        }
        const wii_remote::Vec3 acc =
            m_motion[hand].Update(aim_valid[hand] ? &aims[hand].orientation : nullptr,
                                  position_valid ? &grip_position : nullptr,
                                  velocity_valid ? &grip_velocity : nullptr, input_time);
        (hand == 0 ? sample.nunchuk_acc : sample.acc) = acc;
    }

    if (withheld) {
        // Waving a controller around the panel must not trick or wheelie.
        sample.acc = OpenXRWiiRemoteSample{}.acc;
        sample.nunchuk_acc = OpenXRWiiRemoteSample{}.nunchuk_acc;
        m_pointer.Reset();
        OpenXRPublishWiiRemote(m_joystick_id, sample);
        return;
    }

    wii_remote::Screen target{};
    wii_remote::ScreenHit hit{};
    if (screen.valid && aim_valid[1]) {
        target.pose = ToWiiRemotePose(screen.pose);
        target.half_width = screen.half_width_meters;
        target.half_height = screen.half_height_meters;
        hit = wii_remote::RaycastScreen(aims[1], target);
    }
    if (screen.valid && aim_valid[1]) {
        m_horizon = wii_remote::Horizon(aims[1], target);
    }
    const wii_remote::ScreenHit pointer = m_pointer.Update(hit, input_time);
    if (pointer.valid) {
        sample.pointer_valid = true;
        sample.pointer = wii_remote::KpadPosition(pointer);
        // Held with the position through a tracking blip.
        sample.horizon = m_horizon;
        sample.distance_meters = pointer.distance_meters;
        if (!m_logged_pointer) {
            m_logged_pointer = true;
            Log(OpenXRLogLevel::Info, "OpenXR Wii Remote pointer reached the virtual screen");
        }
    }
    OpenXRPublishWiiRemote(m_joystick_id, sample);
}

void OpenXRInput::ResetDriving() {
    m_wheel = {};
    WheelGeometry unused{};
    m_wheel_reference.Resolve(unused, false, false, false, false, 0, 0.0f);
    m_wheel_visual.Reset();
    m_wheel_held = {};
    m_wheel_time = 0;
    m_driving = {};
    m_throw.Reset();
    m_throw_sequence.Reset();
    OpenXRPublishDriving(m_driving);
}

void OpenXRInput::LoadItemThrowSettings() {
    const auto flag = [](const std::string& text, bool fallback) {
        if (text == "0" || text == "false" || text == "off") return false;
        if (text == "1" || text == "true" || text == "on") return true;
        return fallback;
    };
    const auto number = [](const std::string& text, float fallback, float low, float high) {
        if (text.empty()) return fallback;
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        return end != text.c_str() && value >= low && value <= high ? value : fallback;
    };
    m_throw_enabled = flag(ItemThrowSetting("KARTFRAME_THROW", "cockpit_item_throw"), true);
    const std::string hand = ItemThrowSetting("KARTFRAME_THROW_HAND", "cockpit_item_hand");
    if (hand == "off") m_throw_enabled = false;
    m_throw_hand = hand == "right" ? 1u : 0u;
    // Quest-tuned defaults (kControllerTuning: 18 cm forward, 15 cm back within 120 ms).
    item_throw::Tuning tuning = item_throw::kControllerTuning;
    tuning.forward_travel =
        number(ItemThrowSetting("KARTFRAME_THROW_FORWARD_CM", "item_throw_forward_cm"), tuning.forward_travel * 100.0f, 5.0f, 60.0f) / 100.0f;
    tuning.backward_travel =
        number(ItemThrowSetting("KARTFRAME_THROW_BACK_CM", "item_throw_back_cm"), tuning.backward_travel * 100.0f, 5.0f, 60.0f) / 100.0f;
    // The detector keeps 32 samples: about 0.26 s at 120 Hz.
    tuning.window_seconds =
        number(ItemThrowSetting("KARTFRAME_THROW_WINDOW_MS", "item_throw_window_ms"), tuning.window_seconds * 1000.0f, 40.0f, 250.0f) / 1000.0f;
    m_throw.SetTuning(tuning);
    m_throw.Reset();
    m_throw_sequence.Reset();
    // KARTFRAME_THROW_INJECT=<start s>/<every s>[/f|b|a] (tests): throws without a swing, forward,
    // backward or alternating, like the Quest's adb injection; they skip the item check.
    m_throw_inject_start = -1.0f;
    m_throw_inject_count = 0;
    if (const char* inject = std::getenv("KARTFRAME_THROW_INJECT"); inject != nullptr && *inject != '\0') {
        float start = 0.0f, every = 0.0f;
        char dir = 'f';
        if (std::sscanf(inject, "%f/%f/%c", &start, &every, &dir) >= 2 && start >= 0.0f && every > 0.2f) {
            m_throw_inject_start = start;
            m_throw_inject_every = every;
            m_throw_inject_dir = dir == 'b' || dir == 'a' ? dir : 'f';
        }
    }
    m_throw_created = std::chrono::steady_clock::now();
    std::ostringstream message;
    message << "item throw: " << (m_throw_enabled ? "on" : "off") << ", " << (m_throw_hand ? "right" : "left")
            << " hand, " << std::lround(tuning.forward_travel * 100.0f) << " cm forward / "
            << std::lround(tuning.backward_travel * 100.0f) << " cm back within "
            << std::lround(tuning.window_seconds * 1000.0f) << " ms";
    if (m_throw_inject_start >= 0.0f) {
        message << "; injected from " << m_throw_inject_start << " s every " << m_throw_inject_every << " s ("
                << m_throw_inject_dir << ')';
    }
    message << "; gamepad rumble ";
    if (OpenXRGamepadRumbleForwarded()) {
        message << "amplitude " << OpenXRGamepadRumbleBase();
    } else {
        message << "off (KARTFRAME_RUMBLE)";
    }
    Log(OpenXRLogLevel::Info, message.str());
    m_gamepad_rumble.SetBase(OpenXRGamepadRumbleBase());
}

void OpenXRInput::UpdateItemThrow(float dt_seconds, std::array<wii_remote::HandInputs, kHands>& hands, QuestInput& port,
                                  bool withheld) {
    const uint32_t hand = m_throw_hand;
    item_throw::Direction direction = item_throw::Direction::None;
    if (!withheld && m_driving.cockpit_active && m_throw_enabled) {
        // The grip of the hand that shows the item, in the seated frame. The detector follows it with or
        // without an item, so it always knows when the hand left the wheel.
        const DrivingHand& located = m_driving.hands[hand];
        const std::array<float, 12>& pose = located.seat_from_grip;
        item_throw::HandSample sample{};
        sample.tracked = located.tracked;
        sample.held = located.held;
        sample.bare = false; // no hand tracking on the native port
        sample.position = {pose[3], pose[7], pose[11]};
        direction = m_throw.Update(sample, dt_seconds);
        // A swing with nothing to throw throws nothing: an item in the hand, or one already out
        // behind or around the kart (which empties the hand).
        if (direction != item_throw::Direction::None) {
            const HeldItem item = MkwVRFirstPersonGetHeldItem();
            if (!item.tethered && (!item.valid || item.count == 0)) {
                const auto& measure = m_throw.Last();
                std::ostringstream message;
                message << "Swing " << item_throw::DirectionLabel(direction) << " ignored: nothing to throw ("
                        << std::lround(std::fabs(measure.travel) * 100.0f) << " cm in "
                        << std::lround(measure.seconds * 1000.0f) << " ms)";
                Log(OpenXRLogLevel::Info, message.str());
                direction = item_throw::Direction::None;
            }
        }
    } else {
        m_throw.Reset();
    }
    // Injected throws: the Quest's adb property (Android), KARTFRAME_THROW_INJECT (tests).
    const bool injected_forward = Injected("throw_forward");
    const bool injected_backward = Injected("throw_backward");
    bool injected = false;
    if ((injected_forward || injected_backward) && !m_injected_throw_held && !withheld) {
        direction = injected_forward ? item_throw::Direction::Forward : item_throw::Direction::Backward;
        injected = true;
    }
    m_injected_throw_held = injected_forward || injected_backward;
    if (m_throw_inject_start >= 0.0f && !withheld) {
        const float since = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_throw_created).count();
        if (since >= m_throw_inject_start + m_throw_inject_every * static_cast<float>(m_throw_inject_count)) {
            const bool back = m_throw_inject_dir == 'b' || (m_throw_inject_dir == 'a' && (m_throw_inject_count & 1));
            direction = back ? item_throw::Direction::Backward : item_throw::Direction::Forward;
            injected = true;
            ++m_throw_inject_count;
        }
    }
    if (direction != item_throw::Direction::None) {
        m_throw_sequence.Start(direction);
        if (RuntimeConfigFile::VrWheelTuning().haptics) {
            constexpr XrDuration kThrowPulseNs = 40'000'000;
            ApplyHaptic(hand, 0.5f, kThrowPulseNs);
        }
        // What the swing measured, to tune the thresholds from a run log.
        std::ostringstream message;
        message << "Held item thrown " << item_throw::DirectionLabel(direction);
        if (injected) {
            message << " (injected)";
        } else {
            const auto& measure = m_throw.Last();
            message << " (controller, " << std::lround(std::fabs(measure.travel) * 100.0f) << " cm in "
                    << std::lround(measure.seconds * 1000.0f) << " ms" << (measure.bridged ? ", across a tracking gap" : "")
                    << ')';
        }
        message << (OpenXRGetControllerMode() == OpenXRControllerMode::Gamepad ? " on the port" : " on the Nunchuk");
        Log(OpenXRLogLevel::Info, message.str());
    }
    if (withheld) {
        m_throw_sequence.Reset();
        return;
    }
    if (OpenXRGetControllerMode() == OpenXRControllerMode::Gamepad) {
        // pad.cpp maps this port over GameCube port 0: stick Y aims (a held wheel only replaces X),
        // the item field is L. With swap_item_trick the game's item button is the trick field.
        const bool swap = RuntimeConfigFile::Get().vrButtonMapping.swapItemTrick;
        float item = swap ? (port.trick ? 1.0f : 0.0f) : port.item;
        item_throw::ApplyToPort(m_throw_sequence, port.steering_y, item, dt_seconds);
        if (swap) {
            port.trick = item > 0.5f;
        } else {
            port.item = item;
        }
    } else {
        m_throw_sequence.Apply(hands[0], dt_seconds);
    }
}

void OpenXRInput::UpdateDriving(XrTime display_time, const driving::SeatFrame& seat,
                                std::array<wii_remote::HandInputs, kHands>& hands, bool withheld) {
    const FirstPersonAnchor anchor = MkwVRFirstPersonGetAnchor();
    if (!seat.valid || !anchor.valid || !anchor.cockpit) {
        if (m_driving.cockpit_active || m_wheel_time != 0) {
            ResetDriving();
        }
        return;
    }
    const WheelTuning tuning = RuntimeConfigFile::VrWheelTuning();
    const bool hand_steering = RuntimeConfigFile::VrHandSteering();
    const bool steering_wheel = RuntimeConfigFile::VrSteeringWheel();
    const bool native_steering_wheel = RuntimeConfigFile::VrNativeSteeringWheel();
    const float dt = m_wheel_time != 0 && display_time > m_wheel_time
                         ? static_cast<float>(display_time - m_wheel_time) * 1.0e-9f
                         : 1.0f / 90.0f;
    m_wheel_time = display_time;

    DrivingSnapshot snapshot{};
    snapshot.cockpit_active = true;
    snapshot.vehicle_identity = anchor.vehicle_identity;
    snapshot.hand_steering = hand_steering;
    snapshot.bike = anchor.bike;
    // The vehicle's own control is the one turning (or none is shown at all),
    // so the overlay adds no separate wheel.
    snapshot.synthetic_control =
        steering_wheel && !(native_steering_wheel && anchor.native_mesh_prepared);

    // Which control the hands reach for: the vehicle's own wherever its
    // geometry is known and no separate wheel is drawn, a handlebar always
    // (held over a brief gap while gripped), otherwise the VR wheel in front
    // of the seat.
    WheelGeometry geometry = anchor.native_wheel;
    const bool geometry_valid = m_wheel_reference.Resolve(geometry, true, geometry.valid,
                                                          m_wheel_held[0] || m_wheel_held[1], anchor.bike,
                                                          anchor.vehicle_identity, dt);
    if (anchor.bike && !geometry_valid) {
        geometry = {};
        geometry.center = {0.0f, SteeringWheel::Height, SteeringWheel::Depth};
        geometry.right = {1.0f, 0.0f, 0.0f};
        geometry.up = {0.0f, 0.0f, -1.0f};
        geometry.normal = {0.0f, 1.0f, 0.0f};
        geometry.radius = 0.25f;
        geometry.valid = true;
    }
    const bool uses_geometry = anchor.bike || (geometry_valid && !snapshot.synthetic_control);
    if (uses_geometry != m_wheel_uses_geometry || anchor.bike != m_wheel_bike) {
        m_wheel = {};
        m_wheel_uses_geometry = uses_geometry;
        m_wheel_bike = anchor.bike;
    }
    snapshot.control = geometry;

    constexpr XrSpaceLocationFlags kPoseValid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    XrSpaceLocation head_location{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(m_runtime->ViewSpace(), m_runtime->AppSpace(), display_time, &head_location)) &&
        (head_location.locationFlags & kPoseValid) == kPoseValid) {
        const auto& pose = head_location.pose;
        snapshot.head_tracked = true;
        snapshot.seat_from_head = driving::SeatFromApp(seat,
            {pose.position.x,pose.position.y,pose.position.z},
            {pose.orientation.x,pose.orientation.y,pose.orientation.z,pose.orientation.w});
    }
    std::array<WheelHand, kHands> wheel_hands{};
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        bool tracked = false;
        std::array<float, 12> seat_from_grip = snapshot.hands[hand].seat_from_grip;
        if (m_grip_spaces[hand] != XR_NULL_HANDLE) {
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_SUCCEEDED(xrLocateSpace(m_grip_spaces[hand], m_runtime->AppSpace(), display_time, &location)) &&
                (location.locationFlags & kPoseValid) == kPoseValid) {
                tracked = true;
                const auto& pose = location.pose;
                seat_from_grip = driving::SeatFromApp(seat, {pose.position.x, pose.position.y, pose.position.z},
                                                      {pose.orientation.x, pose.orientation.y,
                                                       pose.orientation.z, pose.orientation.w});
            }
        }
        const float squeeze = hands[hand].squeeze;
        // Hands are shown only while they can steer.
        snapshot.hands[hand] = {tracked && (hand_steering || RuntimeConfigFile::VrBodyIk()), false, squeeze, seat_from_grip};
        wheel_hands[hand] = {seat_from_grip[3], seat_from_grip[7], seat_from_grip[11], squeeze, tracked};
        if (uses_geometry) {
            wheel_hands[hand] = geometry.ToWheel(wheel_hands[hand]);
        }
    }
    // A USB wheel drives the race through the GameCube pad: it steers, the
    // cockpit's wheel shows its angle, and the hands cannot take hold.
    float hardware_steering = 0.0f;
    const bool hardware_wheel = physical_wheel::SteeringSnapshot(hardware_steering);
    const bool active = hand_steering && !withheld && !hardware_wheel;
    const WheelState wheel = m_wheel.Update(wheel_hands, active, dt,
                                            uses_geometry ? geometry.radius : SteeringWheel::Radius,
                                            anchor.bike, tuning);
    for (uint32_t hand = 0; hand < kHands; ++hand) {
        if (wheel.held[hand] != m_wheel_held[hand] && active && tuning.haptics) {
            constexpr XrDuration kGrabPulseNs = 25'000'000;
            constexpr XrDuration kReleasePulseNs = 15'000'000;
            ApplyHaptic(hand, wheel.held[hand] ? 0.25f : 0.12f, wheel.held[hand] ? kGrabPulseNs : kReleasePulseNs);
        }
        snapshot.hands[hand].held = wheel.held[hand];
    }
    m_wheel_held = wheel.held;
    snapshot.held = wheel.held;
    driving::ApplyHandSteering(hands, wheel);
    const float max_angle = driving::MaxWheelAngle(anchor.bike, tuning);
    if (hardware_wheel) {
        snapshot.steering_input = std::clamp(hardware_steering, -1.0f, 1.0f);
        // The hardware wheel is already smooth; follow it directly.
        snapshot.visual_angle = m_wheel_visual.Update(true, snapshot.steering_input * max_angle, 0.0f, max_angle, dt);
    } else {
        snapshot.steering_input = withheld ? 0.0f : hands[0].stick_x;
        snapshot.visual_angle = m_wheel_visual.Update(wheel.held[0] || wheel.held[1], wheel.visualAngle,
                                                      snapshot.steering_input, max_angle, dt);
    }
    m_driving = snapshot;
    OpenXRPublishDriving(snapshot);
}

void OpenXRInput::UpdateRumble() {
    if (OpenXRGetControllerMode() == OpenXRControllerMode::Gamepad) {
        // The GameCube motor (PAD__ControlMotor_HLE), shaped into an amplitude (openxr_gamepad_rumble.h).
        const bool motor = OpenXRGamepadRumbleForwarded() && OpenXRGamepadRumbleMotor();
        const bool was_playing = m_gamepad_rumble.Playing();
        const auto out = m_gamepad_rumble.Update(motor);
        if (out.action == gamepad_rumble::Shaper::Action::Pulse) {
            for (uint32_t hand = 0; hand < kHandCount; ++hand) {
                ApplyHaptic(hand, out.amplitude, out.duration_ns);
                m_haptics_active[hand] = true;
            }
        } else if (out.action == gamepad_rumble::Shaper::Action::Stop) {
            StopRumble();
        }
        // A line per rumble (the first 20, then every 100th), for the run log.
        if (m_gamepad_rumble.Playing() != was_playing) {
            if (m_gamepad_rumble.Playing()) ++m_gamepad_rumble_starts;
            if (m_gamepad_rumble_starts <= 20 || m_gamepad_rumble_starts % 100 == 0) {
                std::ostringstream message;
                message << "gamepad rumble #" << m_gamepad_rumble_starts
                        << (m_gamepad_rumble.Playing() ? " on, amplitude " : " off");
                if (m_gamepad_rumble.Playing()) message << out.amplitude;
                Log(OpenXRLogLevel::Info, message.str());
            }
        }
        return;
    }
    m_gamepad_rumble.Reset();
    if (!OpenXRWiiRemoteRumbleRequested() || !OpenXRWiiRemoteOwnsGamepad(m_joystick_id)) {
        StopRumble();
        return;
    }
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        ApplyHaptic(hand, 1.0f, kRumblePulseNs);
        m_haptics_active[hand] = true;
    }
}

void OpenXRInput::StopRumble() {
    for (uint32_t hand = 0; hand < kHandCount; ++hand) {
        if (m_haptics_active[hand]) {
            ApplyHaptic(hand, 0.0f, 0);
            m_haptics_active[hand] = false;
        }
    }
}

void OpenXRInput::ApplyHaptic(uint32_t hand, float amplitude, XrDuration duration) {
    if (!m_created || m_runtime == nullptr || hand >= kHandCount || m_haptic == XR_NULL_HANDLE) {
        return;
    }
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = duration;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = m_haptic;
    info.subactionPath = m_hand_paths[hand];
    if (vibration.amplitude <= 0.0f) {
        xrStopHapticFeedback(m_runtime->Session(), &info);
        return;
    }
    xrApplyHapticFeedback(m_runtime->Session(), &info,
                          reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

bool OpenXRInput::Check(XrResult result, const char* operation) {
    if (m_runtime != nullptr) {
        m_runtime->ObserveResult(result);
    }
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    std::ostringstream message;
    message << operation << " failed (" << result << ')';
    m_last_error = message.str();
    Log(OpenXRLogLevel::Error, m_last_error);
    return false;
}

void OpenXRInput::Log(OpenXRLogLevel level, const std::string& message) const noexcept {
    if (!m_logger) {
        return;
    }
    try {
        m_logger(level, message);
    } catch (...) {
    }
}

} // namespace mkw::vr

#endif // defined(MKW_ENABLE_OPENXR)

// The GameCube motor bridge (openxr_input.h), in every build: pad.cpp calls it with or without OpenXR.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace mkw::vr {
// Named rather than anonymous: runtime sources are unity-built in groups.
namespace gamepad_rumble_bridge {
std::atomic<bool> g_motor{false};
} // namespace gamepad_rumble_bridge

float OpenXRGamepadRumbleBase() noexcept {
    static const float base = [] {
        constexpr float kDefault = 0.7f;
        const char* value = std::getenv("KARTFRAME_RUMBLE");
        if (value == nullptr || *value == '\0' || !std::strcmp(value, "1") || !std::strcmp(value, "on")) return kDefault;
        if (!std::strcmp(value, "0") || !std::strcmp(value, "off")) return 0.0f;
        char* end = nullptr;
        const float amplitude = std::strtof(value, &end);
        if (end == value || !(amplitude >= 0.0f)) return kDefault;
        return amplitude <= 0.0f ? 0.0f : std::clamp(amplitude, 0.05f, 1.0f);
    }();
    return base;
}

void OpenXRSetGamepadRumble(bool on) noexcept {
    gamepad_rumble_bridge::g_motor.store(on, std::memory_order_relaxed);
}

bool OpenXRGamepadRumbleMotor() noexcept {
    return gamepad_rumble_bridge::g_motor.load(std::memory_order_relaxed);
}
} // namespace mkw::vr
