#include "region_selector.h"
#include "ui/ui.h"

#include <windowsx.h>

#include <algorithm>
#include <string>

namespace {
    constexpr const wchar_t* kClassName = L"VoiceToChatRegionSelector";

    struct State {
        HDC shot = nullptr;      // lo schermo com'era
        HDC dimmed = nullptr;    // lo stesso, scurito
        HBITMAP shotBitmap = nullptr, dimmedBitmap = nullptr;
        HGDIOBJ shotOld = nullptr, dimmedOld = nullptr;
        int width = 0, height = 0;
        POINT start{}, current{};
        bool dragging = false;
        bool done = false;
        bool cancelled = false;
    };
    State* g_state = nullptr;

    RECT selection(const State& s) {
        return RECT{ std::min(s.start.x, s.current.x), std::min(s.start.y, s.current.y), std::max(s.start.x, s.current.x),
                     std::max(s.start.y, s.current.y) };
    }

    // Rettangolo arrotondato in GDI semplice: qui si disegna con l'origine
    // spostata (vedi paint), che GDI+ potrebbe non rispettare.
    void roundBox(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, Ui::brush(fill));
        RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        State& s = *g_state;
        const RECT& p = ps.rcPaint;
        const int pw = p.right - p.left, ph = p.bottom - p.top;
        if (pw <= 0 || ph <= 0) {
            EndPaint(hwnd, &ps);
            return;
        }
        // Doppio buffer grande solo quanto la parte da ridisegnare (la finestra copre tutti i monitor).
        HDC dc = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, pw, ph);
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        SetViewportOrgEx(dc, -p.left, -p.top, nullptr);
        BitBlt(dc, p.left, p.top, pw, ph, s.dimmed, p.left, p.top, SRCCOPY);
        const UINT dpi = Ui::dpiOf(hwnd);

        if (s.dragging || s.done) {
            RECT r = selection(s);
            BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, s.shot, r.left, r.top, SRCCOPY);
            RECT frame = r;
            InflateRect(&frame, 2, 2);
            for (int i = 0; i < 2; ++i) {
                FrameRect(dc, &frame, Ui::brush(Ui::Color::Accent));
                InflateRect(&frame, -1, -1);
            }
            std::wstring size = std::to_wstring(r.right - r.left) + L" × " + std::to_wstring(r.bottom - r.top);
            RECT label{ r.left, r.bottom + Ui::px(6, dpi), r.left + Ui::px(160, dpi), r.bottom + Ui::px(28, dpi) };
            if (label.bottom > s.height) OffsetRect(&label, 0, -(r.bottom - r.top) - Ui::px(36, dpi));
            roundBox(dc, label, Ui::px(4, dpi), Ui::Color::Nav, Ui::Color::Nav);
            Ui::drawText(dc, size, label, Ui::font(Ui::Font::Bold, dpi), Ui::Color::Text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Istruzioni in alto, sul monitor principale.
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        POINT origin{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
        const int bw = Ui::px(640, dpi), bh = Ui::px(64, dpi);
        const int bx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2 - bw / 2 - origin.x;
        const int by = mi.rcMonitor.top + Ui::px(28, dpi) - origin.y;
        RECT banner{ bx, by, bx + bw, by + bh };
        RECT dummy;
        if (IntersectRect(&dummy, &banner, &p)) {
            roundBox(dc, banner, Ui::px(10, dpi), Ui::Color::Nav, Ui::Color::Accent);
            RECT t1{ banner.left, banner.top + Ui::px(10, dpi), banner.right, banner.top + Ui::px(34, dpi) };
            Ui::drawText(dc, L"Trascina un rettangolo intorno alla chat del gioco", t1, Ui::font(Ui::Font::Bold, dpi), Ui::Color::Text,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT t2{ banner.left, banner.top + Ui::px(34, dpi), banner.right, banner.bottom - Ui::px(8, dpi) };
            Ui::drawText(dc, L"Comprendi i nomi dei giocatori. Esc o tasto destro per annullare.", t2, Ui::font(Ui::Font::Small, dpi),
                         Ui::Color::TextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SetViewportOrgEx(dc, 0, 0, nullptr);
        BitBlt(hdc, p.left, p.top, pw, ph, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
    }

    void invalidateSelection(HWND hwnd, const RECT& before, const RECT& after) {
        // Solo la parte che cambia (più l'etichetta con le misure): niente sfarfallio.
        RECT u;
        UnionRect(&u, &before, &after);
        const int margin = Ui::px(48, Ui::dpiOf(hwnd));
        InflateRect(&u, margin, margin);
        u.right += Ui::px(160, Ui::dpiOf(hwnd));
        InvalidateRect(hwnd, &u, FALSE);
    }

    LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        State* s = g_state;
        switch (msg) {
        case WM_LBUTTONDOWN:
            s->start = s->current = POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            s->dragging = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (s->dragging) {
                RECT before = selection(*s);
                s->current = POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                invalidateSelection(hwnd, before, selection(*s));
            }
            return 0;
        case WM_LBUTTONUP:
            if (s->dragging) {
                s->current = POINT{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                s->dragging = false;
                ReleaseCapture();
                s->done = true;
            }
            return 0;
        case WM_RBUTTONUP:
            s->cancelled = true;
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) s->cancelled = true;
            return 0;
        case WM_CAPTURECHANGED:
            if (s->dragging && (HWND)l != hwnd) s->cancelled = true;
            return 0;
        case WM_ACTIVATE:
            // Alt+Tab via (o un'altra finestra in primo piano): si annulla.
            if (LOWORD(w) == WA_INACTIVE && !s->done) s->cancelled = true;
            return 0;
        case WM_DPICHANGED:
            return 0; // copre tutti i monitor: niente ridimensionamenti
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }
}

namespace RegionSelector {

bool selectRegion(HINSTANCE instance, RECT& out) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    State state;
    state.width = vw;
    state.height = vh;
    HDC screen = GetDC(nullptr);
    state.shot = CreateCompatibleDC(screen);
    state.dimmed = CreateCompatibleDC(screen);
    state.shotBitmap = CreateCompatibleBitmap(screen, vw, vh);
    state.dimmedBitmap = CreateCompatibleBitmap(screen, vw, vh);
    state.shotOld = SelectObject(state.shot, state.shotBitmap);
    state.dimmedOld = SelectObject(state.dimmed, state.dimmedBitmap);
    BitBlt(state.shot, 0, 0, vw, vh, screen, vx, vy, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    // Copia scurita: la foto al 40% di luminosità.
    BitBlt(state.dimmed, 0, 0, vw, vh, state.shot, 0, 0, SRCCOPY);
    {
        HDC black = CreateCompatibleDC(state.dimmed);
        HBITMAP px = CreateCompatibleBitmap(state.dimmed, 1, 1);
        HGDIOBJ old = SelectObject(black, px);
        SetPixel(black, 0, 0, RGB(0, 0, 0));
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 153, 0 };
        GdiAlphaBlend(state.dimmed, 0, 0, vw, vh, black, 0, 0, 1, 1, blend);
        SelectObject(black, old);
        DeleteObject(px);
        DeleteDC(black);
    }
    g_state = &state;

    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"Scegli la zona", WS_POPUP, vx, vy, vw, vh, nullptr,
                                nullptr, instance, nullptr);
    bool ok = false;
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        // Ciclo dei messaggi "annidato": finisce con la scelta o l'annullamento.
        // Un WM_QUIT (chiusura del programma) si rimette in coda per il ciclo principale.
        MSG msg;
        while (!state.done && !state.cancelled) {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r == 0) {
                PostQuitMessage((int)msg.wParam);
                state.cancelled = true;
                break;
            }
            if (r < 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (state.done && !state.cancelled) {
            RECT r = selection(state);
            out = RECT{ vx + r.left, vy + r.top, vx + r.right, vy + r.bottom };
            ok = (out.right - out.left) >= 40 && (out.bottom - out.top) >= 20;
        }
        DestroyWindow(hwnd);
    }
    UnregisterClassW(kClassName, instance);
    g_state = nullptr;

    SelectObject(state.shot, state.shotOld);
    SelectObject(state.dimmed, state.dimmedOld);
    DeleteObject(state.shotBitmap);
    DeleteObject(state.dimmedBitmap);
    DeleteDC(state.shot);
    DeleteDC(state.dimmed);
    return ok;
}

} // namespace RegionSelector
