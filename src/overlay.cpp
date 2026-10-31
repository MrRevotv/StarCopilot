#include "overlay.h"
#include "ui/ui.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

// La finestra è "layered" con UpdateLayeredWindow: ogni fotogramma si disegna
// in un'immagine a 32 bit con il canale alfa (premoltiplicato) e Windows la
// compone sopra il resto. Dove l'immagine è trasparente si vede quello che
// c'è dietro (e i clic passano).

namespace {
    constexpr UINT_PTR kUnlockTimer = 1;
    constexpr UINT_PTR kTopmostTimer = 2;
    constexpr const wchar_t* kClassName = L"IrisOrbOverlay";
    constexpr float kRadius = 0.21f; // nucleo rispetto al lato: l'alone (circa 2,2 volte) resta dentro la finestra

    HWND g_hwnd = nullptr;
    bool g_visible = false;
    int g_alpha = 230;
    int g_size = 160;
    int g_savedX = -1, g_savedY = -1;
    bool g_locked = true;
    bool g_moved = false;
    int g_secondsLeft = 0;
    std::function<void(int, int)> g_onMoved;
    Orb::Animation g_anim;

    // L'immagine su cui si disegna (DIB a 32 bit, dall'alto in basso).
    HDC g_memDc = nullptr;
    HBITMAP g_dib = nullptr;
    HGDIOBJ g_oldBitmap = nullptr;
    void* g_bits = nullptr;
    int g_dibW = 0, g_dibH = 0;

    int sidePx(UINT dpi) { return Ui::px(g_size, dpi); }

    void freeDib() {
        if (g_memDc) {
            if (g_oldBitmap) SelectObject(g_memDc, g_oldBitmap);
            DeleteDC(g_memDc);
        }
        if (g_dib) DeleteObject(g_dib);
        g_memDc = nullptr;
        g_dib = nullptr;
        g_oldBitmap = nullptr;
        g_bits = nullptr;
        g_dibW = g_dibH = 0;
    }

