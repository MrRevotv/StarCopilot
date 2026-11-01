#include "ocr_overlay.h"
#include "ui/ui.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>

namespace {
    constexpr int kMinWidth = 320;
    constexpr int kMaxWidth = 760;
    constexpr int kPadding = 12;
    constexpr int kLineGap = 6;
    constexpr int kRegionGap = 8;
    constexpr UINT_PTR kUnlockTimer = 1;
    constexpr UINT_PTR kTopmostTimer = 2;
    constexpr const wchar_t* kClassName = L"VoiceToChatOcrOverlay";

    HWND g_hwnd = nullptr;
    std::vector<OcrOverlay::Line> g_lines;
    RECT g_region{};
    bool g_hasRegion = false;
    bool g_shown = false; // c'è qualcosa da mostrare (indipendentemente dallo sblocco)
    int g_alpha = 235;
    int g_savedX = -1, g_savedY = -1;
    bool g_locked = true;
    bool g_moved = false;
    bool g_dragging = false; // tra WM_ENTERSIZEMOVE e WM_EXITSIZEMOVE: la posizione la decide il mouse
    bool g_hiddenFromCapture = false;
    int g_secondsLeft = 0;
    std::function<void(int, int)> g_onMoved;

    UINT dpiAt(const RECT& r) {
        UINT x = 96, y = 96;
        if (FAILED(GetDpiForMonitor(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y))) return 96;
        return x;
    }

    bool hasManualPosition() {
        return !(g_savedX == -1 && g_savedY == -1) && MonitorFromPoint(POINT{ g_savedX + 10, g_savedY + 10 }, MONITOR_DEFAULTTONULL);
    }

    struct Measured {
        int prefixWidth = 0;
        bool prefixOwnLine = false;
        int height = 0;
    };

    Measured measureLine(HDC hdc, const OcrOverlay::Line& line, int innerWidth, UINT dpi) {
        Measured m;
        HFONT bold = Ui::font(Ui::Font::Bold, dpi), normal = Ui::font(Ui::Font::Normal, dpi);
        if (!line.prefix.empty()) {
            HGDIOBJ old = SelectObject(hdc, bold);
            SIZE size{};
            GetTextExtentPoint32W(hdc, line.prefix.c_str(), (int)line.prefix.size(), &size);
            SelectObject(hdc, old);
            m.prefixWidth = size.cx;
            // Nome lunghissimo: va su una riga sua, il testo sotto a tutta larghezza.
            m.prefixOwnLine = m.prefixWidth > innerWidth * 2 / 5;
        }
        if (m.prefixOwnLine) {
            m.height = Ui::textHeight(hdc, line.prefix, innerWidth, bold) + Ui::textHeight(hdc, line.text, innerWidth, normal);
        } else {
            int textH = Ui::textHeight(hdc, line.text, innerWidth - m.prefixWidth, normal);
            int prefixH = line.prefix.empty() ? 0 : Ui::textHeight(hdc, line.prefix, innerWidth, bold);
            m.height = std::max(textH, prefixH);
        }
        return m;
    }

    // Il riquadro si vede nelle catture dello schermo (Discord, OBS), tranne
    // quando copre la zona letta: lì il programma rileggerebbe la propria traduzione.
    void updateCaptureVisibility() {
        RECT r, overlap;
        if (!g_hwnd || !GetWindowRect(g_hwnd, &r)) return;
        const bool hide = g_hasRegion && IntersectRect(&overlap, &r, &g_region);
        if (hide == g_hiddenFromCapture) return;
        if (SetWindowDisplayAffinity(g_hwnd, hide ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : WDA_NONE)) g_hiddenFromCapture = hide;
    }

