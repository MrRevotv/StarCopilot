#include "pages.h"
#include "orb.h"
#include "../app_paths.h"
#include "../audio_capture.h"
#include "../game_input.h"
#include "../joystick_input.h"
#include "../overlay.h"
#include "../text_utils.h"
#include "../tts_engine.h"
#include "../voice_effect.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
    // Misure dei pannelli (a 96 DPI).
    constexpr int kCardX = 16;
    constexpr int kCardW = kPageWidth - 2 * kCardX; // 568
    constexpr int kIn = kCardX + 18;                // dove comincia il contenuto
    constexpr int kInW = kCardW - 36;               // larghezza del contenuto

    std::wstring w(const std::string& s) { return TextUtils::utf8ToWide(s); }
    std::string u8(const std::wstring& s) { return TextUtils::wideToUtf8(s); }

    std::wstring windowText(HWND h) {
        int len = GetWindowTextLengthW(h);
        std::wstring t(len + 1, L'\0');
        GetWindowTextW(h, t.data(), len + 1);
        t.resize(len);
        return t;
    }

    std::wstring percent(int alpha) { return std::to_wstring((alpha * 100 + 127) / 255) + L"%"; }

    std::wstring decimal(double v, int digits) {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.*f", digits, v);
        std::wstring s = buf;
        std::replace(s.begin(), s.end(), L'.', L',');
        return s;
    }

    void openFile(HWND owner, const std::wstring& path, bool notepad) {
        if (notepad) {
            std::wstring args = L"\"" + path + L"\"";
            ShellExecuteW(owner, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void copyToClipboard(HWND owner, const std::wstring& text) {
        if (!OpenClipboard(owner)) return;
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (mem) {
            memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
    }

    // Invio dentro una casella di testo = clic sul pulsante accanto.
    LRESULT CALLBACK enterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR buttonId) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) {
            SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM((int)buttonId, BN_CLICKED), 0);
            return 0;
        }
        if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, enterProc, id);
        return DefSubclassProc(h, msg, wp, lp);
    }

    // Barra del volume del microfono: verde quando Iris sente che qualcuno parla.
    void drawMeter(const DRAWITEMSTRUCT* dis, float level, bool speaking) {
        const RECT r = dis->rcItem;
        Ui::Buffered buffer(dis->hDC, r);
        HDC dc = buffer.dc();
        FillRect(dc, &r, Ui::brush(Ui::Color::Panel));
        const int h = r.bottom - r.top;
        Ui::fillRound(dc, r, h / 2, Ui::Color::Input);
        double db = 20.0 * std::log10(std::max((double)level, 1e-5));
        double pos = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        const int x = r.left + (int)(pos * (r.right - r.left));
        if (x > r.left + h / 2) {
            RECT fill{ r.left, r.top, x, r.bottom };
            Ui::fillRound(dc, fill, h / 2, speaking ? Ui::Color::Ok : Ui::Color::Idle);
        }
    }

    HWND createMeter(HWND parent, int id) {
        return CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    }

    int measureText(const std::wstring& text, int width, Ui::Font f, UINT dpi) {
        HDC dc = GetDC(nullptr);
        int h = Ui::textHeight(dc, text, Ui::px(width, dpi), Ui::font(f, dpi));
        ReleaseDC(nullptr, dc);
        return MulDiv(h, 96, (int)dpi) + 2;
    }

    void centerLabel(HWND label) {
        LONG_PTR style = GetWindowLongPtrW(label, GWL_STYLE);
        SetWindowLongPtrW(label, GWL_STYLE, (style & ~(LONG_PTR)(SS_LEFT | SS_RIGHT)) | SS_CENTER | SS_ENDELLIPSIS);
    }

    // --- La conversazione ------------------------------------------------------------
    // Disegnata da noi, a "fumetti": le tue richieste a destra, le risposte di
    // Iris a sinistra, quello che sta facendo (ricerche...) in piccolo al centro.
    // Resta in fondo da sola, a meno che tu non stia leggendo più su.
    // Doppio clic su un messaggio = copialo.
    class ChatView {
    public:
        static HWND create(HWND parent, int id) {
            static bool registered = false;
            if (!registered) {
                WNDCLASSW wc{};
                wc.style = CS_DBLCLKS;
                wc.lpfnWndProc = proc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"IrisChat";
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                RegisterClassW(&wc);
                registered = true;
            }
            auto* self = new ChatView();
            HWND h = CreateWindowExW(0, L"IrisChat", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                                     GetModuleHandleW(nullptr), self);
            if (h) Ui::darkControl(h);
            else delete self;
            return h;
        }

        static void setEntries(HWND h, std::vector<Assistant::Entry> entries) {
            auto* self = get(h);
            if (!self) return;
            RECT rc;
            GetClientRect(h, &rc);
            const bool atBottom = self->m_scroll >= self->m_total - (int)rc.bottom - 4;
            self->m_entries = std::move(entries);
            self->m_stickBottom = atBottom || self->m_stickBottom;
            InvalidateRect(h, nullptr, FALSE);
        }

    private:
        struct Layout {
            RECT bubble; // senza scorrimento
            RECT text;
        };
        std::vector<Assistant::Entry> m_entries;
        std::vector<Layout> m_layout;
        int m_total = 0;
        int m_scroll = 0;
        bool m_stickBottom = true;

        static ChatView* get(HWND h) { return reinterpret_cast<ChatView*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

        static std::wstring textOf(const Assistant::Entry& e) {
            std::wstring t = w(e.text);
            if (e.kind == Assistant::Entry::Kind::Tool) t = L"› " + t;
            if (e.pending) t += L" ▍";
            return t;
        }

        static Ui::Font fontOf(const Assistant::Entry& e) {
            return e.kind == Assistant::Entry::Kind::Tool || e.kind == Assistant::Entry::Kind::Info ? Ui::Font::Small : Ui::Font::Normal;
        }

        static SIZE measure(HDC dc, const std::wstring& text, int maxWidth, HFONT f) {
            HGDIOBJ old = SelectObject(dc, f);
            RECT r{ 0, 0, std::max(1, maxWidth), 0 };
            DrawTextW(dc, text.empty() ? L" " : text.c_str(), -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc, old);
            return SIZE{ r.right - r.left, r.bottom - r.top };
        }

        void layout(HWND h, int width, UINT dpi) {
            m_layout.clear();
            HDC dc = GetDC(h);
            const int pad = Ui::px(10, dpi), inX = Ui::px(12, dpi), inY = Ui::px(8, dpi), gap = Ui::px(8, dpi);
            const int maxBubble = width * 78 / 100;
            int y = pad;
            for (const auto& e : m_entries) {
                const std::wstring t = textOf(e);
                Layout l{};
                if (e.kind == Assistant::Entry::Kind::User || e.kind == Assistant::Entry::Kind::Iris) {
                    SIZE s = measure(dc, t, maxBubble - 2 * inX, Ui::font(Ui::Font::Normal, dpi));
                    const int bw = s.cx + 2 * inX, bh = s.cy + 2 * inY;
                    const int x = e.kind == Assistant::Entry::Kind::User ? width - pad - bw : pad;
                    l.bubble = RECT{ x, y, x + bw, y + bh };
                    l.text = RECT{ x + inX, y + inY, x + bw - inX, y + bh - inY };
                    y += bh + gap;
                } else {
                    SIZE s = measure(dc, t, width - 4 * pad, Ui::font(Ui::Font::Small, dpi));
                    l.text = RECT{ 2 * pad, y, width - 2 * pad, y + s.cy };
                    l.bubble = l.text;
                    y += s.cy + gap;
                }
                m_layout.push_back(l);
            }
            ReleaseDC(h, dc);
            m_total = y + pad - gap;
        }

        void updateScrollBar(HWND h, int viewHeight) {
            if (m_stickBottom) m_scroll = std::max(0, m_total - viewHeight);
            m_scroll = std::clamp(m_scroll, 0, std::max(0, m_total - viewHeight));
            SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL };
            si.nMin = 0;
            si.nMax = std::max(0, m_total - 1);
            si.nPage = (UINT)viewHeight;
            si.nPos = m_scroll;
            SetScrollInfo(h, SB_VERT, &si, TRUE);
        }

        void paint(HWND h) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const UINT dpi = Ui::dpiOf(h);
            layout(h, rc.right, dpi);
            updateScrollBar(h, rc.bottom);
            {
                Ui::Buffered buffer(hdc, rc);
                HDC dc = buffer.dc();
                FillRect(dc, &rc, Ui::brush(RGB(19, 21, 27)));
                if (m_entries.empty()) {
                    RECT t{ Ui::px(16, dpi), Ui::px(16, dpi), rc.right - Ui::px(16, dpi), rc.bottom - Ui::px(16, dpi) };
                    Ui::drawText(dc,
                                 L"Chiamami dicendo \"Iris\" e poi cosa ti serve, per esempio: \"Iris, che tempo fa domani?\", "
                                 L"\"Iris, cerca le ultime notizie su Star Citizen\", \"Iris, metti un timer di 10 minuti\".\n\n"
                                 L"Oppure scrivimi qui sotto.",
                                 t, Ui::font(Ui::Font::Normal, dpi), Ui::Color::TextDim, DT_LEFT | DT_WORDBREAK);
                }
                for (size_t i = 0; i < m_entries.size() && i < m_layout.size(); ++i) {
                    const auto& e = m_entries[i];
                    RECT b = m_layout[i].bubble, t = m_layout[i].text;
                    OffsetRect(&b, 0, -m_scroll);
                    OffsetRect(&t, 0, -m_scroll);
                    if (b.bottom < 0 || b.top > rc.bottom) continue;
                    switch (e.kind) {
                    case Assistant::Entry::Kind::User:
                        Ui::fillRound(dc, b, Ui::px(10, dpi), RGB(22, 82, 110));
                        Ui::drawText(dc, textOf(e), t, Ui::font(Ui::Font::Normal, dpi), Ui::Color::Text, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
                        break;
                    case Assistant::Entry::Kind::Iris:
                        Ui::fillRound(dc, b, Ui::px(10, dpi), Ui::Color::Secondary);
                        Ui::drawText(dc, textOf(e), t, Ui::font(Ui::Font::Normal, dpi), Ui::Color::Text, DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL);
                        break;
                    case Assistant::Entry::Kind::Tool:
                        Ui::drawText(dc, textOf(e) + L"  · " + w(e.time), t, Ui::font(Ui::Font::Small, dpi), RGB(90, 160, 190),
                                     DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);
                        break;
                    case Assistant::Entry::Kind::Info:
                        Ui::drawText(dc, textOf(e) + L"  · " + w(e.time), t, Ui::font(Ui::Font::Small, dpi), Ui::Color::TextDim,
                                     DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);
                        break;
                    }
                }
            }
            EndPaint(h, &ps);
        }

        void scrollTo(HWND h, int pos) {
            RECT rc;
            GetClientRect(h, &rc);
            int old = m_scroll;
            m_scroll = std::clamp(pos, 0, std::max(0, m_total - (int)rc.bottom));
            m_stickBottom = m_scroll >= m_total - (int)rc.bottom - 4;
            if (m_scroll != old) InvalidateRect(h, nullptr, FALSE);
        }

        static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
            if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            ChatView* self = get(h);
            if (!self) return DefWindowProcW(h, msg, wp, lp);
            switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                self->paint(h);
                return 0;
            case WM_SIZE:
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            case WM_MOUSEWHEEL: {
                RECT rc;
                GetClientRect(h, &rc);
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                bool atTop = self->m_scroll <= 0, atBottom = self->m_scroll >= self->m_total - (int)rc.bottom;
                if ((delta > 0 && atTop) || (delta < 0 && atBottom)) return SendMessageW(GetParent(h), msg, wp, lp);
                self->scrollTo(h, self->m_scroll - delta * Ui::px(48, Ui::dpiOf(h)) / WHEEL_DELTA);
                return 0;
            }
            case WM_VSCROLL: {
                SCROLLINFO si{ sizeof(si), SIF_ALL };
                GetScrollInfo(h, SB_VERT, &si);
                int pos = si.nPos, step = Ui::px(30, Ui::dpiOf(h));
                switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= step; break;
                case SB_LINEDOWN: pos += step; break;
                case SB_PAGEUP: pos -= (int)si.nPage; break;
                case SB_PAGEDOWN: pos += (int)si.nPage; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = self->m_total; break;
                }
                self->scrollTo(h, pos);
                return 0;
            }
            case WM_LBUTTONDBLCLK: {
                const int y = GET_Y_LPARAM(lp) + self->m_scroll, x = GET_X_LPARAM(lp);
                for (size_t i = 0; i < self->m_layout.size() && i < self->m_entries.size(); ++i) {
                    const RECT& b = self->m_layout[i].bubble;
                    if (y >= b.top && y < b.bottom && x >= b.left && x < b.right) {
                        copyToClipboard(h, w(self->m_entries[i].text));
                        break;
                    }
                }
                return 0;
            }
            case WM_NCDESTROY:
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                delete self;
                return 0;
            }
            return DefWindowProcW(h, msg, wp, lp);
        }
    };

    Orb::Mode orbMode(Assistant::State s) {
        switch (s) {
        case Assistant::State::Starting: return Orb::Mode::Starting;
        case Assistant::State::Idle: return Orb::Mode::Idle;
        case Assistant::State::Listening: return Orb::Mode::Listening;
        case Assistant::State::Thinking: return Orb::Mode::Thinking;
        case Assistant::State::Speaking: return Orb::Mode::Speaking;
        case Assistant::State::Error: return Orb::Mode::Error;
        }
        return Orb::Mode::Idle;
    }
}

