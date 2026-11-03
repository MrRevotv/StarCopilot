#include "quick_bar.h"
#include "../game_input.h"
#include "../log.h"

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

    constexpr const wchar_t* kWheelClass = L"IrisWheel";
    constexpr const wchar_t* kPanelClass = L"IrisHudPanel";
    constexpr DWORD kExcludeFromCapture = 0x11; // WDA_EXCLUDEFROMCAPTURE
    constexpr UINT_PTR kAnimTimer = 1;
    constexpr UINT_PTR kTopmostTimer = 2;
    constexpr int kShadow = 10;      // margine per l'ombra
    constexpr int kCut = 9;          // angoli tagliati
    constexpr int kPanelWidth = 344;
    constexpr int kPanelGap = 14;
    constexpr int kPanelTop = 24;    // i pannelli in fila: distanza dal bordo alto dello schermo
    // La ruota (a 96 DPI).
    constexpr int kOuterR = 196;     // bordo esterno delle voci
    constexpr int kInnerR = 92;      // bordo interno delle voci: dentro è il centro
    constexpr int kHubR = 80;        // il disco centrale
    constexpr int kMargin = 22;      // attorno alla ruota: tacche e ombra
    constexpr int kCaptionGap = 12, kCaptionW = 400, kCaptionH = 54;
    constexpr int kOpenMs = 150, kCloseMs = 110;
    constexpr double kPi = 3.14159265358979323846;

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
        void push(HWND hwnd, POINT pos, BYTE alpha = 255) {
            POINT src{ 0, 0 };
            SIZE size{ w, h };
            BLENDFUNCTION blend{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
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
    // Centrato su x, con il centro delle maiuscole su y.
    void drawTrackedCentered(Graphics& g, const std::wstring& text, const Font& font, float x, float y, float tracking, const Color& color) {
        const float w = trackedWidth(g, text, font, tracking);
        drawTracked(g, text, font, x - w / 2, y - font.GetSize() * 0.62f, tracking, color);
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

    // Il rombo di Iris con il nucleo acceso.
    void drawMark(Graphics& g, float mx, float my, float r, float s, const Color& c) {
        PointF d[] = { { mx, my - r }, { mx + r, my }, { mx, my + r }, { mx - r, my } };
        SolidBrush f(withAlpha(c, 46));
        g.FillPolygon(&f, d, 4);
        Pen p(c, 1.4f * s);
        g.DrawPolygon(&p, d, 4);
        const float r2 = r * 0.36f;
        PointF core[] = { { mx, my - r2 }, { mx + r2, my }, { mx, my + r2 }, { mx - r2, my } };
        SolidBrush cb(c);
        g.FillPolygon(&cb, core, 4);
    }

    UINT dpiOfMonitor(HMONITOR m) {
        UINT x = 96, y = 96;
        if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &x, &y))) return 96;
        return x;
    }

    void keepOutOfCaptures(HWND hwnd) { SetWindowDisplayAffinity(hwnd, kExcludeFromCapture); }

    HINSTANCE g_instance = nullptr;
    QuickBar::Callbacks g_cb;
    bool g_hiddenAll = false;

    // --- La ruota --------------------------------------------------------------------------------------

    HWND g_wheel = nullptr;
    Surface g_wheelSurface;
    std::vector<QuickBar::Button> g_buttons;
    std::wstring g_keyHint, g_status;
    int g_statusTone = 0;
    int g_hover = -1, g_pressed = -1; // voce sotto il mouse; -1 nessuna, -2 il centro
    bool g_shown = false;             // aperta per Iris: risponde a mouse e tasti
    bool g_opening = false;           // dentro show(): le perdite di fuoco di passaggio non la chiudono
    std::wstring g_notice;            // un problema da dire nella didascalia (es. il gioco non lascia il primo piano)
    float g_scale = 1;
    int g_wx = 0, g_wy = 0, g_ww = 0, g_wh = 0; // finestra
    HWND g_prevForeground = nullptr;  // a chi ridare il primo piano (il gioco)
    ULONGLONG g_animStart = 0;
    bool g_animIn = true, g_animating = false;
    BYTE g_alpha = 0;

    constexpr int kHub = -2;
    constexpr UINT WM_WHEEL_CHECK_FOCUS = WM_APP + 40; // dopo una perdita di fuoco: è andata davvero a un'altra finestra?

    std::string className(HWND h) {
        char c[64] = {};
        if (h) GetClassNameA(h, c, 64);
        return c[0] ? c : "nessuna";
    }

    float ringCenterY() { return (kOuterR + kMargin) * g_scale; }

    PointF polar(float cx, float cy, float r, float degreesFromTop) {
        const double a = (degreesFromTop - 90.0) * kPi / 180.0;
        return PointF(cx + (float)(r * std::cos(a)), cy + (float)(r * std::sin(a)));
    }

    // Uno spicchio di corona tra i raggi r0 e r1 (angoli GDI+: da ore 3, in senso orario).
    void wedge(GraphicsPath& p, float cx, float cy, float r0, float r1, float start, float sweep) {
        p.AddArc(cx - r1, cy - r1, 2 * r1, 2 * r1, start, sweep);
        p.AddArc(cx - r0, cy - r0, 2 * r0, 2 * r0, start + sweep, -sweep);
        p.CloseFigure();
    }

    // Le icone delle voci, a linee come l'HUD: (x, y) è il centro, u metà del lato.
    void drawIcon(Graphics& g, const std::string& id, float x, float y, float u, float s, const Color& c, bool on) {
        Pen pen(c, 1.5f * s);
        pen.SetLineJoin(LineJoinRound);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        SolidBrush fill(c);
        auto P = [&](float a, float b) { return PointF(x + a * u, y + b * u); };
        if (id == "mining") { // un cristallo
            PointF gem[] = { P(-0.45f, -0.9f), P(0.45f, -0.9f), P(0.92f, -0.32f), P(0, 0.95f), P(-0.92f, -0.32f) };
            g.DrawPolygon(&pen, gem, 5);
            g.DrawLine(&pen, P(-0.92f, -0.32f), P(0.92f, -0.32f));
            PointF left[] = { P(-0.45f, -0.9f), P(-0.25f, -0.32f), P(0, 0.95f) };
            PointF right[] = { P(0.45f, -0.9f), P(0.25f, -0.32f), P(0, 0.95f) };
            g.DrawLines(&pen, left, 3);
            g.DrawLines(&pen, right, 3);
        } else if (id == "record") { // registrazione: il pallino diventa rosso quando è acceso
            g.DrawEllipse(&pen, x - 0.9f * u, y - 0.9f * u, 1.8f * u, 1.8f * u);
            SolidBrush dot(on ? kRed : c);
            g.FillEllipse(&dot, x - 0.42f * u, y - 0.42f * u, 0.84f * u, 0.84f * u);
        } else if (id == "chat") { // una nuvoletta con due righe
            const float l = x - 0.95f * u, t = y - 0.82f * u, r = x + 0.95f * u, b = y + 0.48f * u, d = 0.5f * u;
            GraphicsPath p;
            p.AddArc(l, t, d, d, 180, 90);
            p.AddArc(r - d, t, d, d, 270, 90);
            p.AddArc(r - d, b - d, d, d, 0, 90);
            p.AddLine(P(-0.08f, 0.48f), P(-0.6f, 0.98f));
            p.AddLine(P(-0.6f, 0.98f), P(-0.45f, 0.48f));
            p.AddArc(l, b - d, d, d, 90, 90);
            p.CloseFigure();
            g.DrawPath(&pen, &p);
            g.DrawLine(&pen, P(-0.5f, -0.3f), P(0.5f, -0.3f));
            g.DrawLine(&pen, P(-0.5f, 0.05f), P(0.15f, 0.05f));
        } else if (id == "axis") { // un mirino
            g.DrawEllipse(&pen, x - 0.6f * u, y - 0.6f * u, 1.2f * u, 1.2f * u);
            g.DrawLine(&pen, P(0, -1), P(0, -0.32f));
            g.DrawLine(&pen, P(0, 0.32f), P(0, 1));
            g.DrawLine(&pen, P(-1, 0), P(-0.32f, 0));
            g.DrawLine(&pen, P(0.32f, 0), P(1, 0));
            g.FillEllipse(&fill, x - 0.13f * u, y - 0.13f * u, 0.26f * u, 0.26f * u);
        } else if (id == "orb") { // la sfera con un'orbita
            g.DrawEllipse(&pen, x - 0.56f * u, y - 0.56f * u, 1.12f * u, 1.12f * u);
            GraphicsState st = g.Save();
            g.TranslateTransform(x, y);
            g.RotateTransform(-24);
            g.DrawEllipse(&pen, -1.0f * u, -0.34f * u, 2.0f * u, 0.68f * u);
            g.Restore(st);
        } else if (id == "missions") { // una bandierina
            g.DrawLine(&pen, P(-0.55f, -0.95f), P(-0.55f, 0.95f));
            PointF flag[] = { P(-0.55f, -0.9f), P(0.85f, -0.48f), P(-0.55f, -0.06f) };
            g.DrawPolygon(&pen, flag, 3);
            g.DrawLine(&pen, P(-0.9f, 0.95f), P(-0.2f, 0.95f));
        } else if (id == "refinery") { // una clessidra
            g.DrawLine(&pen, P(-0.72f, -0.95f), P(0.72f, -0.95f));
            g.DrawLine(&pen, P(-0.72f, 0.95f), P(0.72f, 0.95f));
            PointF glass[] = { P(-0.52f, -0.95f), P(0.52f, -0.95f), P(0.09f, 0), P(0.52f, 0.95f), P(-0.52f, 0.95f), P(-0.09f, 0) };
            g.DrawPolygon(&pen, glass, 6);
            PointF sand[] = { P(-0.34f, 0.88f), P(0.34f, 0.88f), P(0, 0.42f) };
            g.FillPolygon(&fill, sand, 3);
        } else if (id == "log") { // il diario: un foglio con le righe
            GraphicsPath p;
            cutPath(p, x - 0.75f * u, y - 0.95f * u, 1.5f * u, 1.9f * u, 0.4f * u);
            g.DrawPath(&pen, &p);
            g.DrawLine(&pen, P(-0.38f, -0.35f), P(0.42f, -0.35f));
            g.DrawLine(&pen, P(-0.38f, 0.05f), P(0.42f, 0.05f));
            g.DrawLine(&pen, P(-0.38f, 0.45f), P(0.12f, 0.45f));
        } else { // "iris": la finestra di Iris, il suo rombo
            drawMark(g, x, y, 0.95f * u, s, c);
        }
    }

    void layoutWheel(HMONITOR mon) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        g_scale = dpiOfMonitor(mon) / 96.0f;
        const int side = (int)std::lround(2 * (kOuterR + kMargin) * g_scale);
        g_ww = side;
        g_wh = side + (int)std::lround((kCaptionGap + kCaptionH + kShadow) * g_scale);
        const int cx = (mi.rcMonitor.left + mi.rcMonitor.right) / 2, cy = (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
        g_wx = cx - g_ww / 2;
        g_wy = cy - side / 2;
    }

    void paintWheel() {
        if (!g_wheelSurface.ensure(g_ww, g_wh)) return;
        std::memset(g_wheelSurface.bits, 0, (size_t)g_ww * g_wh * 4);
        const float s = g_scale;
        Bitmap bmp(g_ww, g_wh, g_ww * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(g_wheelSurface.bits));
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        const float cx = g_ww / 2.0f, cy = ringCenterY();
        const float R = kOuterR * s, r0 = kInnerR * s, hub = kHubR * s;
        const int n = (int)g_buttons.size();
        const float step = n > 0 ? 360.0f / n : 360.0f;

        // Ombra morbida sotto la ruota.
        for (int i = (int)(16 * s); i >= 1; --i) {
            const float k = 1.0f - (float)i / (16 * s);
            SolidBrush b(Color((BYTE)(16 * k * k), 0, 0, 0));
            g.FillEllipse(&b, cx - R - i, cy - R - i + 3 * s, 2 * (R + i), 2 * (R + i));
        }
        // La corona: più chiara verso il bordo.
        {
            GraphicsPath ring;
            ring.SetFillMode(FillModeAlternate);
            ring.AddEllipse(cx - R, cy - R, 2 * R, 2 * R);
            ring.AddEllipse(cx - r0, cy - r0, 2 * r0, 2 * r0);
            GraphicsPath outer;
            outer.AddEllipse(cx - R, cy - R, 2 * R, 2 * R);
            PathGradientBrush shade(&outer);
            shade.SetCenterPoint(PointF(cx, cy));
            shade.SetCenterColor(Color(240, 4, 8, 13));
            Color edge[] = { Color(240, 15, 29, 43) };
            INT count = 1;
            shade.SetSurroundColors(edge, &count);
            g.FillPath(&shade, &ring);
        }
        // Tacche all'esterno: fitte e spente, più lunghe al centro di ogni voce.
        {
            Pen tick(Color(40, 63, 216, 245), 1.0f);
            for (int a = 0; a < 360; a += 5) g.DrawLine(&tick, polar(cx, cy, R + 5 * s, (float)a), polar(cx, cy, R + 9 * s, (float)a));
            for (int i = 0; i < n; ++i) {
                const bool hover = i == g_hover;
                Pen mark(hover ? kCyan : Color(120, 63, 216, 245), (hover ? 2.0f : 1.2f) * s);
                g.DrawLine(&mark, polar(cx, cy, R + 5 * s, i * step), polar(cx, cy, R + (hover ? 15 : 12) * s, i * step));
            }
        }

        Font label(displayFamily(), 10.5f * s, FontStyleBold, UnitPixel);
        for (int i = 0; i < n; ++i) {
            const QuickBar::Button& b = g_buttons[(size_t)i];
            const float mid = i * step;
            const float start = mid - step / 2 + 1.0f - 90.0f, sweep = step - 2.0f;
            const bool hover = i == g_hover, pressed = hover && g_pressed == i;
            // Il fondo dello spicchio: acceso, sotto il mouse, premuto.
            int a = b.on ? 30 : 0;
            if (hover) a = !b.enabled ? 16 : b.on ? (pressed ? 112 : 80) : (pressed ? 96 : 60);
            if (a > 0) {
                GraphicsPath w;
                wedge(w, cx, cy, r0 + 2 * s, R - 2 * s, start, sweep);
                SolidBrush f(b.enabled ? Color((BYTE)a, 63, 216, 245) : Color((BYTE)a, 143, 169, 182));
                g.FillPath(&f, &w);
            }
            // Il filetto sul bordo esterno.
            if (b.on || (hover && b.enabled)) {
                const float rr = R - 4 * s;
                Pen edge(b.on ? kCyan : withAlpha(kText, 210), (b.on ? 3.0f : 2.0f) * s);
                g.DrawArc(&edge, cx - rr, cy - rr, 2 * rr, 2 * rr, start + 1.5f, sweep - 3.0f);
            }
            // Separatore con la voce prima.
            Pen sep(Color(46, 63, 216, 245), 1.0f);
            g.DrawLine(&sep, polar(cx, cy, r0 + 5 * s, mid - step / 2), polar(cx, cy, R - 5 * s, mid - step / 2));
            // Icona e nome.
            const PointF c = polar(cx, cy, (r0 + R) / 2 + 2 * s, mid);
            const Color col = !b.enabled ? withAlpha(kText3, 190) : hover ? Color(255, 255, 255, 255) : b.on ? kCyan : kText2;
            drawIcon(g, b.id, c.X, c.Y - 10 * s, 10.5f * s, s, col, b.on);
            drawTrackedCentered(g, b.label, label, c.X, c.Y + 15 * s, 1.3f * s, col);
        }
        // I bordi della corona.
        {
            Pen outerPen(Color(125, 63, 216, 245), 1.2f * s), innerPen(Color(80, 63, 216, 245), 1.0f);
            g.DrawEllipse(&outerPen, cx - R, cy - R, 2 * R, 2 * R);
            g.DrawEllipse(&innerPen, cx - r0, cy - r0, 2 * r0, 2 * r0);
        }

        // Il centro: lo stato di Iris, o la voce sotto il mouse.
        {
            SolidBrush hubFill(Color(248, 6, 11, 18));
            g.FillEllipse(&hubFill, cx - hub, cy - hub, 2 * hub, 2 * hub);
            Pen hubPen(g_hover == kHub ? Color(230, 63, 216, 245) : Color(110, 63, 216, 245), (g_hover == kHub ? 1.6f : 1.2f) * s);
            g.DrawEllipse(&hubPen, cx - hub, cy - hub, 2 * hub, 2 * hub);
            const float hr = hub - 7 * s;
            Pen dots(Color(50, 63, 216, 245), 1.0f);
            dots.SetDashStyle(DashStyleDot);
            g.DrawEllipse(&dots, cx - hr, cy - hr, 2 * hr, 2 * hr);
        }
        if (g_hover >= 0 && g_hover < n) {
            // La freccetta verso la voce.
            const float th = g_hover * step;
            PointF tri[] = { polar(cx, cy, hub + 9 * s, th), polar(cx, cy, hub + 1 * s, th - 4.5f), polar(cx, cy, hub + 1 * s, th + 4.5f) };
            SolidBrush pb(kCyan);
            g.FillPolygon(&pb, tri, 3);
            const QuickBar::Button& b = g_buttons[(size_t)g_hover];
            Font name(displayFamily(), 15 * s, FontStyleBold, UnitPixel);
            Font state(displayFamily(), 10 * s, FontStyleBold, UnitPixel);
            drawTrackedCentered(g, b.label, name, cx, cy - 9 * s, 2.0f * s, !b.enabled ? kText2 : b.on ? kCyan : kText);
            if (!b.state.empty()) {
                const Color tc = toneColor(b.stateTone);
                const float tw = trackedWidth(g, b.state, state, 1.4f * s);
                const float x0 = cx - (tw + 12 * s) / 2;
                SolidBrush glow(withAlpha(tc, 60)), dot(tc);
                g.FillEllipse(&glow, x0 - 2 * s, cy + 13 * s - 5 * s, 10 * s, 10 * s);
                g.FillEllipse(&dot, x0, cy + 13 * s - 3 * s, 6 * s, 6 * s);
                drawTracked(g, b.state, state, x0 + 12 * s, cy + 13 * s - 6.2f * s, 1.4f * s, tc);
            }
        } else {
            Font brand(displayFamily(), 15 * s, FontStyleBold, UnitPixel);
            Font tiny(displayFamily(), 10 * s, FontStyleBold, UnitPixel);
            drawMark(g, cx, cy - 28 * s, 8.5f * s, s, kCyan);
            drawTrackedCentered(g, L"IRIS", brand, cx, cy - 3 * s, 3.2f * s, kText);
            const Color tc = toneColor(g_statusTone);
            const float tw = trackedWidth(g, g_status, tiny, 1.4f * s);
            const float x0 = cx - (tw + 12 * s) / 2, y0 = cy + 20 * s;
            SolidBrush glow(withAlpha(tc, 60)), dot(tc);
            g.FillEllipse(&glow, x0 - 2 * s, y0 - 5 * s, 10 * s, 10 * s);
            g.FillEllipse(&dot, x0, y0 - 3 * s, 6 * s, 6 * s);
            drawTracked(g, g_status, tiny, x0 + 12 * s, y0 - 6.2f * s, 1.4f * s, kText2);
        }

        // La didascalia sotto: cosa fa la voce, o come si torna al gioco.
        {
            const float capW = kCaptionW * s, capH = kCaptionH * s;
            const float capX = cx - capW / 2, capY = 2 * (kOuterR + kMargin) * s + kCaptionGap * s;
            drawCard(g, capX, capY, capW, capH, s);
            const std::wstring back = (g_keyHint.empty() ? L"" : g_keyHint + L" o ") + L"Esc: torna al gioco";
            std::wstring text;
            Color color = kText2;
            if (g_hover >= 0 && g_hover < n) {
                text = g_buttons[(size_t)g_hover].hint;
                color = g_buttons[(size_t)g_hover].enabled ? kText : kAmber;
            } else if (g_hover == kHub) {
                text = L"Clic qui, tasto destro, " + back;
                color = kText;
            } else if (!g_notice.empty()) {
                text = g_notice;
                color = kAmber;
            } else {
                text = L"Punta una voce e clicca (o frecce e Invio)  ·  " + back;
            }
            Font body(bodyFamily(), 12.5f * s, FontStyleRegular, UnitPixel);
            StringFormat f;
            f.SetAlignment(StringAlignmentCenter);
            f.SetLineAlignment(StringAlignmentCenter);
            f.SetTrimming(StringTrimmingEllipsisWord);
            SolidBrush brush(color);
            g.DrawString(text.c_str(), (INT)text.size(), &body, RectF(capX + 18 * s, capY + 5 * s, capW - 36 * s, capH - 10 * s), &f, &brush);
        }
    }

    void pushWheel() {
        if (g_wheel) g_wheelSurface.push(g_wheel, POINT{ g_wx, g_wy }, g_alpha);
    }

    void repaintWheel() {
        if (!g_wheel || !g_shown || g_hiddenAll) return;
        paintWheel();
        pushWheel();
    }

    void startAnimation(bool in) {
        g_animIn = in;
        g_animating = true;
        g_animStart = GetTickCount64();
        SetTimer(g_wheel, kAnimTimer, 15, nullptr);
    }

    bool fadingOut() { return g_animating && !g_animIn; }

    // Subito, senza dissolvenza (nascondi tutto, chiusura del programma, una ruota rimasta a schermo).
    void hideWheelNow(bool backToGame) {
        if (!g_wheel) return;
        g_shown = g_opening = false;
        g_hover = g_pressed = -1;
        KillTimer(g_wheel, kAnimTimer);
        g_animating = false;
        if (backToGame && GetForegroundWindow() == g_wheel && g_prevForeground && IsWindow(g_prevForeground)) SetForegroundWindow(g_prevForeground);
        ShowWindow(g_wheel, SW_HIDE);
    }

    // A schermo ma non più aperta per Iris (non dovrebbe succedere): mai lasciarla lì a ignorare tutto.
    bool closeStray(bool backToGame) {
        if (g_shown || !g_wheel || !IsWindowVisible(g_wheel) || fadingOut()) return false;
        Log::warn("Ruota rimasta a schermo da chiusa: la tolgo");
        hideWheelNow(backToGame);
        return true;
    }

    // La voce sotto il punto (coordinate della finestra della ruota).
    int wheelHitAt(int x, int y) {
        if (y > 2 * (kOuterR + kMargin) * g_scale) return -1; // la didascalia
        const int i = QuickBar::sectorAt(x - g_ww / 2.0, y - ringCenterY(), (int)g_buttons.size(), kInnerR * g_scale);
        return i < 0 ? kHub : i;
    }

    void setHover(int h) {
        if (h == g_hover) return;
        g_hover = h;
        repaintWheel();
    }

    // Frecce e rotellina: la voce dopo o prima (anche se il mouse è bloccato dal gioco).
    void stepHover(int delta) {
        const int n = (int)g_buttons.size();
        if (n == 0) return;
        const int from = g_hover >= 0 ? g_hover : (delta > 0 ? -1 : 0);
        setHover(((from + delta) % n + n) % n);
    }

    void activate(int index) {
        if (index == kHub) {
            QuickBar::hide(true);
        } else if (index >= 0 && index < (int)g_buttons.size() && g_buttons[(size_t)index].enabled) {
            const std::string id = g_buttons[(size_t)index].id;
            if (g_cb.clicked) g_cb.clicked(id); // può chiudere la ruota (es. la finestra di Iris davanti)
        }
    }

    LRESULT CALLBACK wheelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                Log::info("Ruota: perso il fuoco (ora davanti: " + className((HWND)lp) + ")");
                // Hai cliccato fuori (sul gioco) o sei passato a un'altra finestra: la ruota si chiude da sola.
                // Non subito: mentre si apre (e durante l'aggancio all'input del gioco) Windows toglie e
                // ridà il fuoco; si guarda dopo, a cose ferme, chi è davvero davanti.
                if (g_shown && !g_opening) PostMessageW(hwnd, WM_WHEEL_CHECK_FOCUS, 0, 0);
            }
            break; // poi DefWindowProc: attivata, la ruota prende anche la tastiera (Esc, frecce, Invio)
        case WM_WHEEL_CHECK_FOCUS:
            if (g_shown && !g_opening && GetForegroundWindow() != hwnd) {
                Log::info("Ruota chiusa: davanti c'è un'altra finestra (" + className(GetForegroundWindow()) + ")");
                QuickBar::hide(false);
            }
            return 0;
        case WM_CLOSE: // Alt+F4 sulla ruota: si chiude, non si distrugge
            QuickBar::hide(true);
            return 0;
        case WM_MOUSEMOVE: {
            if (!g_shown) {
                closeStray(false);
                return 0;
            }
            setHover(wheelHitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_shown && (g_pressed != -1 || g_hover != -1)) {
                g_pressed = -1;
                g_hover = -1;
                repaintWheel();
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (g_shown) stepHover(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
            return 0;
        case WM_LBUTTONDOWN:
            if (!g_shown) {
                closeStray(true);
                return 0;
            }
            g_hover = wheelHitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            g_pressed = g_hover;
            repaintWheel();
            return 0;
        case WM_LBUTTONUP: {
            if (!g_shown) return 0;
            const int h = wheelHitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            const int pressed = g_pressed;
            g_pressed = -1;
            if (h == pressed && h != -1) activate(h);
            repaintWheel();
            return 0;
        }
        case WM_RBUTTONUP:
            QuickBar::hide(true);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) QuickBar::hide(true);
            else if (!g_shown) closeStray(true);
            else if (wp == VK_RETURN || wp == VK_SPACE) activate(g_hover);
            else if (wp == VK_RIGHT || wp == VK_DOWN) stepHover(1);
            else if (wp == VK_LEFT || wp == VK_UP) stepHover(-1);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const bool hand = g_hover == kHub || (g_hover >= 0 && g_hover < (int)g_buttons.size() && g_buttons[(size_t)g_hover].enabled);
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_TIMER:
            if (wp == kAnimTimer) {
                const double t = std::clamp((double)(GetTickCount64() - g_animStart) / (g_animIn ? kOpenMs : kCloseMs), 0.0, 1.0);
                const double e = 1.0 - std::pow(1.0 - t, 3.0); // parte veloce, arriva piano
                g_alpha = (BYTE)std::lround(255 * (g_animIn ? e : 1.0 - e));
                pushWheel();
                if (t >= 1.0) {
                    KillTimer(hwnd, kAnimTimer);
                    g_animating = false;
                    if (!g_animIn) ShowWindow(hwnd, SW_HIDE);
                    else if (!g_shown) ShowWindow(hwnd, SW_HIDE); // chiusa mentre si apriva: niente ruota a schermo
                }
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
        POINT pos{ -1, -1 };    // angolo della scheda (senza ombra), pixel dello schermo; -1 = da mettere in fila
        bool userPlaced = false; // trascinato da te (o posizione salvata): resta lì
        bool dragging = false;
        POINT grab{};
        bool hoverClose = false;
        float scale = 1;
        int w = 0, h = 0;       // finestra (ombra compresa)
        RectF closeBox;
        float headerH = 0;
    };
    std::map<std::string, std::unique_ptr<Panel>> g_panels;

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

    // La scheda sullo schermo (senza ombra).
    RECT cardRect(const Panel& p) {
        const int shadow = (int)std::lround(kShadow * p.scale);
        return RECT{ p.pos.x, p.pos.y, p.pos.x + p.w - 2 * shadow, p.pos.y + p.h - 2 * shadow };
    }

    // Un pannello nuovo va in alto, nel primo posto libero della fila (al centro dello schermo,
    // da sinistra); se la fila è piena, sotto il più basso.
    void placeInRow(Panel& p, HMONITOR mon) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(mon, &mi);
        const float s = p.scale;
        const int shadow = (int)std::lround(kShadow * s);
        const int cardW = p.w - 2 * shadow, cardH = p.h - 2 * shadow, gap = (int)std::lround(kPanelGap * s);
        const int top = mi.rcMonitor.top + (int)std::lround(kPanelTop * s);
        const int total = 3 * cardW + 2 * gap;
        const int left = (mi.rcMonitor.left + mi.rcMonitor.right) / 2 - total / 2;
        std::vector<RECT> taken;
        for (auto& [id, o] : g_panels) {
            if (o.get() != &p && o->open && o->pos.x != -1) taken.push_back(cardRect(*o));
        }
        for (int i = 0; i < 3; ++i) {
            const RECT slot{ left + i * (cardW + gap), top, left + i * (cardW + gap) + cardW, top + cardH };
            const bool busy = std::any_of(taken.begin(), taken.end(), [&](const RECT& r) {
                RECT x;
                return IntersectRect(&x, &slot, &r) != FALSE;
            });
            if (!busy) {
                p.pos = POINT{ slot.left, slot.top };
                return;
            }
        }
        int bottom = top;
        for (const RECT& r : taken) bottom = std::max(bottom, (int)r.bottom);
        p.pos = POINT{ left, std::min(bottom + gap, (int)mi.rcMonitor.bottom - cardH) };
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
        if (p.pos.x == -1 && p.pos.y == -1) placeInRow(p, mon);
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

    // Ridisegna e mostra un pannello APERTO. Un pannello chiuso non torna mai a schermo da qui:
    // prima il passaggio del mouse sulla X (che ridisegnava dopo la chiusura) lo rimetteva su.
    void showPanel(Panel& p) {
        if (!p.hwnd || !p.open || g_hiddenAll) return;
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
            if (p && p->open) {
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
                showPanel(*p); // chiuso: non fa niente
            }
            return 0;
        case WM_LBUTTONUP:
            if (p) {
                if (p->dragging) {
                    p->dragging = false;
                    ReleaseCapture();
                    p->userPlaced = true;
                    if (g_cb.moved) g_cb.moved(p->id, p->pos.x, p->pos.y);
                    showPanel(*p); // un monitor con un'altra scala: si ridisegna
                } else if (p->open && p->closeBox.Contains((REAL)GET_X_LPARAM(lp), (REAL)GET_Y_LPARAM(lp))) {
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
    wc.lpfnWndProc = wheelProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWheelClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW pc{};
    pc.lpfnWndProc = panelProc;
    pc.hInstance = instance;
    pc.lpszClassName = kPanelClass;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&pc);
    // La ruota NON ha WS_EX_NOACTIVATE: deve prendere il primo piano, così il gioco libera il cursore.
    g_wheel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kWheelClass, L"Iris", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance,
                              nullptr);
    if (g_wheel) keepOutOfCaptures(g_wheel);
}

void destroy() {
    for (auto& [id, p] : g_panels) {
        if (p->hwnd) DestroyWindow(p->hwnd);
        p->surface.free();
    }
    g_panels.clear();
    if (g_wheel) DestroyWindow(g_wheel);
    g_wheel = nullptr;
    g_shown = false;
    g_wheelSurface.free();
    delete g_fonts; // prima che GDI+ si spenga (Ui::shutdown)
    g_fonts = nullptr;
}

void setButtons(const std::vector<Button>& buttons, const std::wstring& keyHint, const std::wstring& status, int statusTone) {
    g_buttons = buttons;
    g_keyHint = keyHint;
    g_status = status;
    g_statusTone = statusTone;
    if (g_hover >= (int)g_buttons.size()) g_hover = -1;
    repaintWheel();
}

void show(bool centerCursor) {
    if (!g_wheel || !g_fonts || g_hiddenAll) return;
    if (g_shown) {
        GameInput::takeForeground(g_wheel);
        return;
    }
    KillTimer(g_wheel, kAnimTimer); // si stava chiudendo: si riapre da capo
    g_animating = false;
    HWND fg = GetForegroundWindow();
    g_prevForeground = fg != g_wheel && IsWindowVisible(fg) ? fg : nullptr;
    layoutWheel(MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY));
    g_hover = g_pressed = -1;
    g_notice.clear();
    // Aperta da qui in poi. Finché show() non finisce le perdite di fuoco non la chiudono: prendere il
    // primo piano al gioco (aggancio all'input) toglie e ridà il fuoco, e prima lasciava una ruota
    // a schermo che Iris credeva chiusa e che ignorava mouse, Esc e Shift+Tab.
    g_shown = g_opening = true;
    POINT cursor;
    if (centerCursor) {
        cursor = POINT{ g_wx + g_ww / 2, g_wy + (int)std::lround(ringCenterY()) };
        SetCursorPos(cursor.x, cursor.y);
    } else if (GetCursorPos(&cursor) && cursor.x >= g_wx && cursor.x < g_wx + g_ww && cursor.y >= g_wy && cursor.y < g_wy + g_wh) {
        g_hover = wheelHitAt(cursor.x - g_wx, cursor.y - g_wy);
    }
    paintWheel();
    g_alpha = 0;
    pushWheel();
    // A schermo senza attivarla, poi il primo piano in un colpo solo (diretto o con l'aggancio).
    ShowWindow(g_wheel, SW_SHOWNOACTIVATE);
    SetWindowPos(g_wheel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool front = GameInput::takeForeground(g_wheel);
    // Il cursore della ruota si vede, anche se il gioco (a cui ci siamo agganciati) lo teneva nascosto.
    int count = ShowCursor(TRUE);
    while (count < 0) count = ShowCursor(TRUE);
    if (count > 0) ShowCursor(FALSE);
    if (!front) {
        const std::wstring close = g_keyHint.empty() ? L"Esc" : g_keyHint;
        if (fg && GameInput::windowElevated(fg) && !GameInput::selfElevated()) {
            g_notice = L"Il gioco è avviato come amministratore: Windows non lascia passare Iris davanti. Avvia anche Iris come amministratore. " + close + L" chiude.";
        } else {
            g_notice = L"Il gioco non ha lasciato il primo piano a Iris: se il cursore non si muove, " + close + L" chiude la ruota.";
        }
        paintWheel();
    }
    g_opening = false;
    startAnimation(true);
}

void hide(bool backToGame) {
    if (!g_wheel) return;
    if (!g_shown) {
        closeStray(backToGame); // a schermo ma già chiusa: via comunque
        return;
    }
    g_shown = g_opening = false; // prima di tutto: il cambio di primo piano qui sotto richiama WM_ACTIVATE
    g_hover = g_pressed = -1;
    // Il fuoco torna al gioco solo se ce l'ha ancora la ruota (se sei andato altrove, resta lì).
    if (backToGame && GetForegroundWindow() == g_wheel && g_prevForeground && IsWindow(g_prevForeground)) SetForegroundWindow(g_prevForeground);
    startAnimation(false);
}

bool visible() {
    if (g_hiddenAll || !g_wheel) return false;
    // Anche una ruota rimasta a schermo conta: così il suo tasto la chiude sempre.
    return g_shown || (IsWindowVisible(g_wheel) && !fadingOut());
}

int sectorAt(double dx, double dy, int count, double deadZone) {
    if (count <= 0 || std::hypot(dx, dy) < deadZone) return -1;
    double a = std::atan2(dx, -dy) * 180.0 / kPi; // 0 in alto, in senso orario
    if (a < 0) a += 360.0;
    const double step = 360.0 / count;
    return (int)std::floor((a + step / 2) / step) % count;
}

void setPanel(const std::string& id, const std::wstring& title, const std::vector<Row>& rows) {
    if (!g_fonts) return;
    Panel& p = panel(id);
    const bool changed = p.title != title || rows.size() != p.rows.size() ||
                         !std::equal(rows.begin(), rows.end(), p.rows.begin(), [](const Row& a, const Row& b) {
                             return a.text == b.text && a.right == b.right && a.tone == b.tone && std::fabs(a.progress - b.progress) < 0.002;
                         });
    p.title = title;
    p.rows = rows;
    if (changed && !p.dragging) showPanel(p);
}

void openPanel(const std::string& id, int x, int y) {
    if (!g_fonts) return;
    Panel& p = panel(id);
    p.open = true;
    // L'ultima posizione dove l'avevi trascinato (se quel monitor c'è ancora); se no, in fila in alto.
    if (!p.userPlaced && !(x == -1 && y == -1) && MonitorFromPoint(POINT{ x + 20, y + 20 }, MONITOR_DEFAULTTONULL)) {
        p.pos = POINT{ x, y };
        p.userPlaced = true;
    }
    showPanel(p);
}

void closePanel(const std::string& id) {
    auto it = g_panels.find(id);
    if (it == g_panels.end()) return;
    Panel& p = *it->second;
    p.open = false;
    p.hoverClose = false;
    if (p.dragging) {
        p.dragging = false;
        ReleaseCapture();
    }
    if (!p.userPlaced) p.pos = POINT{ -1, -1 }; // la prossima volta si rimette in fila
    if (p.hwnd) ShowWindow(p.hwnd, SW_HIDE);
}

bool panelOpen(const std::string& id) {
    auto it = g_panels.find(id);
    return it != g_panels.end() && it->second->open;
}

void setHiddenAll(bool hidden) {
    if (hidden == g_hiddenAll) return;
    if (hidden) {
        hideWheelNow(true);
        for (auto& [id, p] : g_panels) {
            if (p->hwnd) ShowWindow(p->hwnd, SW_HIDE);
        }
        g_hiddenAll = true;
    } else {
        g_hiddenAll = false;
        for (auto& [id, p] : g_panels) showPanel(*p);
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
    layoutWheel(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    bool ok = true;
    for (auto [hover, name] : std::initializer_list<std::pair<int, const wchar_t*>>{ { -1, L"ruota" }, { hoverIndex, L"ruota_voce" }, { kHub, L"ruota_centro" } }) {
        g_hover = hover;
        paintWheel();
        ok = savePng(g_wheelSurface, folder + L"\\" + name + L".png") && ok;
    }
    g_hover = -1;
    for (auto& [id, p] : g_panels) {
        layoutPanel(*p);
        paintPanel(*p);
        ok = savePng(p->surface, folder + L"\\pannello_" + std::wstring(id.begin(), id.end()) + L".png") && ok;
    }
    return ok;
}

} // namespace QuickBar
