// mp2-nx: rumble and motion on the Switch, straight through libnx. SDL's Switch backend answers
// "unsupported" to every rumble call and reports no sensors, so Aurora's device layer (rumble for
// pad 0: aurora::device, with GameController::m_hasRumble false on Switch) and Echoes' gyro aim
// (ReadGyroRates, through mp2nx_switch_gyro) had nothing. A reduced port of Alek's
// aurora-aleks lib/device_switch.cpp (MIT): player 1's npad choice, unit conversion, vibration
// devices, and a worker that ends a timed pulse; his HD Rumble layers and per-side sensors are
// left out. Replaces lib/device.cpp, which is compiled empty on Switch.
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

// ---- Player 1's npad (Alek, rc17) ----
// SDL's one Switch joystick reads Handheld and No1 together, so both drive player 1; vibration and
// motion must go to the one in use: the one connected on its own, else the one with the latest
// input (No1 until either has some).
PadState sPadNo1{};
PadState sPadHandheld{};
bool sPadsReady = false;
HidNpadIdType sP1 = HidNpadIdType_No1;
std::mutex sNpadMutex;

bool choose_style(u32 set, HidNpadStyleTag& out) {
  for (const HidNpadStyleTag tag : {HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadJoyDual,
                                    HidNpadStyleTag_NpadJoyRight, HidNpadStyleTag_NpadJoyLeft}) {
    if ((set & tag) != 0) {
      out = tag;
      return true;
    }
  }
  return false;
}

bool npad_style(HidNpadIdType id, HidNpadStyleTag& style) {
  const u32 set = hidGetNpadStyleSet(id);
  if (id == HidNpadIdType_Handheld) {
    if ((set & HidNpadStyleTag_NpadHandheld) == 0) return false;
    style = HidNpadStyleTag_NpadHandheld;
    return true;
  }
  return choose_style(set, style);
}

bool had_input(PadState& pad) {
  padUpdate(&pad);
  if (padGetButtonsDown(&pad) != 0) return true;
  constexpr s32 kOffCentre = 9000;
  for (int stick = 0; stick < 2; ++stick) {
    const HidAnalogStickState s = padGetStickPos(&pad, stick);
    if (s.x > kOffCentre || s.x < -kOffCentre || s.y > kOffCentre || s.y < -kOffCentre) return true;
  }
  return false;
}

// Updates and returns player 1's npad and style; false when none is connected.
bool player_one(HidNpadIdType& id, HidNpadStyleTag& style) {
  std::lock_guard lock{sNpadMutex};
  if (!sPadsReady) {
    padInitialize(&sPadNo1, HidNpadIdType_No1);
    padInitialize(&sPadHandheld, HidNpadIdType_Handheld);
    sPadsReady = true;
  }
  HidNpadStyleTag no1Style{}, hhStyle{};
  const bool no1 = npad_style(HidNpadIdType_No1, no1Style);
  const bool hh = npad_style(HidNpadIdType_Handheld, hhStyle);
  if (no1 && !hh) {
    sP1 = HidNpadIdType_No1;
  } else if (hh && !no1) {
    sP1 = HidNpadIdType_Handheld;
  } else if (no1 && hh) {
    if (had_input(sPadHandheld)) sP1 = HidNpadIdType_Handheld;
    if (had_input(sPadNo1)) sP1 = HidNpadIdType_No1;
  } else {
    return false;
  }
  id = sP1;
  style = sP1 == HidNpadIdType_Handheld ? hhStyle : no1Style;
  return true;
}

// ---- Motion ----
HidSixAxisSensorHandle sSensor{};
bool sSensorStarted = false;
u64 sLastResolveTick = 0;

bool ensure_sensor() {
  // Re-resolve at most twice a second (each call reads HID styles).
  const u64 now = armGetSystemTick();
  if (sSensorStarted && now - sLastResolveTick < armNsToTicks(500'000'000ULL)) return true;
  sLastResolveTick = now;
  HidNpadIdType id{};
  HidNpadStyleTag style{};
  if (!player_one(id, style)) return false;
  HidSixAxisSensorHandle handles[2]{};
  HidSixAxisSensorHandle handle{};
  if (style == HidNpadStyleTag_NpadJoyDual) {
    // A detached pair has a sensor in each Joy-Con: aim with the right one.
    if (R_FAILED(hidGetSixAxisSensorHandles(handles, 2, id, style))) return false;
    handle = handles[1];
  } else {
    if (R_FAILED(hidGetSixAxisSensorHandles(handles, 1, id, style))) return false;
    handle = handles[0];
  }
  if (sSensorStarted && sSensor.type_value != handle.type_value) {
    hidStopSixAxisSensor(sSensor);
    sSensorStarted = false;
  }
  sSensor = handle;
  if (!sSensorStarted) {
    sSensorStarted = R_SUCCEEDED(hidStartSixAxisSensor(sSensor));
    std::fprintf(stderr, "mp2-nx: motion sensor on npad %s: %s\n", id == HidNpadIdType_Handheld ? "Handheld" : "No1",
                 sSensorStarted ? "started" : "failed");
  }
  return sSensorStarted;
}

// libnx: revolutions/s and g. SDL (what Echoes and Aurora are tuned for): radians/s and m/s^2.
// Axes are passed as libnx gives them, as Alek's port does.
bool read_motion(float gyro[3], float accel[3]) {
  if (!ensure_sensor()) return false;
  HidSixAxisSensorState state{};
  if (hidGetSixAxisSensorStates(sSensor, &state, 1) == 0 ||
      (state.attributes & HidSixAxisSensorAttribute_IsConnected) == 0) {
    return false;
  }
  constexpr float kRevToRad = 6.28318530717958647692f;
  constexpr float kG = 9.80665f;
  if (gyro != nullptr) {
    gyro[0] = state.angular_velocity.x * kRevToRad;
    gyro[1] = state.angular_velocity.y * kRevToRad;
    gyro[2] = state.angular_velocity.z * kRevToRad;
  }
  if (accel != nullptr) {
    accel[0] = state.acceleration.x * kG;
    accel[1] = state.acceleration.y * kG;
    accel[2] = state.acceleration.z * kG;
  }
  return true;
}