    // Misura e posiziona il riquadro per le righe attuali.
    void layout() {
        if (!g_hwnd) return;
        RECT anchor = g_hasRegion ? g_region : RECT{ 100, 100, 100 + 480, 100 + 200 };
        if (hasManualPosition()) anchor = RECT{ g_savedX, g_savedY, g_savedX + (anchor.right - anchor.left), g_savedY + 10 };
        const UINT dpi = dpiAt(anchor);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT mon = mi.rcMonitor;

        const int width = std::clamp((int)(g_region.right - g_region.left), Ui::px(kMinWidth, dpi), Ui::px(kMaxWidth, dpi));
        const int inner = width - 2 * Ui::px(kPadding, dpi);
        const int maxHeight = (mon.bottom - mon.top) * 3 / 5;

        HDC hdc = GetDC(g_hwnd);
        std::vector<int> heights;
        for (const auto& l : g_lines) heights.push_back(measureLine(hdc, l, inner, dpi).height);
        ReleaseDC(g_hwnd, hdc);
        // Troppo alto per lo schermo: si tolgono i messaggi più vecchi (in alto).
        int height = 0;
        size_t first = g_lines.size();
        while (first > 0) {
            int add = heights[first - 1] + (first < g_lines.size() ? Ui::px(kLineGap, dpi) : 0);
            if (height + add + 2 * Ui::px(kPadding, dpi) > maxHeight && first < g_lines.size()) break;
            height += add;
            first--;
        }
        if (first > 0) g_lines.erase(g_lines.begin(), g_lines.begin() + first);
        if (!g_locked) {
            // La riga "Trascina per spostare..." in cima (vedi paint).
            HDC dc = GetDC(g_hwnd);
            height += Ui::textHeight(dc, L"Trascina per spostare", inner, Ui::font(Ui::Font::Normal, dpi)) + Ui::px(kLineGap, dpi);
            ReleaseDC(g_hwnd, dc);
        }
        height += 2 * Ui::px(kPadding, dpi);
        height = std::max(height, Ui::px(40, dpi));

        int x, y;
        if (hasManualPosition()) {
            x = g_savedX;
            y = g_savedY;
        } else {
            const int gap = Ui::px(kRegionGap, dpi);
            x = anchor.left;
            y = anchor.bottom + gap; // sotto la zona letta
            if (y + height > mon.bottom) y = anchor.top - height - gap; // o sopra
            if (y < mon.top) {
                // Né sopra né sotto: di fianco, mai sopra la zona.
                y = anchor.top;
                x = anchor.right + gap;
                if (x + width > mon.right) x = anchor.left - width - gap;
            }
            x = std::clamp(x, (int)mon.left, std::max((int)mon.left, (int)mon.right - width));
            y = std::clamp(y, (int)mon.top, std::max((int)mon.top, (int)mon.bottom - height));
        }
        // Mentre lo trascini (e arriva un messaggio) cambia la misura, non la posizione.
        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | (g_dragging ? SWP_NOMOVE : 0));
        updateCaptureVisibility(); // anche se si è spostata solo la zona
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    void setLocked(bool locked) {
        g_locked = locked;
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (locked) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        else ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }

    // Il riquadro è stato lasciato in un punto nuovo: da lì in poi la posizione è quella.
    void saveMovedPosition() {
        if (!g_moved) return;
        g_moved = false;
        RECT r;
        if (!GetWindowRect(g_hwnd, &r)) return;
        g_savedX = r.left;
        g_savedY = r.top;
        if (g_onMoved) g_onMoved(r.left, r.top);
    }

