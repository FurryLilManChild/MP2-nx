// mp2-nx: the port menu on the Switch (opened with + and - together, the port's own PortMenu
// binding). Echoes Unbound's F1 window is built for a mouse and keyboard; on the Switch neither
// the pad nor the touch screen reached it (test2 and test3). This menu reads the Joy-Cons and the
// touch screen straight from libnx, as the earlier ports' menus do, and draws with ImGui's draw
// lists only, so no ImGui navigation or focus is involved. It changes the same settings the F1
// window does (port_overlay.cpp's variables and setters), so they are saved to the same
// settings.cfg, and adds what the Switch needs: save states, a resolution for handheld and one
// for docked (applied when the console changes mode), and an FPS counter.
#include "../../include/mp2nx_app.h"
#include "debug_ui_internal.h"

#include "port_input_devices.h"
#include "port_mouse.h"
#include "port_overlay.h"
#include "port_savestate.h"
#include "port_video.h"

#include "MetroidPrime/Player/CGameOptions.hpp"
#include "MetroidPrime/Player/CGameState.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <switch.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

extern "C" void mp2nx_load_boost(int reason, int on);
extern "C" void mp2nx_start_log_flusher(void);
// Aurora's encoding.cpp: the frame capture (left stick click)
extern "C" void aurora_frame_capture_request(const char* dir);
extern "C" unsigned aurora_frame_capture_done(char* lastDir, unsigned size);
#if MP2NX_DEKO3D
// switch/source/deko/dk_smoke.cpp: the deko3d phase 0 smoke test
// switch/deko/dk_surface.cpp: the renderer of this start and the choice for the next
extern "C" bool mp2nx_dk_active(void);
extern "C" bool mp2nx_dk_fell_back(void);
extern "C" bool mp2nx_dk_chosen(void);
extern "C" void mp2nx_dk_set_chosen(bool on);
extern "C" const char* mp2nx_dk_cycle_label(void);
#endif

