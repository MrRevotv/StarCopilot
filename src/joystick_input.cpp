#include "joystick_input.h"
#include "log.h"
#include "text_utils.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

// Due lucchetti, mai uno dentro l'altro durante le chiamate a DirectInput:
//  - g_pollMutex: le API DirectInput (oggetti, apertura, lettura). Le usano
//    solo poll() (thread dei tasti) e initialize/shutdown;
//  - g_mutex: lo stato letto da tutti (pulsanti premuti, nomi, elenco). Si
//    tiene solo per copiare dati, mai mentre si chiama DirectInput: così la
//    finestra, che legge l'elenco, non può restare bloccata ad aspettare
//    DirectInput (che a sua volta potrebbe aspettare la finestra).
namespace {
    // Un asse letto: quale campo di DIJOYSTATE2 e l'intervallo dei valori.
    struct AxisReader {
        int kind = 0;   // 0 X, 1 Y, 2 Z, 3 Rx, 4 Ry, 5 Rz, 6 slider
        int slider = 0; // quale slider (0 o 1)
        LONG min = -10000, max = 10000;
    };

    struct DeviceState {
        JoystickInput::DeviceInfo info;
        bool buttons[128] = {};
        bool connected = false;
        JoystickInput::Layout layout;
        std::vector<AxisReader> readers; // in parallelo a layout.axisNames
        std::vector<double> axes;
        std::vector<int> hats;
    };

    std::mutex g_pollMutex;
    IDirectInput8W* g_di = nullptr;                          // solo con g_pollMutex
    HWND g_window = nullptr;                                 // solo con g_pollMutex
    std::map<std::string, IDirectInputDevice8W*> g_handles;  // solo con g_pollMutex
    std::chrono::steady_clock::time_point g_lastEnum{};      // solo con g_pollMutex

    std::mutex g_mutex;
    std::map<std::string, DeviceState> g_state;       // per id
    std::vector<std::string> g_order;                 // ordine di enumerazione (per le impostazioni vecchie)
    std::map<std::string, std::string> g_knownNames;  // anche delle periferiche scollegate

    // Periferiche collegate/scollegate: lo dice Windows (WM_DEVICECHANGE alla
    // finestra nascosta). Cercarle sempre costa decine di millisecondi, che
    // ritarderebbero il push-to-talk.
    std::atomic<bool> g_rescan{ true };

    std::string guidToString(const GUID& g) {
        wchar_t buf[64] = {};
        StringFromGUID2(g, buf, 64);
        return TextUtils::wideToUtf8(buf);
    }

    BOOL CALLBACK enumCallback(LPCDIDEVICEINSTANCEW instance, LPVOID ctx) {
        auto* found = static_cast<std::vector<DIDEVICEINSTANCEW>*>(ctx);
        found->push_back(*instance);
        return DIENUM_CONTINUE;
    }

    // Gli oggetti della periferica: assi (nell'ordine di DirectInput, come Axis Visualizer),
    // nomi dei pulsanti e quanti cappelli.
    struct ObjectScan {
        IDirectInputDevice8W* dev = nullptr;
        JoystickInput::Layout layout;
        std::vector<AxisReader> readers;
        int sliders = 0;
    };

    BOOL CALLBACK objectCallback(LPCDIDEVICEOBJECTINSTANCEW obj, LPVOID ctx) {
        auto* scan = static_cast<ObjectScan*>(ctx);
        const std::string name = TextUtils::trim(TextUtils::wideToUtf8(obj->tszName));
        if (DIDFT_GETTYPE(obj->dwType) & DIDFT_AXIS) {
            AxisReader r;
            const GUID& t = obj->guidType;
            const char* fallback = "Asse";
            if (t == GUID_XAxis) r.kind = 0, fallback = "Asse X";
            else if (t == GUID_YAxis) r.kind = 1, fallback = "Asse Y";
            else if (t == GUID_ZAxis) r.kind = 2, fallback = "Asse Z";
            else if (t == GUID_RxAxis) r.kind = 3, fallback = "Rotazione X";
            else if (t == GUID_RyAxis) r.kind = 4, fallback = "Rotazione Y";
            else if (t == GUID_RzAxis) r.kind = 5, fallback = "Rotazione Z";
            else if (t == GUID_Slider) {
                r.kind = 6;
                r.slider = scan->sliders++;
                fallback = "Slider";
                if (r.slider >= 2) return DIENUM_CONTINUE; // DIJOYSTATE2 ne ha due
            } else {
                return DIENUM_CONTINUE;
            }
            // Un intervallo simmetrico: il centro è 0 su ogni periferica.
            DIPROPRANGE range{};
            range.diph.dwSize = sizeof(range);
            range.diph.dwHeaderSize = sizeof(range.diph);
            range.diph.dwObj = obj->dwType;
            range.diph.dwHow = DIPH_BYID;
            range.lMin = -10000;
            range.lMax = 10000;
            if (FAILED(scan->dev->SetProperty(DIPROP_RANGE, &range.diph)) && SUCCEEDED(scan->dev->GetProperty(DIPROP_RANGE, &range.diph))) {
                r.min = range.lMin;
                r.max = range.lMax;
            }
            scan->layout.axisNames.push_back(name.empty() ? fallback : name);
            scan->layout.axisKinds.push_back(r.kind);
            scan->readers.push_back(r);
        } else if (DIDFT_GETTYPE(obj->dwType) & DIDFT_BUTTON) {
            if (scan->layout.buttonNames.size() < 128) {
                scan->layout.buttonNames.push_back(name.empty() ? "Pulsante " + std::to_string(scan->layout.buttonNames.size() + 1) : name);
            }
        } else if (DIDFT_GETTYPE(obj->dwType) & DIDFT_POV) {
            scan->layout.hats = std::min(4, scan->layout.hats + 1);
        }
        return DIENUM_CONTINUE;
    }