// --- Stato in parole e colori ------------------------------------------------------

std::wstring stateLabel(Assistant::State state) {
    switch (state) {
    case Assistant::State::Starting: return L"AVVIO";
    case Assistant::State::Idle: return L"IN ATTESA";
    case Assistant::State::Listening: return L"TI ASCOLTO";
    case Assistant::State::Thinking: return L"CI PENSO";
    case Assistant::State::Speaking: return L"PARLO";
    case Assistant::State::Error: return L"ATTENZIONE";
    }
    return L"";
}

COLORREF stateColor(Assistant::State state) {
    switch (state) {
    case Assistant::State::Starting: return Ui::Color::Busy;
    case Assistant::State::Idle: return Ui::Color::Accent;
    case Assistant::State::Listening: return Ui::Color::Ok;
    case Assistant::State::Thinking: return Ui::Color::Busy;
    case Assistant::State::Speaking: return Ui::Color::AccentHover;
    case Assistant::State::Error: return Ui::Color::Error;
    }
    return Ui::Color::Idle;
}

// --- Pagina: parte comune ------------------------------------------------------------

Page::Page(Assistant& assistant, const wchar_t* title) : m_assistant(assistant), m_title(title) {}

Page::~Page() {
    if (m_hwnd && IsWindow(m_hwnd)) SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
}

HWND Page::create(HWND parent) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"IrisPage";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    m_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"IrisPage", m_title, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr,
                             GetModuleHandleW(nullptr), this);
    m_dpi = Ui::dpiOf(parent);
    m_form.attach(m_hwnd);
    m_form.applyDpi(m_dpi);
    build();
    return m_hwnd;
}

int Page::contentHeight() const {
    return m_form.bottom() + 16;
}

void Page::applyDpi(UINT dpi) {
    if (dpi == m_dpi) return;
    m_dpi = dpi;
    rebuild();
}

