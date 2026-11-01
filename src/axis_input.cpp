#include "axis_input.h"
#include "hotkey_listener.h"
#include "joystick_input.h"
#include "text_utils.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace Axis {

namespace {
    const std::vector<std::string> kXInputAxes = { "Levetta SX - orizzontale", "Levetta SX - verticale", "Levetta DX - orizzontale",
                                                   "Levetta DX - verticale",   "Grilletto SX (LT)",      "Grilletto DX (RT)",
                                                   "Grilletti combinati (RT - LT)" };
    const std::vector<std::string> kXInputButtons = { "A", "B", "X", "Y", "LB", "RB", "LS", "RS", "BACK", "START" };
    const WORD kXInputMasks[] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x0100, 0x0200, 0x0040, 0x0080, 0x0020, 0x0010 };
    const std::vector<std::string> kMouseAxes = { "Orizzontale", "Verticale", "Rotella" };
    const std::vector<std::string> kMouseButtons = { "SX", "C", "DX", "4", "5" };
    const int kMouseKeys[] = { VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    const std::vector<std::string> kKeyboardAxes = { "Tasti orizzontali", "Tasti verticali", "Tasti barra" };

    std::atomic<double> g_wheelTotal{ 0 };

    // XInput si carica al volo: niente da collegare, e senza la DLL i pad semplicemente non ci sono.
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    XInputGetStateFn xinput() {
        static XInputGetStateFn fn = []() -> XInputGetStateFn {
            HMODULE dll = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!dll) dll = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            return dll ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(dll, "XInputGetState")) : nullptr;
        }();
        return fn;
    }

    bool xinputState(int slot, XINPUT_STATE& state) {
        auto fn = xinput();
        return fn && fn((DWORD)slot, &state) == ERROR_SUCCESS;
    }

    bool keyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

    double clamp1(double v) { return std::clamp(v, -1.0, 1.0); }

    DeviceInfo xinputInfo(int slot) {
        DeviceInfo d;
        d.id = "xinput:" + std::to_string(slot);
        d.name = "PAD XBOX " + std::to_string(slot + 1) + " (XINPUT)";
        d.axisNames = kXInputAxes;
        d.buttonNames = kXInputButtons;
        d.hats = 1;
        d.defaultX = 0;
        d.defaultY = 1;
        d.defaultZ = 6;
        d.defaultInvertY = true; // su XInput la levetta in alto è positiva, a schermo Y cresce in basso
        return d;
    }

    DeviceInfo mouseInfo() {
        DeviceInfo d;
        d.id = "mouse";
        d.name = "MOUSE";
        d.kind = "mouse";
        d.axisNames = kMouseAxes;
        d.buttonNames = kMouseButtons;
        return d;
    }

    DeviceInfo keyboardInfo() {
        DeviceInfo d;
        d.id = "keyboard";
        d.name = "TASTIERA";
        d.kind = "keyboard";
        d.axisNames = kKeyboardAxes;
        d.defaultZ = 2;
        return d;
    }

    DeviceInfo dinputInfo(const std::string& irisId, const std::string& name) {
        DeviceInfo d;
        d.id = "dinput:" + JoystickInput::toAxisId(irisId);
        d.name = name.empty() ? "Periferica di gioco" : name;
        JoystickInput::Layout layout;
        if (JoystickInput::layoutOf(irisId, layout)) {
            d.axisNames = layout.axisNames;
            d.buttonNames = layout.buttonNames;
            d.hats = layout.hats;
            auto indexOf = [&](int kind) {
                for (size_t i = 0; i < layout.axisKinds.size(); ++i) {
                    if (layout.axisKinds[i] == kind) return (int)i;
                }
                return -1;
            };
            d.defaultX = std::max(0, indexOf(0));
            d.defaultY = indexOf(1) >= 0 ? indexOf(1) : 1;
            d.defaultZ = indexOf(2) >= 0 ? indexOf(2) : indexOf(6);
        }
        return d;
    }
}

