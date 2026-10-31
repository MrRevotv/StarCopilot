#include "game_input.h"
#include "log.h"
#include "text_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace GameInput {

std::recursive_mutex& inputLock() {
    static std::recursive_mutex lock;
    return lock;
}

namespace {
    struct KeyEntry { const char* name; unsigned short scan; };
    const KeyEntry kKeys[] = {
        { "Escape", 0x01 }, { "Esc", 0x01 }, { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 }, { "6", 0x07 },
        { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0A }, { "0", 0x0B }, { "Minus", 0x0C }, { "Equals", 0x0D }, { "Backspace", 0x0E },
        { "Tab", 0x0F }, { "Q", 0x10 }, { "W", 0x11 }, { "E", 0x12 }, { "R", 0x13 }, { "T", 0x14 }, { "Y", 0x15 }, { "U", 0x16 },
        { "I", 0x17 }, { "O", 0x18 }, { "P", 0x19 }, { "LBracket", 0x1A }, { "RBracket", 0x1B }, { "Enter", 0x1C }, { "Return", 0x1C },
        { "LCtrl", 0x1D }, { "Ctrl", 0x1D }, { "Control", 0x1D }, { "A", 0x1E }, { "S", 0x1F }, { "D", 0x20 }, { "F", 0x21 },
        { "G", 0x22 }, { "H", 0x23 }, { "J", 0x24 }, { "K", 0x25 }, { "L", 0x26 }, { "Semicolon", 0x27 }, { "Apostrophe", 0x28 },
        { "Grave", 0x29 }, { "Backtick", 0x29 }, { "LShift", 0x2A }, { "Shift", 0x2A }, { "Backslash", 0x2B }, { "Z", 0x2C },
        { "X", 0x2D }, { "C", 0x2E }, { "V", 0x2F }, { "B", 0x30 }, { "N", 0x31 }, { "M", 0x32 }, { "Comma", 0x33 }, { "Period", 0x34 },
        { "Slash", 0x35 }, { "RShift", 0x36 }, { "NumMultiply", 0x37 }, { "LAlt", 0x38 }, { "Alt", 0x38 }, { "Space", 0x39 },
        { "CapsLock", 0x3A }, { "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E }, { "F5", 0x3F }, { "F6", 0x40 },
        { "F7", 0x41 }, { "F8", 0x42 }, { "F9", 0x43 }, { "F10", 0x44 }, { "NumLock", 0x45 }, { "ScrollLock", 0x46 }, { "Num7", 0x47 },
        { "Num8", 0x48 }, { "Num9", 0x49 }, { "NumMinus", 0x4A }, { "Num4", 0x4B }, { "Num5", 0x4C }, { "Num6", 0x4D },
        { "NumPlus", 0x4E }, { "Num1", 0x4F }, { "Num2", 0x50 }, { "Num3", 0x51 }, { "Num0", 0x52 }, { "NumPeriod", 0x53 },
        { "F11", 0x57 }, { "F12", 0x58 },
        // estesi
        { "NumEnter", 0xE01C }, { "RCtrl", 0xE01D }, { "NumDivide", 0xE035 }, { "RAlt", 0xE038 }, { "AltGr", 0xE038 },
        { "Home", 0xE047 }, { "Up", 0xE048 }, { "PageUp", 0xE049 }, { "Left", 0xE04B }, { "Right", 0xE04D }, { "End", 0xE04F },
        { "Down", 0xE050 }, { "PageDown", 0xE051 }, { "Insert", 0xE052 }, { "Delete", 0xE053 }, { "LWin", 0xE05B }, { "RWin", 0xE05C },
    };

    std::string lower(const std::string& s) {
        std::string out = s;
        for (char& c : out) c = (char)std::tolower((unsigned char)c);
        return out;
    }

    void keyEvent(unsigned short scan, bool up) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)(scan & 0xFF);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
        if ((scan & 0xE000) == 0xE000) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        SendInput(1, &in, sizeof(INPUT));
    }

    void mouseButton(DWORD flag) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = flag;
        SendInput(1, &in, sizeof(INPUT));
    }

    void moveAbsolute(int x, int y) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = (LONG)((x - vx) * 65535.0 / std::max(1, vw - 1));
        in.mi.dy = (LONG)((y - vy) * 65535.0 / std::max(1, vh - 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        SendInput(1, &in, sizeof(INPUT));
    }

    // Toglie l'accento: "à" -> 'a' (la tastiera fisica non ha le lettere accentate maiuscole).
    wchar_t deaccent(wchar_t c) {
        static const wchar_t* from = L"àáâäèéêëìíîïòóôöùúûüçñÀÁÂÄÈÉÊËÌÍÎÏÒÓÔÖÙÚÛÜÇÑ";
        static const wchar_t* to = L"aaaaeeeeiiiioooouuuucnAAAAEEEEIIIIOOOOUUUUCN";
        for (int i = 0; from[i]; ++i) {
            if (from[i] == c) return to[i];
        }
        return c;
    }

    // 1 = elevato, 0 = no, -1 = non si può sapere (accesso negato: di solito vuol dire elevato).
    int processElevated(HANDLE process) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return -1;
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) && elevation.TokenIsElevated;
        CloseHandle(token);
        return elevated ? 1 : 0;
    }
}