void Page::rebuild() {
    SendMessageW(m_hwnd, WM_SETREDRAW, FALSE, 0);
    HWND child;
    while ((child = GetWindow(m_hwnd, GW_CHILD)) != nullptr) DestroyWindow(child);
    m_form = Ui::Form{};
    m_form.attach(m_hwnd);
    m_form.applyDpi(m_dpi);
    build();
    SendMessageW(m_hwnd, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
    contentChanged();
}

void Page::contentChanged() {
    PostMessageW(GetAncestor(m_hwnd, GA_ROOT), WM_APP_CONTENT_CHANGED, 0, 0);
}

int Page::beginCard(int y) {
    return y + 42;
}

int Page::endCard(int top, int y, const std::wstring& title) {
    y += 18;
    m_form.card(kCardX, top, kCardW, y - top, title);
    return y + 14;
}

HWND Page::label(const std::wstring& text, int x, int y, int width, int h, COLORREF color, Ui::Font font, int id) {
    return m_form.place(Ui::createLabel(m_hwnd, text, color, Ui::Color::Panel, id), x, y, width, h, font);
}

int Page::note(const std::wstring& text, int x, int y, int width, COLORREF color, HWND* out) {
    int h = measureText(text, width, Ui::Font::Normal, m_dpi);
    HWND l = label(text, x, y, width, h, color);
    if (out) *out = l;
    return y + h + 6;
}

HWND Page::button(int id, const std::wstring& text, Ui::ButtonKind kind, int x, int y, int width, int h) {
    return m_form.place(Ui::createButton(m_hwnd, id, text, kind, Ui::Color::Panel), x, y, width, h);
}

HWND Page::toggle(int id, const std::wstring& text, int x, int y, int width) {
    return m_form.place(Ui::createButton(m_hwnd, id, text, Ui::ButtonKind::Toggle, Ui::Color::Panel), x, y, width, 30);
}

HWND Page::combo(int id, const std::vector<std::wstring>& items, int x, int y, int width) {
    return m_form.place(Ui::createCombo(m_hwnd, id, items), x, y, width, 320);
}

LRESULT CALLBACK Page::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    auto* page = reinterpret_cast<Page*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!page) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_COMMAND:
        page->onCommand(LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (Ui::drawButton(dis) || page->onDrawItem(dis)) return TRUE;
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        LRESULT r = Ui::ctlColor(msg, (HDC)wp, (HWND)lp);
        if (r) return r;
        break;
    }
    case WM_CTLCOLORBTN:
        return (LRESULT)Ui::brush(Ui::Color::Panel);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        {
            Ui::Buffered buffer(hdc, ps.rcPaint);
            HDC dc = buffer.dc();
            FillRect(dc, &ps.rcPaint, Ui::brush(Ui::Color::Background));
            page->m_form.paintCards(dc, page->m_dpi);
            page->paintExtra(dc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    if (msg >= WM_APP && msg < 0xC000) return page->onAppMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// =====================================================================================
// Iris: la sfera, lo stato, la conversazione, la casella per scriverle
// =====================================================================================

namespace {

class IrisPage : public Page {
public:
    explicit IrisPage(Assistant& a) : Page(a, L"Iris") {}

    void onShow() override {
        updateStatus();
        fillConversation();
        updateHeard();
    }
    void onEngineEvent(Assistant::Event e) override {
        if (e == Assistant::Event::Status) updateStatus();
        else if (e == Assistant::Event::Conversation) fillConversation();
        else if (e == Assistant::Event::Heard) updateHeard();
    }
    void onTimer() override {
        if (m_orb) Orb::update(m_orb, orbMode(m_state), m_assistant.micLevel(), m_assistant.voiceLevel(), m_assistant.userSpeaking());
    }

protected:
    enum { ID_ORB = 100, ID_RETRY, ID_CHAT, ID_EDIT, ID_SEND, ID_STOP, ID_NEW };

    void build() override {
        int y = 16;
        int top = y;
        y += 12;
        m_orb = m_form.place(Orb::create(m_hwnd, ID_ORB), kIn, y, kInW, 176);
        y += 178;
        m_stateLabel = label(L"AVVIO", kIn, y, kInW, 28, Ui::Color::Text, Ui::Font::Status);
        centerLabel(m_stateLabel);
        y += 30;
        const int detailHeight = measureText(L"x\nx", kInW, Ui::Font::Normal, m_dpi);
        m_detail = label(L"", kIn, y, kInW, detailHeight, Ui::Color::TextDim);
        centerLabel(m_detail);
        y += detailHeight + 2;
        m_heard = label(L"", kIn, y, kInW, 18, Ui::Color::TextDim, Ui::Font::Small);
        centerLabel(m_heard);
        // "Riprova" compare solo se l'avvio fallisce, al posto dell'ultima frase sentita.
        m_retry = button(ID_RETRY, L"Riprova", Ui::ButtonKind::Primary, kIn + (kInW - 140) / 2, y - 6, 140);
        ShowWindow(m_retry, SW_HIDE);
        y += 18;
        y = endCard(top, y - 8, L"");

        top = y;
        y = beginCard(y);
        m_chat = m_form.place(ChatView::create(m_hwnd, ID_CHAT), kIn, y, kInW, 262);
        y += 270;
        m_edit = m_form.place(Ui::createEdit(m_hwnd, ID_EDIT, false, false, 2000), kIn, y + 1, kInW - 112, 28);
        SetWindowSubclass(m_edit, enterProc, 1, ID_SEND);
        SendMessageW(m_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Scrivi a Iris... (Invio per mandare)");
        button(ID_SEND, L"Invia", Ui::ButtonKind::Primary, kIn + kInW - 102, y, 102, 30);
        y += 38;
        button(ID_STOP, L"Zitta", Ui::ButtonKind::Secondary, kIn, y, 120);
        button(ID_NEW, L"Nuova conversazione", Ui::ButtonKind::Secondary, kIn + 130, y, 200);
        label(L"Doppio clic = copia", kIn + 340, y + 8, kInW - 340, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 32;
        endCard(top, y, L"CONVERSAZIONE");
        onShow();
    }

    void onCommand(int id, int code, HWND) override {
        switch (id) {
        case ID_SEND: {
            std::string text = TextUtils::trim(u8(windowText(m_edit)));
            if (text.empty()) break;
            m_assistant.sendText(text);
            SetWindowTextW(m_edit, L"");
            break;
        }
        case ID_STOP:
            if (code == BN_CLICKED) m_assistant.stopSpeaking();
            break;
        case ID_NEW:
            if (code == BN_CLICKED) m_assistant.newConversation();
            break;
        case ID_RETRY:
            if (code == BN_CLICKED) m_assistant.retryStart();
            break;
        }
    }

private:
    void updateStatus() {
        Assistant::Status st = m_assistant.status();
        m_state = st.state;
        Ui::setText(m_stateLabel, stateLabel(st.state));
        Ui::setLabelColor(m_stateLabel, stateColor(st.state));
        Ui::setText(m_detail, w(st.text));
        ShowWindow(m_retry, st.canRetry ? SW_SHOWNA : SW_HIDE);
    }

    void updateHeard() {
        std::string heard = m_assistant.lastHeard();
        Ui::setText(m_heard, heard.empty() ? L"" : L"Ultima cosa sentita: «" + w(heard) + L"»");
    }

    void fillConversation() { ChatView::setEntries(m_chat, m_assistant.conversation()); }

    HWND m_orb = nullptr, m_stateLabel = nullptr, m_detail = nullptr, m_heard = nullptr, m_retry = nullptr;
    HWND m_chat = nullptr, m_edit = nullptr;
    Assistant::State m_state = Assistant::State::Starting;
};

// =====================================================================================
// Ascolto e voce: "Iris", microfono, tasti per parlare, voce
// =====================================================================================

class VoicePage : public Page {
public:
    explicit VoicePage(Assistant& a) : Page(a, L"Ascolto e voce") {}
    ~VoicePage() override { stopCapture(); }

    void onShow() override { refresh(); }
    void onEngineEvent(Assistant::Event e) override {
        if (e != Assistant::Event::Settings) return;
        if (m_assistant.settings().triggers.size() != m_rows) PostMessageW(m_hwnd, WM_APP_REBUILD, 0, 0);
        else refresh();
    }
    void onTimer() override {
        if (m_meter) InvalidateRect(m_meter, nullptr, FALSE);
        // Probabilità di voce e rumore di fondo: per capire perché non ti sente.
        if (m_diag && ++m_ticks % 6 == 0) {
            wchar_t buf[160];
            swprintf(buf, 160, L"Voce: %d%%   ·   volume %.4f   ·   rumore di fondo %.4f%s", (int)std::lround(m_assistant.voiceProbability() * 100),
                     m_assistant.micLevel(), m_assistant.noiseFloor(), m_assistant.userSpeaking() ? L"   ·   TI SENTO" : L"");
            std::wstring t = buf;
            std::replace(t.begin(), t.end(), L'.', L',');
            Ui::setText(m_diag, t);
            Ui::setLabelColor(m_diag, m_assistant.userSpeaking() ? Ui::Color::Ok : Ui::Color::TextDim);
        }
    }

protected:
    enum {
        ID_WAKE = 100, ID_METER, ID_MIC, ID_SENS_MINUS, ID_SENS_PLUS, ID_PAUSE_MINUS, ID_PAUSE_PLUS, ID_FOLLOW_MINUS, ID_FOLLOW_PLUS,
        ID_GAIN_MINUS, ID_GAIN_PLUS, ID_ADD, ID_SPEECH, ID_VOICE, ID_RATE_MINUS, ID_RATE_PLUS, ID_TEST, ID_OUTPUT, ID_ENGINE, ID_EFFECT,
        ID_REMOVE_BASE = 200
    };

    void build() override {
        AppSettings s = m_assistant.settings();
        int y = 16;

        // --- Ascolto
        int top = y;
        y = beginCard(y);
        m_wake = toggle(ID_WAKE, L"Sempre in ascolto: si sveglia quando dici \"Iris\"", kIn, y, kInW);
        y += 36;
        y = note(L"Puoi dire tutto insieme (\"Iris, che ore sono?\") oppure solo \"Iris\", aspettare il suono e poi parlare. "
                 L"Il microfono resta sul PC: si trascrive tutto qui e niente parte verso internet.",
                 kIn, y, kInW);
        y += 4;
        m_meter = m_form.place(createMeter(m_hwnd, ID_METER), kIn, y + 4, kInW, 12);
        y += 22;
        m_diag = label(L"", kIn, y, kInW, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 24;
        label(L"Microfono", kIn, y, 200, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 20;
        m_mic = combo(ID_MIC, {}, kIn, y, kInW);
        fillMicrophones();
        y += 42;
        auto row = [&](HWND& lbl, int minus, int plus) {
            lbl = label(L"", kIn, y + 6, kInW - 90, 22);
            button(minus, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
            button(plus, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
            y += 36;
        };
        row(m_sensLabel, ID_SENS_MINUS, ID_SENS_PLUS);
        row(m_pauseLabel, ID_PAUSE_MINUS, ID_PAUSE_PLUS);
        row(m_followLabel, ID_FOLLOW_MINUS, ID_FOLLOW_PLUS);
        row(m_gainLabel, ID_GAIN_MINUS, ID_GAIN_PLUS);
        y = note(L"La barra diventa verde quando Iris sente che qualcuno parla. Se si sveglia da sola con i rumori, abbassa la "
                 L"sensibilità; se non ti sente, alzala o alza il volume del microfono.",
                 kIn, y + 2, kInW);
        y = endCard(top, y - 6, L"ASCOLTO");

        // --- Tasti per parlare
        top = y;
        y = beginCard(y);
        y = note(L"Facoltativi: tieni premuto un tasto (o un pulsante del joystick) mentre parli, senza bisogno di dire \"Iris\". "
                 L"Premerlo mentre Iris parla la zittisce.",
                 kIn, y, kInW);
        y += 4;
        m_rows = s.triggers.size();
        m_triggerLabels.clear();
        for (size_t i = 0; i < s.triggers.size(); ++i) {
            m_triggerLabels.push_back(label(L"", kIn, y + 6, kInW - 110, 22));
            button(ID_REMOVE_BASE + (int)i, L"Togli", Ui::ButtonKind::Danger, kIn + kInW - 96, y, 96, 30);
            y += 36;
        }
        m_add = button(ID_ADD, L"+ Aggiungi un tasto", Ui::ButtonKind::Primary, kIn, y, 210);
        if (s.triggers.size() >= 6) EnableWindow(m_add, FALSE);
        m_captureLabel = label(L"", kIn + 222, y + 6, kInW - 222, 22, Ui::Color::Accent);
        y += 32;
        y = endCard(top, y, L"TASTI PER PARLARE");

        // --- Voce
        top = y;
        y = beginCard(y);
        m_speech = toggle(ID_SPEECH, L"Iris risponde a voce", kIn, y, kInW);
        y += 38;
        label(L"Motore della voce", kIn, y, 300, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 20;
        m_engine = combo(ID_ENGINE, { L"Neurale: Qwen3-TTS (naturale, sulla scheda video, +3 GB)", L"Windows (leggera, sul processore)" },
                         kIn, y, kInW);
        y += 42;
        label(L"Effetto sulla voce (voce da macchina, apposta)", kIn, y, kInW, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 20;
        std::vector<std::wstring> effectNames;
        for (const auto& e : VoiceEffect::presets()) effectNames.push_back(w(e.label));
        m_effect = combo(ID_EFFECT, effectNames, kIn, y, kInW);
        y += 42;
        label(L"Voce di Windows (quando il motore è Windows, o se la voce neurale non è disponibile)", kIn, y, kInW, 18,
              Ui::Color::TextDim, Ui::Font::Small);
        y += 20;
        m_voices = TtsEngine::voices();
        std::vector<std::wstring> voiceNames;
        for (const auto& v : m_voices) voiceNames.push_back(w(v));
        if (voiceNames.empty()) voiceNames.push_back(L"(nessuna voce di Windows trovata)");
        m_voice = combo(ID_VOICE, voiceNames, kIn, y, kInW - 160);
        button(ID_TEST, L"Prova la voce", Ui::ButtonKind::Secondary, kIn + kInW - 150, y - 1, 150, 30);
        y += 42;
        label(L"Dove parla (uscita audio)", kIn, y, 300, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 20;
        m_output = combo(ID_OUTPUT, {}, kIn, y, kInW);
        fillOutputs();
        y += 42;
        row(m_rateLabel, ID_RATE_MINUS, ID_RATE_PLUS);
        y = note(L"La voce neurale è quella di base di Qwen3-TTS (licenza Apache 2.0): genera la voce sulla scheda video, circa "
                 L"4 volte più veloce del parlato. La velocità qui sopra vale solo per le voci di Windows.",
                 kIn, y + 2, kInW);
        endCard(top, y - 6, L"VOCE");
        refresh();
    }

    bool onDrawItem(const DRAWITEMSTRUCT* dis) override {
        if (dis->CtlID != ID_METER) return false;
        drawMeter(dis, m_assistant.micLevel(), m_assistant.userSpeaking());
        return true;
    }

    void onCommand(int id, int code, HWND) override {
        // Gli elenchi si rifanno ogni volta che si aprono: una periferica collegata
        // dopo l'avvio compare subito, senza riavviare il programma.
        if (id == ID_MIC && code == CBN_DROPDOWN) {
            fillMicrophones();
            return;
        }
        if (id == ID_OUTPUT && code == CBN_DROPDOWN) {
            fillOutputs();
            return;
        }
        if (id == ID_MIC && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(m_mic, CB_GETCURSEL, 0, 0);
            std::vector<std::string> names;
            if (sel > 0 && sel - 1 < (int)m_devices.size()) names.push_back(m_devices[sel - 1]);
            m_assistant.updateSettings([&](AppSettings& s) { s.deviceNames = names; });
            return;
        }
        if (id == ID_ENGINE && code == CBN_SELCHANGE) {
            const bool neural = SendMessageW(m_engine, CB_GETCURSEL, 0, 0) == 0;
            m_assistant.updateSettings([&](AppSettings& s) { s.voiceEngine = neural ? "neurale" : "windows"; });
            if (!neural) m_assistant.testVoice();
            return;
        }
        if (id == ID_EFFECT && code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW(m_effect, CB_GETCURSEL, 0, 0);
            const auto& list = VoiceEffect::presets();
            if (sel >= 0 && sel < (int)list.size()) {
                const std::string id = list[sel].id;
                m_assistant.updateSettings([&](AppSettings& s) { s.voiceEffect = id; });
                m_assistant.testVoice();
            }
            return;
        }
        if (id == ID_OUTPUT && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(m_output, CB_GETCURSEL, 0, 0);
            std::string name = sel > 0 && sel - 1 < (int)m_outputs.size() ? m_outputs[sel - 1] : std::string();
            m_assistant.updateSettings([&](AppSettings& s) { s.outputDeviceName = name; });
            m_assistant.testVoice();
            return;
        }
        if (id == ID_VOICE && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(m_voice, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)m_voices.size()) {
                // "Microsoft Elsa (it-IT)" -> "Microsoft Elsa"
                std::string name = m_voices[sel];
                size_t paren = name.rfind(" (");
                if (paren != std::string::npos) name = name.substr(0, paren);
                m_assistant.updateSettings([&](AppSettings& s) { s.voiceName = name; });
                m_assistant.testVoice();
            }
            return;
        }
        if (code != BN_CLICKED) return;
        if (id >= ID_REMOVE_BASE && id < ID_REMOVE_BASE + 16) {
            size_t index = (size_t)(id - ID_REMOVE_BASE);
            m_assistant.updateSettings([&](AppSettings& s) {
                if (index < s.triggers.size()) s.triggers.erase(s.triggers.begin() + index);
            });
            return;
        }
        auto toggleSetting = [&](HWND t, auto setter) {
            bool on = !Ui::isChecked(t);
            Ui::setChecked(t, on);
            m_assistant.updateSettings([&](AppSettings& s) { setter(s, on); });
        };
        switch (id) {
        case ID_WAKE: toggleSetting(m_wake, [](AppSettings& s, bool on) { s.wakeWordEnabled = on; }); break;
        case ID_SPEECH: toggleSetting(m_speech, [](AppSettings& s, bool on) { s.speechEnabled = on; }); break;
        case ID_SENS_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.vadThreshold = std::round((s.vadThreshold + 0.05) * 100) / 100; }); break;
        case ID_SENS_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.vadThreshold = std::round((s.vadThreshold - 0.05) * 100) / 100; }); break;
        case ID_PAUSE_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.endSilenceMs -= 100; }); break;
        case ID_PAUSE_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.endSilenceMs += 100; }); break;
        case ID_FOLLOW_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.followUpSeconds -= 1; }); break;
        case ID_FOLLOW_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.followUpSeconds += 1; }); break;
        case ID_GAIN_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.micGain = std::round((s.micGain - 0.1f) * 10.0f) / 10.0f; }); break;
        case ID_GAIN_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.micGain = std::round((s.micGain + 0.1f) * 10.0f) / 10.0f; }); break;
        case ID_RATE_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.speechRate = std::round((s.speechRate - 0.1) * 10) / 10; }); break;
        case ID_RATE_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.speechRate = std::round((s.speechRate + 0.1) * 10) / 10; }); break;
        case ID_TEST: m_assistant.testVoice(); break;
        case ID_ADD: startCapture(); break;
        }
    }

    LRESULT onAppMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_APP_REBUILD) {
            if (!m_capturing) rebuild();
            return 0;
        }
        if (msg != WM_APP_CAPTURED) return 0;
        std::unique_ptr<TalkTrigger> t(reinterpret_cast<TalkTrigger*>(lp));
        struct Resume {
            VoicePage* page;
            ~Resume() { page->stopCapture(); }
        } resume{ this };
        if (!wp || !t) {
            Ui::setText(m_captureLabel, L"Annullato.");
            return 0;
        }
        AppSettings s = m_assistant.settings();
        if (std::find(s.triggers.begin(), s.triggers.end(), *t) != s.triggers.end()) {
            Ui::setText(m_captureLabel, L"Questo tasto c'è già.");
        } else {
            Ui::setText(m_captureLabel, L"");
            m_assistant.updateSettings([&](AppSettings& x) { x.triggers.push_back(*t); });
        }
        return 0;
    }