void addMouseWheel(double notches) {
    double current = g_wheelTotal.load();
    while (!g_wheelTotal.compare_exchange_weak(current, current + notches)) {
    }
}

std::vector<DeviceInfo> devices() {
    std::vector<DeviceInfo> out;
    for (int slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        XINPUT_STATE st{};
        if (xinputState(slot, st)) out.push_back(xinputInfo(slot));
    }
    for (const auto& j : JoystickInput::devices()) out.push_back(dinputInfo(j.id, j.name));
    out.push_back(mouseInfo());
    out.push_back(keyboardInfo());
    return out;
}

DeviceInfo describe(const std::string& id) {
    if (id == "mouse") return mouseInfo();
    if (id == "keyboard") return keyboardInfo();
    if (id.rfind("xinput:", 0) == 0) {
        DeviceInfo d = xinputInfo(std::atoi(id.c_str() + 7));
        XINPUT_STATE st{};
        d.connected = xinputState(std::atoi(id.c_str() + 7), st);
        return d;
    }
    if (id.rfind("dinput:", 0) == 0) {
        const std::string irisId = JoystickInput::fromAxisId(id.substr(7));
        DeviceInfo d = dinputInfo(irisId, JoystickInput::nameOf(irisId));
        d.id = id;
        JoystickInput::Snapshot snap;
        d.connected = JoystickInput::snapshot(irisId, snap);
        return d;
    }
    DeviceInfo d;
    d.id = id;
    d.name = "—";
    d.connected = false;
    return d;
}

Source::Source(const Pane& pane, const Settings& settings) : m_pane(pane), m_keys(settings.keyboard), m_mouseRange(settings.mouseRange) {
    m_lastWheelTotal = g_wheelTotal.load();
    const DeviceInfo info = describe(pane.deviceId);
    m_state.axisNames = info.axisNames;
    m_state.buttonNames = info.buttonNames;
    if (pane.deviceId == "keyboard") {
        // I pulsanti della tastiera sono i tasti scelti, con il loro nome.
        m_state.buttonNames.clear();
        for (int vk : pane.buttons) {
            if (vk > 0) m_state.buttonNames.push_back(HotkeyListener::keyName(vk));
        }
    }
    m_state.axes.assign(m_state.axisNames.size(), 0.0);
    m_state.buttons.assign(m_state.buttonNames.size(), false);
    m_state.hats.assign((size_t)info.hats, -1);
}

std::optional<double> Source::mapped(int index, bool invert) const {
    if (index < 0 || index >= (int)m_state.axes.size()) return std::nullopt;
    return invert ? -m_state.axes[index] : m_state.axes[index];
}