unsigned short scanCode(const std::string& keyName) {
    const std::string k = TextUtils::trim(keyName);
    if (k.size() > 2 && (k[0] == '0') && (k[1] == 'x' || k[1] == 'X')) return (unsigned short)std::strtoul(k.c_str() + 2, nullptr, 16);
    const std::string l = lower(k);
    for (const auto& e : kKeys) {
        if (lower(e.name) == l) return e.scan;
    }
    return 0;
}

std::vector<unsigned short> parseCombo(const std::string& combo) {
    std::vector<unsigned short> codes;
    size_t start = 0;
    while (start <= combo.size()) {
        size_t plus = combo.find('+', start);
        std::string part = TextUtils::trim(combo.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (!part.empty()) {
            unsigned short c = scanCode(part);
            if (c == 0) return {};
            codes.push_back(c);
        }
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return codes;
}

std::vector<std::string> keyNames() {
    std::vector<std::string> out;
    for (const auto& e : kKeys) out.push_back(e.name);
    return out;
}

void pressCombo(const std::string& combo, int holdMs, int gapMs, bool dryRun) {
    const auto codes = parseCombo(combo);
    if (codes.empty()) {
        Log::warn("Tasto sconosciuto: \"" + combo + "\"");
        return;
    }
    if (dryRun) {
        Log::info("[PROVA] premerei " + combo);
        Sleep(gapMs);
        return;
    }
    for (auto c : codes) keyEvent(c, false);
    Sleep(holdMs);
    for (auto it = codes.rbegin(); it != codes.rend(); ++it) keyEvent(*it, true);
    Sleep(gapMs);
}

void holdCombo(const std::string& combo, int ms, int gapMs, bool dryRun) {
    const auto codes = parseCombo(combo);
    if (codes.empty()) {
        Log::warn("Tasto sconosciuto: \"" + combo + "\"");
        return;
    }
    if (dryRun) {
        Log::info("[PROVA] terrei premuto " + combo + " per " + std::to_string(ms) + " ms");
        return;
    }
    for (auto c : codes) keyEvent(c, false);
    Sleep(ms);
    for (auto it = codes.rbegin(); it != codes.rend(); ++it) keyEvent(*it, true);
    Sleep(gapMs);
}

void typeScanCodes(const std::string& text, int perCharMs, bool dryRun) {
    if (dryRun) {
        Log::info("[PROVA] scriverei \"" + text + "\"");
        return;
    }
    for (wchar_t raw : TextUtils::utf8ToWide(text)) {
        const wchar_t ch = deaccent(raw);
        unsigned short sc = 0;
        if (ch == L' ') sc = scanCode("Space");
        else if (iswalpha(ch) && ch < 128) sc = scanCode(std::string(1, (char)towupper(ch)));
        else if (iswdigit(ch)) sc = scanCode(std::string(1, (char)ch));
        // Il meno della riga dei numeri è una POSIZIONE: sulla tastiera italiana è
        // l'apostrofo ("CRU-L1" diventava "CRU'L1"). Quello del tastierino è uguale ovunque.
        else if (ch == L'-') sc = scanCode("NumMinus");
        else if (ch == L'.') sc = scanCode("Period");
        else if (ch == L',') sc = scanCode("Comma");
        if (sc == 0) continue; // simbolo non digitabile: si salta
        keyEvent(sc, false);
        Sleep(30); // il gioco vuole vedere il tasto "tenuto"
        keyEvent(sc, true);
        Sleep(perCharMs);
    }
}

void typeUnicode(const std::string& text, int perCharMs, bool dryRun) {
    if (dryRun) {
        Log::info("[PROVA] scriverei (Unicode) \"" + text + "\"");
        return;
    }
    for (wchar_t ch : TextUtils::utf8ToWide(text)) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wScan = in[1].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(perCharMs);
    }
}

POINT cursorPosition() {
    POINT p{};
    GetCursorPos(&p);
    return p;
}

void moveMouse(int x, int y, bool smooth, int steps, int stepDelayMs, bool dryRun) {
    if (dryRun) {
        Log::info("[PROVA] sposterei il mouse a " + std::to_string(x) + "," + std::to_string(y));
        return;
    }
    if (smooth && steps > 1) {
        // A piccoli passi: alcuni giochi ignorano un salto secco del cursore.
        const POINT start = cursorPosition();
        for (int i = 1; i <= steps; ++i) {
            moveAbsolute(start.x + (x - start.x) * i / steps, start.y + (y - start.y) * i / steps);
            Sleep(stepDelayMs);
        }
    }
    moveAbsolute(x, y);
    Sleep(40);
}

void click(int x, int y, bool smooth, int steps, int stepDelayMs, int settleMs, bool dryRun) {
    moveMouse(x, y, smooth, steps, stepDelayMs, dryRun);
    if (dryRun) {
        Log::info("[PROVA] cliccherei a " + std::to_string(x) + "," + std::to_string(y));
        return;
    }
    Sleep(120); // il gioco deve "vedere" il cursore fermo lì
    mouseButton(MOUSEEVENTF_LEFTDOWN);
    Sleep(45);
    mouseButton(MOUSEEVENTF_LEFTUP);
    Sleep(settleMs);
}

HWND findGameWindow(const std::string& processName, const std::string& windowTitle) {
    struct Search {
        std::wstring exe;
        HWND found = nullptr;
    } search;
    search.exe = TextUtils::utf8ToWide(lower(processName));
    if (!search.exe.empty() && search.exe.size() < 4) search.exe += L".exe";
    else if (search.exe.size() >= 4 && search.exe.substr(search.exe.size() - 4) != L".exe") search.exe += L".exe";
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* s = reinterpret_cast<Search*>(l);
            if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!p) return TRUE;
            wchar_t path[MAX_PATH];
            DWORD len = MAX_PATH;
            bool match = false;
            if (QueryFullProcessImageNameW(p, 0, path, &len)) {
                std::wstring name(path, len);
                size_t slash = name.find_last_of(L"\\/");
                name = name.substr(slash == std::wstring::npos ? 0 : slash + 1);
                CharLowerBuffW(name.data(), (DWORD)name.size());
                match = name == s->exe;
            }
            CloseHandle(p);
            if (match) {
                s->found = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found) return search.found;
    if (!windowTitle.empty()) return FindWindowW(nullptr, TextUtils::utf8ToWide(windowTitle).c_str());
    return nullptr;
}