private:
    // Riempie un elenco di periferiche: "Predefinito di Windows", quelle collegate e quella
    // scelta anche se ora non c'è. Seleziona quella scelta.
    static void fillDevices(HWND combo, std::vector<std::string>& list, const std::vector<std::string>& present,
                            const std::string& chosen, const wchar_t* defaultLabel) {
        list = present;
        if (!chosen.empty() && std::find(list.begin(), list.end(), chosen) == list.end()) list.push_back(chosen);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)defaultLabel);
        int sel = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            const bool missing = std::find(present.begin(), present.end(), list[i]) == present.end();
            const std::wstring text = w(list[i]) + (missing ? L" (non collegato)" : L"");
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
            if (list[i] == chosen) sel = (int)i + 1;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);
    }

    void fillMicrophones() {
        AppSettings s = m_assistant.settings();
        std::vector<std::string> present;
        for (const auto& d : AudioCapture::listCaptureDevices()) present.push_back(d.name);
        fillDevices(m_mic, m_devices, present, s.deviceNames.empty() ? std::string() : s.deviceNames.front(), L"Predefinito di Windows");
    }

    void fillOutputs() {
        fillDevices(m_output, m_outputs, TtsEngine::outputDevices(), m_assistant.settings().outputDeviceName, L"Predefinita di Windows");
    }

    void refresh() {
        AppSettings s = m_assistant.settings();
        Ui::setChecked(m_wake, s.wakeWordEnabled);
        Ui::setChecked(m_speech, s.speechEnabled);
        SendMessageW(m_engine, CB_SETCURSEL, s.voiceEngine == "windows" ? 1 : 0, 0);
        {
            const auto& list = VoiceEffect::presets();
            int effectSel = 0;
            for (size_t i = 0; i < list.size(); ++i) {
                if (s.voiceEffect == list[i].id) effectSel = (int)i;
            }
            SendMessageW(m_effect, CB_SETCURSEL, effectSel, 0);
        }
        int micSel = 0;
        if (!s.deviceNames.empty()) {
            auto it = std::find(m_devices.begin(), m_devices.end(), s.deviceNames.front());
            if (it != m_devices.end()) micSel = (int)(it - m_devices.begin()) + 1;
        }
        SendMessageW(m_mic, CB_SETCURSEL, micSel, 0);
        int outSel = 0;
        if (!s.outputDeviceName.empty()) {
            auto it = std::find(m_outputs.begin(), m_outputs.end(), s.outputDeviceName);
            if (it != m_outputs.end()) outSel = (int)(it - m_outputs.begin()) + 1;
        }
        SendMessageW(m_output, CB_SETCURSEL, outSel, 0);
        int voiceSel = -1;
        const std::string wanted = TextUtils::toLower(s.voiceName);
        for (size_t i = 0; i < m_voices.size(); ++i) {
            if (TextUtils::toLower(m_voices[i]).find(wanted) != std::string::npos) { voiceSel = (int)i; break; }
        }
        SendMessageW(m_voice, CB_SETCURSEL, voiceSel, 0);

        const double v = s.vadThreshold;
        const wchar_t* level = v <= 0.3 ? L"molto alta" : v <= 0.45 ? L"alta" : v <= 0.6 ? L"media" : v <= 0.75 ? L"bassa" : L"molto bassa";
        Ui::setText(m_sensLabel, std::wstring(L"Sensibilità: ") + level + L" (soglia " + decimal(v, 2) + L")");
        Ui::setText(m_pauseLabel, L"Pausa che chiude la frase: " + decimal(s.endSilenceMs / 1000.0, 1) + L" s");
        Ui::setText(m_followLabel, s.followUpSeconds == 0 ? std::wstring(L"Dopo una risposta: serve sempre \"Iris\"")
                                                          : L"Dopo una risposta ti ascolta ancora per " + std::to_wstring(s.followUpSeconds) + L" s");
        Ui::setText(m_gainLabel, L"Volume del microfono: " + std::to_wstring((int)std::lround(s.micGain * 100)) + L"%");
        Ui::setText(m_rateLabel, L"Velocità della voce: " + std::to_wstring((int)std::lround(s.speechRate * 100)) + L"%");

        auto connected = JoystickInput::devices();
        for (size_t i = 0; i < m_triggerLabels.size() && i < s.triggers.size(); ++i) {
            const TalkTrigger& t = s.triggers[i];
            std::wstring text = w(HotkeyListener::describeTrigger(t));
            if (t.type == TalkTrigger::Type::Joystick) {
                bool present = std::any_of(connected.begin(), connected.end(), [&](const auto& d) { return d.id == t.joystickId; });
                if (!present) text += L"  (non collegato)";
            }
            Ui::setText(m_triggerLabels[i], text);
        }
        if (m_triggerLabels.empty() && !m_capturing) Ui::setText(m_captureLabel, L"Nessun tasto: si usa solo \"Iris\".");
    }

    void startCapture() {
        if (m_capturing) {
            m_captureCancel = true;
            return;
        }
        m_capturing = true;
        m_captureCancel = false;
        m_assistant.setTalkPaused(true);
        Ui::setText(m_add, L"Annulla");
        Ui::setText(m_captureLabel, L"Premi il tasto o il pulsante del joystick da usare...");
        HWND target = m_hwnd;
        m_thread = std::thread([this, target]() {
            auto* t = new TalkTrigger();
            bool ok = HotkeyListener::captureNextInput(*t, m_captureCancel, 15000, true);
            if (!PostMessageW(target, WM_APP_CAPTURED, ok ? 1 : 0, (LPARAM)t)) delete t;
        });
    }

    void stopCapture() {
        if (m_thread.joinable()) {
            m_captureCancel = true;
            m_thread.join();
        }
        if (m_capturing) m_assistant.setTalkPaused(false);
        m_capturing = false;
        if (IsWindow(m_add)) Ui::setText(m_add, L"+ Aggiungi un tasto");
        if (IsWindow(m_hwnd) && m_assistant.settings().triggers.size() != m_rows) PostMessageW(m_hwnd, WM_APP_REBUILD, 0, 0);
    }

    std::vector<std::string> m_devices, m_voices, m_outputs;
    size_t m_rows = 0;
    std::vector<HWND> m_triggerLabels;
    HWND m_wake = nullptr, m_meter = nullptr, m_mic = nullptr, m_sensLabel = nullptr, m_pauseLabel = nullptr, m_followLabel = nullptr;
    HWND m_gainLabel = nullptr, m_add = nullptr, m_captureLabel = nullptr, m_speech = nullptr, m_voice = nullptr, m_rateLabel = nullptr;
    HWND m_diag = nullptr, m_output = nullptr, m_engine = nullptr, m_effect = nullptr;
    unsigned m_ticks = 0;
    bool m_capturing = false;
    std::atomic<bool> m_captureCancel{ false };
    std::thread m_thread;
};

