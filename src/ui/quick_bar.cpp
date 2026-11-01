#include "quick_bar.h"

#include <objidl.h>
#include <gdiplus.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace {
    using namespace Gdiplus;

    constexpr const wchar_t* kBarClass = L"IrisQuickBar";
    constexpr const wchar_t* kPanelClass = L"IrisHudPanel";
    constexpr DWORD kExcludeFromCapture = 0x11; // WDA_EXCLUDEFROMCAPTURE
    constexpr UINT_PTR kAnimTimer = 1;
    constexpr UINT_PTR kTopmostTimer = 2;
    constexpr int kBarHeight = 52;   // a 96 DPI
    constexpr int kShadow = 10;      // margine per l'ombra
    constexpr int kTopGap = 8;       // distanza dal bordo alto dello schermo
    constexpr int kCut = 9;          // angoli tagliati
    constexpr int kPanelWidth = 344;
    constexpr int kPanelGap = 14;
    constexpr int kAnimMs = 190;

    // I colori dell'interfaccia (app.css).
    const Color kText(255, 214, 236, 243), kText2(255, 143, 169, 182), kText3(255, 87, 112, 126);
    const Color kCyan(255, 63, 216, 245), kGreen(255, 70, 227, 160), kAmber(255, 246, 183, 64), kRed(255, 255, 93, 108);

    Color withAlpha(const Color& c, int a) { return Color((BYTE)std::clamp(a, 0, 255), c.GetR(), c.GetG(), c.GetB()); }

    Color toneColor(int tone) {
        switch (tone) {
        case 1: return kGreen;
        case 2: return kAmber;
        case 3: return kRed;
        case 4: return kText3;
        case 5: return kText3;
        case 6: return kCyan;
        default: return kText;
        }
    }

    // Un'immagine a 32 bit premoltiplicata, quella che va a schermo con UpdateLayeredWindow.
    struct Surface {
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ old = nullptr;
        void* bits = nullptr;
        int w = 0, h = 0;

        void free() {
            if (dc) {
                if (old) SelectObject(dc, old);
                DeleteDC(dc);
            }
            if (bitmap) DeleteObject(bitmap);
            *this = Surface{};
        }
        bool ensure(int width, int height) {
            if (bitmap && w == width && h == height) return true;
            free();
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            HDC screen = GetDC(nullptr);
            bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            dc = CreateCompatibleDC(screen);
            ReleaseDC(nullptr, screen);
            if (!bitmap || !dc || !bits) {
                free();
                return false;
            }
            old = SelectObject(dc, bitmap);
            w = width;
            h = height;
            return true;
        }
        void push(HWND hwnd, POINT pos) {
            POINT src{ 0, 0 };
            SIZE size{ w, h };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, nullptr, &pos, &size, dc, &src, 0, &blend, ULW_ALPHA);
        }
    };

    // I caratteri (creati con GDI+ acceso, distrutti prima che si spenga: vedi destroy()).
    struct Fonts {
        std::unique_ptr<FontFamily> display, body, mono, symbol;
    };
    Fonts* g_fonts = nullptr;

    const FontFamily* family(std::unique_ptr<FontFamily>& f, const wchar_t* name, const wchar_t* fallback) {
        if (!f) {
            f = std::make_unique<FontFamily>(name);
            if (!f->IsAvailable()) f = std::make_unique<FontFamily>(fallback);
        }
        return f.get();
    }
    const FontFamily* displayFamily() { return family(g_fonts->display, L"Bahnschrift", L"Segoe UI"); }
    const FontFamily* bodyFamily() { return family(g_fonts->body, L"Segoe UI", L"Arial"); }
    const FontFamily* monoFamily() { return family(g_fonts->mono, L"Cascadia Mono", L"Consolas"); }
    const FontFamily* symbolFamily() { return family(g_fonts->symbol, L"Segoe UI Symbol", L"Segoe UI"); }

    const StringFormat* typographic() {
        static StringFormat* f = [] {
            auto* x = new StringFormat(StringFormat::GenericTypographic());
            x->SetFormatFlags(x->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
            return x;
        }();
        return f;
    }

    // Testo con spaziatura tra le lettere (come le maiuscole HUD della pagina).
    float trackedWidth(Graphics& g, const std::wstring& text, const Font& font, float tracking) {
        float w = 0;
        for (wchar_t c : text) {
            RectF box;
            const wchar_t s[2] = { c, 0 };
            g.MeasureString(s, 1, &font, PointF(0, 0), typographic(), &box);
            w += box.Width + tracking;
        }
        return text.empty() ? 0 : w - tracking;
    }
    void drawTracked(Graphics& g, const std::wstring& text, const Font& font, float x, float y, float tracking, const Color& color) {
        SolidBrush brush(color);
        for (wchar_t c : text) {
            const wchar_t s[2] = { c, 0 };
            RectF box;
            g.MeasureString(s, 1, &font, PointF(0, 0), typographic(), &box);
            g.DrawString(s, 1, &font, PointF(x, y), typographic(), &brush);
            x += box.Width + tracking;
        }
    }
    float textWidth(Graphics& g, const std::wstring& text, const Font& font) {
        RectF box;
        g.MeasureString(text.c_str(), (INT)text.size(), &font, PointF(0, 0), typographic(), &box);
        return box.Width;
    }
    // Testo su una riga, tagliato con "…" se non ci sta.
    void drawClipped(Graphics& g, const std::wstring& text, const Font& font, RectF box, const Color& color, StringAlignment align = StringAlignmentNear) {
        StringFormat f(StringFormat::GenericTypographic());
        f.SetFormatFlags(f.GetFormatFlags() | StringFormatFlagsNoWrap);
        f.SetTrimming(StringTrimmingEllipsisCharacter);
        f.SetAlignment(align);
        f.SetLineAlignment(StringAlignmentCenter);
        SolidBrush brush(color);
        g.DrawString(text.c_str(), (INT)text.size(), &font, box, &f, &brush);
    }

    // Il contorno a angoli tagliati (in alto a sinistra e in basso a destra, come i pannelli della pagina).
    void cutPath(GraphicsPath& p, float x, float y, float w, float h, float cut) {
        PointF pts[] = { { x + cut, y }, { x + w, y }, { x + w, y + h - cut }, { x + w - cut, y + h }, { x, y + h }, { x, y + cut } };
        p.AddPolygon(pts, 6);
    }

    void drawCard(Graphics& g, float x, float y, float w, float h, float s) {
        const float cut = kCut * s;
        // Ombra morbida attorno (a strati).
        for (int i = (int)(kShadow * s); i >= 1; --i) {
            GraphicsPath p;
            cutPath(p, x - i, y - i + 2 * s, w + 2 * i, h + 2 * i, cut + i);
            const float k = 1.0f - (float)i / (kShadow * s);
            SolidBrush b(Color((BYTE)(18 * k * k), 0, 0, 0));
            g.FillPath(&b, &p);
        }
        GraphicsPath card;
        cutPath(card, x, y, w, h, cut);
        LinearGradientBrush fill(PointF(0, y), PointF(0, y + h), Color(242, 13, 25, 37), Color(242, 6, 11, 18));
        g.FillPath(&fill, &card);
        Pen border(Color(95, 63, 216, 245), 1.0f);
        g.DrawPath(&border, &card);
        // I due angoli tagliati marcati in ciano, il filetto in basso.
        Pen accent(Color(200, 63, 216, 245), 1.4f * s);
        g.DrawLine(&accent, PointF(x, y + cut), PointF(x + cut, y));
        g.DrawLine(&accent, PointF(x + w - cut, y + h), PointF(x + w, y + h - cut));
        Pen hair(Color(120, 63, 216, 245), 1.0f);
        g.DrawLine(&hair, PointF(x + 16 * s, y + h - 0.5f), PointF(x + w - cut - 8 * s, y + h - 0.5f));
    }

    UINT dpiOfMonitor(HMONITOR m) {
        UINT x = 96, y = 96;
        if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y))) return 96;
        return x;
    }

    void keepOutOfCaptures(HWND hwnd) { SetWindowDisplayAffinity(hwnd, kExcludeFromCapture); }

    HINSTANCE g_instance = nullptr;
    QuickBar::Callbacks g_cb;

    // --- La barra -------------------------------------------------------------------------------------

    struct Hit {
        std::string id;
        RectF box;
        bool enabled = true;
    };

    HWND g_bar = nullptr;
    Surface g_barSurface;
    std::vector<QuickBar::Button> g_buttons;
    std::wstring g_keyHint, g_status;
    int g_statusTone = 0;
    std::vector<Hit> g_hits;
    int g_hover = -1, g_pressed = -1;
    bool g_shown = false;      // vista (o in arrivo)
    bool g_hiddenAll = false;
    bool g_barBeforeHide = false;
    float g_scale = 1;
    RECT g_monitorRect{};
    int g_barX = 0, g_barW = 0, g_barH = 0; // finestra (ombra compresa)
    ULONGLONG g_animStart = 0;
    bool g_animIn = true;

    int currentY(double t) {
        const int shown = g_monitorRect.top + (int)std::lround(kTopGap * g_scale) - (int)std::lround(kShadow * g_scale);
        const int hidden = g_monitorRect.top - g_barH;
        const double e = 1.0 - std::pow(1.0 - std::clamp(t, 0.0, 1.0), 3.0); // esce veloce, arriva piano
        return g_animIn ? (int)std::lround(hidden + (shown - hidden) * e) : (int)std::lround(shown + (hidden - shown) * e);
    }

    void layoutBar() {
        // Il monitor del gioco (la finestra in primo piano), se no il principale.
        HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        g_monitorRect = mi.rcMonitor;
        g_scale = dpiOfMonitor(mon) / 96.0f;
        const float s = g_scale;

        Bitmap probe(1, 1, PixelFormat32bppPARGB);
        Graphics g(&probe);
        Font brand(displayFamily(), 14 * s, FontStyleBold, UnitPixel);
        Font tiny(displayFamily(), 10 * s, FontStyleBold, UnitPixel);
        Font label(displayFamily(), 11.5f * s, FontStyleBold, UnitPixel);

        g_hits.clear();
        const float pad = (kShadow + 18) * s;
        float x = pad;
        x += 18 * s + 10 * s;                                          // marchio
        x += trackedWidth(g, L"IRIS", brand, 3.2f * s) + 14 * s;       // nome
        x += 14 * s + trackedWidth(g, g_status, tiny, 1.4f * s) + 16 * s; // stato
        x += 1 + 14 * s;                                               // separatore
        for (const auto& b : g_buttons) {
            const float w = trackedWidth(g, b.label, label, 1.6f * s) + 28 * s;
            g_hits.push_back({ b.id, RectF(x, (kShadow + 9) * s, w, (kBarHeight - 18) * s), b.enabled });
            x += w + 4 * s;
        }
        const float content = x + 220 * s /* suggerimento */ + 2 * 32 * s + pad;
        const int monitorW = g_monitorRect.right - g_monitorRect.left;
        g_barW = (int)std::lround(std::min<float>((float)monitorW - 24 * s, std::max({ content, 820 * s, monitorW * 0.5f })));
        g_barH = (int)std::lround((kBarHeight + 2 * kShadow) * s);
        g_barX = g_monitorRect.left + (monitorW - g_barW) / 2;
        // Impostazioni e chiudi, a destra.
        const float right = g_barW - pad;
        g_hits.push_back({ "settings", RectF(right - 2 * 32 * s - 4 * s, (kShadow + 11) * s, 32 * s, 30 * s), true });
        g_hits.push_back({ "close", RectF(right - 32 * s, (kShadow + 11) * s, 32 * s, 30 * s), true });
    }

    void paintBar() {
        if (!g_barSurface.ensure(g_barW, g_barH)) return;
        std::memset(g_barSurface.bits, 0, (size_t)g_barW * g_barH * 4);
        const float s = g_scale;
        Bitmap bmp(g_barW, g_barH, g_barW * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(g_barSurface.bits));
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        const float cx = kShadow * s, cy = kShadow * s, cw = g_barW - 2 * kShadow * s, ch = kBarHeight * s;
        drawCard(g, cx, cy, cw, ch, s);
        const float mid = cy + ch / 2;

        Font brand(displayFamily(), 14 * s, FontStyleBold, UnitPixel);
        Font tiny(displayFamily(), 10 * s, FontStyleBold, UnitPixel);
        Font label(displayFamily(), 11.5f * s, FontStyleBold, UnitPixel);
        Font hint(bodyFamily(), 11.5f * s, FontStyleRegular, UnitPixel);
        Font symbol(symbolFamily(), 15 * s, FontStyleRegular, UnitPixel);

        // Marchio: un rombo con il nucleo acceso, poi IRIS e lo stato.
        float x = (kShadow + 18) * s;
        {
            const float r = 9 * s, mx = x + r;
            PointF d[] = { { mx, mid - r }, { mx + r, mid }, { mx, mid + r }, { mx - r, mid } };
            SolidBrush f(Color(46, 63, 216, 245));
            g.FillPolygon(&f, d, 4);
            Pen p(kCyan, 1.4f * s);
            g.DrawPolygon(&p, d, 4);
            const float r2 = 3.2f * s;
            PointF c[] = { { mx, mid - r2 }, { mx + r2, mid }, { mx, mid + r2 }, { mx - r2, mid } };
            SolidBrush core(kCyan);
            g.FillPolygon(&core, c, 4);
            x += 2 * r + 10 * s;
        }
        drawTracked(g, L"IRIS", brand, x, mid - 9 * s, 3.2f * s, kText);
        x += trackedWidth(g, L"IRIS", brand, 3.2f * s) + 14 * s;
        {
            const Color c = toneColor(g_statusTone);
            SolidBrush glow(withAlpha(c, 60)), dot(c);
            g.FillEllipse(&glow, x - 2 * s, mid - 5 * s, 10 * s, 10 * s);
            g.FillEllipse(&dot, x, mid - 3 * s, 6 * s, 6 * s);
            x += 14 * s;
            drawTracked(g, g_status, tiny, x, mid - 6.5f * s, 1.4f * s, kText2);
            x += trackedWidth(g, g_status, tiny, 1.4f * s) + 16 * s;
        }
        Pen sep(Color(70, 63, 216, 245), 1.0f);
        g.DrawLine(&sep, PointF(x, mid - 12 * s), PointF(x, mid + 12 * s));

        // I pulsanti.
        for (size_t i = 0; i < g_hits.size(); ++i) {
            const Hit& h = g_hits[i];
            const bool hover = (int)i == g_hover && h.enabled, pressed = (int)i == g_pressed && hover;
            if (h.id == "settings" || h.id == "close") {
                if (hover) {
                    SolidBrush b(h.id == "close" ? Color(60, 255, 93, 108) : Color(36, 63, 216, 245));
                    g.FillEllipse(&b, h.box.X + 2 * s, h.box.Y, h.box.Height, h.box.Height);
                }
                const wchar_t* glyph = h.id == "close" ? L"✕" : L"⚙";
                RectF box(h.box.X + 2 * s, h.box.Y, h.box.Height, h.box.Height);
                drawClipped(g, glyph, symbol, box, hover ? (h.id == "close" ? Color(255, 255, 214, 218) : kText) : kText2, StringAlignmentCenter);
                continue;
            }
            const QuickBar::Button* b = nullptr;
            for (const auto& x2 : g_buttons) {
                if (x2.id == h.id) b = &x2;
            }
            if (!b) continue;
            if (b->on || hover) {
                GraphicsPath p;
                cutPath(p, h.box.X, h.box.Y, h.box.Width, h.box.Height, 5 * s);
                SolidBrush fill(b->on ? Color(pressed ? 70 : 44, 63, 216, 245) : Color(pressed ? 48 : 26, 63, 216, 245));
                g.FillPath(&fill, &p);
            }
            if (b->on) {
                SolidBrush line(kCyan);
                g.FillRectangle(&line, h.box.X + 6 * s, h.box.Y + h.box.Height - 2 * s, h.box.Width - 12 * s, 2 * s);
            }
            const Color c = !b->enabled ? withAlpha(kText3, 150) : b->on ? kCyan : hover ? Color(255, 255, 255, 255) : kText2;
            const float tw = trackedWidth(g, b->label, label, 1.6f * s);
            drawTracked(g, b->label, label, h.box.X + (h.box.Width - tw) / 2, mid - 7.5f * s, 1.6f * s, c);
        }

        // Il suggerimento: quello del pulsante sotto il mouse, se no i tasti.
        std::wstring text = g_keyHint;
        Color hintColor = kText3;
        if (g_hover >= 0 && g_hover < (int)g_hits.size()) {
            const Hit& h = g_hits[(size_t)g_hover];
            if (h.id == "settings") text = L"Apri la finestra di Iris";
            else if (h.id == "close") text = L"Nascondi la barra";
            else {
                for (const auto& b : g_buttons) {
                    if (b.id == h.id) text = b.hint;
                }
            }
            hintColor = kText2;
        }
        const Hit& firstRight = g_hits[g_hits.size() - 2];
        float hintRight = firstRight.box.X - 12 * s;
        float hintLeft = g_hits.size() > 2 ? g_hits[g_hits.size() - 3].box.GetRight() + 16 * s : hintRight - 200 * s;
        if (hintRight - hintLeft > 40 * s) drawClipped(g, text, hint, RectF(hintLeft, mid - 10 * s, hintRight - hintLeft, 20 * s), hintColor, StringAlignmentFar);
    }

    void pushBar(int y) {
        if (!g_bar) return;
        g_barSurface.push(g_bar, POINT{ g_barX, y });
    }

    void repaintBar() {
        if (!g_bar || !g_shown || g_hiddenAll) return;
        paintBar();
        RECT r;
        GetWindowRect(g_bar, &r);
        pushBar(r.top);
    }

    void startAnimation(bool in) {
        g_animIn = in;
        g_animStart = GetTickCount64();
        SetTimer(g_bar, kAnimTimer, 15, nullptr);
    }

    int hitAt(int x, int y) {
        for (size_t i = 0; i < g_hits.size(); ++i) {
            if (g_hits[i].box.Contains((REAL)x, (REAL)y)) return (int)i;
        }
        return -1;
    }

    LRESULT CALLBACK barProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // il gioco resta in primo piano
        case WM_NCHITTEST: {
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            return r == HTCLIENT ? HTCLIENT : r;
        }
        case WM_MOUSEMOVE: {
            const int h = hitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (h != g_hover) {
                g_hover = h;
                repaintBar();
            }
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hover != -1 || g_pressed != -1) {
                g_hover = g_pressed = -1;
                repaintBar();
            }
            return 0;
        case WM_LBUTTONDOWN:
            g_pressed = g_hover;
            repaintBar();
            return 0;
        case WM_LBUTTONUP: {
            const int h = hitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            const int pressed = g_pressed;
            g_pressed = -1;
            if (h >= 0 && h == pressed && g_hits[(size_t)h].enabled) {
                const std::string id = g_hits[(size_t)h].id;
                if (id == "close") QuickBar::hide();
                else if (g_cb.clicked) g_cb.clicked(id);
            }
            repaintBar();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const bool hand = g_hover >= 0 && g_hits[(size_t)g_hover].enabled;
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wp == kAnimTimer) {
                const double t = (double)(GetTickCount64() - g_animStart) / kAnimMs;
                pushBar(currentY(t));
                if (t >= 1.0) {
                    KillTimer(hwnd, kAnimTimer);
                    if (!g_animIn) ShowWindow(hwnd, SW_HIDE);
                }
                return 0;
            }
            if (wp == kTopmostTimer && IsWindowVisible(hwnd)) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            }
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // --- I pannelli -----------------------------------------------------------------------------------

    struct Panel {
        std::string id;
        HWND hwnd = nullptr;
        Surface surface;
        std::wstring title;
        std::vector<QuickBar::Row> rows;
        bool open = false;
        POINT pos{ -1, -1 };  // angolo della scheda (senza ombra), pixel dello schermo
        bool dragging = false;
        POINT grab{};
        bool hoverClose = false;
        float scale = 1;
        int w = 0, h = 0;     // finestra (ombra compresa)
        RectF closeBox;
        float headerH = 0;
    };
    std::map<std::string, std::unique_ptr<Panel>> g_panels;

    int panelOrder(const std::string& id) { return id == "missions" ? 0 : id == "refinery" ? 1 : 2; }

    Panel* findPanel(HWND hwnd) {
        for (auto& [id, p] : g_panels) {
            if (p->hwnd == hwnd) return p.get();
        }
        return nullptr;
    }

    float rowHeight(const QuickBar::Row& r, float s) {
        if (r.tone == 5) return 26 * s;
        return (r.progress >= 0 ? 28 : 22) * s;
    }

    void layoutPanel(Panel& p) {
        HMONITOR mon = p.pos.x == -1 ? MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY)
                                      : MonitorFromPoint(p.pos, MONITOR_DEFAULTTONEAREST);
        p.scale = dpiOfMonitor(mon) / 96.0f;
        const float s = p.scale;
        p.headerH = 38 * s;
        float h = p.headerH + 8 * s;
        const std::vector<QuickBar::Row> rows = p.rows.empty() ? std::vector<QuickBar::Row>{ { L"Niente da mostrare.", L"", 4 } } : p.rows;
        for (const auto& r : rows) h += rowHeight(r, s);
        h += 10 * s;
        p.w = (int)std::lround((kPanelWidth + 2 * kShadow) * s);
        p.h = (int)std::lround(h + 2 * kShadow * s);
        p.closeBox = RectF((kShadow + kPanelWidth - 34) * s, (kShadow + 5) * s, 28 * s, 28 * s);
        if (p.pos.x == -1 && p.pos.y == -1) {
            // Sotto la barra, i tre pannelli in fila al centro dello schermo.
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(mon, &mi);
            const int cardW = (int)std::lround(kPanelWidth * s), gap = (int)std::lround(kPanelGap * s);
            const int total = 3 * cardW + 2 * gap;
            const int cx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2;
            p.pos = POINT{ cx - total / 2 + panelOrder(p.id) * (cardW + gap), mi.rcMonitor.top + (int)std::lround((kTopGap + kBarHeight + 16) * s) };
        }
    }

    void paintPanel(Panel& p) {
        if (!p.surface.ensure(p.w, p.h)) return;
        std::memset(p.surface.bits, 0, (size_t)p.w * p.h * 4);
        const float s = p.scale;
        Bitmap bmp(p.w, p.h, p.w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(p.surface.bits));
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        const float x0 = kShadow * s, y0 = kShadow * s, cw = kPanelWidth * s, ch = p.h - 2 * kShadow * s;
        drawCard(g, x0, y0, cw, ch, s);

        Font title(displayFamily(), 12 * s, FontStyleBold, UnitPixel);
        Font section(displayFamily(), 10 * s, FontStyleBold, UnitPixel);
        Font body(bodyFamily(), 12.5f * s, FontStyleRegular, UnitPixel);
        Font mono(monoFamily(), 11.5f * s, FontStyleRegular, UnitPixel);
        Font symbol(symbolFamily(), 13 * s, FontStyleRegular, UnitPixel);

        // Intestazione: barretta ciano, titolo, X.
        SolidBrush bar(kCyan);
        g.FillRectangle(&bar, x0 + 16 * s, y0 + 13 * s, 3 * s, 12 * s);
        drawTracked(g, p.title, title, x0 + 26 * s, y0 + 11.5f * s, 2.4f * s, kCyan);
        if (p.hoverClose) {
            SolidBrush hb(Color(60, 255, 93, 108));
            g.FillEllipse(&hb, p.closeBox);
        }
        drawClipped(g, L"✕", symbol, p.closeBox, p.hoverClose ? Color(255, 255, 214, 218) : kText3, StringAlignmentCenter);
        Pen rule(Color(45, 63, 216, 245), 1.0f);
        g.DrawLine(&rule, PointF(x0 + 14 * s, y0 + p.headerH), PointF(x0 + cw - 14 * s, y0 + p.headerH));

        float y = y0 + p.headerH + 6 * s;
        const float left = x0 + 16 * s, right = x0 + cw - 16 * s;
        const std::vector<QuickBar::Row> rows = p.rows.empty() ? std::vector<QuickBar::Row>{ { L"Niente da mostrare.", L"", 4 } } : p.rows;
        for (const auto& r : rows) {
            const float rh = rowHeight(r, s);
            if (r.tone == 5) {
                drawTracked(g, r.text, section, left, y + 10 * s, 1.8f * s, kText3);
                y += rh;
                continue;
            }
            const float lineH = 20 * s;
            float textRight = right;
            if (!r.right.empty()) {
                const float rw = std::min(textWidth(g, r.right, mono) + 2 * s, 140 * s);
                drawClipped(g, r.right, mono, RectF(right - rw, y, rw, lineH), r.tone == 1 ? kGreen : kText2, StringAlignmentFar);
                textRight = right - rw - 10 * s;
            }
            // Un rombetto colorato per i toni importanti.
            float tx = left;
            if (r.tone >= 1 && r.tone <= 3) {
                SolidBrush d(toneColor(r.tone));
                const float m = y + lineH / 2, k = 3.2f * s;
                PointF pts[] = { { left + k, m - k }, { left + 2 * k, m }, { left + k, m + k }, { left, m } };
                g.FillPolygon(&d, pts, 4);
                tx += 12 * s;
            }
            drawClipped(g, r.text, body, RectF(tx, y, std::max(10.0f, textRight - tx), lineH), r.tone == 4 ? kText3 : r.tone == 3 ? Color(255, 255, 176, 184) : kText);
            if (r.progress >= 0) {
                const float py = y + lineH + 2 * s, pw = right - left;
                SolidBrush track(Color(40, 63, 216, 245)), fill(r.progress >= 1 ? kGreen : kCyan);
                g.FillRectangle(&track, left, py, pw, 3 * s);
                g.FillRectangle(&fill, left, py, (REAL)(pw * std::clamp(r.progress, 0.0, 1.0)), 3 * s);
            }
            y += rh;
        }
    }

    void showPanel(Panel& p) {
        if (!p.hwnd) return;
        layoutPanel(p);
        paintPanel(p);
        const int shadow = (int)std::lround(kShadow * p.scale);
        p.surface.push(p.hwnd, POINT{ p.pos.x - shadow, p.pos.y - shadow });
        if (!IsWindowVisible(p.hwnd)) ShowWindow(p.hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(p.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    LRESULT CALLBACK panelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Panel* p = findPanel(hwnd);
        switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            if (p) {
                const float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
                if (p->closeBox.Contains(x, y)) return 0;
                if (y <= (kShadow * p->scale) + p->headerH) {
                    POINT c;
                    GetCursorPos(&c);
                    p->dragging = true;
                    p->grab = POINT{ c.x - p->pos.x, c.y - p->pos.y };
                    SetCapture(hwnd);
                }
            }
            return 0;
        case WM_MOUSEMOVE:
            if (p) {
                if (p->dragging) {
                    POINT c;
                    GetCursorPos(&c);
                    p->pos = POINT{ c.x - p->grab.x, c.y - p->grab.y };
                    const int shadow = (int)std::lround(kShadow * p->scale);
                    SetWindowPos(hwnd, nullptr, p->pos.x - shadow, p->pos.y - shadow, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else {
                    const bool over = p->closeBox.Contains((REAL)GET_X_LPARAM(lp), (REAL)GET_Y_LPARAM(lp));
                    if (over != p->hoverClose) {
                        p->hoverClose = over;
                        showPanel(*p);
                    }
                    TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&t);
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            if (p && p->hoverClose) {
                p->hoverClose = false;
                showPanel(*p);
            }
            return 0;
        case WM_LBUTTONUP:
            if (p) {
                if (p->dragging) {
                    p->dragging = false;
                    ReleaseCapture();
                    if (g_cb.moved) g_cb.moved(p->id, p->pos.x, p->pos.y);
                    showPanel(*p); // un monitor con un'altra scala: si ridisegna
                } else if (p->closeBox.Contains((REAL)GET_X_LPARAM(lp), (REAL)GET_Y_LPARAM(lp))) {
                    const std::string id = p->id;
                    QuickBar::closePanel(id);
                    if (g_cb.closed) g_cb.closed(id);
                }
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (p) p->dragging = false;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && p) {
                POINT c;
                GetCursorPos(&c);
                ScreenToClient(hwnd, &c);
                const bool header = c.y <= (kShadow * p->scale) + p->headerH && !p->closeBox.Contains((REAL)c.x, (REAL)c.y);
                SetCursor(LoadCursorW(nullptr, p->closeBox.Contains((REAL)c.x, (REAL)c.y) ? IDC_HAND : header ? IDC_SIZEALL : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wp == kTopmostTimer && IsWindowVisible(hwnd)) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Panel& panel(const std::string& id) {
        auto& slot = g_panels[id];
        if (!slot) {
            slot = std::make_unique<Panel>();
            slot->id = id;
            slot->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kPanelClass, L"Iris", WS_POPUP, 0, 0, 1, 1,
                                         nullptr, nullptr, g_instance, nullptr);
            if (slot->hwnd) {
                keepOutOfCaptures(slot->hwnd);
                SetTimer(slot->hwnd, kTopmostTimer, 2000, nullptr);
            }
        }
        return *slot;
    }
} // namespace

namespace QuickBar {

void create(HINSTANCE instance, Callbacks callbacks) {
    g_instance = instance;
    g_cb = std::move(callbacks);
    g_fonts = new Fonts();
    WNDCLASSW wc{};
    wc.lpfnWndProc = barProc;
    wc.hInstance = instance;
    wc.lpszClassName = kBarClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW pc{};
    pc.lpfnWndProc = panelProc;
    pc.hInstance = instance;
    pc.lpszClassName = kPanelClass;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&pc);
    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kBarClass, L"Iris", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                            instance, nullptr);
    if (g_bar) {
        keepOutOfCaptures(g_bar);
        SetTimer(g_bar, kTopmostTimer, 2000, nullptr);
    }
}

void destroy() {
    for (auto& [id, p] : g_panels) {
        if (p->hwnd) DestroyWindow(p->hwnd);
        p->surface.free();
    }
    g_panels.clear();
    if (g_bar) DestroyWindow(g_bar);
    g_bar = nullptr;
    g_barSurface.free();
    delete g_fonts; // prima che GDI+ si spenga (Ui::shutdown)
    g_fonts = nullptr;
}

void setButtons(const std::vector<Button>& buttons, const std::wstring& keyHint, const std::wstring& status, int statusTone) {
    const bool sameShape = buttons.size() == g_buttons.size() &&
                           std::equal(buttons.begin(), buttons.end(), g_buttons.begin(), [](const Button& a, const Button& b) { return a.id == b.id && a.label == b.label; }) &&
                           status == g_status;
    g_buttons = buttons;
    g_keyHint = keyHint;
    g_status = status;
    g_statusTone = statusTone;
    if (!g_bar || !g_fonts) return;
    if (!sameShape && g_shown) {
        // Larghezze diverse: si rifà la disposizione mantenendo la barra dov'è.
        layoutBar();
    } else if (g_hits.empty()) {
        layoutBar();
    }
    repaintBar();
}

void show() {
    if (!g_bar || !g_fonts || g_shown) return;
    if (g_hiddenAll) {
        g_barBeforeHide = true;
        return;
    }
    layoutBar();
    g_hover = g_pressed = -1;
    paintBar();
    g_shown = true;
    pushBar(currentY(0)); // fuori dallo schermo, in alto
    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    SetWindowPos(g_bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    startAnimation(true);
}

void hide() {
    if (!g_bar || !g_shown) return;
    g_shown = false;
    g_hover = g_pressed = -1;
    startAnimation(false);
}

void toggle() {
    if (g_hiddenAll) {
        setHiddenAll(false);
        if (!g_shown) show();
        return;
    }
    g_shown ? hide() : show();
}

bool visible() { return g_shown && !g_hiddenAll; }

void setPanel(const std::string& id, const std::wstring& title, const std::vector<Row>& rows) {
    if (!g_fonts) return;
    Panel& p = panel(id);
    const bool changed = p.title != title || rows.size() != p.rows.size() ||
                         !std::equal(rows.begin(), rows.end(), p.rows.begin(), [](const Row& a, const Row& b) {
                             return a.text == b.text && a.right == b.right && a.tone == b.tone && std::fabs(a.progress - b.progress) < 0.002;
                         });
    p.title = title;
    p.rows = rows;
    if (changed && p.open && !g_hiddenAll && !p.dragging) showPanel(p);
}

void openPanel(const std::string& id, int x, int y) {
    if (!g_fonts) return;
    Panel& p = panel(id);
    p.open = true;
    if (p.pos.x == -1 && p.pos.y == -1 && !(x == -1 && y == -1) && MonitorFromPoint(POINT{ x + 20, y + 20 }, MONITOR_DEFAULTTONULL)) p.pos = POINT{ x, y };
    if (!g_hiddenAll) showPanel(p);
}

void closePanel(const std::string& id) {
    auto it = g_panels.find(id);
    if (it == g_panels.end()) return;
    it->second->open = false;
    if (it->second->hwnd) ShowWindow(it->second->hwnd, SW_HIDE);
}

bool panelOpen(const std::string& id) {
    auto it = g_panels.find(id);
    return it != g_panels.end() && it->second->open;
}

void setHiddenAll(bool hidden) {
    if (hidden == g_hiddenAll) return;
    if (hidden) {
        g_barBeforeHide = g_shown;
        if (g_bar) {
            KillTimer(g_bar, kAnimTimer);
            ShowWindow(g_bar, SW_HIDE);
        }
        g_shown = false;
        for (auto& [id, p] : g_panels) {
            if (p->hwnd) ShowWindow(p->hwnd, SW_HIDE);
        }
        g_hiddenAll = true;
    } else {
        g_hiddenAll = false;
        for (auto& [id, p] : g_panels) {
            if (p->open) showPanel(*p);
        }
        if (g_barBeforeHide) show();
    }
}

namespace {
    bool savePng(const Surface& surface, const std::wstring& path) {
        if (!surface.bits) return false;
        Bitmap layer(surface.w, surface.h, surface.w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(surface.bits));
        Bitmap out(surface.w, surface.h, PixelFormat32bppARGB);
        {
            Graphics g(&out);
            LinearGradientBrush sky(PointF(0, 0), PointF((REAL)surface.w, (REAL)surface.h), Color(255, 38, 58, 78), Color(255, 12, 18, 26));
            g.FillRectangle(&sky, 0, 0, surface.w, surface.h); // un "cielo" da gioco sotto
            g.DrawImage(&layer, 0, 0, surface.w, surface.h);
        }
        UINT n = 0, bytes = 0;
        GetImageEncodersSize(&n, &bytes);
        if (!bytes) return false;
        std::vector<BYTE> buf(bytes);
        auto* codecs = reinterpret_cast<ImageCodecInfo*>(buf.data());
        GetImageEncoders(n, bytes, codecs);
        for (UINT i = 0; i < n; ++i) {
            if (std::wstring(codecs[i].MimeType) == L"image/png") return out.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Ok;
        }
        return false;
    }
}

bool savePreview(const std::wstring& folder, int hoverIndex) {
    if (!g_fonts) return false;
    layoutBar();
    g_hover = hoverIndex;
    paintBar();
    g_hover = -1;
    bool ok = savePng(g_barSurface, folder + L"\\barra.png");
    for (auto& [id, p] : g_panels) {
        layoutPanel(*p);
        paintPanel(*p);
        ok = savePng(p->surface, folder + L"\\pannello_" + std::wstring(id.begin(), id.end()) + L".png") && ok;
    }
    return ok;
}

} // namespace QuickBar
