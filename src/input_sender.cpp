#include "input_sender.h"
#include "text_utils.h"

#include <chrono>
#include <thread>

namespace {
    INPUT scanInput(WORD scan, bool extended, bool keyUp) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (keyUp ? KEYEVENTF_KEYUP : 0);
        return in;
    }

    WORD scanOf(UINT vk, HKL layout, bool& extended) {
        UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, layout);
        extended = (sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100;
        return static_cast<WORD>(sc & 0xFF);
    }

    void sendStroke(const InputSender::Stroke& s) {
        std::vector<INPUT> events;
        if (s.unicode) {
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = s.ch;
            in.ki.dwFlags = KEYEVENTF_UNICODE;
            events.push_back(in);
            in.ki.dwFlags |= KEYEVENTF_KEYUP;
            events.push_back(in);
        } else {
            const WORD lshift = 0x2A, lctrl = 0x1D, alt = 0x38;
            // AltGr = Ctrl sinistro + Alt destro (che è un tasto "esteso").
            const bool altGr = s.ctrl && s.alt;
            if (s.shift) events.push_back(scanInput(lshift, false, false));
            if (s.ctrl) events.push_back(scanInput(lctrl, false, false));
            if (s.alt) events.push_back(scanInput(alt, altGr, false));
            events.push_back(scanInput(s.scan, s.extended, false));
            events.push_back(scanInput(s.scan, s.extended, true));
            if (s.alt) events.push_back(scanInput(alt, altGr, true));
            if (s.ctrl) events.push_back(scanInput(lctrl, false, true));
            if (s.shift) events.push_back(scanInput(lshift, false, true));
        }
        SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    }

    void sleepMs(int ms) {
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

namespace InputSender {

HKL foregroundLayout() {
    HWND fg = GetForegroundWindow();
    DWORD thread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    return GetKeyboardLayout(thread);
}

std::vector<Stroke> planText(const std::wstring& text, HKL layout, Unmappable mode, std::wstring* dropped) {
    std::vector<Stroke> plan;
    auto addChar = [&](wchar_t ch) -> bool {
        SHORT res = VkKeyScanExW(ch, layout);
        if (res == -1) return false;
        BYTE vk = LOBYTE(res), mods = HIBYTE(res);
        // Bit 8+ = stati particolari (kana...) che non sappiamo riprodurre.
        if (mods & ~0x07) return false;
        Stroke s;
        s.scan = scanOf(vk, layout, s.extended);
        if (s.scan == 0) return false;
        s.shift = (mods & 1) != 0;
        s.ctrl = (mods & 2) != 0;
        s.alt = (mods & 4) != 0;
        // Solo Ctrl (senza Alt) = un carattere di controllo: non è testo.
        if (s.ctrl && !s.alt) return false;
        plan.push_back(s);
        return true;
    };

    for (wchar_t raw : text) {
        wchar_t ch = raw;
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (ch < 32) continue;
        if (addChar(ch)) continue;
        if (mode == Unmappable::Unicode) {
            Stroke s;
            s.unicode = true;
            s.ch = ch;
            plan.push_back(s);
            continue;
        }
        std::wstring repl = TextUtils::transliterate(ch);
        bool ok = !repl.empty();
        for (wchar_t r : repl) ok = addChar(r) && ok;
        if (!ok && dropped) dropped->push_back(ch);
    }
    return plan;
}

bool waitForModifiersReleased(int timeoutMs) {
    const int keys[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
    auto start = std::chrono::steady_clock::now();
    while (true) {
        bool anyDown = false;
        for (int k : keys) anyDown = anyDown || (GetAsyncKeyState(k) & 0x8000) != 0;
        if (!anyDown) return true;
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) return false;
        sleepMs(15);
    }
}

void pressKey(int virtualKey) {
    Stroke s;
    s.scan = scanOf((UINT)virtualKey, foregroundLayout(), s.extended);
    if (s.scan == 0) return;
    sendStroke(s);
}

void sendChatMessage(const std::string& utf8Text, const Options& o) {
    std::wstring text = TextUtils::utf8ToWide(utf8Text);
    if (text.empty()) return;
    HKL layout = foregroundLayout();
    std::vector<Stroke> plan = planText(text, layout, o.unmappable);
    if (plan.empty()) return;

    if (o.openChat) {
        pressKey(o.openKeyVk);
        sleepMs(o.delayAfterOpenMs);
    }
    for (const Stroke& s : plan) {
        sendStroke(s);
        sleepMs(o.interCharDelayMs);
    }
    if (o.autoSend) {
        sleepMs(o.delayBeforeSendMs);
        pressKey(o.sendKeyVk);
    }
}

} // namespace InputSender
