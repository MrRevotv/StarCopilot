#include "joystick_input.h"
#include "log.h"
#include "text_utils.h"

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <algorithm>
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
    struct DeviceState {
        JoystickInput::DeviceInfo info;
        bool buttons[128] = {};
        bool connected = false;
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
        for (const auto& info : opened) {
            DeviceState& s = g_state[info.id];
            s = DeviceState{};
            s.info = info;
            s.connected = true;
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

} // namespace JoystickInput