    LONG readAxis(const DIJOYSTATE2& s, const AxisReader& r) {
        switch (r.kind) {
        case 0: return s.lX;
        case 1: return s.lY;
        case 2: return s.lZ;
        case 3: return s.lRx;
        case 4: return s.lRy;
        case 5: return s.lRz;
        default: return s.rglSlider[std::clamp(r.slider, 0, 1)];
        }
    }

    double normalize(LONG v, LONG min, LONG max) {
        if (max <= min) return 0;
        const double center = (min + max) / 2.0, half = (max - min) / 2.0;
        return std::clamp((v - center) / half, -1.0, 1.0);
    }

    void releaseHandle(IDirectInputDevice8W* dev) {
        if (!dev) return;
        dev->Unacquire();
        dev->Release();
    }

    // Cerca le periferiche collegate e apre quelle nuove. Con g_pollMutex.
    void rescanLocked() {
        g_lastEnum = std::chrono::steady_clock::now();
        if (!g_di) return;
        std::vector<DIDEVICEINSTANCEW> found;
        g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enumCallback, &found, DIEDFL_ATTACHEDONLY);

        std::vector<std::string> order;
        std::vector<JoystickInput::DeviceInfo> opened;
        std::vector<ObjectScan> scans;
        for (const auto& inst : found) {
            const std::string id = guidToString(inst.guidInstance);
            order.push_back(id);
            if (g_handles.count(id)) continue; // già aperta

            IDirectInputDevice8W* dev = nullptr;
            if (FAILED(g_di->CreateDevice(inst.guidInstance, &dev, nullptr))) continue;
            if (FAILED(dev->SetDataFormat(&c_dfDIJoystick2)) ||
                FAILED(dev->SetCooperativeLevel(g_window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
                dev->Release();
                continue;
            }
            DIDEVCAPS caps{};
            caps.dwSize = sizeof(caps);
            dev->GetCapabilities(&caps);
            ObjectScan scan;
            scan.dev = dev;
            dev->EnumObjects(objectCallback, &scan, DIDFT_AXIS | DIDFT_BUTTON | DIDFT_POV);
            scan.dev = nullptr;
            scans.push_back(std::move(scan));
            dev->Acquire();
            g_handles[id] = dev;

            JoystickInput::DeviceInfo info;
            info.id = id;
            info.name = TextUtils::wideToUtf8(inst.tszProductName);
            info.buttonCount = (int)std::min<DWORD>(caps.dwButtons, 128);
            opened.push_back(info);
            Log::info("Joystick: " + info.name + " (" + std::to_string(info.buttonCount) + " pulsanti)");
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        g_order = order;
        for (size_t i = 0; i < opened.size(); ++i) {
            const auto& info = opened[i];
            DeviceState& s = g_state[info.id];
            s = DeviceState{};
            s.info = info;
            s.connected = true;
            s.layout = scans[i].layout;
            s.readers = scans[i].readers;
            s.axes.assign(s.readers.size(), 0.0);
            s.hats.assign((size_t)s.layout.hats, -1);
            g_knownNames[info.id] = info.name;
        }
    }

    LRESULT CALLBACK hiddenProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_DEVICECHANGE) g_rescan = true;
        return DefWindowProcW(h, m, w, l);
    }
}

namespace JoystickInput {

bool initialize() {
    std::lock_guard<std::mutex> pollLock(g_pollMutex);
    if (g_di) return true;
    // DirectInput vuole una finestra "nostra" per il livello di cooperazione:
    // una nascosta, solo per questo (in background legge comunque). Va creata
    // dal thread della finestra principale, che resta vivo e riceve i messaggi.
    WNDCLASSW wc{};
    wc.lpfnWndProc = hiddenProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"IrisJoystickWindow";
    RegisterClassW(&wc);
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&g_di, nullptr))) {
        g_di = nullptr;
        Log::error("Joystick: DirectInput non disponibile");
        return false;
    }
    rescanLocked();
    g_rescan = false;
    return true;
}