namespace PortDebug::SwitchMenu {
namespace {

// ---- Our own settings (the F1 window has no per-mode resolution or FPS counter) ----
constexpr const char* kConfigPath = MP2NX_APP_DIR "/switch_menu.cfg";
// Render scale per mode: 0 = the screen's own resolution, else a multiple of 480 lines.
float sHandheldScale = 0.f;
float sDockedScale = 0.f;
bool sShowFps = false;
bool sMenuButtonsDefaulted = false; // the Switch default for Separate menu buttons was applied once
bool sDekoTest = false;             // run the deko3d test on the next start (once; cleared when it starts)
bool sDekoTestThisStart = false;    // it runs on this start
bool sConfigLoaded = false;
int sAppliedMode = -1; // AppletOperationMode the resolution was last applied for

void LoadConfig() {
  sConfigLoaded = true;
  FILE* f = std::fopen(kConfigPath, "r");
  if (f == nullptr) {
    return;
  }
  char line[128];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    float v = 0.f;
    int i = 0;
    if (std::sscanf(line, "handheld_scale=%f", &v) == 1) {
      sHandheldScale = v;
    } else if (std::sscanf(line, "docked_scale=%f", &v) == 1) {
      sDockedScale = v;
    } else if (std::sscanf(line, "fps_counter=%d", &i) == 1) {
      sShowFps = i != 0;
    } else if (std::sscanf(line, "menu_buttons_defaulted=%d", &i) == 1) {
      sMenuButtonsDefaulted = i != 0;
    } else if (std::sscanf(line, "deko3d_test=%d", &i) == 1) {
      sDekoTest = i != 0;
    }
  }
  std::fclose(f);
}

void SaveConfig() {
  FILE* f = std::fopen(kConfigPath, "w");
  if (f == nullptr) {
    std::fprintf(stderr, "mp2-nx: menu: could not write %s\n", kConfigPath);
    return;
  }
  std::fprintf(f, "handheld_scale=%.2f\ndocked_scale=%.2f\nfps_counter=%d\nmenu_buttons_defaulted=%d\ndeko3d_test=%d\n",
               sHandheldScale, sDockedScale, sShowFps ? 1 : 0, sMenuButtonsDefaulted ? 1 : 0, sDekoTest ? 1 : 0);
  std::fclose(f);
}

bool Docked() { return appletGetOperationMode() == AppletOperationMode_Console; }

void ApplyModeScale(bool force) {
  const int mode = Docked() ? 1 : 0;
  if (!force && mode == sAppliedMode) {
    return;
  }
  sAppliedMode = mode;
  const float scale = mode == 1 ? sDockedScale : sHandheldScale;
  if (scale != PortOverlay::RenderScale()) {
    PortOverlay::SetRenderScale(scale);
    PortVideo::ResetDynamicRes();
  }
}

// ---- Input (libnx) ----
PadState sPad;
bool sPadReady = false;
u64 sHeldLast = 0;
u64 sRepeatMask = 0;
u64 sRepeatAt = 0;
bool sTouchWasDown = false;

constexpr u64 kUp = HidNpadButton_Up | HidNpadButton_StickLUp;
constexpr u64 kDown = HidNpadButton_Down | HidNpadButton_StickLDown;
constexpr u64 kLeft = HidNpadButton_Left | HidNpadButton_StickLLeft;
constexpr u64 kRight = HidNpadButton_Right | HidNpadButton_StickLRight;

struct Input {
  bool up = false, down = false, left = false, right = false;
  bool a = false, b = false, l = false, r = false;
  bool tap = false;
  float tapX = 0.f, tapY = 0.f; // in ImGui display coordinates
};

u64 NowMs() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

Input ReadInput(bool fresh) {
  Input in;
  if (!sPadReady) {
    padInitializeAny(&sPad);
    // libnx reports no touches until the touch screen is initialised once (aurora-aleks/Dusklight
    // switch_touch.cpp:183).
    hidInitializeTouchScreen();
    sPadReady = true;
  }
  padUpdate(&sPad);
  const u64 held = padGetButtons(&sPad);
  // Just opened: + and - (and whatever else) are still down. Only presses after this count.
  if (fresh) {
    sHeldLast = held;
    sRepeatMask = 0;
    HidTouchScreenState touch{};
    sTouchWasDown = hidGetTouchScreenStates(&touch, 1) > 0 && touch.count > 0;
    return in;
  }
  const u64 pressed = held & ~sHeldLast;
  sHeldLast = held;

  // Held directions repeat: after 350 ms, then every 90 ms.
  const u64 dirs = kUp | kDown | kLeft | kRight;
  u64 fire = pressed;
  const u64 now = NowMs();
  const u64 heldDir = held & dirs;
  if ((pressed & dirs) != 0) {
    sRepeatMask = pressed & dirs;
    sRepeatAt = now + 350;
  } else if (sRepeatMask != 0 && (heldDir & sRepeatMask) != 0 && now >= sRepeatAt) {
    fire |= heldDir & sRepeatMask;
    sRepeatAt = now + 90;
  } else if ((heldDir & sRepeatMask) == 0) {
    sRepeatMask = 0;
  }
  in.up = (fire & kUp) != 0;
  in.down = (fire & kDown) != 0;
  in.left = (fire & kLeft) != 0;
  in.right = (fire & kRight) != 0;
  in.a = (pressed & HidNpadButton_A) != 0;
  in.b = (pressed & HidNpadButton_B) != 0;
  in.l = (pressed & (HidNpadButton_L | HidNpadButton_ZL)) != 0;
  in.r = (pressed & (HidNpadButton_R | HidNpadButton_ZR)) != 0;

  HidTouchScreenState touch{};
  const bool touchDown = hidGetTouchScreenStates(&touch, 1) > 0 && touch.count > 0;
  if (touchDown && !sTouchWasDown) {
    // The touch screen reports 1280x720 whatever the output resolution is.
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    in.tap = true;
    in.tapX = static_cast< float >(touch.touches[0].x) * display.x / 1280.f;
    in.tapY = static_cast< float >(touch.touches[0].y) * display.y / 720.f;
  }
  sTouchWasDown = touchDown;
  return in;
}

// ---- Rows ----
struct Row {
  std::string label;
  std::string value;                  // empty: an action row
  std::string note;                   // shown for the selected row
  std::function< void(int) > change;  // left (-1) / right (+1); A steps forward too
  std::function< void() > activate;   // A on an action row
  bool enabled = true;
};

enum Page { kGame, kSaveStates, kControls, kButtons, kVideo, kSystem, kPageCount };
const char* const kPageNames[kPageCount] = {"Game", "Save states", "Controls", "Buttons", "Video", "System"};

int sPage = kGame;
int sSelected[kPageCount] = {};
int sScroll[kPageCount] = {};
int sSlot = 1;
bool sConfirmTitle = false;
bool sConfirmExit = false;
bool sCloseRequested = false;
bool sWasVisible = false;

// Toast after a save state request: the result message, for a few seconds.
std::string sToast;
u64 sToastUntil = 0;
bool sWatchStateMessage = false;
std::string sStateMessageBefore;
u64 sWatchUntil = 0;

void Toast(const std::string& text, u64 ms = 3000) {
  sToast = text;
  sToastUntil = NowMs() + ms;
}

const char* OnOff(bool v) { return v ? "On" : "Off"; }

// ---- Button remapping ----
// Writes the "All controllers" (Default) profile of Echoes' controls.toml, as the F1 Remap page
// does (port_input_remap_ui.cpp SaveDraft): Materialize the built-in bindings, replace the pad
// ones, PortInputDevices::SetUserBindings. A button already used by another action here is
// swapped onto it, so nothing is left unbound by accident.
using PortInput::Action;
using PortInput::Family;
struct RemapEntry {
  Action action;
  const char* label;
};
const RemapEntry kRemap[] = {
    {Action::PadA, "Fire (A)"},          {Action::PadB, "Jump (B)"},
    {Action::PadX, "Morph Ball (X)"},    {Action::PadY, "Missile (Y)"},
    {Action::PadL, "Lock-on (L)"},       {Action::PadR, "Free aim (R)"},
    {Action::PadZ, "Map (Z)"},           {Action::PadStart, "Pause (Start)"},
    {Action::SpringBall, "Spring Ball"},
};

SDL_Gamepad* PlayerPad() {
  const s32 index = PADGetIndexForPort(PAD_CHAN0);
  return PADGetSDLGamepadForIndex(index >= 0 ? static_cast< u32 >(index) : 0);
}

std::string InputName(const PortInput::Input& in) {
  if (in.device == PortInput::Device::PadAxis) {
    if (in.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return "ZL";
    if (in.code == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return "ZR";
    return "Stick";
  }
  if (in.device != PortInput::Device::PadButton) {
    return "?";
  }
  const auto button = static_cast< SDL_GamepadButton >(in.code);
  switch (button) {
  case SDL_GAMEPAD_BUTTON_SOUTH:
  case SDL_GAMEPAD_BUTTON_EAST:
  case SDL_GAMEPAD_BUTTON_WEST:
  case SDL_GAMEPAD_BUTTON_NORTH: {
    // The pad's own label for that position (A, B, X, Y as printed on it).
    SDL_Gamepad* pad = PlayerPad();
    const SDL_GamepadButtonLabel label =
        pad != nullptr ? SDL_GetGamepadButtonLabel(pad, button) : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
    default: break;
    }
    return button == SDL_GAMEPAD_BUTTON_SOUTH ? "Bottom button"
           : button == SDL_GAMEPAD_BUTTON_EAST ? "Right button"
           : button == SDL_GAMEPAD_BUTTON_WEST ? "Left button" : "Top button";
  }
  case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L";
  case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R";
  case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "Left stick click";
  case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "Right stick click";
  case SDL_GAMEPAD_BUTTON_START: return "+";
  case SDL_GAMEPAD_BUTTON_BACK: return "-";
  case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
  case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
  case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
  case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
  default: return "Button " + std::to_string(in.code);
  }
}

bool IsPadInput(const PortInput::Input& in) {
  return in.device == PortInput::Device::PadButton || in.device == PortInput::Device::PadAxis;
}

// The single pad inputs an action has now (built-in plus the player's changes).
std::vector< PortInput::Input > CurrentPadInputs(Action action) {
  // Built once per change of the bindings, not once per row and frame.
  static PortInput::Profile profile;
  static uint64_t builtFor = ~uint64_t{0};
  if (builtFor != PortInputDevices::UserBindingsVersion()) {
    profile = PortInput::Profile{};
    PortInputDevices::BuildProfile(profile);
    builtFor = PortInputDevices::UserBindingsVersion();
  }
  std::vector< PortInput::Input > out;
  for (const PortInput::Binding& b : profile.bindings) {
    if (b.action != action || b.count != 1 || !IsPadInput(b.inputs[0])) continue;
    if (std::none_of(out.begin(), out.end(),
                     [&](const PortInput::Input& x) { return PortInput::SamePhysicalInput(x, b.inputs[0]); })) {
      out.push_back(b.inputs[0]);
    }
  }
  return out;
}

std::string BindingText(Action action) {
  const auto inputs = CurrentPadInputs(action);
  if (inputs.empty()) return "None";
  std::string text;
  for (const auto& in : inputs) {
    if (!text.empty()) text += " / ";
    text += InputName(in);
  }
  return text;
}

PortInput::UserProfile& DefaultProfile(PortInput::UserBindings& work) {
  for (auto& p : work.profiles) {
    if (p.match.empty()) return p;
  }
  PortInput::UserProfile p;
  p.name = "Default";
  work.profiles.push_back(p);
  return work.profiles.back();
}

// Replaces the action's pad bindings with `in` (or none when `in` is not valid).
void ReplacePad(PortInput::UserProfile& profile, const PortInput::Profile& inherited, Action action,
                const PortInput::Input& in) {
  PortInput::Materialize(profile, action, Family::Pad, inherited);
  for (size_t i = profile.bindings.size(); i-- > 0;) {
    const PortInput::Binding& b = profile.bindings[i];
    if (b.action == action && PortInput::BindingFamily(b) == Family::Pad) {
      PortInput::RemoveUserBinding(profile, i);
    }
  }
  if (in.Valid()) {
    PortInput::Binding nb;
    nb.action = action;
    nb.inputs[0] = in;
    nb.count = 1;
    PortInput::AddUserBinding(profile, nb);
  }
}

bool AssignButton(Action action, const PortInput::Input& in) {
  const auto oldInputs = CurrentPadInputs(action);
  const PortInput::Input oldInput = oldInputs.empty() ? PortInput::Input{} : oldInputs.front();
  auto current = PortInputDevices::UserBindings();
  PortInput::UserBindings work = current ? *current : PortInput::UserBindings{};
  PortInput::Profile inherited;
  PortInputDevices::BuildBaseProfile(inherited);
  PortInput::UserProfile& profile = DefaultProfile(work);
  for (const RemapEntry& e : kRemap) {
    if (e.action == action) continue;
    const auto others = CurrentPadInputs(e.action);
    const bool usesIt = std::any_of(others.begin(), others.end(),
                                    [&](const PortInput::Input& x) { return PortInput::SamePhysicalInput(x, in); });
    if (usesIt) {
      ReplacePad(profile, inherited, e.action, oldInput); // the swap
    }
  }
  // The analog halves of the GameCube's L and R ride along with their clicks: one left on a
  // button taken for something else would still press it (test7: Fire on ZR also pulled the
  // analog R, which is free aim, and Samus stood still while charging).
  for (const Action analog : {Action::LAnalog, Action::RAnalog}) {
    const auto inputs = CurrentPadInputs(analog);
    if (std::any_of(inputs.begin(), inputs.end(),
                    [&](const PortInput::Input& x) { return PortInput::SamePhysicalInput(x, in); })) {
      ReplacePad(profile, inherited, analog, PortInput::Input{});
    }
  }
  ReplacePad(profile, inherited, action, in);
  const bool ok = PortInputDevices::SetUserBindings(work);
  std::fprintf(stderr, "mp2-nx: menu: %s -> %s (%s)\n", std::string(PortInput::Info(action).label).c_str(),
               InputName(in).c_str(), ok ? "saved" : "save FAILED");
  return ok;
}

bool ResetButtons() {
  auto current = PortInputDevices::UserBindings();
  PortInput::UserBindings work = current ? *current : PortInput::UserBindings{};
  for (auto& p : work.profiles) {
    PortInput::ClearFamily(p, Family::Pad);
  }
  return PortInputDevices::SetUserBindings(work);
}

// Capture: wait until every button is up, then take the first press (a button, or ZL/ZR).
struct Capture {
  bool active = false;
  bool armed = false;
  Action action = Action::None;
  std::string label;
  u64 deadline = 0;
} sCapture;
bool sResyncPad = false;

bool ReadPadInput(SDL_Gamepad* pad, PortInput::Input& out, bool& anyDown) {
  anyDown = false;
  for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
    if (b == SDL_GAMEPAD_BUTTON_GUIDE) continue;
    if (SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(b))) {
      anyDown = true;
      out = PortInput::Input{};
      out.device = PortInput::Device::PadButton;
      out.code = static_cast< uint16_t >(b);
      return true;
    }
  }
  for (const SDL_GamepadAxis axis : {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}) {
    if (SDL_GetGamepadAxis(pad, axis) > 20000) {
      anyDown = true;
      out = PortInput::Input{};
      out.device = PortInput::Device::PadAxis;
      out.code = static_cast< uint16_t >(axis);
      out.dir = 1;
      return true;
    }
  }
  return false;
}