// =====================================================================================
// Impostazioni: tu, internet, memoria, scheda video, overlay, file
// =====================================================================================

class SettingsPage : public Page {
public:
    explicit SettingsPage(Assistant& a) : Page(a, L"Impostazioni") {}
    void onShow() override { refresh(true); }
    void onEngineEvent(Assistant::Event e) override {
        if (e == Assistant::Event::Settings) refresh(false);
        if (e == Assistant::Event::Status) Ui::setText(m_gpuInfo, w(m_assistant.gpuSummary()));
    }

protected:
    enum {
        ID_NAME = 100, ID_CITY, ID_SAVE, ID_INTERNET, ID_MEMORY, ID_GPU, ID_OVERLAY, ID_OPACITY_MINUS, ID_OPACITY_PLUS, ID_UNLOCK,
        ID_RESET, ID_FOLDER, ID_LOG, ID_REPLY, ID_SIZE_MINUS, ID_SIZE_PLUS
    };

    static constexpr std::pair<const char*, const wchar_t*> kReplyStyles[] = {
        { "intere", L"Frasi intere, sempre" },
        { "gioco", L"Brevi mentre giochi, frasi intere il resto del tempo" },
        { "brevi", L"Brevi, sempre" },
    };

    static constexpr std::pair<int, const wchar_t*> kGpuLimits[] = {
        { -1, L"Senza limite: tutto sulla scheda (più veloce)" },
        { 8192, L"8 GB" },
        { 6144, L"6 GB" },
        { 4096, L"4 GB" },
        { 2048, L"2 GB" },
        { 0, L"Niente: tutto sul processore (lento)" },
    };