// ---- Rumble ----
constexpr float kFreqLow = 160.0f;
constexpr float kFreqHigh = 320.0f;
std::mutex sRumbleMutex;
std::condition_variable sRumbleWake;
std::thread sRumbleThread;
bool sRumbleQuit = false;
bool sPulsing = false;
std::chrono::steady_clock::time_point sPulseUntil{};
HidVibrationDeviceHandle sMotors[2]{};
s32 sMotorCount = 0;
HidNpadIdType sMotorId = HidNpadIdType_No1;
HidNpadStyleTag sMotorStyle{};
bool sMotorsReady = false;

// Callers hold sRumbleMutex.
void send_motors(float low, float high) {
  if (!sMotorsReady) return;
  HidVibrationValue values[2]{};
  for (s32 i = 0; i < sMotorCount; ++i) {
    values[i].amp_low = low;
    values[i].freq_low = kFreqLow;
    values[i].amp_high = high;
    values[i].freq_high = kFreqHigh;
  }
  hidSendVibrationValues(sMotors, values, sMotorCount);
}

bool resolve_motors() {
  HidNpadIdType id{};
  HidNpadStyleTag style{};
  if (!player_one(id, style)) return false;
  std::lock_guard lock{sRumbleMutex};
  if (sMotorsReady && id == sMotorId && style == sMotorStyle) return true;
  if (sMotorsReady) {
    send_motors(0.f, 0.f); // the controller left behind must not keep buzzing
    sMotorsReady = false;
  }
  const s32 count = (style == HidNpadStyleTag_NpadJoyLeft || style == HidNpadStyleTag_NpadJoyRight) ? 1 : 2;
  HidVibrationDeviceHandle motors[2]{};
  if (R_FAILED(hidInitializeVibrationDevices(motors, count, id, style))) return false;
  sMotors[0] = motors[0];
  sMotors[1] = motors[1];
  sMotorCount = count;
  sMotorId = id;
  sMotorStyle = style;
  sMotorsReady = true;
  return true;
}

void rumble_worker() {
  std::unique_lock lock{sRumbleMutex};
  while (!sRumbleQuit) {
    if (!sPulsing) {
      sRumbleWake.wait(lock);
      continue;
    }
    if (sRumbleWake.wait_until(lock, sPulseUntil) == std::cv_status::timeout && sPulsing &&
        std::chrono::steady_clock::now() >= sPulseUntil) {
      send_motors(0.f, 0.f);
      sPulsing = false;
    }
  }
}

// Stop when the game loses focus (HOME, sleep): a held rumble would otherwise run on.
AppletHookCookie sFocusHook;
void on_applet(AppletHookType hook, void*) {
  if (hook == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus) {
    std::lock_guard lock{sRumbleMutex};
    sPulsing = false;
    send_motors(0.f, 0.f);
  }
}

}  // namespace

namespace aurora::device {

bool rumble_available() noexcept {
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    appletHook(&sFocusHook, on_applet, nullptr);
  }
  return resolve_motors();
}

void rumble(uint16_t lowFreq, uint16_t highFreq, uint16_t durationMs) noexcept {
  if (!resolve_motors()) return;
  std::lock_guard lock{sRumbleMutex};
  if (!sRumbleThread.joinable() && !sRumbleQuit) {
    sRumbleThread = std::thread{rumble_worker};
  }
  const float low = std::clamp(static_cast<float>(lowFreq) / 65535.f, 0.f, 1.f);
  const float high = std::clamp(static_cast<float>(highFreq) / 65535.f, 0.f, 1.f);
  send_motors(low, high);
  if (durationMs != 0 && (low > 0.f || high > 0.f)) {
    sPulsing = true;
    sPulseUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds{durationMs};
  } else {
    sPulsing = false;
  }
  sRumbleWake.notify_all();
}

bool gyro_available() noexcept { return ensure_sensor(); }
bool gyro(float* data, int n) noexcept {
  float g[3]{};
  if (data == nullptr || n <= 0 || !read_motion(g, nullptr)) return false;
  std::copy_n(g, std::min(n, 3), data);
  return true;
}
bool accel_available() noexcept { return ensure_sensor(); }
bool accel(float* data, int n) noexcept {
  float a[3]{};
  if (data == nullptr || n <= 0 || !read_motion(nullptr, a)) return false;
  std::copy_n(a, std::min(n, 3), data);
  return true;
}

void shutdown() noexcept {
  {
    std::lock_guard lock{sRumbleMutex};
    sRumbleQuit = true;
    sPulsing = false;
    send_motors(0.f, 0.f);
  }
  sRumbleWake.notify_all();
  if (sRumbleThread.joinable()) sRumbleThread.join();
  if (sSensorStarted) {
    hidStopSixAxisSensor(sSensor);
    sSensorStarted = false;
  }
}

}  // namespace aurora::device

// For Echoes' gyro aim (platform/input/port_input_devices.cpp ReadGyroRates): SDL's units.
extern "C" bool mp2nx_switch_gyro(float gyro[3], float accel[3], bool* haveAccel) {
  if (!read_motion(gyro, accel)) return false;
  if (haveAccel != nullptr) *haveAccel = true;
  return true;
}