void Source::update(double dt) {
    const std::string& id = m_pane.deviceId;
    InputState& s = m_state;
    if (id == "mouse") {
        s.connected = true;
        POINT p{};
        if (GetCursorPos(&p)) {
            const double cx = GetSystemMetrics(SM_CXSCREEN) / 2.0, cy = GetSystemMetrics(SM_CYSCREEN) / 2.0;
            const double range = std::max(1, m_mouseRange);
            s.axes[0] = clamp1((p.x - cx) / range);
            s.axes[1] = clamp1((p.y - cy) / range);
        }
        const double total = g_wheelTotal.load();
        const double notches = total - m_lastWheelTotal;
        m_lastWheelTotal = total;
        if (notches != 0) s.wheel = clamp1(s.wheel + notches * 0.5);
        s.wheel *= std::exp(-dt / 0.18);
        if (std::fabs(s.wheel) < 0.01) s.wheel = 0;
        s.axes[2] = s.wheel;
        for (size_t i = 0; i < s.buttons.size() && i < 5; ++i) s.buttons[i] = keyDown(kMouseKeys[i]);
    } else if (id == "keyboard") {
        s.connected = true;
        double dx = 0, dy = 0;
        if (keyDown(m_keys.up)) dy -= 1;
        if (keyDown(m_keys.down)) dy += 1;
        if (keyDown(m_keys.left)) dx -= 1;
        if (keyDown(m_keys.right)) dx += 1;
        if (dx != 0 || dy != 0) {
            const double len = std::sqrt(dx * dx + dy * dy);
            dx /= len;
            dy /= len;
        }
        const bool plus = keyDown(m_keys.barPlus), minus = keyDown(m_keys.barMinus);
        const double speed = 1.0 / std::max(0.05, m_keys.barTravelSeconds);
        if (plus) m_bar += speed * dt;
        if (minus) m_bar -= speed * dt;
        if (!plus && !minus) {
            m_bar *= std::exp(-dt / 0.224);
            if (std::fabs(m_bar) < 0.006) m_bar = 0;
        }
        m_bar = clamp1(m_bar);
        s.axes = { dx, dy, m_bar };
        size_t b = 0;
        for (int vk : m_pane.buttons) {
            if (vk > 0 && b < s.buttons.size()) s.buttons[b++] = keyDown(vk);
        }
    } else if (id.rfind("xinput:", 0) == 0) {
        XINPUT_STATE st{};
        s.connected = xinputState(std::atoi(id.c_str() + 7), st);
        if (!s.connected) {
            std::fill(s.axes.begin(), s.axes.end(), 0.0);
            std::fill(s.buttons.begin(), s.buttons.end(), false);
            if (!s.hats.empty()) s.hats[0] = -1;
        } else {
            const XINPUT_GAMEPAD& g = st.Gamepad;
            auto thumb = [](SHORT v) { return clamp1(v / 32767.0); };
            s.axes = { thumb(g.sThumbLX), thumb(g.sThumbLY), thumb(g.sThumbRX), thumb(g.sThumbRY), g.bLeftTrigger / 255.0 * 2.0 - 1.0,
                       g.bRightTrigger / 255.0 * 2.0 - 1.0, (g.bRightTrigger - g.bLeftTrigger) / 255.0 };
            for (size_t i = 0; i < s.buttons.size() && i < 10; ++i) s.buttons[i] = (g.wButtons & kXInputMasks[i]) != 0;
            bool up = (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0, down = (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            bool left = (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0, right = (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            if (up && down) up = down = false;
            if (left && right) left = right = false;
            int angle = -1;
            if (up && right) angle = 45;
            else if (down && right) angle = 135;
            else if (down && left) angle = 225;
            else if (up && left) angle = 315;
            else if (up) angle = 0;
            else if (right) angle = 90;
            else if (down) angle = 180;
            else if (left) angle = 270;
            if (!s.hats.empty()) s.hats[0] = angle;
        }
    } else if (id.rfind("dinput:", 0) == 0) {
        JoystickInput::Snapshot snap;
        s.connected = JoystickInput::snapshot(JoystickInput::fromAxisId(id.substr(7)), snap);
        if (s.connected) {
            s.axes = snap.axes;
            s.buttons = snap.buttons;
            s.hats = snap.hats;
            // La periferica si è appena collegata: i nomi arrivano adesso.
            if (s.axisNames.size() != s.axes.size() || s.buttonNames.size() != s.buttons.size()) {
                const DeviceInfo info = describe(id);
                s.axisNames = info.axisNames;
                s.buttonNames = info.buttonNames;
            }
        } else {
            std::fill(s.axes.begin(), s.axes.end(), 0.0);
            std::fill(s.buttons.begin(), s.buttons.end(), false);
            std::fill(s.hats.begin(), s.hats.end(), -1);
        }
    }
    s.x = mapped(m_pane.axisX, m_pane.invertX).value_or(0.0);
    s.y = mapped(m_pane.axisY, m_pane.invertY).value_or(0.0);
    s.z = mapped(m_pane.axisZ, m_pane.invertZ);
}

int firstPressedKey() {
    // Mouse, Pausa e Stamp esclusi (come Axis Visualizer).
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == 0x13 || vk == 0x2C) continue;
        if (keyDown(vk)) return vk;
    }
    return -1;
}

} // namespace Axis