    void build() override {
        int y = 16;
        int top = y;
        y = beginCard(y);
        label(L"Il tuo nome (come ti chiama Iris)", kIn, y, 260, 18, Ui::Color::TextDim, Ui::Font::Small);
        label(L"La tua città (per il meteo)", kIn + 276, y, 256, 18, Ui::Color::TextDim, Ui::Font::Small);
        y += 20;
        m_name = m_form.place(Ui::createEdit(m_hwnd, ID_NAME, false, false, 60), kIn, y + 1, 260, 28);
        m_city = m_form.place(Ui::createEdit(m_hwnd, ID_CITY, false, false, 80), kIn + 276, y + 1, 256, 28);
        SetWindowSubclass(m_name, enterProc, 1, ID_SAVE);
        SetWindowSubclass(m_city, enterProc, 1, ID_SAVE);
        y += 38;
        button(ID_SAVE, L"Salva", Ui::ButtonKind::Primary, kIn, y, 120);
        m_saved = label(L"", kIn + 132, y + 6, kInW - 132, 22, Ui::Color::Ok);
        y += 32;
        y = endCard(top, y, L"TU");

        top = y;
        y = beginCard(y);
        std::vector<std::wstring> styles;
        for (const auto& r : kReplyStyles) styles.push_back(r.second);
        m_reply = combo(ID_REPLY, styles, kIn, y, kInW);
        y += 40;
        y = note(L"Brevi: poche parole, niente cortesie né domande (\"13 gradi, nuvoloso.\", \"Fatto.\"); i comandi di gioco confermano "
                 L"con una parola (\"Rotta impostata.\") o solo con un suono. I dettagli restano scritti nella conversazione.",
                 kIn, y, kInW);
        y = endCard(top, y - 6, L"QUANTO PARLA");

        top = y;
        y = beginCard(y);
        m_internet = toggle(ID_INTERNET, L"Iris può usare internet (ricerche, pagine web, meteo, notizie)", kIn, y, kInW);
        y += 36;
        y = note(L"Il riconoscimento della voce e il ragionamento restano sul PC. In rete vanno solo le ricerche (DuckDuckGo), le "
                 L"pagine che Iris legge, il meteo (Open-Meteo) e le notizie (Google News), e solo quando servono.",
                 kIn, y, kInW);
        y = endCard(top, y - 6, L"INTERNET");

        top = y;
        y = beginCard(y);
        y = note(L"Quando le dici \"Iris, ricordati che...\", lo scrive in memoria.txt e lo saprà anche le prossime volte. "
                 L"Puoi modificare il file a mano: una cosa per riga.",
                 kIn, y, kInW);
        y += 2;
        button(ID_MEMORY, L"Apri la memoria", Ui::ButtonKind::Secondary, kIn, y, 180);
        m_memoryCount = label(L"", kIn + 192, y + 6, kInW - 192, 22, Ui::Color::TextDim);
        y += 32;
        y = endCard(top, y, L"MEMORIA");

        top = y;
        y = beginCard(y);
        y = note(L"Se un gioco scatta, abbassa il limite: la parte del modello che non ci sta va sul processore (risposte più "
                 L"lente). Cambiandolo, i modelli si ricaricano in qualche secondo.",
                 kIn, y, kInW);
        y += 2;
        std::vector<std::wstring> limits;
        for (const auto& l : kGpuLimits) limits.push_back(l.second);
        m_gpu = combo(ID_GPU, limits, kIn, y, 340);
        y += 40;
        const int infoHeight = measureText(L"x\nx\nx", kInW, Ui::Font::Normal, m_dpi);
        m_gpuInfo = label(L"", kIn, y, kInW, infoHeight, Ui::Color::TextDim);
        y += infoHeight;
        y = endCard(top, y, L"CERVELLO E SCHEDA VIDEO");

        top = y;
        y = beginCard(y);
        m_overlay = toggle(ID_OVERLAY, L"Mostra la sfera di Iris sopra le altre finestre (anche sopra i giochi)", kIn, y, kInW);
        y += 36;
        y = note(L"Solo la sfera, senza sfondo: si accende quando ti ascolta, gira ambra quando pensa, pulsa quando parla, diventa "
                 L"rossa se qualcosa non va. I clic la attraversano: per spostarla sbloccala e trascinala.",
                 kIn, y, kInW);
        m_sizeLabel = label(L"", kIn, y + 6, kInW - 90, 22);
        button(ID_SIZE_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
        button(ID_SIZE_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
        y += 36;
        m_opacityLabel = label(L"", kIn, y + 6, kInW - 90, 22);
        button(ID_OPACITY_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
        button(ID_OPACITY_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
        y += 40;
        button(ID_UNLOCK, L"Sblocca per spostarla (30 s)", Ui::ButtonKind::Secondary, kIn, y, 250);
        button(ID_RESET, L"Rimettila nell'angolo", Ui::ButtonKind::Secondary, kIn + 260, y, 210);
        y += 32;
        y = endCard(top, y, L"SFERA SOPRA I GIOCHI");

        top = y;
        y = beginCard(y);
        button(ID_FOLDER, L"Apri la cartella dei dati", Ui::ButtonKind::Secondary, kIn, y, 230);
        button(ID_LOG, L"Apri il registro", Ui::ButtonKind::Secondary, kIn + 240, y, 180);
        y += 32;
        endCard(top, y, L"FILE");
        refresh(true);
    }

    void onCommand(int id, int code, HWND) override {
        if (id == ID_GPU && code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW(m_gpu, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)std::size(kGpuLimits)) m_assistant.updateSettings([&](AppSettings& s) { s.gpuMemoryLimitMb = kGpuLimits[sel].first; });
            return;
        }
        if (id == ID_REPLY && code == CBN_SELCHANGE) {
            const int sel = (int)SendMessageW(m_reply, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)std::size(kReplyStyles)) m_assistant.updateSettings([&](AppSettings& s) { s.replyStyle = kReplyStyles[sel].first; });
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
        case ID_SAVE: {
            std::string name = TextUtils::trim(u8(windowText(m_name))), city = TextUtils::trim(u8(windowText(m_city)));
            m_assistant.updateSettings([&](AppSettings& s) {
                s.userName = name;
                s.homeCity = city;
            });
            Ui::setText(m_saved, L"Salvato.");
            break;
        }
        case ID_INTERNET: {
            bool on = !Ui::isChecked(m_internet);
            Ui::setChecked(m_internet, on);
            m_assistant.updateSettings([&](AppSettings& s) { s.internetEnabled = on; });
            break;
        }
        case ID_MEMORY: {
            AppPaths::ensureDirectoriesExist();
            const std::wstring path = AppPaths::memoryPath();
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    const char header[] = "# Cose che Iris ricorda di te: una per riga. Le righe con # sono ignorate.\r\n";
                    DWORD written = 0;
                    WriteFile(f, header, (DWORD)strlen(header), &written, nullptr);
                    CloseHandle(f);
                }
            }
            openFile(m_hwnd, path, true);
            break;
        }
        case ID_OVERLAY: {
            bool on = !Ui::isChecked(m_overlay);
            Ui::setChecked(m_overlay, on);
            m_assistant.updateSettings([&](AppSettings& s) { s.overlayEnabled = on; });
            break;
        }
        case ID_OPACITY_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.overlayOpacity -= 13; }); break;
        case ID_OPACITY_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.overlayOpacity += 13; }); break;
        case ID_SIZE_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.overlaySize -= 20; }); break;
        case ID_SIZE_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.overlaySize += 20; }); break;
        case ID_UNLOCK: Overlay::unlockForMove(30); break;
        case ID_RESET:
            m_assistant.updateSettings([](AppSettings& s) {
                s.overlayX = -1;
                s.overlayY = -1;
            });
            break;
        case ID_FOLDER: openFile(m_hwnd, AppPaths::getAppDataDir(), false); break;
        case ID_LOG: openFile(m_hwnd, AppPaths::logPath(), true); break;
        }
    }

