#include "mining_overlay.h"
#include "ui/ui.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

// Il riquadro è una finestra "layered" aggiornata con UpdateLayeredWindow:
// ogni pixel ha la sua trasparenza, così lo sfondo e le scritte hanno opacità
// separate (sfondo a 0 = solo le scritte, con un alone scuro per leggerle sul
// gioco). Le scritte si disegnano con GDI come prima, in bianco su nero in
// un'immagine a parte: quanto è chiaro un pixel dice quanto è coperto dal testo.
namespace {
    constexpr int kMinWidth = 260;
    constexpr int kMaxWidth = 520;
    constexpr int kPadding = 12;
    constexpr int kLineGap = 3;
    constexpr int kRegionGap = 10;
    constexpr int kBorder = 3;          // spessore del contorno della zona
    constexpr int kUnlockedAlpha = 215; // sfondo minimo mentre si sposta: si vede cosa afferrare
    constexpr UINT_PTR kUnlockTimer = 1;
    constexpr UINT_PTR kTopmostTimer = 2;
    constexpr UINT_PTR kHideTimer = 3;
    constexpr const wchar_t* kClassName = L"IrisMiningOverlay";
    constexpr const wchar_t* kBorderClass = L"IrisMiningBorder";
    constexpr DWORD kExcludeFromCapture = 0x11; // WDA_EXCLUDEFROMCAPTURE

    HWND g_hwnd = nullptr;
    HWND g_border = nullptr;
    std::vector<MiningOverlay::Line> g_lines;
    RECT g_region{};
    bool g_hasRegion = false;
    bool g_shown = false;
    int g_backAlpha = 235;
    int g_textAlpha = 255;
    int g_savedX = -1, g_savedY = -1;
    bool g_locked = true;
    bool g_moved = false;
    bool g_dragging = false; // tra WM_ENTERSIZEMOVE e WM_EXITSIZEMOVE: la posizione la decide il mouse
    bool g_hiddenFromCapture = false;
    int g_secondsLeft = 0;
    std::function<void(int, int)> g_onMoved;

    // Un'immagine a 32 bit (dall'alto in basso) su cui disegnare con GDI.
    struct Dib {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ old = nullptr;
        uint32_t* bits = nullptr;
        int w = 0, h = 0;

        void free() {
            if (dc) {
                if (old) SelectObject(dc, old);
                DeleteDC(dc);
            }
            if (bitmap) DeleteObject(bitmap);
            *this = Dib{};
        }

