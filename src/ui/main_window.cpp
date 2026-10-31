#include "main_window.h"
#include "pages.h"
#include "../ocr_overlay.h"
#include "../overlay.h"
#include "../text_utils.h"

#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace {
    constexpr int kNavWidth = 190;
    constexpr int kNavTop = 92;
    constexpr int kNavItemHeight = 42;
    constexpr int kStatusHeight = 118;
    constexpr int kDefaultHeight = 760;
    constexpr int kMinHeight = 460;
    constexpr UINT_PTR kMeterTimer = 1;
    constexpr int ID_NAV_BASE = 1000;
    constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME | WS_CLIPCHILDREN;
    constexpr const wchar_t* kViewportClass = L"IrisViewport";

    struct State {
        Assistant* engine = nullptr;
        HWND hwnd = nullptr;
        HWND viewport = nullptr;
        std::vector<std::unique_ptr<Page>> pages;
        std::vector<HWND> nav;
        int current = -1;
        int scroll = 0;
        UINT dpi = 96;
    };
    State* g = nullptr;

    int px(int v) { return Ui::px(v, g ? g->dpi : 96); }

    // Larghezza fissa: menu + pagina + barra di scorrimento.
    int clientWidth(UINT dpi) {
        return Ui::px(kNavWidth + kPageWidth, dpi) + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    }

    int windowWidth(UINT dpi) {
        RECT r{ 0, 0, clientWidth(dpi), 100 };
        AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dpi);
        return r.right - r.left;
    }

    RECT statusRect() {
        RECT rc;
        GetClientRect(g->hwnd, &rc);
        return RECT{ 0, rc.bottom - px(kStatusHeight), px(kNavWidth), rc.bottom };
    }

    void updateScroll() {
        if (!g || !g->viewport || g->current < 0) return;
        Page* page = g->pages[g->current].get();
        RECT vr;
        GetClientRect(g->viewport, &vr);
        const int content = px(page->contentHeight());
        const int view = std::max(1, (int)vr.bottom);
        g->scroll = std::clamp(g->scroll, 0, std::max(0, content - view));
        SCROLLINFO si{ sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL };
        si.nMin = 0;
        si.nMax = std::max(0, content - 1);
        si.nPage = (UINT)view;
        si.nPos = g->scroll;
        SetScrollInfo(g->viewport, SB_VERT, &si, TRUE);
        SetWindowPos(page->hwnd(), nullptr, 0, -g->scroll, vr.right, std::max(content, view), SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void scrollTo(int pos) {
        g->scroll = pos;
        updateScroll();
    }

    void layout() {
        if (!g || !g->viewport) return;
        RECT rc;
        GetClientRect(g->hwnd, &rc);
        const int navW = px(kNavWidth);
        SetWindowPos(g->viewport, nullptr, navW, 0, rc.right - navW, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        for (size_t i = 0; i < g->nav.size(); ++i) {
            SetWindowPos(g->nav[i], nullptr, 0, px(kNavTop + (int)i * kNavItemHeight), navW, px(kNavItemHeight),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        updateScroll();
        InvalidateRect(g->hwnd, nullptr, FALSE);
    }

    void showPage(int index) {
        if (index < 0 || index >= (int)g->pages.size() || index == g->current) return;
        if (g->current >= 0) {
            ShowWindow(g->pages[g->current]->hwnd(), SW_HIDE);
            Ui::setChecked(g->nav[g->current], false);
        }
        g->current = index;
        g->scroll = 0;
        Ui::setChecked(g->nav[index], true);
        g->pages[index]->onShow();
        updateScroll();
        ShowWindow(g->pages[index]->hwnd(), SW_SHOW);
    }

    // La sfera fluttuante: lo stato di Iris in movimento.
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

    void applyOverlaySettings() {
        AppSettings s = g->engine->settings();
        Overlay::setOpacity(s.overlayOpacity);
        Overlay::setPosition(s.overlayX, s.overlayY);
        Overlay::setSize(s.overlaySize);
        Overlay::setVisible(s.overlayEnabled);
        OcrOverlay::setOpacity(s.ocrOverlayOpacity);
        OcrOverlay::setPosition(s.ocrOverlayX, s.ocrOverlayY);
    }

    void onEngineEvent(Assistant::Event e) {
        switch (e) {
        case Assistant::Event::Status: {
            RECT r = statusRect();
            InvalidateRect(g->hwnd, &r, FALSE);
            break;
        }
        case Assistant::Event::Screen: {
            // La chat degli altri, tradotta, nel riquadro sotto la chat del gioco.
            GameChat::ScreenView view = g->engine->chat().screenView();
            if (view.visible && !view.lines.empty()) {
                RECT region{ view.regionX, view.regionY, view.regionX + view.regionW, view.regionY + view.regionH };
                std::vector<OcrOverlay::Line> lines;
                for (const auto& l : view.lines) {
                    lines.push_back({ TextUtils::utf8ToWide(l.prefix), TextUtils::utf8ToWide(l.text), l.pending });
                }
                OcrOverlay::show(region, lines);
            } else {
                OcrOverlay::hide();
            }
            break;
        }
        case Assistant::Event::Settings:
            applyOverlaySettings();
            break;
        default:
            break;
        }
        for (auto& page : g->pages) page->onEngineEvent(e);
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const UINT dpi = g->dpi;
        {
            Ui::Buffered buffer(hdc, ps.rcPaint);
            HDC dc = buffer.dc();
            RECT nav{ 0, 0, px(kNavWidth), rc.bottom };
            FillRect(dc, &nav, Ui::brush(Ui::Color::Nav));

            RECT title{ px(20), px(24), nav.right - px(10), px(52) };
            Ui::drawText(dc, L"IRIS", title, Ui::font(Ui::Font::Title, dpi), Ui::Color::Text, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT sub{ px(21), px(54), nav.right - px(10), px(72) };
            Ui::drawText(dc, L"assistente vocale", sub, Ui::font(Ui::Font::Small, dpi), Ui::Color::TextDim,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            // Stato del programma, sempre visibile qualunque pagina sia aperta.
            RECT st = statusRect();
            RECT line{ st.left + px(16), st.top, st.right - px(16), st.top + 1 };
            FillRect(dc, &line, Ui::brush(Ui::Color::PanelBorder));
            Assistant::Status status = g->engine->status();
            Ui::fillCircle(dc, st.left + px(24), st.top + px(24), px(5), stateColor(status.state));
            RECT label{ st.left + px(36), st.top + px(14), st.right - px(10), st.top + px(34) };
            Ui::drawText(dc, stateLabel(status.state), label, Ui::font(Ui::Font::Bold, dpi), Ui::Color::Text,
                         DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            RECT detail{ st.left + px(18), st.top + px(38), st.right - px(14), st.bottom - px(10) };
            Ui::drawText(dc, TextUtils::utf8ToWide(status.text), detail, Ui::font(Ui::Font::Small, dpi), Ui::Color::TextDim,
                         DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
        }
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK viewportProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_VSCROLL: {
            if (!g) break;
            SCROLLINFO si{ sizeof(si), SIF_ALL };
            GetScrollInfo(hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wp)) {
            case SB_LINEUP: pos -= px(40); break;
            case SB_LINEDOWN: pos += px(40); break;
            case SB_PAGEUP: pos -= (int)si.nPage; break;
            case SB_PAGEDOWN: pos += (int)si.nPage; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = si.nMax; break;
            }
            scrollTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (g) scrollTo(g->scroll - GET_WHEEL_DELTA_WPARAM(wp) * px(64) / WHEEL_DELTA);
            return 0;
        case WM_SIZE:
            updateScroll();
            return 0;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, Ui::brush(Ui::Color::Background));
            return 1;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            UINT dpi = Ui::dpiOf(hwnd);
            mmi->ptMinTrackSize.x = mmi->ptMaxTrackSize.x = windowWidth(dpi);
            mmi->ptMinTrackSize.y = Ui::px(kMinHeight, dpi);
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            if (!g) break;
            g->dpi = HIWORD(wp);
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, windowWidth(g->dpi), suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            for (auto& page : g->pages) page->applyDpi(g->dpi);
            layout();
            return 0;
        }
        case WM_DRAWITEM:
            if (Ui::drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp))) return TRUE;
            break;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (g && id >= ID_NAV_BASE && id < ID_NAV_BASE + (int)g->nav.size()) showPage(id - ID_NAV_BASE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (!g) break;
            paint(hwnd);
            return 0;
        case WM_TIMER:
            if (g && wp == kMeterTimer) {
                // La sfera fluttuante si muove sempre (anche con la finestra ridotta: è lì per il gioco).
                Overlay::animate(orbMode(g->engine->status().state), g->engine->micLevel(), g->engine->voiceLevel(), g->engine->userSpeaking());
                // La pagina (sfera grande, barra del microfono) solo se la si vede.
                if (g->current >= 0 && !IsIconic(hwnd)) g->pages[g->current]->onTimer();
            }
            return 0;
        case WM_APP_ENGINE:
            if (g) onEngineEvent((Assistant::Event)wp);
            return 0;
        case WM_APP_CONTENT_CHANGED:
            updateScroll();
            return 0;
        case WM_CLOSE:
            if (g && !IsIconic(hwnd)) {
                RECT r;
                GetWindowRect(hwnd, &r);
                g->engine->updateSettings([&](AppSettings& s) {
                    s.windowX = r.left;
                    s.windowY = r.top;
                });
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kMeterTimer);
            if (g) g->engine->setListener(nullptr);
            Overlay::setOnMoved(nullptr);
            OcrOverlay::setOnMoved(nullptr);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            delete g;
            g = nullptr;
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

namespace MainWindow {

HWND create(HINSTANCE instance, Assistant& engine, int showCommand) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    WNDCLASSW vc{};
    vc.lpfnWndProc = viewportProc;
    vc.hInstance = instance;
    vc.lpszClassName = kViewportClass;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&vc);

    g = new State();
    g->engine = &engine;

    AppSettings s = engine.settings();
    const bool savedOnScreen = !(s.windowX == -1 && s.windowY == -1) &&
                               MonitorFromPoint(POINT{ s.windowX + 50, s.windowY + 20 }, MONITOR_DEFAULTTONULL) != nullptr;
    POINT start = savedOnScreen ? POINT{ s.windowX, s.windowY } : POINT{ 0, 0 };
    if (!savedOnScreen) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        start = POINT{ mi.rcWork.left + 60, mi.rcWork.top + 40 };
    }

    HWND hwnd = CreateWindowExW(0, kClassName, L"Iris", kStyle, start.x, start.y, 800, 600, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        delete g;
        g = nullptr;
        return nullptr;
    }
    g->hwnd = hwnd;
    g->dpi = Ui::dpiOf(hwnd);
    Ui::darkTitleBar(hwnd);

    // Misure per la scala del monitor dove si apre; mai più alta dello schermo.
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wr{ 0, 0, clientWidth(g->dpi), px(kDefaultHeight) };
    AdjustWindowRectExForDpi(&wr, kStyle, FALSE, 0, g->dpi);
    const int width = wr.right - wr.left;
    const int height = std::min((int)(wr.bottom - wr.top), (int)(mi.rcWork.bottom - mi.rcWork.top) - px(20));
    int x = start.x, y = start.y;
    if (!savedOnScreen) {
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
    }
    y = std::max(y, (int)mi.rcWork.top);
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    g->viewport = CreateWindowExW(WS_EX_CONTROLPARENT, kViewportClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 0,
                                  0, hwnd, nullptr, instance, nullptr);
    Ui::darkControl(g->viewport);

    g->pages = createPages(engine);
    for (size_t i = 0; i < g->pages.size(); ++i) {
        g->pages[i]->create(g->viewport);
        g->nav.push_back(Ui::createButton(hwnd, ID_NAV_BASE + (int)i, g->pages[i]->title(), Ui::ButtonKind::Nav, Ui::Color::Nav));
    }

    engine.setListener([hwnd](Assistant::Event e) { PostMessageW(hwnd, WM_APP_ENGINE, (WPARAM)e, 0); });
    Overlay::setOnMoved([&engine](int ox, int oy) {
        engine.updateSettings([&](AppSettings& st) {
            st.overlayX = ox;
            st.overlayY = oy;
        });
    });
    OcrOverlay::setOnMoved([&engine](int ox, int oy) {
        engine.updateSettings([&](AppSettings& st) {
            st.ocrOverlayX = ox;
            st.ocrOverlayY = oy;
        });
    });
    applyOverlaySettings();

    layout();
    showPage(0);
    SetTimer(hwnd, kMeterTimer, 33, nullptr); // ~30 al secondo: la sfera si muove fluida
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);
    return hwnd;
}

} // namespace MainWindow