// True while a capture owns the frame.
bool UpdateCapture() {
  if (!sCapture.active) return false;
  SDL_Gamepad* pad = PlayerPad();
  if (pad == nullptr) {
    Toast("No controller found");
    sCapture.active = false;
    return false;
  }
  PortInput::Input in;
  bool anyDown = false;
  const bool got = ReadPadInput(pad, in, anyDown);
  if (!sCapture.armed) {
    sCapture.armed = !anyDown;
  } else if (got) {
    const bool ok = AssignButton(sCapture.action, in);
    Toast(ok ? sCapture.label + ": " + InputName(in) : "Could not save the controls file");
    sCapture.active = false;
    sResyncPad = true; // the press must not also move the menu
    return true;
  }
  if (NowMs() > sCapture.deadline) {
    Toast("No button pressed, nothing changed");
    sCapture.active = false;
    sResyncPad = true;
  }
  return true;
}


std::string ScaleLabel(float scale) {
  char buf[48];
  if (scale <= 0.f) {
    std::snprintf(buf, sizeof(buf), "Screen (%dp)", PortVideo::DrawnLines(0.f));
  } else {
    std::snprintf(buf, sizeof(buf), "%dp", PortVideo::DrawnLines(scale));
  }
  return buf;
}

// 0 (screen) and multiples of 480 lines up to 1080p.
constexpr float kScales[] = {0.f, 1.f, 1.25f, 1.5f, 1.75f, 2.f, 2.25f};
constexpr int kScaleCount = sizeof(kScales) / sizeof(kScales[0]);