    void relock() {
        KillTimer(g_hwnd, kUnlockTimer);
        setLocked(true);
        saveMovedPosition();
        if (!g_shown) ShowWindow(g_hwnd, SW_HIDE);
        else layout();
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        const UINT dpi = Ui::dpiOf(hwnd);
        {
            Ui::Buffered buffer(hdc, r);
            HDC dc = buffer.dc();
            FillRect(dc, &r, Ui::brush(Ui::Color::Nav));
            RECT b = r;
            const int borderWidth = g_locked ? 1 : Ui::px(2, dpi);
            for (int i = 0; i < borderWidth; ++i) {
                FrameRect(dc, &b, Ui::brush(g_locked ? Ui::Color::PanelBorder : Ui::Color::Accent));
                InflateRect(&b, -1, -1);
            }

            const int pad = Ui::px(kPadding, dpi);
            const int inner = (r.right - r.left) - 2 * pad;
            HFONT bold = Ui::font(Ui::Font::Bold, dpi), normal = Ui::font(Ui::Font::Normal, dpi);
            int y = pad;
            std::vector<OcrOverlay::Line> lines = g_lines;
            if (!g_locked) {
                lines.insert(lines.begin(), { L"", L"Trascina per spostare: si blocca tra " + std::to_wstring(g_secondsLeft) + L" s", true });
            }
            for (const auto& line : lines) {
                Measured m = measureLine(dc, line, inner, dpi);
                const COLORREF textColor = line.pending ? Ui::Color::TextDim : Ui::Color::Text;
                if (m.prefixOwnLine) {
                    RECT pr{ pad, y, pad + inner, r.bottom };
                    Ui::drawText(dc, line.prefix, pr, bold, Ui::Color::Accent, DT_LEFT | DT_WORDBREAK);
                    int ph = Ui::textHeight(dc, line.prefix, inner, bold);
                    RECT tr{ pad, y + ph, pad + inner, r.bottom };
                    Ui::drawText(dc, line.text, tr, normal, textColor, DT_LEFT | DT_WORDBREAK);
                } else {
                    if (!line.prefix.empty()) {
                        RECT pr{ pad, y, pad + m.prefixWidth + 1, r.bottom };
                        Ui::drawText(dc, line.prefix, pr, bold, Ui::Color::Accent, DT_LEFT | DT_SINGLELINE);
                    }
                    RECT tr{ pad + m.prefixWidth, y, pad + inner, r.bottom };
                    Ui::drawText(dc, line.text, tr, normal, textColor, DT_LEFT | DT_WORDBREAK);
                }
                y += m.height + Ui::px(kLineGap, dpi);
            }
        }
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_NCHITTEST:
            if (!g_locked) return HTCAPTION;
            break;
        case WM_ENTERSIZEMOVE:
            g_dragging = true;
            break;
        case WM_MOVING:
            g_moved = true;
            break;
        case WM_EXITSIZEMOVE:
            g_dragging = false;
            saveMovedPosition();
            if (!g_locked && g_secondsLeft <= 0) relock(); // il tempo è finito mentre lo trascinavi
            else layout();
            break;
        case WM_WINDOWPOSCHANGED: // anche mentre lo trascini
            updateCaptureVisibility();
            break;
        case WM_TIMER:
            if (w == kUnlockTimer) {
                if (g_secondsLeft > 0) --g_secondsLeft;
                // Non si blocca a metà di un trascinamento: aspetta che lo lasci.
                if (g_secondsLeft <= 0 && !g_dragging) relock();
                else InvalidateRect(hwnd, nullptr, FALSE);
            } else if (w == kTopmostTimer && IsWindowVisible(hwnd)) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            return 0;
        case WM_DPICHANGED:
            // Le misure le rifà layout() (al prossimo aggiornamento o allo sblocco).
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }
}

namespace OcrOverlay {

void create(HINSTANCE instance) {
    if (g_hwnd) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kClassName,
                             L"Voice to Chat - traduzione schermo", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return;
    SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)g_alpha, LWA_ALPHA);
    SetTimer(g_hwnd, kTopmostTimer, 2000, nullptr);
}

void destroy() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}

void show(const RECT& region, const std::vector<Line>& lines) {
    if (!g_hwnd) return;
    const bool sameRegion = g_hasRegion && EqualRect(&g_region, &region);
    bool same = sameRegion && g_shown && lines.size() == g_lines.size();
    for (size_t i = 0; same && i < lines.size(); ++i) {
        same = lines[i].prefix == g_lines[i].prefix && lines[i].text == g_lines[i].text && lines[i].pending == g_lines[i].pending;
    }
    if (same) return;
    g_region = region;
    g_hasRegion = true;
    g_lines = lines;
    g_shown = true;
    layout();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
}

void hide() {
    g_shown = false;
    g_lines.clear();
    if (g_hwnd && g_locked) ShowWindow(g_hwnd, SW_HIDE);
}

void setOpacity(int alpha) {
    g_alpha = alpha;
    if (g_hwnd) SetLayeredWindowAttributes(g_hwnd, 0, (BYTE)alpha, LWA_ALPHA);
}

void setPosition(int x, int y) {
    if (x == g_savedX && y == g_savedY) return;
    g_savedX = x;
    g_savedY = y;
    if (g_hwnd && IsWindowVisible(g_hwnd)) layout();
}

void unlockForMove(int seconds) {
    if (!g_hwnd) return;
    if (!g_shown) {
        // Niente da mostrare: un'anteprima, così c'è qualcosa da trascinare.
        g_lines = { { L"[ESEMPIO] Giocatore: ", L"qui comparirà la traduzione della chat", false } };
    }
    g_secondsLeft = seconds;
    g_moved = false;
    setLocked(false);
    layout();
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hwnd, kUnlockTimer, 1000, nullptr);
}

void setOnMoved(std::function<void(int, int)> onMoved) {
    g_onMoved = std::move(onMoved);
}

} // namespace OcrOverlay
