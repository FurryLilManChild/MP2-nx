// mp2-nx: the port menu on the Switch (opened with + and - together, the port's own PortMenu
// binding). Echoes Unbound's F1 window is built for a mouse and keyboard; on the Switch neither
// the pad nor the touch screen reached it (test2 and test3). This menu reads the Joy-Cons and the
// touch screen straight from libnx, as the earlier ports' menus do, and draws with ImGui's draw
// lists only, so no ImGui navigation or focus is involved. It changes the same settings the F1
// window does (port_overlay.cpp's variables and setters), so they are saved to the same
// settings.cfg, and adds what the Switch needs: save states, a resolution for handheld and one
// for docked (applied when the console changes mode), and an FPS counter.
#include "debug_ui_internal.h"

#include "port_mouse.h"
#include "port_overlay.h"
#include "port_savestate.h"
#include "port_video.h"

#include <imgui.h>
#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace PortDebug::SwitchMenu {
namespace {

// ---- Our own settings (the F1 window has no per-mode resolution or FPS counter) ----
constexpr const char* kConfigPath = "sdmc:/switch/mp2-nx/switch_menu.cfg";
// Render scale per mode: 0 = the screen's own resolution, else a multiple of 480 lines.
float sHandheldScale = 0.f;
float sDockedScale = 0.f;
bool sShowFps = false;
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
  std::fprintf(f, "handheld_scale=%.2f\ndocked_scale=%.2f\nfps_counter=%d\n", sHandheldScale, sDockedScale,
               sShowFps ? 1 : 0);
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

enum Page { kGame, kSaveStates, kControls, kVideo, kSystem, kPageCount };
const char* const kPageNames[kPageCount] = {"Game", "Save states", "Controls", "Video", "System"};

int sPage = kGame;
int sSelected[kPageCount] = {};
int sScroll[kPageCount] = {};
int sSlot = 1;
bool sConfirmTitle = false;
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
    rows.push_back({"Spring Ball", OnOff(sSpringBall), "Jump in morph ball (from Metroid Prime 3)",
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
    rows.push_back({"mp2-nx", "Echoes Unbound on Switch", "", nullptr, nullptr, false});
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
    sCloseRequested = false;
    sSlot = std::clamp(PortSaveState::SelectedSlot(), 1, PortSaveState::kSlotCount);
  }
  const Input in = ReadInput(fresh);

  if (in.l || in.r) {
    sPage = (sPage + (in.r ? 1 : -1) + kPageCount) % kPageCount;
    sConfirmTitle = false;
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
  if (in.up) {
    sel = (sel - 1 + count) % count;
  }
  if (in.down) {
    sel = (sel + 1) % count;
  }
  if (in.up || in.down) {
    sConfirmTitle = sConfirmTitle && rows[sel].label.rfind("Return to title", 0) == 0;
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
    if (sConfirmTitle) {
      sConfirmTitle = false;
    } else {
      sCloseRequested = true;
    }
  }
  if (sCloseRequested) {
    sCloseRequested = false;
    return false;
  }
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
  return true;
}

} // namespace PortDebug::SwitchMenu