float StepScale(float current, int dir) {
  int idx = 0;
  for (int i = 0; i < kScaleCount; ++i) {
    if (std::fabs(kScales[i] - current) < 0.01f) {
      idx = i;
    }
  }
  idx = (idx + dir + kScaleCount) % kScaleCount;
  return kScales[idx];
}

std::string PlayTime(double seconds) {
  const int s = static_cast< int >(seconds);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  return buf;
}

std::string SavedAt(int64_t unixSeconds) {
  if (unixSeconds <= 0) {
    return {};
  }
  const time_t t = static_cast< time_t >(unixSeconds);
  struct tm local{};
  localtime_r(&t, &local);
  char buf[48];
  std::strftime(buf, sizeof(buf), "%b %d, %I:%M %p", &local);
  return buf;
}

void Toggle(bool& v, int) {
  v = !v;
  Changed(true);
}

std::vector< Row > BuildRows(int page) {
  std::vector< Row > rows;
  switch (page) {
  case kGame: {
    rows.push_back({"Resume", "", "Close the menu (B, or + and - together)", nullptr,
                    [] { sCloseRequested = true; }});
    rows.push_back({"Skip cutscenes", OnOff(PortOverlay::SkipCinematics()),
                    "Ends every cutscene as it starts, even ones you have not seen yet",
                    [](int) { PortOverlay::SetSkipCinematics(!PortOverlay::SkipCinematics()); Changed(true); },
                    nullptr});
    rows.push_back({"Fast Morph", OnOff(sFastMorph), "Morph ball without the camera pulling back first",
                    [](int d) { Toggle(sFastMorph, d); }, nullptr});
    rows.push_back({"Spring Ball", OnOff(sSpringBall),
                    "Jump in morph ball: right stick up (L too with Twin stick). Needs the Morph Ball Bombs",
                    [](int d) { Toggle(sSpringBall, d); }, nullptr});
    rows.push_back({sConfirmTitle ? "Return to title: press A again" : "Return to title", "",
                    "Unsaved progress is lost", nullptr, [] {
                      if (!sConfirmTitle) {
                        sConfirmTitle = true;
                        return;
                      }
                      sConfirmTitle = false;
                      PortOverlay::RequestReturnToTitle();
                      sCloseRequested = true;
                    }});
    // The F1 window's "Exit game" (debug_ui/game_sections.cpp): the same quit as closing the window,
    // so the main loop shuts down cleanly and the process ends through AM (startup.cpp).
    rows.push_back({sConfirmExit ? "Exit game: press A again" : "Exit game", "",
                    "Back to the HOME menu. Progress since the last save is lost", nullptr, [] {
                      if (!sConfirmExit) {
                        sConfirmExit = true;
                        return;
                      }
                      sConfirmExit = false;
                      PortOverlaySettings::SaveSettings();
                      SDL_Event quit{};
                      quit.type = SDL_EVENT_QUIT;
                      SDL_PushEvent(&quit);
                      sCloseRequested = true;
                    }});
    break;
  }
  case kSaveStates: {
    const PortSaveState::Info info = PortSaveState::SlotInfo(sSlot);
    std::string slotValue = std::to_string(sSlot) + " of " + std::to_string(PortSaveState::kSlotCount);
    std::string slotNote = info.exists ? info.room + ", play time " + PlayTime(info.playTime) +
                                             (info.savedAt > 0 ? ", saved " + SavedAt(info.savedAt) : "")
                                       : "Empty";
    rows.push_back({"Slot", slotValue, slotNote, [](int d) {
                      sSlot = (sSlot - 1 + d + PortSaveState::kSlotCount) % PortSaveState::kSlotCount + 1;
                    },
                    nullptr});
    rows.push_back({"Save state to slot " + std::to_string(sSlot), "",
                    info.exists ? "Replaces what is in this slot" : "Saves right where you are", nullptr, [] {
                      sStateMessageBefore = PortSaveState::LastMessage();
                      PortSaveState::SetSelectedSlot(sSlot);
                      PortSaveState::RequestSave(sSlot);
                      sWatchStateMessage = true;
                      sWatchUntil = NowMs() + 5000;
                      Toast("Saving state to slot " + std::to_string(sSlot) + "...");
                      sCloseRequested = true;
                    }});
    Row load{"Load slot " + std::to_string(sSlot), "",
             info.exists ? "Your current spot is kept as an undo" : "Nothing saved in this slot", nullptr, [] {
               sStateMessageBefore = PortSaveState::LastMessage();
               PortSaveState::SetSelectedSlot(sSlot);
               PortSaveState::RequestLoad(sSlot);
               sWatchStateMessage = true;
               sWatchUntil = NowMs() + 8000;
               Toast("Loading slot " + std::to_string(sSlot) + "...");
               sCloseRequested = true;
             }};
    load.enabled = info.exists;
    rows.push_back(load);
    const PortSaveState::Info undo = PortSaveState::SlotInfo(PortSaveState::kUndoSlot);
    Row undoRow{"Undo last load", "", undo.exists ? "Goes back to where you were before the last load: " + undo.room
                                                  : "No load to undo yet",
                nullptr, [] {
                  sStateMessageBefore = PortSaveState::LastMessage();
                  PortSaveState::RequestLoad(PortSaveState::kUndoSlot);
                  sWatchStateMessage = true;
                  sWatchUntil = NowMs() + 8000;
                  Toast("Undoing the last load...");
                  sCloseRequested = true;
                }};
    undoRow.enabled = undo.exists;
    rows.push_back(undoRow);
    break;
  }
  case kControls: {
    rows.push_back({"Twin stick aiming", OnOff(PortMouse::StickEnabled()),
                    "Right stick aims freely, like a modern shooter, instead of free-look",
                    [](int) { PortMouse::SetStickEnabled(!PortMouse::StickEnabled()); Changed(true); }, nullptr});
    if (PortMouse::StickEnabled()) {
      char speed[32];
      std::snprintf(speed, sizeof(speed), "%.0f", PortMouse::StickAimRate());
      rows.push_back({"Stick aim speed", speed, "How fast the right stick turns at full tilt", [](int d) {
                        const float v = std::clamp(PortMouse::StickAimRate() + d * 200.f, 100.f, 8100.f);
                        PortMouse::SetStickAimRate(v);
                        Changed(true);
                      },
                      nullptr});
      rows.push_back({"Invert aim left/right", OnOff(PortMouse::InvertX()), "Right stick, left and right",
                      [](int) { PortMouse::SetInvertX(!PortMouse::InvertX()); Changed(true); }, nullptr});
      rows.push_back({"Invert aim up/down", OnOff(PortMouse::InvertY()), "Right stick, up and down",
                      [](int) { PortMouse::SetInvertY(!PortMouse::InvertY()); Changed(true); }, nullptr});
      rows.push_back({"Aim crosshair", OnOff(sAimCrosshair), "Shows the crosshair while the right stick aims",
                      [](int d) { Toggle(sAimCrosshair, d); }, nullptr});
    }
    {
      static const char* const kGyroModes[] = {"Off", "Hold to aim", "Always on", "Hold to pause", "Toggle"};
      const int mode = sGyroAim ? std::clamp(sGyroMode, PortOverlay::kGyroModeMin, PortOverlay::kGyroModeMax) : 0;
      rows.push_back({"Gyro aim", kGyroModes[mode],
                      PortMouse::StickEnabled()
                          ? "Tilt the controller to aim. The gyro button is right stick click"
                          : "Tilt the controller to aim. Needs Twin stick aiming on",
                      [](int d) {
                        int m = sGyroAim ? std::clamp(sGyroMode, PortOverlay::kGyroModeMin, PortOverlay::kGyroModeMax) : 0;
                        m = (m + d + 5) % 5;
                        sGyroAim = m > 0;
                        if (m > 0) sGyroMode = m;
                        Changed(true);
                      },
                      nullptr});
      if (sGyroAim) {
        char v[16];
        std::snprintf(v, sizeof(v), "%.1fx", static_cast< double >(sGyroSensitivity));
        rows.push_back({"Gyro sensitivity", v, "How far the view turns per turn of the controller", [](int d) {
                          sGyroSensitivity = std::clamp(sGyroSensitivity + d * 0.1f, PortOverlay::kGyroSensitivityMin,
                                                        PortOverlay::kGyroSensitivityMax);
                          Changed(true);
                        },
                        nullptr});
      }
    }
    rows.push_back({"Rumble", gpGameState != nullptr && gpGameState->GameOptions().GetIsRumbleEnabled() ? "On" : "Off",
                    "Same switch as Rumble in the game's pause menu", [](int) {
                      if (gpGameState != nullptr) {
                        gpGameState->GameOptions().SetIsRumbleEnabled(!gpGameState->GameOptions().GetIsRumbleEnabled());
                      }
                    },
                    nullptr});
    rows.push_back({"Lock-on toggle", OnOff(sLockOnToggle), "Tap to lock on and tap again to let go, instead of holding",
                    [](int d) { Toggle(sLockOnToggle, d); }, nullptr});
    rows.push_back({"Sticky charge", OnOff(sStickyCharge), "Charge shots stay charged without holding fire",
                    [](int d) { Toggle(sStickyCharge, d); }, nullptr});
    rows.push_back({"Auto-aim", OnOff(sAutoAim), "Shots bend toward the target you are facing",
                    [](int d) { Toggle(sAutoAim, d); }, nullptr});
    rows.push_back({"Invert map rotation left/right", OnOff(sMapInvertX), "On the map screen",
                    [](int d) { Toggle(sMapInvertX, d); }, nullptr});
    rows.push_back({"Invert map rotation up/down", OnOff(sMapInvertY), "On the map screen",
                    [](int d) { Toggle(sMapInvertY, d); }, nullptr});
    break;
  }
  case kButtons: {
    for (const RemapEntry& e : kRemap) {
      if (e.action == Action::SpringBall && !sSpringBall) continue;
      rows.push_back({e.label, BindingText(e.action),
                      "Press A, then the Switch button you want. If another action uses it, they swap",
                      nullptr, [e] {
                        sCapture.active = true;
                        sCapture.armed = false;
                        sCapture.action = e.action;
                        sCapture.label = e.label;
                        sCapture.deadline = NowMs() + 6000;
                      }});
    }
    rows.push_back({"Buttons in the game's menus", PortOverlay::MenuStandardButtons() ? "Always standard" : "Follow my remaps",
                    "Standard: A selects and B goes back in menus and the map, whatever you remap. Pause and map follow your remaps",
                    [](int) { PortOverlay::SetMenuStandardButtons(!PortOverlay::MenuStandardButtons()); }, nullptr});
    rows.push_back({"Reset buttons to default", "", "Puts every button back the way it started", nullptr, [] {
                      Toast(ResetButtons() ? "Buttons reset" : "Could not save the controls file");
                    }});
    break;
  }
  case kVideo: {
    const bool docked = Docked();
    rows.push_back({std::string("Resolution, handheld") + (docked ? "" : " (now)"), ScaleLabel(sHandheldScale),
                    "Lower is smoother, higher is sharper. The handheld screen is 720p", [](int d) {
                      sHandheldScale = StepScale(sHandheldScale, d);
                      SaveConfig();
                      ApplyModeScale(true);
                    },
                    nullptr});
    rows.push_back({std::string("Resolution, docked") + (docked ? " (now)" : ""), ScaleLabel(sDockedScale),
                    "Lower is smoother, higher is sharper. The TV output is 1080p", [](int d) {
                      sDockedScale = StepScale(sDockedScale, d);
                      SaveConfig();
                      ApplyModeScale(true);
                    },
                    nullptr});
    rows.push_back({"Dynamic resolution", OnOff(sDynamicRes),
                    "Drops the resolution in busy scenes to hold the frame rate, up to the setting above",
                    [](int) {
                      sDynamicRes = !sDynamicRes;
                      PortVideo::ResetDynamicRes();
                      Changed(true);
                    },
                    nullptr});
    rows.push_back({"FPS counter", OnOff(sShowFps), "Top left corner", [](int) {
                      sShowFps = !sShowFps;
                      SaveConfig();
                    },
                    nullptr});
    rows.push_back({"Anti-aliasing", PortOverlay::Msaa() >= 4 ? "4x MSAA" : "Off",
                    "Smooths jagged edges. Costs GPU time, try it handheld first", [](int) {
                      PortOverlay::SetMsaa(PortOverlay::Msaa() >= 4 ? 1 : 4);
                      Changed(true);
                    },
                    nullptr});
    {
      static constexpr int kAniso[] = {1, 2, 4, 8, 16};
      char v[16];
      std::snprintf(v, sizeof(v), "%dx", PortOverlay::Anisotropy());
      rows.push_back({"Texture filtering", v, "Sharper floors and walls at an angle", [](int d) {
                        int idx = 0;
                        for (int i = 0; i < 5; ++i) {
                          if (kAniso[i] == PortOverlay::Anisotropy()) idx = i;
                        }
                        PortOverlay::SetAnisotropy(kAniso[(idx + d + 5) % 5]);
                        Changed(true);
                      },
                      nullptr});
    }
    rows.push_back({"Aspect ratio", sAspectMode == PortOverlay::kAspect_4_3 ? "4:3 (original)" : "16:9",
                    "16:9 fills the screen; 4:3 is the GameCube picture", [](int) {
                      PortOverlay::SetAspectMode(sAspectMode == PortOverlay::kAspect_4_3 ? PortOverlay::kAspect_Window
                                                                                          : PortOverlay::kAspect_4_3);
                      Changed(true);
                    },
                    nullptr});
    {
      char v[24];
      std::snprintf(v, sizeof(v), sFirstPersonFov > 0.f ? "%.0f" : "55 (original)",
                    sFirstPersonFov > 0.f ? sFirstPersonFov : 55.f);
      rows.push_back({"Field of view", v, "How wide the view is, in degrees", [](int d) {
                        float fov = sFirstPersonFov > 0.f ? sFirstPersonFov : 55.f;
                        fov = std::clamp(fov + d * 5.f, 30.f, 120.f);
                        sFirstPersonFov = std::fabs(fov - 55.f) < 0.5f ? 0.f : fov;
                        Changed(true);
                      },
                      nullptr});
    }
    {
      char v[16];
      std::snprintf(v, sizeof(v), "%d%%", PortOverlay::HudScale());
      rows.push_back({"HUD size", v, "Shrinks the HUD toward the middle of the screen", [](int d) {
                        PortOverlay::SetHudScale(
                            std::clamp(PortOverlay::HudScale() + d * 5, PortOverlay::kHudScaleMin, PortOverlay::kHudScaleMax));
                        Changed(true);
                      },
                      nullptr});
    }
    rows.push_back({"Cutscene black bars", OnOff(sCinemaBars), "Off lets cutscenes fill the screen",
                    [](int d) { Toggle(sCinemaBars, d); }, nullptr});
    rows.push_back({"Hide helmet", OnOff(sHideHelmet), "Hides the visor frame around the view",
                    [](int d) { Toggle(sHideHelmet, d); }, nullptr});
    break;
  }
  case kSystem:
  default: {
    rows.push_back({"Close menu", "", "", nullptr, [] { sCloseRequested = true; }});
    rows.push_back({"Mode", Docked() ? "Docked" : "Handheld", "", nullptr, nullptr, false});
    char fps[32];
    std::snprintf(fps, sizeof(fps), "%.0f", static_cast< double >(sFps));
    rows.push_back({"Frames per second", fps, "", nullptr, nullptr, false});
#if MP2NX_DEKO3D
    rows.push_back({"Renderer (next start)", mp2nx_dk_chosen() ? "deko3d (test)" : "Vulkan",
                    mp2nx_dk_active() ? "This start uses deko3d. Vulkan is the safe choice; restart to apply"
                                      : "deko3d is the new renderer, still being tested. Restart to apply",
                    [](int) {
                      const bool next = !mp2nx_dk_chosen();
                      mp2nx_dk_set_chosen(next);
                      Toast(next ? "Close and restart the game to use deko3d" : "Vulkan on the next start");
                    },
                    nullptr});
#endif
    rows.push_back({MP2NX_APP_NAME, "Echoes Unbound on Switch", "", nullptr, nullptr, false});
    break;
  }
  }
  return rows;
}

