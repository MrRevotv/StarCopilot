#include "ui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <map>
#include <utility>

// gdiplus.h usa min/max, che NOMINMAX toglie da windows.h.
namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

namespace {
    constexpr UINT_PTR kButtonSubclassId = 0x5654; // "VT"
    constexpr UINT_PTR kComboSubclassId = 0x5655;

    // Stile di etichette e caselle di testo, salvato in GWLP_USERDATA:
    // bit 0-23 colore del testo, 24-47 sfondo, 48 = "è nostro".
    constexpr LONG_PTR kStyleMarker = (LONG_PTR)1 << 48;

    LONG_PTR packStyle(COLORREF fg, COLORREF bg) {
        return kStyleMarker | (LONG_PTR)(fg & 0xFFFFFF) | ((LONG_PTR)(bg & 0xFFFFFF) << 24);
    }

    struct ButtonInfo {
        Ui::ButtonKind kind;
        COLORREF behind;
        bool hot = false;
        bool checked = false;
    };

    ULONG_PTR g_gdiplusToken = 0;
    using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
    AllowDarkModeForWindowFn g_allowDarkModeForWindow = nullptr;

    std::map<std::pair<int, UINT>, HFONT> g_fonts;
    std::map<COLORREF, HBRUSH> g_brushes;

    DWORD windowsBuild() {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        if (fn && fn(&v) == 0) return v.dwBuildNumber;
        return 0;
    }

    Gdiplus::Color gdiColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

    void roundPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
        float d = std::min({ radius * 2.0f, r.Width, r.Height });
        if (d <= 0.5f) {
            path.AddRectangle(r);
            return;
        }
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
    }

    COLORREF mix(COLORREF a, COLORREF b, int percentB) {
        auto ch = [&](int x, int y) { return (x * (100 - percentB) + y * percentB) / 100; };
        return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
    }

    LRESULT CALLBACK buttonProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
        auto* info = reinterpret_cast<ButtonInfo*>(ref);
        switch (msg) {
        case WM_MOUSEMOVE:
            if (!info->hot) {
                info->hot = true;
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            info->hot = false;
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_LBUTTONDBLCLK:
            // Due clic veloci sono due clic (un interruttore premuto due volte torna com'era).
            return DefSubclassProc(h, WM_LBUTTONDOWN, w, l);
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, buttonProc, id);
            delete info;
            break;
        }
        return DefSubclassProc(h, msg, w, l);
    }

    // La rotellina sopra un menu a tendina chiuso fa scorrere la pagina, non
    // cambia la scelta (si cambierebbe lingua senza volerlo).
    LRESULT CALLBACK comboProc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
        switch (msg) {
        case WM_MOUSEWHEEL:
            if (!SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) return SendMessageW(GetParent(h), msg, w, l);
            break;
        case WM_NCDESTROY:
            RemoveWindowSubclass(h, comboProc, id);
            break;
        }
        return DefSubclassProc(h, msg, w, l);
    }

    ButtonInfo* buttonInfo(HWND h) {
        DWORD_PTR ref = 0;
        if (!h || !GetWindowSubclass(h, buttonProc, kButtonSubclassId, &ref)) return nullptr;
        return reinterpret_cast<ButtonInfo*>(ref);
    }

    bool isCombo(HWND h) {
        wchar_t cls[32] = {};
        GetClassNameW(h, cls, 32);
        return _wcsicmp(cls, WC_COMBOBOXW) == 0;
    }
}

namespace Ui {

void initialize() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);

    // Funzioni non documentate di uxtheme (le usano anche Blocco note e
    // Esplora risorse) per menu e barre di scorrimento scure. Ci sono da
    // Windows 10 1903: prima, o se mancano, tutto funziona con i colori chiari.
    if (windowsBuild() < 18362) return;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    g_allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    if (setPreferredAppMode) setPreferredAppMode(2); // sempre scuro
}

