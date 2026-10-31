// Le pagine della chat di gioco: le funzioni di Voice to Chat dentro Iris
// (dalle sue pagine Principale, Tasti e chat, Microfono, Traduzione schermo,
// Messaggi filtrati), con i tasti e le impostazioni della chat di Iris.

#include "pages.h"
#include "../app_paths.h"
#include "../glossary.h"
#include "../joystick_input.h"
#include "../languages.h"
#include "../ocr_engine.h"
#include "../ocr_overlay.h"
#include "../region_selector.h"
#include "../text_utils.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {
    constexpr int kCardX = 16;
    constexpr int kCardW = kPageWidth - 2 * kCardX;
    constexpr int kIn = kCardX + 18;
    constexpr int kInW = kCardW - 36;

    std::wstring w(const std::string& s) { return TextUtils::utf8ToWide(s); }
    std::string u8(const std::wstring& s) { return TextUtils::wideToUtf8(s); }

    std::wstring windowText(HWND h) {
        int len = GetWindowTextLengthW(h);
        std::wstring t(len + 1, L'\0');
        GetWindowTextW(h, t.data(), len + 1);
        t.resize(len);
        return t;
    }

    std::vector<std::wstring> languageNames() {
        std::vector<std::wstring> names;
        for (const auto& l : supportedLanguages()) names.push_back(l.displayName);
        return names;
    }

    std::wstring percent(int alpha) { return std::to_wstring((alpha * 100 + 127) / 255) + L"%"; }

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

    void openInNotepad(HWND owner, const std::wstring& path) {
        std::wstring args = L"\"" + path + L"\"";
        ShellExecuteW(owner, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }

    LRESULT CALLBACK enterProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR buttonId) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) {
            SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM((int)buttonId, BN_CLICKED), 0);
            return 0;
        }
        if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
        if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, enterProc, id);
        return DefSubclassProc(h, msg, wp, lp);
    }

    int measureText(const std::wstring& text, int width, Ui::Font f, UINT dpi) {
        HDC dc = GetDC(nullptr);
        int h = Ui::textHeight(dc, text, Ui::px(width, dpi), Ui::font(f, dpi));
        ReleaseDC(nullptr, dc);
        return MulDiv(h, 96, (int)dpi) + 2;
    }

    // Barra del volume del microfono con la soglia del silenzio della chat (scala in dB).
    void drawThresholdMeter(const DRAWITEMSTRUCT* dis, float level, double threshold) {
        const RECT r = dis->rcItem;
        const UINT dpi = Ui::dpiOf(dis->hwndItem);
        Ui::Buffered buffer(dis->hDC, r);
        HDC dc = buffer.dc();
        FillRect(dc, &r, Ui::brush(Ui::Color::Panel));
        const int h = r.bottom - r.top;
        Ui::fillRound(dc, r, h / 2, Ui::Color::Input);
        auto toX = [&](double v) {
            double db = 20.0 * std::log10(std::max(v, 1e-5));
            double pos = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
            return r.left + (int)(pos * (r.right - r.left));
        };
        const int x = toX(level);
        if (x > r.left + h / 2) {
            RECT fill{ r.left, r.top, x, r.bottom };
            Ui::fillRound(dc, fill, h / 2, level >= threshold ? Ui::Color::Ok : Ui::Color::Idle);
        }
        const int tx = toX(threshold);
        RECT mark{ tx - Ui::px(1, dpi), r.top - 1, tx + Ui::px(1, dpi), r.bottom + 1 };
        FillRect(dc, &mark, Ui::brush(RGB(255, 255, 255)));
    }

    HWND createMeter(HWND parent, int id) {
        return CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    }

    // --- Cronologia delle frasi (come in Voice to Chat) ------------------------------------
    // Doppio clic su una frase = copia la traduzione (WM_COMMAND, codice 1).
    class HistoryView {
    public:
        static HWND create(HWND parent, int id) {
            static bool registered = false;
            if (!registered) {
                WNDCLASSW wc{};
                wc.style = CS_DBLCLKS;
                wc.lpfnWndProc = proc;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"IrisChatHistory";
                wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                RegisterClassW(&wc);
                registered = true;
            }
            auto* self = new HistoryView();
            HWND h = CreateWindowExW(0, L"IrisChatHistory", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, parent,
                                     (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), self);
            if (h) Ui::darkControl(h);
            else delete self;
            return h;
        }

        static void setEntries(HWND h, std::vector<GameChat::HistoryEntry> newestFirst) {
            auto* self = get(h);
            if (!self) return;
            self->m_entries = std::move(newestFirst);
            self->m_scroll = 0;
            self->m_selected = -1;
            InvalidateRect(h, nullptr, FALSE);
        }

        static std::string writtenAt(HWND h, int index) {
            auto* self = get(h);
            if (!self || index < 0 || index >= (int)self->m_entries.size()) return {};
            const auto& e = self->m_entries[index];
            return e.written.empty() ? e.heard : e.written;
        }

        static int selected(HWND h) {
            auto* self = get(h);
            return self ? self->m_selected : -1;
        }

    private:
        std::vector<GameChat::HistoryEntry> m_entries;
        std::vector<int> m_tops;
        int m_total = 0;
        int m_scroll = 0;
        int m_selected = -1;

        static HistoryView* get(HWND h) { return reinterpret_cast<HistoryView*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

        std::wstring heardLine(const GameChat::HistoryEntry& e) const { return L"«" + w(e.heard) + L"»"; }
        std::wstring writtenLine(const GameChat::HistoryEntry& e) const {
            return e.written.empty() ? L"(niente scritto)" : L"→ " + w(e.written);
        }

        void measure(HWND h, int width, UINT dpi) {
            m_tops.clear();
            HDC dc = GetDC(h);
            const int pad = Ui::px(8, dpi), timeW = Ui::px(44, dpi);
            const int textW = std::max(10, width - timeW - 2 * pad);
            int y = 0;
            for (const auto& e : m_entries) {
                m_tops.push_back(y);
                int eh = pad + Ui::textHeight(dc, heardLine(e), textW, Ui::font(Ui::Font::Normal, dpi)) + Ui::px(2, dpi) +
                         Ui::textHeight(dc, writtenLine(e), textW, Ui::font(Ui::Font::Bold, dpi));
                if (!e.note.empty()) eh += Ui::px(2, dpi) + Ui::textHeight(dc, w(e.note), textW, Ui::font(Ui::Font::Small, dpi));
                y += eh + pad;
            }
            ReleaseDC(h, dc);
            m_total = y;
        }

        void updateScrollBar(HWND h, int viewHeight) {
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
            measure(h, rc.right, dpi);
            updateScrollBar(h, rc.bottom);
            {
                Ui::Buffered buffer(hdc, rc);
                HDC dc = buffer.dc();
                FillRect(dc, &rc, Ui::brush(Ui::Color::Input));
                const int pad = Ui::px(8, dpi), timeW = Ui::px(44, dpi);
                if (m_entries.empty()) {
                    RECT t{ pad, pad, rc.right - pad, rc.bottom - pad };
                    Ui::drawText(dc, L"Nessuna frase ancora: tieni premuto il tasto della chat e parla, o di' \"Iris, scrivi in chat...\".", t,
                                 Ui::font(Ui::Font::Normal, dpi), Ui::Color::TextDim, DT_LEFT | DT_WORDBREAK);
                }
                for (size_t i = 0; i < m_entries.size(); ++i) {
                    const int top = m_tops[i] - m_scroll;
                    const int bottom = (i + 1 < m_tops.size() ? m_tops[i + 1] : m_total) - m_scroll;
                    if (bottom < 0 || top > rc.bottom) continue;
                    const auto& e = m_entries[i];
                    if ((int)i == m_selected) {
                        RECT sel{ 0, top, rc.right, bottom };
                        FillRect(dc, &sel, Ui::brush(Ui::Color::Secondary));
                    }
                    RECT line{ 0, bottom - 1, rc.right, bottom };
                    FillRect(dc, &line, Ui::brush(Ui::Color::PanelBorder));
                    RECT tr{ pad, top + pad, pad + timeW, bottom };
                    Ui::drawText(dc, w(e.time), tr, Ui::font(Ui::Font::Small, dpi), Ui::Color::TextDim, DT_LEFT | DT_TOP | DT_SINGLELINE);
                    const int x = pad + timeW, textW = rc.right - x - pad;
                    int y = top + pad;
                    RECT hr{ x, y, x + textW, bottom };
                    Ui::drawText(dc, heardLine(e), hr, Ui::font(Ui::Font::Normal, dpi), Ui::Color::TextDim, DT_LEFT | DT_WORDBREAK);
                    y += Ui::textHeight(dc, heardLine(e), textW, Ui::font(Ui::Font::Normal, dpi)) + Ui::px(2, dpi);
                    RECT wr{ x, y, x + textW, bottom };
                    Ui::drawText(dc, writtenLine(e), wr, Ui::font(Ui::Font::Bold, dpi), e.written.empty() ? Ui::Color::Error : Ui::Color::Text,
                                 DT_LEFT | DT_WORDBREAK);
                    if (!e.note.empty()) {
                        y += Ui::textHeight(dc, writtenLine(e), textW, Ui::font(Ui::Font::Bold, dpi)) + Ui::px(2, dpi);
                        RECT nr{ x, y, x + textW, bottom };
                        Ui::drawText(dc, w(e.note), nr, Ui::font(Ui::Font::Small, dpi), e.typed ? Ui::Color::TextDim : Ui::Color::Busy,
                                     DT_LEFT | DT_WORDBREAK);
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
            if (m_scroll != old) InvalidateRect(h, nullptr, FALSE);
        }

        static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
            if (msg == WM_NCCREATE) {
                SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            }
            HistoryView* self = get(h);
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
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK: {
                int y = GET_Y_LPARAM(lp) + self->m_scroll, hit = -1;
                for (size_t i = 0; i < self->m_tops.size(); ++i) {
                    int bottom = i + 1 < self->m_tops.size() ? self->m_tops[i + 1] : self->m_total;
                    if (y >= self->m_tops[i] && y < bottom) hit = (int)i;
                }
                self->m_selected = hit;
                InvalidateRect(h, nullptr, FALSE);
                if (msg == WM_LBUTTONDBLCLK && hit >= 0) {
                    SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), 1), (LPARAM)h);
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

    // =====================================================================================
    // Chat di gioco: tasti, lingue, ultime frasi, prova, scrittura, soglia, glossario
    // =====================================================================================

    class ChatPage : public Page {
    public:
        explicit ChatPage(Assistant& a) : Page(a, L"Chat di gioco") {}
        ~ChatPage() override { stopCapture(); }

        void onShow() override {
            refresh();
            fillHistory();
            updateNotice();
        }
        void onEngineEvent(Assistant::Event e) override {
            if (e == Assistant::Event::ChatHistory) fillHistory();
            else if (e == Assistant::Event::Status) updateNotice();
            else if (e == Assistant::Event::Settings) {
                // Il numero di tasti è cambiato: righe da rifare.
                if (m_assistant.settings().chatTriggers.size() != m_rows) PostMessageW(m_hwnd, WM_APP_REBUILD, 0, 0);
                else refresh();
            }
        }
        void onTimer() override {
            if (m_meter) InvalidateRect(m_meter, nullptr, FALSE);
        }

    protected:
        enum {
            ID_ENABLE = 100, ID_ADD, ID_SOURCE, ID_TARGET, ID_SWAP, ID_GAMING, ID_AUTOSEND, ID_HISTORY, ID_TEST_EDIT, ID_TEST,
            ID_CHAT_OPEN, ID_OPEN_KEY, ID_SEND_KEY, ID_DELAY_MINUS, ID_DELAY_PLUS, ID_TYPING_MINUS, ID_TYPING_PLUS, ID_UNICODE,
            ID_METER, ID_THRESHOLD_MINUS, ID_THRESHOLD_PLUS, ID_CALIBRATE, ID_GLOSSARY, ID_REMOVE_BASE = 200
        };
        enum class Capture { None, Talk, OpenKey, SendKey };

        void build() override {
            AppSettings s = m_assistant.settings();
            int y = 16;

            // Accensione e stato
            int top = y;
            y = beginCard(y);
            m_enable = toggle(ID_ENABLE, L"Detta in chat: parli in italiano, va nella chat del gioco già tradotto", kIn, y, kInW);
            y += 36;
            y = note(L"Come Voice to Chat, dentro Iris e con i suoi stessi modelli: tieni premuto il tasto della chat, parla e rilascialo. "
                     L"Puoi anche dire \"Iris, scrivi in chat: arrivo tra due minuti\". Iris scrive solo se in primo piano c'è il gioco "
                     L"(con Iris davanti traduce e basta, per provare).",
                     kIn, y, kInW);
            const int noticeHeight = measureText(L"x\nx", kInW, Ui::Font::Normal, m_dpi);
            m_notice = label(L"", kIn, y, kInW, noticeHeight, Ui::Color::TextDim);
            y += noticeHeight;
            y = endCard(top, y, L"CHAT DI GIOCO");

            // Tasti
            top = y;
            y = beginCard(y);
            y = note(L"Tieni premuto uno di questi tasti mentre parli. Un tasto che usi anche per Iris (pagina Ascolto e voce) vale per "
                     L"tutti e due: con Star Citizen in primo piano la frase va in chat, a meno che tu dica \"Iris\"; fuori dal gioco va "
                     L"a Iris. I tasti solo per la chat scrivono in qualunque gioco (come Voice to Chat).",
                     kIn, y, kInW);
            y += 4;
            m_rows = s.chatTriggers.size();
            m_triggerLabels.clear();
            for (size_t i = 0; i < s.chatTriggers.size(); ++i) {
                m_triggerLabels.push_back(label(L"", kIn, y + 6, kInW - 110, 22));
                button(ID_REMOVE_BASE + (int)i, L"Togli", Ui::ButtonKind::Danger, kIn + kInW - 96, y, 96, 30);
                y += 36;
            }
            if (s.chatTriggers.empty()) {
                label(L"Nessun tasto: la chat si usa solo dicendo \"Iris, scrivi in chat...\".", kIn, y + 4, kInW, 22, Ui::Color::Busy);
                y += 32;
            }
            y += 2;
            m_add = button(ID_ADD, L"+ Aggiungi un tasto", Ui::ButtonKind::Primary, kIn, y, 210);
            if (s.chatTriggers.size() >= 6) EnableWindow(m_add, FALSE);
            m_captureLabel = label(L"", kIn + 222, y + 6, kInW - 222, 22, Ui::Color::Accent);
            y += 32;
            y = endCard(top, y, L"TASTI PER DETTARE IN CHAT");

            // Lingue
            top = y;
            y = beginCard(y);
            label(L"Parlo in", kIn, y, 240, 18, Ui::Color::TextDim, Ui::Font::Small);
            label(L"Scrivo in chat in", kIn + 292, y, 240, 18, Ui::Color::TextDim, Ui::Font::Small);
            y += 20;
            const auto names = languageNames();
            m_source = combo(ID_SOURCE, names, kIn, y, 240);
            button(ID_SWAP, L"↔", Ui::ButtonKind::Secondary, kIn + 248, y - 1, 36, 30);
            m_target = combo(ID_TARGET, names, kIn + 292, y, 240);
            y += 42;
            m_gaming = toggle(ID_GAMING, L"Gergo da videogiocatore (\"sono a terra\" diventa \"I'm down\")", kIn, y, kInW);
            y += 36;
            m_autoSend = toggle(ID_AUTOSEND, L"Invio automatico in chat (F10 lo accende e lo spegne anche in gioco)", kIn, y, kInW);
            y += 30;
            y = endCard(top, y, L"LINGUE");

            // Ultime frasi
            top = y;
            y = beginCard(y);
            m_history = m_form.place(HistoryView::create(m_hwnd, ID_HISTORY), kIn, y, kInW, 220);
            y += 226;
            label(L"Doppio clic su una frase per copiarne la traduzione.", kIn, y, kInW, 18, Ui::Color::TextDim, Ui::Font::Small);
            y += 18;
            y = endCard(top, y, L"ULTIME FRASI");

            // Prova
            top = y;
            y = beginCard(y);
            y = note(L"Scrivi una frase come se la dicessi: la traduzione compare tra le ultime frasi, senza essere scritta nel gioco.",
                     kIn, y, kInW);
            y += 2;
            m_testEdit = m_form.place(Ui::createEdit(m_hwnd, ID_TEST_EDIT, false, false, 500), kIn, y + 1, kInW - 122, 28);
            SetWindowSubclass(m_testEdit, enterProc, 1, ID_TEST);
            SendMessageW(m_testEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"es. dove sei? arrivo ad aiutarti");
            button(ID_TEST, L"Traduci", Ui::ButtonKind::Primary, kIn + kInW - 112, y, 112, 30);
            y += 30;
            y = endCard(top, y, L"PROVA LA TRADUZIONE");

            // Scrittura in chat
            top = y;
            y = beginCard(y);
            y = note(L"Iris scrive come una tastiera vera: apre la chat, scrive il messaggio e, se l'invio automatico è acceso, lo "
                     L"invia. I comandi della nave aspettano che il messaggio sia finito (e viceversa).",
                     kIn, y, kInW);
            y += 2;
            m_chatOpen = toggle(ID_CHAT_OPEN, L"Apri la chat prima di scrivere", kIn, y, kInW);
            y += 38;
            m_openKeyLabel = label(L"", kIn, y + 6, kInW - 110, 22);
            m_openKeyButton = button(ID_OPEN_KEY, L"Cambia", Ui::ButtonKind::Secondary, kIn + kInW - 96, y, 96, 30);
            y += 36;
            m_sendKeyLabel = label(L"", kIn, y + 6, kInW - 110, 22);
            m_sendKeyButton = button(ID_SEND_KEY, L"Cambia", Ui::ButtonKind::Secondary, kIn + kInW - 96, y, 96, 30);
            y += 40;
            m_delayLabel = label(L"", kIn, y + 6, kInW - 90, 22);
            button(ID_DELAY_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
            button(ID_DELAY_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
            y += 36;
            m_typingLabel = label(L"", kIn, y + 6, kInW - 90, 22);
            button(ID_TYPING_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
            button(ID_TYPING_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
            y += 40;
            m_unicode = toggle(ID_UNICODE, L"Lettere che la tastiera non ha (ñ, ß, ł...): mandale come Unicode", kIn, y, kInW);
            y += 32;
            y = note(L"Spento: si scrive la lettera semplice più vicina (ñ diventa n), funziona in tutti i giochi. Acceso: la lettera "
                     L"giusta, ma alcuni giochi la ignorano. Se la chat perde delle lettere, aumenta la pausa tra i caratteri.",
                     kIn, y, kInW);
            y = endCard(top, y - 6, L"SCRITTURA IN CHAT");

            // Soglia del silenzio
            top = y;
            y = beginCard(y);
            y = note(L"Sotto la soglia (la riga bianca) per la chat è silenzio: una pressione del tasto senza parlare non scrive niente. "
                     L"Calibra resta in ascolto 2 secondi: stai in silenzio.",
                     kIn, y, kInW);
            y += 2;
            m_meter = m_form.place(createMeter(m_hwnd, ID_METER), kIn, y, kInW, 8);
            y += 18;
            m_thresholdLabel = label(L"", kIn, y + 6, kInW - 220, 22);
            button(ID_CALIBRATE, L"Calibra", Ui::ButtonKind::Secondary, kIn + kInW - 210, y, 120, 30);
            button(ID_THRESHOLD_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
            button(ID_THRESHOLD_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
            y += 32;
            y = endCard(top, y, L"SOGLIA DEL SILENZIO");

            // Glossario
            top = y;
            y = beginCard(y);
            y = note(L"Il gergo della community con la traduzione da usare sempre, una riga per termine: per esempio "
                     L"QT=quantum travel. Le modifiche valgono subito, senza riavviare.",
                     kIn, y, kInW);
            y += 2;
            button(ID_GLOSSARY, L"Apri il glossario", Ui::ButtonKind::Secondary, kIn, y, 190);
            y += 32;
            endCard(top, y, L"GLOSSARIO");
            onShow();
        }

        bool onDrawItem(const DRAWITEMSTRUCT* dis) override {
            if (dis->CtlID != ID_METER) return false;
            drawThresholdMeter(dis, m_assistant.micLevel(), m_assistant.settings().silenceThreshold);
            return true;
        }

        void onCommand(int id, int code, HWND) override {
            if (id == ID_SOURCE || id == ID_TARGET) {
                if (code != CBN_SELCHANGE) return;
                int src = (int)SendMessageW(m_source, CB_GETCURSEL, 0, 0), tgt = (int)SendMessageW(m_target, CB_GETCURSEL, 0, 0);
                m_assistant.updateSettings([&](AppSettings& s) {
                    s.sourceLanguageIndex = src;
                    s.targetLanguageIndex = tgt;
                });
                return;
            }
            if (id == ID_HISTORY) {
                if (code == 1) {
                    std::string text = HistoryView::writtenAt(m_history, HistoryView::selected(m_history));
                    if (!text.empty()) copyToClipboard(m_hwnd, w(text));
                }
                return;
            }
            if (code != BN_CLICKED) return;
            if (id >= ID_REMOVE_BASE && id < ID_REMOVE_BASE + 16) {
                size_t index = (size_t)(id - ID_REMOVE_BASE);
                m_assistant.updateSettings([&](AppSettings& s) {
                    if (index < s.chatTriggers.size()) s.chatTriggers.erase(s.chatTriggers.begin() + index);
                });
                return;
            }
            auto flip = [&](HWND t, auto setter) {
                bool on = !Ui::isChecked(t);
                Ui::setChecked(t, on);
                m_assistant.updateSettings([&](AppSettings& s) { setter(s, on); });
            };
            switch (id) {
            case ID_ENABLE: flip(m_enable, [](AppSettings& s, bool on) { s.chatEnabled = on; }); break;
            case ID_GAMING: flip(m_gaming, [](AppSettings& s, bool on) { s.gamingTone = on; }); break;
            case ID_AUTOSEND: flip(m_autoSend, [](AppSettings& s, bool on) { s.autoSend = on; }); break;
            case ID_CHAT_OPEN: flip(m_chatOpen, [](AppSettings& s, bool on) { s.chatOpenEnabled = on; }); break;
            case ID_UNICODE: flip(m_unicode, [](AppSettings& s, bool on) { s.unicodeFallback = on; }); break;
            case ID_SWAP: m_assistant.updateSettings([](AppSettings& s) { std::swap(s.sourceLanguageIndex, s.targetLanguageIndex); }); break;
            case ID_TEST: {
                std::string text = TextUtils::trim(u8(windowText(m_testEdit)));
                if (text.empty()) break;
                m_assistant.chat().translateTest(text);
                SetWindowTextW(m_testEdit, L"");
                break;
            }
            case ID_ADD: startCapture(Capture::Talk); break;
            case ID_OPEN_KEY: startCapture(Capture::OpenKey); break;
            case ID_SEND_KEY: startCapture(Capture::SendKey); break;
            case ID_DELAY_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.chatOpenDelayMs -= 30; }); break;
            case ID_DELAY_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.chatOpenDelayMs += 30; }); break;
            case ID_TYPING_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.typingDelayMs -= 2; }); break;
            case ID_TYPING_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.typingDelayMs += 2; }); break;
            case ID_THRESHOLD_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.silenceThreshold /= 1.25; }); break;
            case ID_THRESHOLD_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.silenceThreshold *= 1.25; }); break;
            case ID_CALIBRATE: m_assistant.chat().calibrateSilence(); break;
            case ID_GLOSSARY:
                Glossary::load(); // se non c'è, lo crea con gli esempi
                openInNotepad(m_hwnd, Glossary::filePath());
                break;
            }
        }

        LRESULT onAppMessage(UINT msg, WPARAM wp, LPARAM lp) override {
            if (msg == WM_APP_REBUILD) {
                if (m_capture == Capture::None) rebuild();
                return 0;
            }
            if (msg != WM_APP_CAPTURED) return 0;
            std::unique_ptr<TalkTrigger> t(reinterpret_cast<TalkTrigger*>(lp));
            const Capture kind = m_capture;
            // Il tasto si salva PRIMA di riattivare i tasti per parlare (in stopCapture).
            struct Resume {
                ChatPage* page;
                ~Resume() { page->stopCapture(); }
            } resume{ this };
            if (!wp || !t) {
                Ui::setText(m_captureLabel, kind == Capture::Talk ? L"Annullato." : L"");
                refresh();
                return 0;
            }
            if (kind == Capture::Talk) {
                AppSettings s = m_assistant.settings();
                if (std::find(s.chatTriggers.begin(), s.chatTriggers.end(), *t) != s.chatTriggers.end()) {
                    Ui::setText(m_captureLabel, L"Questo tasto c'è già.");
                } else {
                    Ui::setText(m_captureLabel, L"");
                    m_assistant.updateSettings([&](AppSettings& x) { x.chatTriggers.push_back(*t); });
                }
            } else if (kind == Capture::OpenKey) {
                m_assistant.updateSettings([&](AppSettings& x) { x.chatOpenKeyVk = t->vk; });
            } else if (kind == Capture::SendKey) {
                m_assistant.updateSettings([&](AppSettings& x) { x.chatSendKeyVk = t->vk; });
            }
            refresh();
            return 0;
        }

    private:
        void refresh() {
            AppSettings s = m_assistant.settings();
            Ui::setChecked(m_enable, s.chatEnabled);
            auto connected = JoystickInput::devices();
            for (size_t i = 0; i < m_triggerLabels.size() && i < s.chatTriggers.size(); ++i) {
                const TalkTrigger& t = s.chatTriggers[i];
                std::wstring text = w(HotkeyListener::describeTrigger(t));
                if (t.type == TalkTrigger::Type::Joystick) {
                    bool present = std::any_of(connected.begin(), connected.end(), [&](const auto& d) { return d.id == t.joystickId; });
                    if (!present) text += L"  (non collegato)";
                }
                const bool shared = std::find(s.triggers.begin(), s.triggers.end(), t) != s.triggers.end();
                if (shared) text += L"  — anche per Iris";
                Ui::setText(m_triggerLabels[i], text);
                Ui::setLabelColor(m_triggerLabels[i], shared ? Ui::Color::Accent : Ui::Color::Text);
            }
            SendMessageW(m_source, CB_SETCURSEL, s.sourceLanguageIndex, 0);
            SendMessageW(m_target, CB_SETCURSEL, s.targetLanguageIndex, 0);
            Ui::setChecked(m_gaming, s.gamingTone);
            Ui::setChecked(m_autoSend, s.autoSend);
            Ui::setChecked(m_chatOpen, s.chatOpenEnabled);
            Ui::setChecked(m_unicode, s.unicodeFallback);
            if (m_capture != Capture::OpenKey) Ui::setText(m_openKeyLabel, L"Tasto per aprire la chat: " + w(HotkeyListener::keyName(s.chatOpenKeyVk)));
            if (m_capture != Capture::SendKey) Ui::setText(m_sendKeyLabel, L"Tasto per inviare il messaggio: " + w(HotkeyListener::keyName(s.chatSendKeyVk)));
            EnableWindow(m_openKeyButton, s.chatOpenEnabled || m_capture == Capture::OpenKey);
            Ui::setText(m_delayLabel, L"Attesa dopo l'apertura della chat: " + std::to_wstring(s.chatOpenDelayMs) + L" ms");
            Ui::setText(m_typingLabel, L"Pausa tra un carattere e l'altro: " + std::to_wstring(s.typingDelayMs) + L" ms");
            wchar_t buf[64];
            swprintf(buf, 64, L"Soglia del silenzio: %.4f", s.silenceThreshold);
            Ui::setText(m_thresholdLabel, buf);
        }

        void updateNotice() {
            GameChat& chat = m_assistant.chat();
            std::wstring text;
            COLORREF color = Ui::Color::TextDim;
            if (chat.busy()) {
                const std::string b = chat.busyText();
                text = L"In corso: " + w(b.empty() ? std::string("in coda...") : b);
                color = Ui::Color::Busy;
            } else {
                const GameChat::Notice n = chat.notice();
                if (!n.text.empty()) {
                    text = L"Ultimo: " + w(n.text);
                    color = n.error ? Ui::Color::Error : Ui::Color::Ok;
                }
            }
            if (text != m_lastNotice) {
                Ui::setText(m_notice, text);
                Ui::setLabelColor(m_notice, color);
                m_lastNotice = text;
            }
        }

        void fillHistory() {
            auto h = m_assistant.chat().history();
            HistoryView::setEntries(m_history, std::vector<GameChat::HistoryEntry>(h.rbegin(), h.rend()));
        }

        void startCapture(Capture kind) {
            if (m_capture != Capture::None) {
                m_captureCancel = true; // secondo clic = annulla
                return;
            }
            m_capture = kind;
            m_captureCancel = false;
            m_assistant.setTalkPaused(true);
            const bool talk = kind == Capture::Talk;
            if (talk) {
                Ui::setText(m_add, L"Annulla");
                Ui::setText(m_captureLabel, L"Premi il tasto o il pulsante del joystick da usare...");
            } else {
                HWND l = kind == Capture::OpenKey ? m_openKeyLabel : m_sendKeyLabel;
                Ui::setText(l, L"Premi il tasto da usare (Esc per annullare)...");
                Ui::setText(kind == Capture::OpenKey ? m_openKeyButton : m_sendKeyButton, L"Annulla");
            }
            HWND target = m_hwnd;
            m_thread = std::thread([this, talk, target]() {
                auto* t = new TalkTrigger();
                bool ok = HotkeyListener::captureNextInput(*t, m_captureCancel, 15000, talk);
                if (!PostMessageW(target, WM_APP_CAPTURED, ok ? 1 : 0, (LPARAM)t)) delete t;
            });
        }

        void stopCapture() {
            if (m_thread.joinable()) {
                m_captureCancel = true;
                m_thread.join();
            }
            if (m_capture != Capture::None) m_assistant.setTalkPaused(false);
            m_capture = Capture::None;
            if (IsWindow(m_add)) Ui::setText(m_add, L"+ Aggiungi un tasto");
            if (IsWindow(m_openKeyButton)) Ui::setText(m_openKeyButton, L"Cambia");
            if (IsWindow(m_sendKeyButton)) Ui::setText(m_sendKeyButton, L"Cambia");
            if (IsWindow(m_hwnd) && m_assistant.settings().chatTriggers.size() != m_rows) PostMessageW(m_hwnd, WM_APP_REBUILD, 0, 0);
        }

        size_t m_rows = 0;
        std::vector<HWND> m_triggerLabels;
        HWND m_enable = nullptr, m_notice = nullptr, m_add = nullptr, m_captureLabel = nullptr;
        HWND m_source = nullptr, m_target = nullptr, m_gaming = nullptr, m_autoSend = nullptr;
        HWND m_history = nullptr, m_testEdit = nullptr;
        HWND m_chatOpen = nullptr, m_unicode = nullptr, m_openKeyLabel = nullptr, m_openKeyButton = nullptr;
        HWND m_sendKeyLabel = nullptr, m_sendKeyButton = nullptr, m_delayLabel = nullptr, m_typingLabel = nullptr;
        HWND m_meter = nullptr, m_thresholdLabel = nullptr;
        std::wstring m_lastNotice;
        Capture m_capture = Capture::None;
        std::atomic<bool> m_captureCancel{ false };
        std::thread m_thread;
    };

    // =====================================================================================
    // Traduzione della chat degli altri (OCR)
    // =====================================================================================

    class ChatScreenPage : public Page {
    public:
        explicit ChatScreenPage(Assistant& a) : Page(a, L"Traduzione chat") {}
        void onShow() override { refresh(); }
        void onEngineEvent(Assistant::Event e) override {
            if (e == Assistant::Event::Settings) refresh();
        }

    protected:
        enum {
            ID_ENABLE = 100, ID_REGION, ID_TARGET, ID_LANG_SETTINGS, ID_MAX_MINUS, ID_MAX_PLUS, ID_OPACITY_MINUS,
            ID_OPACITY_PLUS, ID_UNLOCK, ID_RESET, ID_FILTER_EDIT, ID_FILTER_SAVE, ID_DEBUG
        };

        void build() override {
            int y = 16;
            int top = y;
            y = beginCard(y);
            y = note(L"Legge una zona dello schermo (la chat del gioco), traduce i messaggi degli altri giocatori e li mostra in un "
                     L"riquadro appena sotto. Legge solo con Star Citizen in primo piano e ogni messaggio si traduce una volta "
                     L"sola. Mentre parli, o mentre Iris usa il modello, si mette in pausa.",
                     kIn, y, kInW);
            y += 2;
            m_enable = toggle(ID_ENABLE, L"Attiva la traduzione della chat", kIn, y, kInW);
            y += 38;
            m_regionLabel = label(L"", kIn, y + 6, kInW - 190, 22);
            button(ID_REGION, L"Scegli la zona", Ui::ButtonKind::Primary, kIn + kInW - 176, y, 176);
            y += 42;
            label(L"Traduci in", kIn, y, 240, 18, Ui::Color::TextDim, Ui::Font::Small);
            y += 20;
            m_target = combo(ID_TARGET, languageNames(), kIn, y, 240);
            y += 42;
            y = note(L"Si traducono solo i messaggi in inglese: quelli in altre lingue (italiano, polacco...) restano come sono.",
                     kIn, y, kInW);
            y += 4;
            const int infoHeight = measureText(L"x\nx\nx", kInW, Ui::Font::Normal, m_dpi);
            m_ocrInfo = label(L"", kIn, y, kInW, infoHeight, Ui::Color::TextDim);
            y += infoHeight + 6;
            button(ID_LANG_SETTINGS, L"Lingue di Windows...", Ui::ButtonKind::Secondary, kIn, y, 200);
            y += 32;
            y = endCard(top, y, L"TRADUZIONE DELLA CHAT DEGLI ALTRI");

            top = y;
            y = beginCard(y);
            m_maxLabel = label(L"", kIn, y + 6, kInW - 90, 22);
            button(ID_MAX_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
            button(ID_MAX_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
            y += 36;
            m_opacityLabel = label(L"", kIn, y + 6, kInW - 90, 22);
            button(ID_OPACITY_MINUS, L"−", Ui::ButtonKind::Secondary, kIn + kInW - 78, y, 36, 30);
            button(ID_OPACITY_PLUS, L"+", Ui::ButtonKind::Secondary, kIn + kInW - 36, y, 36, 30);
            y += 40;
            button(ID_UNLOCK, L"Sblocca per spostarlo (30 s)", Ui::ButtonKind::Secondary, kIn, y, 250);
            button(ID_RESET, L"Rimettilo sotto la zona", Ui::ButtonKind::Secondary, kIn + 260, y, 220);
            y += 32;
            y = endCard(top, y, L"RIQUADRO DELLA TRADUZIONE");

            top = y;
            y = beginCard(y);
            y = note(L"I messaggi di un canale o di un'organizzazione (per esempio LSE) si possono raccogliere nella pagina "
                     L"Messaggi filtrati. Scrivi il nome senza parentesi; vuoto = nessun filtro.",
                     kIn, y, kInW);
            y += 2;
            m_filterEdit = m_form.place(Ui::createEdit(m_hwnd, ID_FILTER_EDIT, false, false, 40), kIn, y + 1, 220, 28);
            SetWindowSubclass(m_filterEdit, enterProc, 1, ID_FILTER_SAVE);
            SendMessageW(m_filterEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"es. LSE");
            button(ID_FILTER_SAVE, L"Salva", Ui::ButtonKind::Primary, kIn + 230, y, 100, 30);
            m_filterSaved = label(L"", kIn + 342, y + 6, kInW - 342, 22, Ui::Color::Ok);
            y += 42;
            button(ID_DEBUG, L"Apri il file di diagnosi dell'OCR", Ui::ButtonKind::Secondary, kIn, y, 290);
            y += 36;
            y = note(L"Il file mostra cosa ha letto l'OCR e come l'ha diviso in messaggi: utile se qualcosa non viene tradotto.",
                     kIn, y, kInW);
            endCard(top, y - 6, L"FILTRO E DIAGNOSI");
            SetWindowTextW(m_filterEdit, w(m_assistant.settings().chatFilterTag).c_str());
            refresh();
        }

        void onCommand(int id, int code, HWND) override {
            if (id == ID_TARGET) {
                if (code != CBN_SELCHANGE) return;
                int tgt = (int)SendMessageW(m_target, CB_GETCURSEL, 0, 0);
                m_assistant.updateSettings([&](AppSettings& s) { s.ocrTargetLanguageIndex = tgt; });
                return;
            }
            if (id == ID_FILTER_EDIT) {
                if (code == EN_CHANGE) Ui::setText(m_filterSaved, L"");
                return;
            }
            if (code != BN_CLICKED) return;
            switch (id) {
            case ID_ENABLE: {
                bool on = !Ui::isChecked(m_enable);
                Ui::setChecked(m_enable, on);
                m_assistant.updateSettings([&](AppSettings& s) { s.ocrEnabled = on; });
                break;
            }
            case ID_REGION: chooseRegion(); break;
            case ID_LANG_SETTINGS: ShellExecuteW(m_hwnd, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL); break;
            case ID_MAX_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.ocrMaxMessages -= 1; }); break;
            case ID_MAX_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.ocrMaxMessages += 1; }); break;
            case ID_OPACITY_MINUS: m_assistant.updateSettings([](AppSettings& s) { s.ocrOverlayOpacity -= 13; }); break;
            case ID_OPACITY_PLUS: m_assistant.updateSettings([](AppSettings& s) { s.ocrOverlayOpacity += 13; }); break;
            case ID_UNLOCK: OcrOverlay::unlockForMove(30); break;
            case ID_RESET:
                m_assistant.updateSettings([](AppSettings& s) {
                    s.ocrOverlayX = -1;
                    s.ocrOverlayY = -1;
                });
                break;
            case ID_FILTER_SAVE: {
                std::string tag = TextUtils::trim(u8(windowText(m_filterEdit)));
                // Le parentesi le aggiunge il gioco: "[LSE]" e "LSE" sono la stessa cosa.
                tag.erase(std::remove_if(tag.begin(), tag.end(), [](char c) { return c == '[' || c == ']'; }), tag.end());
                tag = TextUtils::trim(tag);
                SetWindowTextW(m_filterEdit, w(tag).c_str());
                m_assistant.updateSettings([&](AppSettings& s) { s.chatFilterTag = tag; });
                Ui::setText(m_filterSaved, tag.empty() ? L"Filtro tolto." : L"Salvato.");
                break;
            }
            case ID_DEBUG: {
                std::wstring path = AppPaths::ocrDebugPath();
                if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    MessageBoxW(GetAncestor(m_hwnd, GA_ROOT),
                                L"Il file non c'è ancora: compare dopo la prima lettura dello schermo (attiva la traduzione e scegli la zona).",
                                L"Iris", MB_OK | MB_ICONINFORMATION);
                } else {
                    openInNotepad(m_hwnd, path);
                }
                break;
            }
            }
        }

    private:
        void chooseRegion() {
            HWND main = GetAncestor(m_hwnd, GA_ROOT);
            // La finestra si toglie di mezzo: la "foto" dello schermo deve mostrare il gioco.
            ShowWindow(main, SW_HIDE);
            DwmFlush();
            Sleep(200);
            DwmFlush();
            RECT r{};
            bool ok = RegionSelector::selectRegion(GetModuleHandleW(nullptr), r);
            ShowWindow(main, SW_SHOW);
            SetForegroundWindow(main);
            if (!ok) return;
            m_assistant.updateSettings([&](AppSettings& s) {
                s.ocrRegionX = r.left;
                s.ocrRegionY = r.top;
                s.ocrRegionW = r.right - r.left;
                s.ocrRegionH = r.bottom - r.top;
                s.ocrEnabled = true; // scegliere la zona vuol dire volerla usare
            });
        }

        void refresh() {
            AppSettings s = m_assistant.settings();
            Ui::setChecked(m_enable, s.ocrEnabled);
            SendMessageW(m_target, CB_SETCURSEL, s.ocrTargetLanguageIndex, 0);
            if (s.ocrRegionW > 0 && s.ocrRegionH > 0) {
                Ui::setText(m_regionLabel, L"Zona: " + std::to_wstring(s.ocrRegionW) + L" × " + std::to_wstring(s.ocrRegionH) +
                                               L" pixel, in (" + std::to_wstring(s.ocrRegionX) + L", " + std::to_wstring(s.ocrRegionY) + L")");
                Ui::setLabelColor(m_regionLabel, Ui::Color::Text);
            } else {
                Ui::setText(m_regionLabel, L"Nessuna zona scelta.");
                Ui::setLabelColor(m_regionLabel, Ui::Color::Busy);
            }
            Ui::setText(m_maxLabel, L"Messaggi mostrati: " + std::to_wstring(s.ocrMaxMessages));
            Ui::setText(m_opacityLabel, L"Opacità: " + percent(s.ocrOverlayOpacity));
            std::wstring installed;
            for (const auto& code : OcrEngineWrapper::installedLanguages()) installed += (installed.empty() ? L"" : L", ") + w(code);
            std::wstring text;
            COLORREF color = Ui::Color::TextDim;
            if (installed.empty()) {
                text = L"Windows non ha nessuna lingua per l'OCR: aggiungine una da Impostazioni › Ora e lingua › Lingua.";
                color = Ui::Color::Error;
            } else if (OcrEngineWrapper::isLanguageSupported("en")) {
                text = L"OCR di Windows pronto per: " + installed + L".";
            } else {
                text = L"L'OCR per l'inglese non è installato in Windows: uso " + installed +
                       L" (stesso alfabeto, va bene). Per una lettura migliore puoi installarlo.";
                color = Ui::Color::Busy;
            }
            Ui::setText(m_ocrInfo, text);
            Ui::setLabelColor(m_ocrInfo, color);
        }

        HWND m_enable = nullptr, m_regionLabel = nullptr, m_target = nullptr, m_ocrInfo = nullptr;
        HWND m_maxLabel = nullptr, m_opacityLabel = nullptr, m_filterEdit = nullptr, m_filterSaved = nullptr;
    };

    // =====================================================================================
    // Messaggi filtrati
    // =====================================================================================

    class FilteredPage : public Page {
    public:
        explicit FilteredPage(Assistant& a) : Page(a, L"Messaggi filtrati") {}
        void onShow() override {
            fill();
            refresh();
        }
        void onEngineEvent(Assistant::Event e) override {
            if (e == Assistant::Event::Filtered) fill();
            else if (e == Assistant::Event::Settings) refresh();
        }

    protected:
        enum { ID_LIST = 100, ID_COPY, ID_CLEAR };

        void build() override {
            int y = 16;
            int top = y;
            y = beginCard(y);
            m_filterLabel = label(L"", kIn, y, kInW, 22);
            y += 28;
            m_list = m_form.place(Ui::createEdit(m_hwnd, ID_LIST, true, true), kIn, y, kInW, 400, Ui::Font::Normal);
            y += 410;
            button(ID_COPY, L"Copia tutto", Ui::ButtonKind::Secondary, kIn, y, 150);
            button(ID_CLEAR, L"Svuota", Ui::ButtonKind::Danger, kIn + 160, y, 120);
            y += 32;
            endCard(top, y, L"MESSAGGI FILTRATI");
            onShow();
        }

        void onCommand(int id, int code, HWND) override {
            if (code != BN_CLICKED) return;
            if (id == ID_COPY) copyToClipboard(m_hwnd, windowText(m_list));
            else if (id == ID_CLEAR) m_assistant.chat().clearFilteredMessages();
        }

    private:
        void refresh() {
            AppSettings s = m_assistant.settings();
            if (s.chatFilterTag.empty()) {
                Ui::setText(m_filterLabel, L"Nessun filtro: scegli un canale o un'organizzazione nella pagina Traduzione chat.");
                Ui::setLabelColor(m_filterLabel, Ui::Color::Busy);
            } else {
                Ui::setText(m_filterLabel, L"Messaggi di [" + w(s.chatFilterTag) + L"], già tradotti, dal più vecchio al più nuovo.");
                Ui::setLabelColor(m_filterLabel, Ui::Color::Text);
            }
        }

        void fill() {
            std::wstring text;
            for (const auto& line : m_assistant.chat().filteredMessages()) text += w(line) + L"\r\n";
            if (text == windowText(m_list)) return;
            SetWindowTextW(m_list, text.c_str());
            int len = GetWindowTextLengthW(m_list);
            SendMessageW(m_list, EM_SETSEL, len, len);
            SendMessageW(m_list, EM_SCROLLCARET, 0, 0);
        }

        HWND m_filterLabel = nullptr, m_list = nullptr;
    };
}

std::unique_ptr<Page> createChatPage(Assistant& assistant) {
    return std::make_unique<ChatPage>(assistant);
}

std::unique_ptr<Page> createChatScreenPage(Assistant& assistant) {
    return std::make_unique<ChatScreenPage>(assistant);
}

std::unique_ptr<Page> createFilteredPage(Assistant& assistant) {
    return std::make_unique<FilteredPage>(assistant);
}