        bool ensure(int width, int height) {
            if (bitmap && width == w && height == h) return true;
            free();
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            HDC screen = GetDC(nullptr);
            void* p = nullptr;
            bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
            dc = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!bitmap || !dc || !p) {
                free();
                return false;
            }
            bits = static_cast<uint32_t*>(p);
            old = SelectObject(dc, bitmap);
            w = width;
            h = height;
            return true;
        }
    };
    Dib g_image; // quello che va a schermo (alfa premoltiplicato)
    Dib g_text;  // le scritte in bianco su nero (la copertura)

    UINT dpiAt(const RECT& r) {
        UINT x = 96, y = 96;
        if (FAILED(GetDpiForMonitor(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &x, &y))) return 96;
        return x;
    }

    bool hasManualPosition() {
        return !(g_savedX == -1 && g_savedY == -1) && MonitorFromPoint(POINT{ g_savedX + 10, g_savedY + 10 }, MONITOR_DEFAULTTONULL);
    }

    HFONT fontFor(MiningOverlay::Size s, UINT dpi) {
        switch (s) {
        case MiningOverlay::Size::Small: return Ui::font(Ui::Font::Small, dpi);
        case MiningOverlay::Size::Big: return Ui::font(Ui::Font::Title, dpi);
        default: return Ui::font(Ui::Font::Bold, dpi);
        }
    }

    // Visibile nelle catture (OBS, Discord) tranne quando copre la zona letta:
    // lì la lettura successiva leggerebbe il riquadro stesso.
    void updateCaptureVisibility() {
        RECT r, overlap;
        if (!g_hwnd || !GetWindowRect(g_hwnd, &r)) return;
        const bool hide = g_hasRegion && IntersectRect(&overlap, &r, &g_region);
        if (hide == g_hiddenFromCapture) return;
        if (SetWindowDisplayAffinity(g_hwnd, hide ? kExcludeFromCapture : WDA_NONE)) g_hiddenFromCapture = hide;
    }

    std::vector<MiningOverlay::Line> visibleLines() {
        std::vector<MiningOverlay::Line> lines = g_lines;
        if (!g_locked) {
            lines.insert(lines.begin(), { L"Trascina per spostare: si blocca tra " + std::to_wstring(g_secondsLeft) + L" s",
                                          Ui::Color::Accent, MiningOverlay::Size::Small });
        }
        return lines;
    }

    // Il colore (r, g, b) con opacità a sopra dst (alfa premoltiplicato).
    inline uint32_t over(uint32_t dst, int r, int g, int b, int a) {
        if (a <= 0) return dst;
        const int inv = 255 - a;
        const int da = (int)(dst >> 24), dr = (int)((dst >> 16) & 0xFF), dg = (int)((dst >> 8) & 0xFF), db = (int)(dst & 0xFF);
        const int oa = a + (da * inv + 127) / 255;
        const int orr = (r * a + 127) / 255 + (dr * inv + 127) / 255;
        const int og = (g * a + 127) / 255 + (dg * inv + 127) / 255;
        const int ob = (b * a + 127) / 255 + (db * inv + 127) / 255;
        return ((uint32_t)std::min(oa, 255) << 24) | ((uint32_t)std::min(orr, 255) << 16) | ((uint32_t)std::min(og, 255) << 8) |
               (uint32_t)std::min(ob, 255);
    }

    // Disegna il riquadro (w × h) e lo manda a schermo; pos = nullptr lascia la finestra dov'è.
    void render(const POINT* pos, int w, int h, UINT dpi) {
        if (w <= 0 || h <= 0 || !g_image.ensure(w, h) || !g_text.ensure(w, h)) return;
        const RECT r{ 0, 0, w, h };
        const auto lines = visibleLines();
        const int pad = Ui::px(kPadding, dpi);
        const int inner = w - 2 * pad;

        // 1) Lo sfondo, con GDI (senza alfa), poi l'alfa scelta su tutti i pixel.
        HDC dc = g_image.dc;
        FillRect(dc, &r, Ui::brush(Ui::Color::Nav));
        RECT b = r;
        const int borderWidth = g_locked ? 1 : Ui::px(2, dpi);
        for (int i = 0; i < borderWidth; ++i) {
            FrameRect(dc, &b, Ui::brush(g_locked ? Ui::Color::PanelBorder : Ui::Color::Accent));
            InflateRect(&b, -1, -1);
        }
        // Una striscia colorata a sinistra: il colore della prima riga importante.
        COLORREF stripe = Ui::Color::Accent;
        for (const auto& l : g_lines) {
            if (l.size == MiningOverlay::Size::Big) {
                stripe = l.color;
                break;
            }
        }
        RECT s{ 1, 1, 1 + Ui::px(4, dpi), h - 1 };
        FillRect(dc, &s, Ui::brush(stripe));
        GdiFlush();
        // Sbloccato: sempre uno sfondo, altrimenti non c'è niente da afferrare
        // (i pixel del tutto trasparenti lasciano passare il mouse).
        const int backA = g_locked ? g_backAlpha : std::max(g_backAlpha, kUnlockedAlpha);
        const size_t count = (size_t)w * h;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t p = g_image.bits[i];
            const int pr = (int)((p >> 16) & 0xFF), pg = (int)((p >> 8) & 0xFF), pb = (int)(p & 0xFF);
            g_image.bits[i] = ((uint32_t)backA << 24) | ((uint32_t)((pr * backA + 127) / 255) << 16) |
                              ((uint32_t)((pg * backA + 127) / 255) << 8) | (uint32_t)((pb * backA + 127) / 255);
        }

        // 2) Le scritte in bianco su nero; per ogni riga di pixel, il colore della sua riga di testo.
        std::memset(g_text.bits, 0, count * 4);
        std::vector<COLORREF> rowColor((size_t)h, Ui::Color::Text);
        int y = pad;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) y += Ui::px(kLineGap, dpi);
            HFONT f = fontFor(lines[i].size, dpi);
            const int lh = Ui::textHeight(g_text.dc, lines[i].text, inner, f);
            RECT tr{ pad, y, pad + inner, y + lh };
            Ui::drawText(g_text.dc, lines[i].text, tr, f, RGB(255, 255, 255), DT_LEFT | DT_WORDBREAK);
            // L'alone sborda di qualche pixel: metà dello spazio tra le righe a testa.
            const int from = std::max(0, y - (i ? Ui::px(kLineGap, dpi) / 2 : y));
            const int to = (i + 1 == lines.size()) ? h : std::min(h, y + lh + Ui::px(kLineGap, dpi) / 2);
            for (int row = from; row < to; ++row) rowColor[(size_t)row] = lines[i].color;
            y += lh;
        }
        GdiFlush();
        // Copertura: la media dei tre canali (il ClearType li tinge in modo diverso ai bordi).
        std::vector<uint8_t> cover(count);
        for (size_t i = 0; i < count; ++i) {
            const uint32_t p = g_text.bits[i];
            cover[i] = (uint8_t)((((p >> 16) & 0xFF) + ((p >> 8) & 0xFF) + (p & 0xFF)) / 3);
        }

        // 3) L'alone scuro attorno alle lettere: tanto più forte quanto meno sfondo c'è.
        const int haloStrength = (255 - backA) * 9 / 10; // 0..229
        std::vector<uint8_t> halo;
        if (haloStrength > 0) {
            const int radius = std::max(1, Ui::px(1, dpi));
            std::vector<uint8_t> tmp(count);
            halo.assign(count, 0);
            for (int yy = 0; yy < h; ++yy) { // massimo in orizzontale...
                for (int xx = 0; xx < w; ++xx) {
                    uint8_t m = 0;
                    for (int k = std::max(0, xx - radius); k <= std::min(w - 1, xx + radius); ++k) m = std::max(m, cover[(size_t)yy * w + k]);
                    tmp[(size_t)yy * w + xx] = m;
                }
            }
            for (int yy = 0; yy < h; ++yy) { // ...e in verticale
                for (int xx = 0; xx < w; ++xx) {
                    uint8_t m = 0;
                    for (int k = std::max(0, yy - radius); k <= std::min(h - 1, yy + radius); ++k) m = std::max(m, tmp[(size_t)k * w + xx]);
                    halo[(size_t)yy * w + xx] = m;
                }
            }
        }

        // 4) Tutto insieme: sfondo, alone, scritte (con la loro opacità).
        for (int yy = 0; yy < h; ++yy) {
            const COLORREF c = rowColor[(size_t)yy];
            const int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
            for (int xx = 0; xx < w; ++xx) {
                const size_t i = (size_t)yy * w + xx;
                uint32_t p = g_image.bits[i];
                if (!halo.empty() && halo[i]) p = over(p, 0, 0, 0, halo[i] * haloStrength / 255 * g_textAlpha / 255);
                if (cover[i]) p = over(p, cr, cg, cb, cover[i] * g_textAlpha / 255);
                g_image.bits[i] = p;
            }
        }

        POINT src{ 0, 0 };
        SIZE size{ w, h };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_hwnd, nullptr, const_cast<POINT*>(pos), &size, g_image.dc, &src, 0, &blend, ULW_ALPHA);
    }

    void layout() {
        if (!g_hwnd) return;
        RECT anchor = g_hasRegion ? g_region : RECT{};
        if (!g_hasRegion) {
            MONITORINFO pm{ sizeof(pm) };
            GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &pm);
            const int cx = (pm.rcMonitor.left + pm.rcMonitor.right) / 2;
            anchor = RECT{ cx - 150, pm.rcMonitor.top + 60, cx + 150, pm.rcMonitor.top + 61 };
        }
        if (hasManualPosition()) anchor = RECT{ g_savedX, g_savedY, g_savedX + 300, g_savedY + 10 };
        // Mentre lo trascini conta dov'è adesso (scala del monitor compresa), non dove era.
        RECT current{};
        if (g_dragging && GetWindowRect(g_hwnd, &current)) anchor = current;
        const UINT dpi = dpiAt(anchor);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT mon = mi.rcMonitor;

        const std::vector<MiningOverlay::Line> lines = visibleLines();
        const int pad = Ui::px(kPadding, dpi);
        HDC hdc = GetDC(g_hwnd);
        int widest = 0;
        for (const auto& l : lines) {
            HGDIOBJ old = SelectObject(hdc, fontFor(l.size, dpi));
            SIZE sz{};
            GetTextExtentPoint32W(hdc, l.text.c_str(), (int)l.text.size(), &sz);
            SelectObject(hdc, old);
            widest = std::max(widest, (int)sz.cx);
        }
        // Solo righe grandi (informazioni base): il riquadro sta stretto attorno al testo.
        const bool onlyBig = std::all_of(lines.begin(), lines.end(), [](const auto& l) { return l.size == MiningOverlay::Size::Big; });
        const int minWidth = onlyBig ? 0 : Ui::px(kMinWidth, dpi);
        const int width = std::clamp(widest + 2 * pad + Ui::px(4, dpi), minWidth, Ui::px(kMaxWidth, dpi));
        const int inner = width - 2 * pad;
        int height = 2 * pad;
        for (size_t i = 0; i < lines.size(); ++i) {
            height += Ui::textHeight(hdc, lines[i].text, inner, fontFor(lines[i].size, dpi)) + (i ? Ui::px(kLineGap, dpi) : 0);
        }
        ReleaseDC(g_hwnd, hdc);

        if (g_dragging) {
            // Lo sta spostando il mouse: si aggiorna il contenuto, la posizione no
            // (prima tornava accanto alla zona a ogni secondo del conto alla rovescia).
            render(nullptr, width, height, dpi);
            return;
        }
        int x, y;
        if (hasManualPosition()) {
            x = g_savedX;
            y = g_savedY;
        } else {
            const int gap = Ui::px(kRegionGap, dpi);
            x = (anchor.left + anchor.right) / 2 - width / 2; // centrato sulla zona
            y = anchor.bottom + gap;                           // sotto
            if (y + height > mon.bottom) y = anchor.top - height - gap; // o sopra
            if (y < mon.top) {
                // Né sopra né sotto: di fianco, mai sopra la zona.
                y = anchor.top;
                x = anchor.right + gap;
                if (x + width > mon.right) x = anchor.left - width - gap;
            }
        }
        x = std::clamp(x, (int)mon.left, std::max((int)mon.left, (int)mon.right - width));
        y = std::clamp(y, (int)mon.top, std::max((int)mon.top, (int)mon.bottom - height));
        const POINT pos{ x, y };
        render(&pos, width, height, dpi);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        updateCaptureVisibility();
    }

    void setLocked(bool locked) {
        g_locked = locked;
        LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
        if (locked) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        else ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
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
            else layout();                                  // nella scala del monitor dove l'hai lasciato
            break;
        case WM_WINDOWPOSCHANGED:
            updateCaptureVisibility();
            break;
        case WM_TIMER:
            if (w == kUnlockTimer) {
                if (g_secondsLeft > 0) --g_secondsLeft;
                // Non si blocca a metà di un trascinamento: aspetta che lo lasci.
                if (g_secondsLeft <= 0 && !g_dragging) relock();
                else layout();
            } else if (w == kTopmostTimer && IsWindowVisible(hwnd)) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            } else if (w == kHideTimer) {
                KillTimer(hwnd, kHideTimer);
                MiningOverlay::hide();
            }
            return 0;
        case WM_DPICHANGED:
            // Le misure le rifà layout(), alla fine del trascinamento.
            return 0;
        case WM_DESTROY:
            g_image.free();
            g_text.free();
            break;
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }

    LRESULT CALLBACK borderProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT r;
            GetClientRect(hwnd, &r);
            FillRect(hdc, &r, Ui::brush(Ui::Color::Accent));
            EndPaint(hwnd, &ps);
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, w, l);
    }
}