ImU32 Col(float r, float g, float b, float a = 1.f) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const std::string& text) {
  dl->AddText(font, size, pos, col, text.c_str());
}

float TextWidth(ImFont* font, float size, const std::string& text) {
  return font->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str()).x;
}

} // namespace

// Every frame, open or not: the docked/handheld resolution, the FPS counter and the toast.
void DrawAlways() {
  if (!sVisible) {
    sWasVisible = false; // closed with + and - (the port's own toggle) or by Draw
  }
  if (!sConfigLoaded) {
    LoadConfig();
    mp2nx_load_boost(0, 1); // load_boost.cpp: the title's startup warmup (cleared by CMFGameLoader)
    // The owner: remapping a gameplay button must not move the game's menu buttons (A confirms, B backs
    // out). Echoes has this as "Separate menu buttons" (port_input_devices.cpp ApplyMenuButtons),
    // off by default; on the Switch it starts on, once, so a later choice in the menu is kept.
    if (!sMenuButtonsDefaulted) {
      sMenuButtonsDefaulted = true;
      PortOverlay::SetMenuStandardButtons(true);
      SaveConfig();
    }
    mp2nx_start_log_flusher(); // misc_switch.cpp
#if MP2NX_DEKO3D
    if (mp2nx_dk_fell_back()) {
      Toast("deko3d did not show a picture twice, so the game is back on Vulkan", 8000);
    } else if (mp2nx_dk_active()) {
      Toast("Renderer: deko3d (test). Switch back in System > Renderer", 6000);
    }
#endif
  }
  ApplyModeScale(false);
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  const u64 now = NowMs();
  if (sWatchStateMessage) {
    const std::string msg = PortSaveState::LastMessage();
    if (!msg.empty() && msg != sStateMessageBefore) {
      Toast(msg, 4000);
      sWatchStateMessage = false;
    } else if (now > sWatchUntil) {
      sWatchStateMessage = false;
    }
  }
  // Frame capture: a left stick click (unbound by default: Echoes binds the right stick click to gyro aim, and
  // the left only if a radial menu is put on it) saves the next frame's render passes, as PAM pictures and
  // passes.txt (and on deko3d draws.txt), in <app>/captures/capNNN_<renderer>/.
  {
    static bool sStickWas = false;
    static unsigned sDoneSeen = 0;
    SDL_Gamepad* pad = PlayerPad();
    const bool stick = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    if (stick && !sStickWas && !sVisible) {
#if MP2NX_DEKO3D
      const char* renderer = mp2nx_dk_active() ? "deko3d" : "vulkan";
#else
      const char* renderer = "vulkan";
#endif
      mkdir(MP2NX_APP_DIR "/captures", 0777);
      char dir[160] = "";
      for (int n = 1; n < 1000; ++n) {
        bool taken = false;
        for (const char* r : {"deko3d", "vulkan"}) {
          char probe[160];
          std::snprintf(probe, sizeof probe, MP2NX_APP_DIR "/captures/cap%03d_%s", n, r);
          struct stat st{};
          if (stat(probe, &st) == 0) taken = true;
        }
        if (!taken) {
          std::snprintf(dir, sizeof dir, MP2NX_APP_DIR "/captures/cap%03d_%s", n, renderer);
          break;
        }
      }
      if (dir[0] != 0 && mkdir(dir, 0777) == 0) {
        aurora_frame_capture_request(dir);
        Toast("Capturing a frame...", 2000);
      } else {
        Toast("Frame capture: could not make a folder", 4000);
      }
    }
    sStickWas = stick;
    char last[160] = "";
    const unsigned done = aurora_frame_capture_done(last, sizeof last);
    if (done != sDoneSeen) {
      sDoneSeen = done;
      const char* name = std::strrchr(last, '/');
      Toast(std::string("Frame capture saved: ") + (name ? name + 1 : last), 5000);
    }
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float small = display.y * 0.032f;
  if (sShowFps) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f FPS", static_cast< double >(sFps));
    const ImVec2 pos(display.y * 0.02f, display.y * 0.02f);
    dl->AddText(font, small, ImVec2(pos.x + 2, pos.y + 2), Col(0, 0, 0, 0.8f), buf);
    dl->AddText(font, small, pos, Col(1, 1, 0.4f), buf);
  }
#if MP2NX_DEKO3D
  // mp2-nx (deko-14): the deko3d test cycle's mode (deko3d_env.txt MP2DK_CYCLE or MP2DK_MODE)
  if (const char* test = mp2nx_dk_cycle_label()) {
    const ImVec2 pos(display.y * 0.02f, display.y * 0.02f + small * 1.3f);
    dl->AddText(font, small, ImVec2(pos.x + 2, pos.y + 2), Col(0, 0, 0, 0.9f), test);
    dl->AddText(font, small, pos, Col(0.4f, 1, 1), test);
  }
#endif
  if (!sToast.empty() && now < sToastUntil) {
    const float w = TextWidth(font, small, sToast);
    const ImVec2 pos((display.x - w) * 0.5f, display.y * 0.88f);
    const float pad = small * 0.5f;
    dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad), ImVec2(pos.x + w + pad, pos.y + small + pad), Col(0, 0, 0, 0.75f),
                      pad);
    dl->AddText(font, small, pos, Col(1, 1, 1), sToast.c_str());
  }
}