void shutdown() {
    for (auto& [key, f] : g_fonts) DeleteObject(f);
    g_fonts.clear();
    for (auto& [key, b] : g_brushes) DeleteObject(b);
    g_brushes.clear();
    if (g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

UINT dpiOf(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    return dpi ? dpi : 96;
}

HFONT font(Font role, UINT dpi) {
    auto key = std::make_pair((int)role, dpi);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end()) return it->second;
    int size = 14, weight = FW_NORMAL;
    const wchar_t* face = L"Segoe UI";
    switch (role) {
    case Font::Normal: break;
    case Font::Small: size = 12; break;
    case Font::Bold: weight = FW_SEMIBOLD; break;
    case Font::Header: size = 12; weight = FW_BOLD; break;
    case Font::Title: size = 21; weight = FW_SEMIBOLD; break;
    case Font::Status: size = 16; weight = FW_SEMIBOLD; break;
    case Font::Mono: size = 13; face = L"Consolas"; break;
    }
    HFONT f = CreateFontW(-px(size, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    g_fonts[key] = f;
    return f;
}

HBRUSH brush(COLORREF color) {
    auto it = g_brushes.find(color);
    if (it != g_brushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(color);
    g_brushes[color] = b;
    return b;
}

void darkTitleBar(HWND window) {
    BOOL on = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (19 nelle prime versioni di Windows 10).
    if (FAILED(DwmSetWindowAttribute(window, 20, &on, sizeof(on)))) DwmSetWindowAttribute(window, 19, &on, sizeof(on));
    // Windows 11: barra del titolo dello stesso colore del menu.
    COLORREF caption = Color::Nav, text = Color::Text;
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36, &text, sizeof(text));
}

void darkControl(HWND control, const wchar_t* theme) {
    if (g_allowDarkModeForWindow) g_allowDarkModeForWindow(control, TRUE);
    SetWindowTheme(control, theme, nullptr);
}

HWND createButton(HWND parent, int id, const std::wstring& text, ButtonKind kind, COLORREF behind) {
    HWND h = CreateWindowExW(0, WC_BUTTONW, text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (h) SetWindowSubclass(h, buttonProc, kButtonSubclassId, reinterpret_cast<DWORD_PTR>(new ButtonInfo{ kind, behind }));
    return h;
}

void setChecked(HWND button, bool checked) {
    ButtonInfo* info = buttonInfo(button);
    if (!info || info->checked == checked) return;
    info->checked = checked;
    InvalidateRect(button, nullptr, FALSE);
}

bool isChecked(HWND button) {
    ButtonInfo* info = buttonInfo(button);
    return info && info->checked;
}

void setText(HWND control, const std::wstring& text) {
    if (!control) return;
    int len = GetWindowTextLengthW(control);
    std::wstring current(len + 1, L'\0');
    GetWindowTextW(control, current.data(), len + 1);
    current.resize(len);
    if (current == text) return; // niente sfarfallio se non cambia
    SetWindowTextW(control, text.c_str());
    InvalidateRect(control, nullptr, FALSE);
}

bool drawButton(const DRAWITEMSTRUCT* dis) {
    if (dis->CtlType != ODT_BUTTON) return false;
    ButtonInfo* info = buttonInfo(dis->hwndItem);
    if (!info) return false;

    const UINT dpi = dpiOf(dis->hwndItem);
    const RECT r = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool keyboardFocus = (dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT);

    int len = GetWindowTextLengthW(dis->hwndItem);
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(dis->hwndItem, text.data(), len + 1);
    text.resize(len);

    Buffered buffer(dis->hDC, r);
    HDC dc = buffer.dc();
    FillRect(dc, &r, brush(info->behind));

    switch (info->kind) {
    case ButtonKind::Primary:
    case ButtonKind::Secondary:
    case ButtonKind::Danger: {
        COLORREF fill, border = CLR_INVALID, textColor = Color::Text;
        if (info->kind == ButtonKind::Primary) {
            fill = pressed ? Color::AccentPressed : info->hot ? Color::AccentHover : Color::Accent;
            textColor = RGB(255, 255, 255);
        } else if (info->kind == ButtonKind::Danger) {
            fill = pressed ? mix(Color::Danger, 0, 20) : info->hot ? Color::DangerHover : Color::Danger;
            textColor = RGB(255, 255, 255);
        } else {
            fill = pressed ? Color::Panel : info->hot ? Color::SecondaryHover : Color::Secondary;
            border = Color::PanelBorder;
        }
        if (disabled) {
            fill = Color::Secondary;
            textColor = Color::TextDim;
            border = CLR_INVALID;
        }
        if (keyboardFocus) border = Color::AccentHover;
        fillRound(dc, r, px(6, dpi), fill, border);
        RECT tr = r;
        InflateRect(&tr, -px(10, dpi), 0);
        drawText(dc, text, tr, font(info->kind == ButtonKind::Secondary ? Font::Normal : Font::Bold, dpi), textColor,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case ButtonKind::Nav: {
        COLORREF fill = info->checked ? Color::Panel : info->hot ? RGB(20, 22, 28) : Color::Nav;
        FillRect(dc, &r, brush(fill));
        if (info->checked) {
            RECT bar{ r.left, r.top + px(9, dpi), r.left + px(3, dpi), r.bottom - px(9, dpi) };
            fillRound(dc, bar, px(2, dpi), Color::Accent);
        }
        RECT tr = r;
        tr.left += px(20, dpi);
        tr.right -= px(8, dpi);
        drawText(dc, text, tr, font(info->checked ? Font::Bold : Font::Normal, dpi),
                 info->checked ? Color::Text : info->hot ? Color::Text : Color::TextDim,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    case ButtonKind::Toggle: {
        const int sw = px(38, dpi), sh = px(20, dpi);
        const int cy = (r.top + r.bottom) / 2;
        RECT track{ r.right - sw, cy - sh / 2, r.right, cy + sh / 2 };
        COLORREF fill = info->checked ? (info->hot ? Color::AccentHover : Color::Accent)
                                      : (info->hot ? mix(Color::ToggleOff, Color::Text, 15) : Color::ToggleOff);
        if (disabled) fill = Color::Secondary;
        fillRound(dc, track, sh / 2, fill, keyboardFocus ? Color::AccentHover : CLR_INVALID);
        const int knob = sh / 2 - px(3, dpi);
        const int cx = info->checked ? track.right - sh / 2 : track.left + sh / 2;
        fillCircle(dc, cx, cy, knob, disabled ? Color::TextDim : RGB(255, 255, 255));
        RECT tr = r;
        tr.right = track.left - px(12, dpi);
        drawText(dc, text, tr, font(Font::Normal, dpi), disabled ? Color::TextDim : Color::Text,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
    }
    }
    return true;
}

HWND createLabel(HWND parent, const std::wstring& text, COLORREF color, COLORREF behind, int id) {
    HWND h = CreateWindowExW(0, WC_STATICW, text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (h) SetWindowLongPtrW(h, GWLP_USERDATA, packStyle(color, behind));
    return h;
}

void setColors(HWND control, COLORREF text, COLORREF background) {
    SetWindowLongPtrW(control, GWLP_USERDATA, packStyle(text, background));
    InvalidateRect(control, nullptr, TRUE);
}

void setLabelColor(HWND label, COLORREF color) {
    LONG_PTR style = GetWindowLongPtrW(label, GWLP_USERDATA);
    if (!(style & kStyleMarker)) return;
    COLORREF bg = (COLORREF)((style >> 24) & 0xFFFFFF);
    LONG_PTR packed = packStyle(color, bg);
    if (packed == style) return;
    SetWindowLongPtrW(label, GWLP_USERDATA, packed);
    InvalidateRect(label, nullptr, TRUE);
}

HWND createCombo(HWND parent, int id, const std::vector<std::wstring>& items) {
    HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (!h) return h;
    darkControl(h, L"DarkMode_CFD");
    SetWindowSubclass(h, comboProc, kComboSubclassId, 0);
    for (const auto& item : items) SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)item.c_str());
    return h;
}

HWND createEdit(HWND parent, int id, bool multiline, bool readOnly, int maxChars) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    style |= multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL) : ES_AUTOHSCROLL;
    if (readOnly) style |= ES_READONLY;
    // Su più righe senza bordo (il riquadro più chiaro basta, come la cronologia):
    // il bordo del tema scuro qui sarebbe bianco.
    HWND h = CreateWindowExW(multiline ? 0 : WS_EX_CLIENTEDGE, WC_EDITW, L"", style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(nullptr), nullptr);
    if (!h) return h;
    darkControl(h, multiline ? L"DarkMode_Explorer" : L"DarkMode_CFD");
    SetWindowLongPtrW(h, GWLP_USERDATA, packStyle(Color::Text, Color::Input));
    if (maxChars > 0) SendMessageW(h, EM_SETLIMITTEXT, maxChars, 0);
    if (multiline) SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    return h;
}

LRESULT ctlColor(UINT msg, HDC hdc, HWND control) {
    LONG_PTR style = GetWindowLongPtrW(control, GWLP_USERDATA);
    if (style & kStyleMarker) {
        COLORREF fg = (COLORREF)(style & 0xFFFFFF), bg = (COLORREF)((style >> 24) & 0xFFFFFF);
        SetTextColor(hdc, fg);
        SetBkColor(hdc, bg);
        return (LRESULT)brush(bg);
    }
    if (msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLOREDIT) {
        // La lista che si apre sotto un menu a tendina.
        SetTextColor(hdc, Color::Text);
        SetBkColor(hdc, Color::Input);
        return (LRESULT)brush(Color::Input);
    }
    return 0;
}

void fillRound(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::RectF rect((float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
    Gdiplus::GraphicsPath path;
    roundPath(path, rect, (float)radius);
    Gdiplus::SolidBrush b(gdiColor(fill));
    g.FillPath(&b, &path);
    if (border != CLR_INVALID) {
        Gdiplus::RectF inner(rect.X + 0.5f, rect.Y + 0.5f, rect.Width - 1.0f, rect.Height - 1.0f);
        Gdiplus::GraphicsPath borderPath;
        roundPath(borderPath, inner, (float)radius);
        Gdiplus::Pen pen(gdiColor(border), 1.0f);
        g.DrawPath(&pen, &borderPath);
    }
}

void fillCircle(HDC hdc, int cx, int cy, int radius, COLORREF fill) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush b(gdiColor(fill));
    g.FillEllipse(&b, (float)(cx - radius), (float)(cy - radius), (float)(radius * 2), (float)(radius * 2));
}

void drawText(HDC hdc, const std::wstring& text, RECT r, HFONT f, COLORREF color, UINT format) {
    HGDIOBJ old = SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), (int)text.size(), &r, format | DT_NOPREFIX);
    SelectObject(hdc, old);
}

int textHeight(HDC hdc, const std::wstring& text, int width, HFONT f) {
    HGDIOBJ old = SelectObject(hdc, f);
    RECT r{ 0, 0, std::max(1, width), 0 };
    DrawTextW(hdc, text.empty() ? L" " : text.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(hdc, old);
    return r.bottom - r.top;
}

Buffered::Buffered(HDC target, const RECT& area) : m_target(target), m_area(area) {
    const int w = area.right, h = area.bottom;
    if (area.right <= area.left || area.bottom <= area.top || w <= 0 || h <= 0) return;
    m_dc = CreateCompatibleDC(target);
    m_bitmap = CreateCompatibleBitmap(target, w, h);
    if (!m_dc || !m_bitmap) {
        if (m_bitmap) DeleteObject(m_bitmap);
        if (m_dc) DeleteDC(m_dc);
        m_dc = nullptr;
        m_bitmap = nullptr;
        return;
    }
    m_old = SelectObject(m_dc, m_bitmap);
}

Buffered::~Buffered() {
    if (!m_dc) return;
    BitBlt(m_target, m_area.left, m_area.top, m_area.right - m_area.left, m_area.bottom - m_area.top, m_dc, m_area.left, m_area.top, SRCCOPY);
    SelectObject(m_dc, m_old);
    DeleteObject(m_bitmap);
    DeleteDC(m_dc);
}

HWND Form::place(HWND control, int x, int y, int w, int h, Font role) {
    if (!control) return control;
    m_items.push_back({ control, RECT{ x, y, x + w, y + h }, role });
    SendMessageW(control, WM_SETFONT, (WPARAM)font(role, m_dpi), FALSE);
    if (isCombo(control)) {
        SendMessageW(control, CB_SETITEMHEIGHT, (WPARAM)-1, px(22, m_dpi));
        SendMessageW(control, CB_SETITEMHEIGHT, 0, px(22, m_dpi));
    }
    SetWindowPos(control, nullptr, px(x, m_dpi), px(y, m_dpi), px(w, m_dpi), px(h, m_dpi), SWP_NOZORDER | SWP_NOACTIVATE);
    return control;
}

void Form::move(HWND control, int x, int y, int w, int h) {
    for (auto& item : m_items) {
        if (item.hwnd != control) continue;
        item.r = RECT{ x, y, x + w, y + h };
        SetWindowPos(control, nullptr, px(x, m_dpi), px(y, m_dpi), px(w, m_dpi), px(h, m_dpi), SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
}

void Form::card(int x, int y, int w, int h, const std::wstring& title) {
    m_cards.push_back({ RECT{ x, y, x + w, y + h }, title });
}

void Form::applyDpi(UINT dpi) {
    m_dpi = dpi;
    // Gli elementi tolti nel frattempo (righe ricostruite) si dimenticano.
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [](const Item& i) { return !IsWindow(i.hwnd); }), m_items.end());
    for (const auto& item : m_items) {
        SendMessageW(item.hwnd, WM_SETFONT, (WPARAM)font(item.role, dpi), FALSE);
        if (isCombo(item.hwnd)) {
            SendMessageW(item.hwnd, CB_SETITEMHEIGHT, (WPARAM)-1, px(22, dpi));
            SendMessageW(item.hwnd, CB_SETITEMHEIGHT, 0, px(22, dpi));
        }
        SetWindowPos(item.hwnd, nullptr, px(item.r.left, dpi), px(item.r.top, dpi), px(item.r.right - item.r.left, dpi),
                     px(item.r.bottom - item.r.top, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(item.hwnd, nullptr, TRUE);
    }
}

void Form::paintCards(HDC hdc, UINT dpi) const {
    for (const auto& c : m_cards) {
        RECT r{ px(c.r.left, dpi), px(c.r.top, dpi), px(c.r.right, dpi), px(c.r.bottom, dpi) };
        fillRound(hdc, r, px(10, dpi), Color::Panel, Color::PanelBorder);
        if (!c.title.empty()) {
            RECT tr{ r.left + px(16, dpi), r.top + px(12, dpi), r.right - px(16, dpi), r.top + px(30, dpi) };
            drawText(hdc, c.title, tr, font(Font::Header, dpi), Color::Accent, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
    }
}

int Form::bottom() const {
    int b = 0;
    for (const auto& item : m_items) {
        if (IsWindow(item.hwnd)) b = std::max(b, (int)item.r.bottom);
    }
    for (const auto& c : m_cards) b = std::max(b, (int)c.r.bottom);
    return b;
}

} // namespace Ui
