#include "hotkey_listener.h"
#include "joystick_input.h"
#include "text_utils.h"

#include <windows.h>
#include <chrono>
#include <map>

HotkeyListener::~HotkeyListener() {
    stop();
}

void HotkeyListener::registerToggleKey(int vk, std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_toggles.push_back({ vk, false, std::move(cb) });
}

void HotkeyListener::setActions(std::vector<Action> actions) {
    // Un tasto già tenuto giù nel momento del cambio non fa partire l'azione.
    std::vector<bool> down;
    for (const auto& a : actions) down.push_back(anyDown(a.triggers));
    std::lock_guard<std::mutex> lock(m_mutex);
    m_actions = std::move(actions);
    m_actionWasDown = std::move(down);
}

void HotkeyListener::setTalkTriggers(const std::vector<TalkTrigger>& triggers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_triggers = triggers;
}

std::vector<TalkTrigger> HotkeyListener::getTalkTriggers() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_triggers;
}

void HotkeyListener::setChatTriggers(const std::vector<TalkTrigger>& triggers) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_chatTriggers = triggers;
}

bool HotkeyListener::isAnyChatTriggerDown() {
    std::vector<TalkTrigger> triggers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        triggers = m_chatTriggers;
    }
    return anyDown(triggers);
}

bool HotkeyListener::isAnyTalkTriggerDown() {
    return anyDown(getTalkTriggers());
}

bool HotkeyListener::anyDown(const std::vector<TalkTrigger>& triggers) {
    for (const auto& t : triggers) {
        if (t.type == TalkTrigger::Type::Keyboard) {
            if (GetAsyncKeyState(t.vk) & 0x8000) return true;
        } else if (JoystickInput::isButtonDown(t.joystickId, t.buttonIndex)) {
            return true;
        }
    }
    return false;
}

std::string HotkeyListener::keyName(int vk) {
    switch (vk) {
    case VK_LBUTTON: return "tasto sinistro del mouse";
    case VK_RBUTTON: return "tasto destro del mouse";
    case VK_MBUTTON: return "tasto centrale del mouse (rotellina)";
    case VK_XBUTTON1: return "tasto laterale del mouse (indietro)";
    case VK_XBUTTON2: return "tasto laterale del mouse (avanti)";
    case VK_RETURN: return "Invio";
    case VK_ESCAPE: return "Esc";
    case VK_SPACE: return "Spazio";
    case VK_TAB: return "Tab";
    case VK_CAPITAL: return "Bloc Maiusc";
    case VK_SHIFT: return "Shift";
    case VK_LSHIFT: return "Shift sinistro";
    case VK_RSHIFT: return "Shift destro";
    case VK_CONTROL: return "Ctrl";
    case VK_LCONTROL: return "Ctrl sinistro";
    case VK_RCONTROL: return "Ctrl destro";
    case VK_MENU: return "Alt";
    case VK_LMENU: return "Alt sinistro";
    case VK_RMENU: return "Alt Gr";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Canc";
    case VK_INSERT: return "Ins";
    case VK_HOME: return "Home";
    case VK_END: return "Fine";
    case VK_PRIOR: return "Pagina su";
    case VK_NEXT: return "Pagina giù";
    case VK_UP: return "Freccia su";
    case VK_DOWN: return "Freccia giù";
    case VK_LEFT: return "Freccia sinistra";
    case VK_RIGHT: return "Freccia destra";
    case VK_NUMLOCK: return "Bloc Num";
    case VK_SCROLL: return "Bloc Scorr";
    case VK_PAUSE: return "Pausa";
    }
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Tastierino " + std::to_string(vk - VK_NUMPAD0);
    // Gli altri: il nome che dà Windows per la tastiera in uso (es. "Ò", "+").
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC_EX);
    if (sc) {
        LONG lparam = (LONG)((sc & 0xFF) << 16);
        if ((sc & 0xFF00) == 0xE000) lparam |= 1 << 24;
        wchar_t name[64] = {};
        if (GetKeyNameTextW(lparam, name, 64) > 0) return TextUtils::wideToUtf8(name);
    }
    return "tasto " + std::to_string(vk);
}