private:
    void refresh(bool texts) {
        AppSettings s = m_assistant.settings();
        if (texts) {
            SetWindowTextW(m_name, w(s.userName).c_str());
            SetWindowTextW(m_city, w(s.homeCity).c_str());
        }
        Ui::setChecked(m_internet, s.internetEnabled);
        Ui::setChecked(m_overlay, s.overlayEnabled);
        Ui::setText(m_opacityLabel, L"Opacità: " + percent(s.overlayOpacity));
        Ui::setText(m_sizeLabel, L"Dimensione: " + std::to_wstring(s.overlaySize) + L" pixel");
        int gpuSel = -1;
        for (int i = 0; i < (int)std::size(kGpuLimits); ++i) {
            if (kGpuLimits[i].first == s.gpuMemoryLimitMb) gpuSel = i;
        }
        SendMessageW(m_gpu, CB_SETCURSEL, gpuSel, 0);
        int replySel = 1;
        for (int i = 0; i < (int)std::size(kReplyStyles); ++i) {
            if (s.replyStyle == kReplyStyles[i].first) replySel = i;
        }
        SendMessageW(m_reply, CB_SETCURSEL, replySel, 0);
        std::string summary = m_assistant.gpuSummary();
        Ui::setText(m_gpuInfo, summary.empty() ? L"Caricamento dei modelli..." : w(summary));
        const size_t n = m_assistant.memory().size();
        Ui::setText(m_memoryCount, n == 0 ? std::wstring(L"Ancora niente in memoria.") : n == 1 ? std::wstring(L"1 cosa in memoria.")
                                                                                            : std::to_wstring(n) + L" cose in memoria.");
    }

    HWND m_name = nullptr, m_city = nullptr, m_saved = nullptr, m_internet = nullptr, m_memoryCount = nullptr;
    HWND m_gpu = nullptr, m_gpuInfo = nullptr, m_overlay = nullptr, m_opacityLabel = nullptr, m_reply = nullptr, m_sizeLabel = nullptr;
};

// =====================================================================================
// Star Citizen: stato del gioco, prova dei comandi, calibrazione della mappa, registro
// =====================================================================================

constexpr UINT WM_APP_GAME_TEXT = WM_APP + 20; // lParam = std::string* (messaggio), wParam = 1 se finito

class GamePage : public Page {
public:
    explicit GamePage(Assistant& a) : Page(a, L"Star Citizen") {}
    ~GamePage() override { stopWork(); }

    void onShow() override { refresh(true); }
    void onEngineEvent(Assistant::Event e) override {
        if (e == Assistant::Event::Settings) refresh(false);
    }
    void onTimer() override {
        if (++m_ticks % 30 == 0) refresh(false); // circa una volta al secondo
    }

protected:
    enum {
        ID_ENABLE = 100, ID_DRYRUN, ID_PROFILE, ID_RELOAD, ID_DEBUG, ID_EDIT, ID_TEST, ID_LOG, ID_UNKNOWN_FILE,
        ID_CAL_BASE = 200, ID_READ_BASE = 220
    };

    void build() override {
        int y = 16;
        int top = y;
        y = beginCard(y);
        const int statusHeight = measureText(L"x\nx", kInW, Ui::Font::Normal, m_dpi);
        m_status = label(L"", kIn, y, kInW, statusHeight, Ui::Color::TextDim);
        y += statusHeight + 6;
        m_enable = toggle(ID_ENABLE, L"Comandi di gioco attivi (quando Star Citizen è aperto)", kIn, y, kInW);
        y += 36;
        m_dryRun = toggle(ID_DRYRUN, L"Modalità prova: scrive cosa farebbe, senza premere tasti", kIn, y, kInW);
        y += 40;
        button(ID_PROFILE, L"Apri il profilo dei comandi", Ui::ButtonKind::Secondary, kIn, y, 230);
        button(ID_RELOAD, L"Ricarica", Ui::ButtonKind::Secondary, kIn + 240, y, 110);
        button(ID_DEBUG, L"Immagini dell'OCR", Ui::ButtonKind::Secondary, kIn + 360, y, kInW - 360);
        y += 40;
        m_profileError = label(L"", kIn, y, kInW, 20, Ui::Color::Error, Ui::Font::Small);
        y += 22;
        y = note(L"Gioca in finestra senza bordi. Dì \"Iris, carrello\", \"Iris, rotta per Crusader\", \"Iris, prepara la nave\"... (o tieni "
                 L"premuto il tasto per parlare). Le frasi del profilo vanno subito; le altre le capisce il modello. Controlla che i tasti "
                 L"del profilo siano i tuoi (in gioco: Opzioni > Comandi).",
                 kIn, y, kInW);
        y = endCard(top, y - 6, L"STAR CITIZEN");

        top = y;
        y = beginCard(y);
        m_edit = m_form.place(Ui::createEdit(m_hwnd, ID_EDIT, false, false, 300), kIn, y + 1, kInW - 112, 28);
        SetWindowSubclass(m_edit, enterProc, 1, ID_TEST);
        SendMessageW(m_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"es. rotta per Seraphim, carrello, scudi");
        button(ID_TEST, L"Prova", Ui::ButtonKind::Primary, kIn + kInW - 102, y, 102, 30);
        y += 38;
        m_testResult = label(L"", kIn, y, kInW, 20, Ui::Color::TextDim, Ui::Font::Small);
        y += 22;
        y = endCard(top, y, L"PROVA UN COMANDO");

        top = y;
        y = beginCard(y);
        y = note(L"Premi Calibra: porto io Star Citizen davanti. Apri la mappa stellare (se non lo è già), porta il mouse sul punto e premi "
                 L"F8; per le zone F8 sull'angolo in alto a sinistra e poi su quello in basso a destra. A ogni F8 senti un suono, alla fine "
                 L"torno qui col risultato. Leggi porta il gioco davanti e mostra cosa vede l'OCR in quella zona.",
                 kIn, y, kInW);
        y += 2;
        static const wchar_t* names[] = { L"Casella di ricerca (un punto)", L"Zona dei risultati", L"Pulsante Imposta rotta",
                                          L"Pulsante Annulla (rotta attiva)" };
        for (int i = 0; i < 4; ++i) {
            label(names[i], kIn, y + 6, kInW - 230, 22);
            button(ID_CAL_BASE + i, L"Calibra", Ui::ButtonKind::Secondary, kIn + kInW - 220, y, 105, 30);
            button(ID_READ_BASE + i, L"Leggi", Ui::ButtonKind::Secondary, kIn + kInW - 105, y, 105, 30);
            y += 36;
        }
        m_calResult = m_form.place(Ui::createEdit(m_hwnd, 0, true, true), kIn, y + 4, kInW, 96);
        y += 104;
        y = endCard(top, y, L"MAPPA STELLARE");

        top = y;
        y = beginCard(y);
        m_log = m_form.place(Ui::createEdit(m_hwnd, ID_LOG, true, true), kIn, y, kInW, 150);
        y += 158;
        button(ID_UNKNOWN_FILE, L"Apri le frasi non capite", Ui::ButtonKind::Secondary, kIn, y, 230);
        y += 32;
        endCard(top, y, L"ULTIME AZIONI E FRASI NON CAPITE");
        refresh(true);
    }