    bool ensureDib(int w, int h) {
        if (g_dib && w == g_dibW && h == g_dibH) return true;
        freeDib();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // dall'alto in basso, come lo vuole GDI+ con passo positivo
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        g_dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
        g_memDc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!g_dib || !g_memDc || !g_bits) {
            freeDib();
            return false;
        }
        g_oldBitmap = SelectObject(g_memDc, g_dib);
        g_dibW = w;
        g_dibH = h;
        return true;
    }

    // Un fotogramma: la sfera (e, se sbloccata, un fondo per afferrarla e il conto alla rovescia).
    void render() {
        if (!g_hwnd || !IsWindowVisible(g_hwnd)) return;
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        if (w <= 0 || h <= 0 || !ensureDib(w, h)) return;
        std::memset(g_bits, 0, (size_t)w * h * 4);
        {
            Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(g_bits));
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
            if (!g_locked) {
                // Sbloccata: un disco scuro da afferrare, con il bordo e il tempo che resta.
                const float m = 2.0f;
                Gdiplus::SolidBrush back(Gdiplus::Color(150, 14, 18, 26));
                g.FillEllipse(&back, m, m, w - 2 * m, h - 2 * m);
                Gdiplus::Pen border(Gdiplus::Color(230, 30, 170, 220), 2.0f);
                border.SetDashStyle(Gdiplus::DashStyleDash);
                g.DrawEllipse(&border, m, m, w - 2 * m, h - 2 * m);
            }
            Orb::draw(g, (float)w, (float)h, g_anim, kRadius);
            if (!g_locked) {
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                Gdiplus::FontFamily family(L"Segoe UI");
                Gdiplus::Font font(&family, std::max(9.0f, h * 0.075f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::StringFormat fmt;
                fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
                fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::SolidBrush text(Gdiplus::Color(255, 230, 240, 250));
                const std::wstring s = L"Trascina · " + std::to_wstring(g_secondsLeft) + L" s";
                Gdiplus::RectF box(0.0f, h * 0.78f, (float)w, h * 0.14f);
                g.DrawString(s.c_str(), -1, &font, box, &fmt, &text);
            }
        }
        POINT src{ 0, 0 };
        SIZE size{ w, h };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, (BYTE)g_alpha, AC_SRC_ALPHA };
        // Posizione invariata (nullptr): così non si litiga con il trascinamento.
        UpdateLayeredWindow(g_hwnd, nullptr, nullptr, &size, g_memDc, &src, 0, &blend, ULW_ALPHA);
    }

    void place() {
        if (!g_hwnd) return;
        UINT dpi = Ui::dpiOf(g_hwnd);
        int side = sidePx(dpi);
        POINT pos{ g_savedX, g_savedY };
        // Posizione salvata su un monitor che non c'è più (o nessuna): angolo in basso a destra.
        if ((g_savedX == -1 && g_savedY == -1) || !MonitorFromPoint(POINT{ g_savedX + side / 2, g_savedY + side / 2 }, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
            pos.x = mi.rcWork.right - side - Ui::px(12, dpi);
            pos.y = mi.rcWork.bottom - side - Ui::px(12, dpi);
        }
        SetWindowPos(g_hwnd, HWND_TOPMOST, pos.x, pos.y, side, side, SWP_NOACTIVATE);
        // Sul monitor di arrivo la scala può essere diversa: si rifanno le misure.
        UINT newDpi = Ui::dpiOf(g_hwnd);
        if (newDpi != dpi) SetWindowPos(g_hwnd, nullptr, 0, 0, sidePx(newDpi), sidePx(newDpi), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        render();
    }

    void setLocked(bool locked) {
        g_locked = locked;
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (locked) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        else ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
        render();
    }

    void relock() {
        KillTimer(g_hwnd, kUnlockTimer);
        setLocked(true);
        if (!g_visible) ShowWindow(g_hwnd, SW_HIDE);
        if (g_moved) {
            g_moved = false;
            RECT r;
            GetWindowRect(g_hwnd, &r);
            g_savedX = r.left;
            g_savedY = r.top;
            if (g_onMoved) g_onMoved(r.left, r.top);
        }
    }

    LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_NCHITTEST:
            if (!g_locked) return HTCAPTION;
            break;
        case WM_MOVING:
            g_moved = true;
            break;
        case WM_TIMER:
            if (w == kUnlockTimer) {
                if (--g_secondsLeft <= 0) relock();
                else render();
            } else if (w == kTopmostTimer && g_visible) {
                // Alcuni giochi si mettono "sempre in primo piano" anche loro: si torna sopra.
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(l);
            UINT dpi = HIWORD(w);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, sidePx(dpi), sidePx(dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            render();
            return 0;
        }
        case WM_DESTROY:
            freeDib();
            break;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }
}

namespace Overlay {

void create(HINSTANCE instance) {
    if (g_hwnd) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kClassName,
                             L"Iris - sfera", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) return;
    // Fuori dalle catture dello schermo (Windows 10 2004 e successivi).
    SetWindowDisplayAffinity(g_hwnd, 0x11 /* WDA_EXCLUDEFROMCAPTURE */);
    place();
    SetTimer(g_hwnd, kTopmostTimer, 2000, nullptr);
}

void destroy() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}

void setVisible(bool visible) {
    g_visible = visible;
    if (!g_hwnd) return;
    if (visible) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        render();
    } else if (g_locked) {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

void animate(Orb::Mode mode, float micLevel, float voiceLevel, bool userSpeaking) {
    Orb::step(g_anim, mode, micLevel, voiceLevel, userSpeaking);
    render();
}

void setOpacity(int alpha) {
    g_alpha = std::clamp(alpha, 60, 255);
    render();
}

void setSize(int size) {
    size = std::clamp(size, 60, 600);
    if (size == g_size) return;
    // Si allarga (o stringe) attorno al centro, non dall'angolo.
    if (g_hwnd) {
        RECT r;
        GetWindowRect(g_hwnd, &r);
        const UINT dpi = Ui::dpiOf(g_hwnd);
        const int oldSide = r.right - r.left, newSide = Ui::px(size, dpi);
        g_size = size;
        if (!(g_savedX == -1 && g_savedY == -1)) {
            g_savedX = r.left + (oldSide - newSide) / 2;
            g_savedY = r.top + (oldSide - newSide) / 2;
            if (g_onMoved) g_onMoved(g_savedX, g_savedY);
        }
        place();
        return;
    }
    g_size = size;
}

void setPosition(int x, int y) {
    if (x == g_savedX && y == g_savedY) return;
    g_savedX = x;
    g_savedY = y;
    place();
}

void unlockForMove(int seconds) {
    if (!g_hwnd) return;
    g_secondsLeft = seconds;
    g_moved = false;
    setLocked(false);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_hwnd, kUnlockTimer, 1000, nullptr);
    render();
}

void setOnMoved(std::function<void(int, int)> onMoved) {
    g_onMoved = std::move(onMoved);
}

} // namespace Overlay