namespace MiningOverlay {

void create(HINSTANCE instance) {
    if (g_hwnd) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    RegisterClassW(&wc);
    // Niente SetLayeredWindowAttributes: con quella UpdateLayeredWindow non funzionerebbe più.
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kClassName,
                             L"Iris - mining", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, nullptr);
    if (g_hwnd) SetTimer(g_hwnd, kTopmostTimer, 2000, nullptr);

    WNDCLASSW bc{};
    bc.lpfnWndProc = borderProc;
    bc.hInstance = instance;
    bc.lpszClassName = kBorderClass;
    RegisterClassW(&bc);
    g_border = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kBorderClass,
                               L"Iris - zona mining", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, nullptr);
    if (g_border) {
        SetLayeredWindowAttributes(g_border, 0, 200, LWA_ALPHA);
        // Il contorno non deve MAI finire nella cattura della zona.
        SetWindowDisplayAffinity(g_border, kExcludeFromCapture);
    }
}

void destroy() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    if (g_border) DestroyWindow(g_border);
    g_hwnd = g_border = nullptr;
}

void show(const RECT& region, bool hasRegion, const std::vector<Line>& lines, int seconds) {
    if (!g_hwnd) return;
    g_region = region;
    g_hasRegion = hasRegion;
    g_lines = lines;
    g_shown = true;
    layout(); // prima il contenuto, poi si mostra: una finestra layered vuota non si vede
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    KillTimer(g_hwnd, kHideTimer);
    if (seconds > 0) SetTimer(g_hwnd, kHideTimer, (UINT)seconds * 1000, nullptr);
}