std::string HotkeyListener::describeTrigger(const TalkTrigger& t) {
    if (t.type == TalkTrigger::Type::Keyboard) return keyName(t.vk);
    std::string name = t.joystickName.empty() ? JoystickInput::nameOf(t.joystickId) : t.joystickName;
    if (name.empty()) name = "Joystick";
    return name + ": pulsante " + std::to_string(t.buttonIndex + 1);
}

bool HotkeyListener::captureNextInput(TalkTrigger& out, std::atomic<bool>& cancel, int timeoutMs, bool allowJoystick) {
    bool keyWasDown[256] = {};
    for (int vk = 1; vk < 256; ++vk) keyWasDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    std::map<std::string, std::vector<bool>> joyWasDown;
    // Lo stato dei joystick lo aggiorna il thread di ascolto (sempre acceso,
    // anche quando il push-to-talk è sospeso per la scelta del tasto).
    auto snapshot = [&](bool initial) -> bool {
        for (const auto& j : JoystickInput::devices()) {
            auto& prev = joyWasDown[j.id];
            if (prev.size() < (size_t)j.buttonCount) prev.resize(j.buttonCount, initial);
            for (int b = 0; b < j.buttonCount; ++b) {
                bool down = JoystickInput::isButtonDown(j.id, b);
                if (!initial && down && !prev[b]) {
                    out = TalkTrigger{};
                    out.type = TalkTrigger::Type::Joystick;
                    out.joystickId = j.id;
                    out.joystickName = j.name;
                    out.buttonIndex = b;
                    return true;
                }
                prev[b] = down;
            }
        }
        return false;
    };
    if (allowJoystick) snapshot(true);

    auto start = std::chrono::steady_clock::now();
    while (!cancel.load()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) return false;
        for (int vk = 1; vk < 256; ++vk) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down && !keyWasDown[vk]) {
                if (vk == VK_ESCAPE) return false;
                // Shift/Ctrl/Alt generici: preferiamo la versione destra/sinistra
                // (arriva nello stesso giro, con un codice più alto).
                bool generic = vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU;
                // Per i tasti della chat (allowJoystick=false) serve la tastiera, non il mouse.
                bool mouse = vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
                if (vk != VK_LBUTTON && !generic && (allowJoystick || !mouse)) {
                    out = TalkTrigger{};
                    out.type = TalkTrigger::Type::Keyboard;
                    out.vk = vk;
                    return true;
                }
            }
            keyWasDown[vk] = down;
        }
        if (allowJoystick && snapshot(false)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return false;
}

void HotkeyListener::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&HotkeyListener::loop, this);
}

void HotkeyListener::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}

void HotkeyListener::loop() {
    while (m_running) {
        JoystickInput::poll();
        bool down = !m_paused && isAnyTalkTriggerDown();
        bool chatDown = !m_paused && isAnyChatTriggerDown();
        if (m_waitRelease) {
            // Ancora premuti da prima della pausa: non contano.
            if (down || chatDown) down = chatDown = false;
            else m_waitRelease = false;
        }
        if (down && !m_wasDown) {
            if (m_onTalkStart) m_onTalkStart();
        } else if (!down && m_wasDown) {
            if (m_onTalkStop) m_onTalkStop();
        }
        m_wasDown = down;
        if (chatDown && !m_chatWasDown) {
            if (m_onChatStart) m_onChatStart();
        } else if (!chatDown && m_chatWasDown) {
            if (m_onChatStop) m_onChatStop();
        }
        m_chatWasDown = chatDown;

        std::vector<std::function<void()>> fire;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& t : m_toggles) {
                bool tDown = (GetAsyncKeyState(t.vk) & 0x8000) != 0;
                if (tDown && !t.wasDown && !m_paused) fire.push_back(t.callback);
                t.wasDown = tDown;
            }
            // Le azioni: lo stato si segue anche in pausa, così il tasto appena scelto
            // (ancora premuto quando la pausa finisce) non parte da solo.
            for (size_t i = 0; i < m_actions.size() && i < m_actionWasDown.size(); ++i) {
                const bool aDown = anyDown(m_actions[i].triggers);
                if (aDown && !m_actionWasDown[i] && !m_paused) fire.push_back(m_actions[i].callback);
                m_actionWasDown[i] = aDown;
            }
        }
        for (auto& f : fire) if (f) f();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (m_wasDown && m_onTalkStop) m_onTalkStop();
    if (m_chatWasDown && m_onChatStop) m_onChatStop();
    m_wasDown = m_chatWasDown = false;
}