    void onCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        GameController& game = m_assistant.game();
        if (id >= ID_CAL_BASE && id < ID_CAL_BASE + 4) {
            startCalibration((GameController::Zone)(id - ID_CAL_BASE));
            return;
        }
        if (id >= ID_READ_BASE && id < ID_READ_BASE + 4) {
            const auto zone = (GameController::Zone)(id - ID_READ_BASE);
            const HWND self = GetAncestor(m_hwnd, GA_ROOT);
            startWork([this, zone, self, &game]() {
                const std::string text = game.readZone(zone); // porta il gioco davanti e legge
                if (m_workCancel) return;
                GameInput::focus(self);
                post(text, true);
            });
            Ui::setText(m_calResult, L"Porto il gioco davanti e leggo la " + w(GameController::zoneName(zone)) + L"...");
            return;
        }
        switch (id) {
        case ID_ENABLE: {
            bool on = !Ui::isChecked(m_enable);
            Ui::setChecked(m_enable, on);
            m_assistant.updateSettings([&](AppSettings& s) { s.gameEnabled = on; });
            break;
        }
        case ID_DRYRUN: {
            bool on = !Ui::isChecked(m_dryRun);
            Ui::setChecked(m_dryRun, on);
            game.setDryRun(on);
            break;
        }
        case ID_PROFILE: openFile(m_hwnd, game.profilePath(), true); break;
        case ID_RELOAD:
            game.load();
            refresh(true);
            break;
        case ID_DEBUG: {
            const std::wstring dir = AppPaths::getAppDataDir() + L"\\debug";
            CreateDirectoryW(dir.c_str(), nullptr);
            openFile(m_hwnd, dir, false);
            break;
        }
        case ID_UNKNOWN_FILE: openFile(m_hwnd, AppPaths::getAppDataDir() + L"\\frasi_non_capite.txt", true); break;
        case ID_TEST: {
            const std::string text = TextUtils::trim(u8(windowText(m_edit)));
            if (text.empty()) break;
            const Cmd::Match m = game.match(text);
            wchar_t buf[32];
            swprintf(buf, 32, L"%d%%", (int)(m.score * 100 + 0.5));
            if (m.found) {
                std::wstring slots;
                for (const auto& [k, v] : m.slots) slots += L", " + w(k) + L" = " + w(v);
                Ui::setText(m_testResult, L"→ " + w(m.command.id()) + slots + L"  (\"" + w(m.phrase) + L"\", " + buf + L")");
                Ui::setLabelColor(m_testResult, Ui::Color::Ok);
            } else {
                Ui::setText(m_testResult, L"Non è nel profilo: lo passo al modello. Il più vicino: \"" + w(m.phrase) + L"\" (" + buf + L")");
                Ui::setLabelColor(m_testResult, Ui::Color::Busy);
            }
            m_assistant.testGameCommand(text);
            SetWindowTextW(m_edit, L"");
            break;
        }
        }
    }

    LRESULT onAppMessage(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg != WM_APP_GAME_TEXT) return 0;
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        if (text) SetWindowTextW(m_calResult, w(*text).c_str());
        if (wp) refresh(false);
        return 0;
    }

private:
    void post(const std::string& text, bool done) {
        auto* copy = new std::string(text);
        if (!PostMessageW(m_hwnd, WM_APP_GAME_TEXT, done ? 1 : 0, (LPARAM)copy)) delete copy;
    }

    void startWork(std::function<void()> work) {
        stopWork();
        m_workCancel = false;
        m_worker = std::thread(std::move(work));
    }

    void stopWork() {
        m_workCancel = true;
        if (m_worker.joinable()) m_worker.join();
    }

    // Aspetta F8 (non già premuto), al massimo 30 secondi.
    bool waitF8() {
        while (GetAsyncKeyState(VK_F8) & 0x8000) {
            if (m_workCancel) return false;
            Sleep(20);
        }
        for (int i = 0; i < 1500; ++i) {
            if (m_workCancel) return false;
            if (GetAsyncKeyState(VK_F8) & 0x8000) return true;
            Sleep(20);
        }
        return false;
    }

    void startCalibration(GameController::Zone zone) {
        GameController& game = m_assistant.game();
        const std::string name = GameController::zoneName(zone);
        const bool point = zone == GameController::Zone::SearchBox;
        Ui::setText(m_calResult, w("Calibro la " + name + ": nel gioco, con la mappa stellare aperta, " +
                                   (point ? std::string("porta il mouse al centro della casella e premi F8.")
                                          : std::string("porta il mouse sull'angolo IN ALTO A SINISTRA e premi F8, poi su quello IN BASSO A "
                                                        "DESTRA e di nuovo F8.")) +
                                   " Senti un suono a ogni F8; alla fine torno qui. Hai 30 secondi."));
        const HWND self = GetAncestor(m_hwnd, GA_ROOT);
        startWork([this, zone, point, name, self, &game]() {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            std::string error;
            // Iris è in primo piano (hai appena premuto Calibra): si passa al gioco da soli.
            if (!game.bringToFront(error)) return post("Non calibrato: " + error, true);
            // Finito (bene o male): un suono, si torna a Iris e si mostra il risultato.
            auto finish = [&](const std::string& text, bool ok) {
                if (m_workCancel) return; // un'altra calibrazione o la finestra che si chiude
                m_assistant.chime(ok);
                GameInput::focus(self);
                post(text, true);
            };
            if (!waitF8()) return finish("Calibrazione annullata (F8 non premuto entro 30 secondi).", false);
            if (!game.cursorRelative(x1, y1, error)) return finish("Non calibrato: " + error + ".", false);
            if (!point) {
                m_assistant.chime(true); // primo angolo preso
                post("Primo angolo preso. Ora porta il mouse sull'angolo IN BASSO A DESTRA della " + name + " e premi F8.", false);
                if (!waitF8()) return finish("Calibrazione annullata (F8 non premuto entro 30 secondi).", false);
                if (!game.cursorRelative(x2, y2, error)) return finish("Non calibrato: " + error + ".", false);
            }
            if (!game.setZone(zone, x1, y1, x2, y2)) return finish("Non calibrato: i due angoli sono troppo vicini. Riprova.", false);
            char buf[200];
            if (point) std::snprintf(buf, sizeof(buf), "Salvata: %s a %.3f, %.3f della finestra.", name.c_str(), x1, y1);
            else std::snprintf(buf, sizeof(buf), "Salvata: %s da %.3f, %.3f a %.3f, %.3f della finestra. Premi Leggi per provarla.", name.c_str(),
                               std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
            finish(buf, true);
        });
    }

    void refresh(bool all) {
        GameController& game = m_assistant.game();
        const GameController::Status st = game.status();
        Ui::setText(m_status, w(st.text));
        Ui::setLabelColor(m_status, st.elevationProblem ? Ui::Color::Error : st.running ? Ui::Color::Ok : Ui::Color::TextDim);
        Ui::setChecked(m_enable, m_assistant.settings().gameEnabled);
        Ui::setChecked(m_dryRun, game.profile().dryRun);
        const std::string err = game.profileError();
        Ui::setText(m_profileError, err.empty() ? L"" : L"Profilo non valido: " + w(err) + L" (resta in uso quello di prima)");
        std::string log;
        for (const auto& a : game.recentActions()) log += a + "\r\n";
        const auto unknown = game.unrecognized();
        if (!unknown.empty()) {
            log += "\r\nFrasi non capite dal profilo (passate al modello):\r\n";
            for (const auto& u : unknown) log += u + "\r\n";
        }
        if (log.empty()) log = "Ancora nessun comando di gioco.";
        const std::wstring wl = w(log);
        if (all || wl != m_lastLog) {
            SetWindowTextW(m_log, wl.c_str());
            m_lastLog = wl;
        }
    }

    HWND m_status = nullptr, m_enable = nullptr, m_dryRun = nullptr, m_profileError = nullptr, m_edit = nullptr, m_testResult = nullptr;
    HWND m_calResult = nullptr, m_log = nullptr;
    std::wstring m_lastLog;
    unsigned m_ticks = 0;
    std::thread m_worker;
    std::atomic<bool> m_workCancel{ false };
};

} // namespace

std::vector<std::unique_ptr<Page>> createPages(Assistant& assistant) {
    std::vector<std::unique_ptr<Page>> pages;
    pages.push_back(std::make_unique<IrisPage>(assistant));
    pages.push_back(std::make_unique<VoicePage>(assistant));
    pages.push_back(std::make_unique<GamePage>(assistant));
    pages.push_back(createChatPage(assistant));
    pages.push_back(createChatScreenPage(assistant));
    pages.push_back(createFilteredPage(assistant));
    pages.push_back(std::make_unique<SettingsPage>(assistant));
    return pages;
}