void hide() {
    g_shown = false;
    g_lines.clear();
    if (g_hwnd) {
        KillTimer(g_hwnd, kHideTimer);
        if (g_locked) ShowWindow(g_hwnd, SW_HIDE);
    }
}

void setOpacity(int backgroundAlpha, int textAlpha) {
    backgroundAlpha = std::clamp(backgroundAlpha, 0, 255);
    textAlpha = std::clamp(textAlpha, 0, 255);
    if (backgroundAlpha == g_backAlpha && textAlpha == g_textAlpha) return;
    g_backAlpha = backgroundAlpha;
    g_textAlpha = textAlpha;
    if (g_hwnd && IsWindowVisible(g_hwnd)) layout();
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
        // Un esempio, così c'è qualcosa da trascinare.
        g_lines = { { L"MINING · 16.960", Ui::Color::TextDim, Size::Small },
                    { L"Copper (Rame) × 4", Ui::Color::Ok, Size::Big },
                    { L"firma base 4.240", Ui::Color::TextDim, Size::Small } };
    }
    KillTimer(g_hwnd, kHideTimer);
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

void setBorder(const RECT& region, bool visible) {
    if (!g_border) return;
    if (!visible || region.right - region.left <= 0 || region.bottom - region.top <= 0) {
        ShowWindow(g_border, SW_HIDE);
        return;
    }
    // Una cornice di kBorder pixel tutta FUORI dalla zona: dentro la finestra c'è un buco.
    const int t = Ui::px(kBorder, dpiAt(region));
    RECT outer = region;
    InflateRect(&outer, t, t);
    const int w = outer.right - outer.left, h = outer.bottom - outer.top;
    HRGN frame = CreateRectRgn(0, 0, w, h);
    HRGN hole = CreateRectRgn(t, t, w - t, h - t);
    CombineRgn(frame, frame, hole, RGN_DIFF);
    DeleteObject(hole);
    SetWindowRgn(g_border, frame, TRUE); // la finestra ne diventa proprietaria
    SetWindowPos(g_border, HWND_TOPMOST, outer.left, outer.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_border, nullptr, TRUE);
}

} // namespace MiningOverlay