bool isForeground(HWND window) {
    return window && GetForegroundWindow() == window;
}

bool focus(HWND window) {
    if (!window) return false;
    if (GetForegroundWindow() == window) return true;
    if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
    // Se Iris è in primo piano (es. hai appena premuto un pulsante) Windows lo permette subito.
    if (SetForegroundWindow(window)) {
        for (int i = 0; i < 10 && GetForegroundWindow() != window; ++i) Sleep(30);
        if (GetForegroundWindow() == window) return true;
    }
    // Altrimenti impedisce di rubare il primo piano: un tocco di Alt sblocca SetForegroundWindow.
    INPUT alt[2] = {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(window);
    for (int i = 0; i < 10 && GetForegroundWindow() != window; ++i) Sleep(30);
    return GetForegroundWindow() == window;
}

bool clientRect(HWND window, RECT& out) {
    if (!window) return false;
    RECT cr;
    if (!GetClientRect(window, &cr)) return false;
    POINT tl{ cr.left, cr.top }, br{ cr.right, cr.bottom };
    ClientToScreen(window, &tl);
    ClientToScreen(window, &br);
    out = RECT{ tl.x, tl.y, br.x, br.y };
    return out.right > out.left && out.bottom > out.top;
}

bool selfElevated() {
    return processElevated(GetCurrentProcess()) == 1;
}

bool windowElevated(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return true; // non possiamo nemmeno guardarlo: quasi certamente è elevato
    const int e = processElevated(p);
    CloseHandle(p);
    return e == 1 || (e < 0 && !selfElevated());
}

} // namespace GameInput