// While the menu is open. False closes it.
bool Draw() {
  if (ImGui::GetCurrentContext() == nullptr) {
    return true;
  }
  const bool fresh = !sWasVisible;
  sWasVisible = true;
  if (fresh) {
    sConfirmTitle = false;
    sConfirmExit = false;
    sCloseRequested = false;
    sSlot = std::clamp(PortSaveState::SelectedSlot(), 1, PortSaveState::kSlotCount);
  }
  const bool capturing = UpdateCapture();
  const Input in = ReadInput(fresh || capturing || sResyncPad);
  sResyncPad = false;

  if (in.l || in.r) {
    sPage = (sPage + (in.r ? 1 : -1) + kPageCount) % kPageCount;
    sConfirmTitle = false;
    sConfirmExit = false;
  }
  std::vector< Row > rows = BuildRows(sPage);
  int& sel = sSelected[sPage];
  sel = std::clamp(sel, 0, static_cast< int >(rows.size()) - 1);

  // Layout
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const float unit = display.y / 720.f;
  const float textSize = 26.f * unit;
  const float smallSize = 21.f * unit;
  const float rowH = 46.f * unit;
  const ImVec2 p0(display.x * 0.12f, display.y * 0.08f);
  const ImVec2 p1(display.x * 0.88f, display.y * 0.92f);
  const float padX = 28.f * unit;
  const float tabsY = p0.y + 18.f * unit;
  const float listTop = tabsY + textSize + 30.f * unit;
  const float footerY = p1.y - smallSize - 16.f * unit;
  const float noteY = footerY - smallSize - 18.f * unit;
  const int visibleRows = std::max(1, static_cast< int >((noteY - 10.f * unit - listTop) / rowH));

  // Tabs (tap targets too)
  std::vector< std::pair< float, float > > tabX;
  {
    float x = p0.x + padX;
    for (int i = 0; i < kPageCount; ++i) {
      const float w = TextWidth(font, textSize, kPageNames[i]);
      tabX.emplace_back(x, x + w);
      x += w + 34.f * unit;
    }
  }

  // Touch: a tab, or a row (select it; a second tap on the selected row acts like A).
  bool tapActivate = false;
  if (in.tap) {
    if (in.tapY >= tabsY - 8.f * unit && in.tapY <= tabsY + textSize + 8.f * unit) {
      for (int i = 0; i < kPageCount; ++i) {
        if (in.tapX >= tabX[i].first - 12.f * unit && in.tapX <= tabX[i].second + 12.f * unit) {
          sPage = i;
          sConfirmTitle = false;
          return true; // rows are rebuilt next frame
        }
      }
    } else if (in.tapY >= listTop && in.tapX >= p0.x && in.tapX <= p1.x) {
      const int idx = sScroll[sPage] + static_cast< int >((in.tapY - listTop) / rowH);
      if (idx >= 0 && idx < static_cast< int >(rows.size()) && idx < sScroll[sPage] + visibleRows) {
        tapActivate = idx == sel;
        sel = idx;
      }
    } else if (in.tapX < p0.x || in.tapX > p1.x || in.tapY < p0.y || in.tapY > p1.y) {
      return false; // a tap outside the panel closes the menu
    }
  }

  // Pad
  const int count = static_cast< int >(rows.size());
  if (!capturing) {
  if (in.up) {
    sel = (sel - 1 + count) % count;
  }
  if (in.down) {
    sel = (sel + 1) % count;
  }
  if (in.up || in.down) {
    sConfirmTitle = sConfirmTitle && rows[sel].label.rfind("Return to title", 0) == 0;
    sConfirmExit = sConfirmExit && rows[sel].label.rfind("Exit game", 0) == 0;
  }
  Row& cur = rows[sel];
  if (cur.enabled) {
    if ((in.left || in.right) && cur.change) {
      cur.change(in.right ? 1 : -1);
    }
    if (in.a || tapActivate) {
      if (cur.activate) {
        cur.activate();
      } else if (cur.change) {
        cur.change(1);
      }
    }
  }
  if (in.b) {
    if (sConfirmTitle || sConfirmExit) {
      sConfirmTitle = false;
      sConfirmExit = false;
    } else {
      sCloseRequested = true;
    }
  }
  if (sCloseRequested) {
    sCloseRequested = false;
    return false;
  }
  } // !capturing
  // The rows may have changed (a toggle shows or hides others): rebuild for drawing.
  rows = BuildRows(sPage);
  sel = std::clamp(sel, 0, static_cast< int >(rows.size()) - 1);
  int& scroll = sScroll[sPage];
  if (sel < scroll) scroll = sel;
  if (sel >= scroll + visibleRows) scroll = sel - visibleRows + 1;
  scroll = std::clamp(scroll, 0, std::max(0, static_cast< int >(rows.size()) - visibleRows));

  // Draw
  const ImU32 accent = Col(1.f, 0.62f, 0.15f);
  dl->AddRectFilled(ImVec2(0, 0), display, Col(0, 0, 0, 0.55f));
  dl->AddRectFilled(p0, p1, Col(0.06f, 0.07f, 0.10f, 0.94f), 14.f * unit);
  dl->AddRect(p0, p1, Col(1.f, 0.62f, 0.15f, 0.6f), 14.f * unit, 0, 2.f * unit);
  for (int i = 0; i < kPageCount; ++i) {
    const bool on = i == sPage;
    Text(dl, font, textSize, ImVec2(tabX[i].first, tabsY), on ? accent : Col(0.65f, 0.67f, 0.72f), kPageNames[i]);
    if (on) {
      dl->AddRectFilled(ImVec2(tabX[i].first, tabsY + textSize + 4.f * unit),
                        ImVec2(tabX[i].second, tabsY + textSize + 7.f * unit), accent);
    }
  }
  {
    const std::string hint = "L / R";
    Text(dl, font, smallSize, ImVec2(p1.x - padX - TextWidth(font, smallSize, hint), tabsY + 3.f * unit),
         Col(0.5f, 0.52f, 0.58f), hint);
  }
  dl->AddLine(ImVec2(p0.x + padX, listTop - 12.f * unit), ImVec2(p1.x - padX, listTop - 12.f * unit),
              Col(1, 1, 1, 0.12f), 1.f * unit);

  for (int i = scroll; i < static_cast< int >(rows.size()) && i < scroll + visibleRows; ++i) {
    const Row& row = rows[i];
    const float y = listTop + static_cast< float >(i - scroll) * rowH;
    const bool selected = i == sel;
    if (selected) {
      dl->AddRectFilled(ImVec2(p0.x + padX * 0.5f, y), ImVec2(p1.x - padX * 0.5f, y + rowH - 4.f * unit),
                        Col(1.f, 0.62f, 0.15f, 0.20f), 8.f * unit);
    }
    const ImU32 textCol = !row.enabled ? Col(0.45f, 0.46f, 0.5f) : selected ? Col(1, 1, 1) : Col(0.85f, 0.86f, 0.9f);
    const float ty = y + (rowH - 4.f * unit - textSize) * 0.5f;
    Text(dl, font, textSize, ImVec2(p0.x + padX, ty), textCol, row.label);
    if (!row.value.empty()) {
      const std::string v = row.change && row.enabled && selected ? "<  " + row.value + "  >" : row.value;
      Text(dl, font, textSize, ImVec2(p1.x - padX - TextWidth(font, textSize, v), ty),
           selected ? accent : Col(0.75f, 0.77f, 0.82f), v);
    }
  }
  if (scroll > 0) {
    Text(dl, font, smallSize, ImVec2(p1.x - padX - smallSize, listTop - 10.f * unit - smallSize), Col(0.6f, 0.6f, 0.6f),
         "^");
  }
  if (scroll + visibleRows < static_cast< int >(rows.size())) {
    Text(dl, font, smallSize, ImVec2(p1.x - padX - smallSize, noteY - smallSize), Col(0.6f, 0.6f, 0.6f), "v");
  }
  if (!rows.empty() && !rows[sel].note.empty()) {
    Text(dl, font, smallSize, ImVec2(p0.x + padX, noteY), Col(0.70f, 0.72f, 0.78f), rows[sel].note);
  }
  const std::string footer = "Up/Down move    Left/Right change    A select    B close    L/R page";
  Text(dl, font, smallSize, ImVec2(p0.x + padX, footerY), Col(0.5f, 0.52f, 0.58f), footer);
  if (sCapture.active) {
    const std::string line1 = "Press the button for " + sCapture.label;
    const std::string line2 = "Waiting " + std::to_string(static_cast< int >((sCapture.deadline - std::min(sCapture.deadline, NowMs())) / 1000 + 1)) + " s";
    const float w = std::max(TextWidth(font, textSize, line1), TextWidth(font, smallSize, line2)) + 80.f * unit;
    const ImVec2 c(display.x * 0.5f, display.y * 0.5f);
    const ImVec2 b0(c.x - w * 0.5f, c.y - 60.f * unit);
    const ImVec2 b1(c.x + w * 0.5f, c.y + 60.f * unit);
    dl->AddRectFilled(b0, b1, Col(0.03f, 0.03f, 0.05f, 0.97f), 12.f * unit);
    dl->AddRect(b0, b1, accent, 12.f * unit, 0, 2.f * unit);
    Text(dl, font, textSize, ImVec2(c.x - TextWidth(font, textSize, line1) * 0.5f, c.y - 36.f * unit), Col(1, 1, 1), line1);
    Text(dl, font, smallSize, ImVec2(c.x - TextWidth(font, smallSize, line2) * 0.5f, c.y + 10.f * unit),
         Col(0.7f, 0.72f, 0.78f), line2);
  }
  return true;
}

} // namespace PortDebug::SwitchMenu