void shutdown() {
    std::lock_guard<std::mutex> pollLock(g_pollMutex);
    for (auto& [id, dev] : g_handles) releaseHandle(dev);
    g_handles.clear();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state.clear();
        g_order.clear();
    }
    if (g_di) g_di->Release();
    g_di = nullptr;
    if (g_window) DestroyWindow(g_window);
    g_window = nullptr;
}

void poll() {
    std::lock_guard<std::mutex> pollLock(g_pollMutex);
    if (!g_di) return;
    // Si cercano periferiche nuove quando Windows segnala un cambiamento (al
    // massimo ogni 2 secondi: una periferica che si stacca e riattacca di
    // continuo non deve rallentare tutto), e comunque ogni 30 secondi.
    const auto sinceLast = std::chrono::steady_clock::now() - g_lastEnum;
    if ((g_rescan.load() && sinceLast > std::chrono::seconds(2)) || sinceLast > std::chrono::seconds(30)) {
        g_rescan = false;
        rescanLocked();
    }

    // Lettura dei pulsanti, senza g_mutex.
    struct Reading {
        std::string id;
        bool ok = false;
        bool buttons[128] = {};
        DIJOYSTATE2 state{};
    };
    std::vector<Reading> readings;
    readings.reserve(g_handles.size());
    for (auto& [id, dev] : g_handles) {
        Reading r;
        r.id = id;
        DIJOYSTATE2 state{};
        if (FAILED(dev->Poll())) {
            dev->Acquire();
            dev->Poll();
        }
        HRESULT hr = dev->GetDeviceState(sizeof(state), &state);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            dev->Acquire();
            hr = dev->GetDeviceState(sizeof(state), &state);
        }
        r.ok = SUCCEEDED(hr);
        if (r.ok) {
            for (int b = 0; b < 128; ++b) r.buttons[b] = (state.rgbButtons[b] & 0x80) != 0;
            r.state = state;
        }
        readings.push_back(std::move(r));
    }

    std::vector<IDirectInputDevice8W*> lost;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Reading& r : readings) {
            DeviceState& s = g_state[r.id];
            if (r.ok) {
                std::copy(std::begin(r.buttons), std::end(r.buttons), std::begin(s.buttons));
                for (size_t a = 0; a < s.readers.size() && a < s.axes.size(); ++a) {
                    s.axes[a] = normalize(readAxis(r.state, s.readers[a]), s.readers[a].min, s.readers[a].max);
                }
                for (size_t h = 0; h < s.hats.size() && h < 4; ++h) {
                    const DWORD pov = r.state.rgdwPOV[h];
                    s.hats[h] = LOWORD(pov) == 0xFFFF || pov > 36000 ? -1 : (int)(pov / 100 % 360);
                }
                continue;
            }
            // Scollegata: la si riapre quando torna.
            Log::info("Joystick scollegato: " + s.info.name);
            s.connected = false;
            std::fill(std::begin(s.buttons), std::end(s.buttons), false);
        }
    }
    for (const Reading& r : readings) {
        if (r.ok) continue;
        lost.push_back(g_handles[r.id]);
        g_handles.erase(r.id);
        g_rescan = true;
    }
    for (auto* dev : lost) releaseHandle(dev);
}

std::vector<DeviceInfo> devices() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<DeviceInfo> result;
    for (const std::string& id : g_order) {
        auto it = g_state.find(id);
        if (it != g_state.end() && it->second.connected) result.push_back(it->second.info);
    }
    return result;
}

bool isButtonDown(const std::string& deviceId, int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex >= 128) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_state.find(deviceId);
    return it != g_state.end() && it->second.connected && it->second.buttons[buttonIndex];
}

std::string idForLegacyIndex(int legacyIndex) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (legacyIndex < 0 || legacyIndex >= (int)g_order.size()) return {};
    return g_order[legacyIndex];
}

std::string nameOf(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_knownNames.find(deviceId);
    return it == g_knownNames.end() ? std::string() : it->second;
}

bool layoutOf(const std::string& deviceId, Layout& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_state.find(deviceId);
    if (it == g_state.end()) return false;
    out = it->second.layout;
    return true;
}

bool snapshot(const std::string& deviceId, Snapshot& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_state.find(deviceId);
    if (it == g_state.end() || !it->second.connected) {
        out = Snapshot{};
        return false;
    }
    const DeviceState& s = it->second;
    out.connected = true;
    out.axes = s.axes;
    out.buttons.assign(s.buttons, s.buttons + std::min<size_t>(128, s.layout.buttonNames.size()));
    out.hats = s.hats;
    return true;
}

std::string toAxisId(const std::string& deviceId) {
    std::string out;
    for (char c : deviceId) {
        if (c != '{' && c != '}') out += (char)std::tolower((unsigned char)c);
    }
    return out;
}

std::string fromAxisId(const std::string& axisGuid) {
    std::string out = "{";
    for (char c : axisGuid) {
        if (c != '{' && c != '}') out += (char)std::toupper((unsigned char)c);
    }
    return out + "}";
}

} // namespace JoystickInput
