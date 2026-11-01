#include "axis_overlay.h"
#include "axis_panes.h"
#include "../axis_server.h"
#include "../log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

using json = nlohmann::json;

namespace AxisOverlay {

namespace {
    constexpr UINT WM_AXIS_OPEN = WM_APP + 1;   // lParam = Settings*, wParam = solo OBS
    constexpr UINT WM_AXIS_CLOSE = WM_APP + 2;
    constexpr UINT WM_AXIS_LOCK = WM_APP + 3;   // wParam = bloccato
    constexpr UINT WM_AXIS_VISIBLE = WM_APP + 4;
    constexpr UINT_PTR kFrameTimer = 1, kTopmostTimer = 2;
    constexpr const wchar_t* kWindowClass = L"IrisAxisOverlay";

    struct Window {
        HWND hwnd = nullptr;
        std::string key, name;
        std::vector<int> paneIndices;                       // nel profilo attivo
        std::vector<std::unique_ptr<AxisDraw::PaneView>> panes;
        std::vector<POINT> offsets;                         // posizione di ogni modulo nella finestra
        int width = 0, height = 0;
        float maxWidth = 0;
        bool fixed = false;                                 // un modulo "FISSO": la finestra non si sposta
        // L'immagine a 32 bit su cui si disegna.
        HDC dc = nullptr;
        HBITMAP bitmap = nullptr;
        HGDIOBJ old = nullptr;
        void* bits = nullptr;
    };

    std::mutex g_statusMutex;
    Status g_status;

    std::thread g_thread;
    std::atomic<DWORD> g_threadId{ 0 };
    HWND g_control = nullptr;
    AxisServer* g_server = nullptr;
    Callbacks g_callbacks;

    // Solo il thread dell'overlay.
    Axis::Settings g_settings;
    std::vector<Window> g_windows;
    bool g_obsOnly = false, g_visible = true, g_locked = false;
    HHOOK g_mouseHook = nullptr;
    LARGE_INTEGER g_lastFrame{}, g_frequency{};
    // Trascinamento col tasto centrale.
    Window* g_dragging = nullptr;
    POINT g_dragOffset{};
    DWORD g_lastMiddleDown = 0;
    POINT g_lastMiddlePoint{};

    void publishStatus() {
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status.open = !g_windows.empty();
            g_status.obsOnly = g_obsOnly;
            g_status.visible = !g_windows.empty() && !g_obsOnly && g_visible;
            g_status.locked = g_locked;
            g_status.windows = (int)g_windows.size();
        }
        if (g_callbacks.changed) g_callbacks.changed();
    }

    void freeBitmap(Window& w) {
        if (w.dc) {
            if (w.old) SelectObject(w.dc, w.old);
            DeleteDC(w.dc);
        }
        if (w.bitmap) DeleteObject(w.bitmap);
        w.dc = nullptr;
        w.bitmap = nullptr;
        w.old = nullptr;
        w.bits = nullptr;
    }

    // Moduli in fila da sinistra, con lo spazio fra uno e l'altro; con "colonne" si va a capo.
    void layoutWindow(Window& w) {
        const float gap = (float)g_settings.gap;
        float widest = 0;
        for (const auto& p : w.panes) widest = std::max(widest, p->width());
        w.maxWidth = g_settings.columns > 0 ? g_settings.columns * (widest + gap) : 0;
        float x = 0, y = 0, rowH = 0, maxX = 0;
        w.offsets.clear();
        for (const auto& p : w.panes) {
            if (w.maxWidth > 0 && x > 0 && x + p->width() + gap > w.maxWidth + 0.5f) {
                x = 0;
                y += rowH;
                rowH = 0;
            }
            w.offsets.push_back(POINT{ (LONG)std::lround(x), (LONG)std::lround(y) });
            x += p->width() + gap;
            rowH = std::max(rowH, p->height() + gap);
            maxX = std::max(maxX, x);
        }
        w.width = std::max(1, (int)std::ceil(maxX));
        w.height = std::max(1, (int)std::ceil(y + rowH));
        freeBitmap(w);
    }

    bool ensureBitmap(Window& w) {
        if (w.bitmap) return true;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w.width;
        bi.bmiHeader.biHeight = -w.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        w.bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &w.bits, nullptr, 0);
        w.dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!w.bitmap || !w.dc || !w.bits) {
            freeBitmap(w);
            return false;
        }
        w.old = SelectObject(w.dc, w.bitmap);
        return true;
    }

    void render(Window& w) {
        if (!w.hwnd || !IsWindowVisible(w.hwnd) || !ensureBitmap(w)) return;
        std::memset(w.bits, 0, (size_t)w.width * w.height * 4);
        {
            Gdiplus::Bitmap bmp(w.width, w.height, w.width * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(w.bits));
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
            for (size_t i = 0; i < w.panes.size(); ++i) w.panes[i]->draw(g, (float)w.offsets[i].x, (float)w.offsets[i].y);
        }
        POINT src{ 0, 0 };
        SIZE size{ w.width, w.height };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, (BYTE)std::lround(std::clamp(g_settings.opacity, 0.0, 1.0) * 255), AC_SRC_ALPHA };
        UpdateLayeredWindow(w.hwnd, nullptr, nullptr, &size, w.dc, &src, 0, &blend, ULW_ALPHA);
    }

    json describe(const Window& w) {
        json panes = json::array();
        for (const auto& p : w.panes) panes.push_back(p->describe());
        return { { "key", w.key },  { "name", w.name }, { "gap", g_settings.gap }, { "maxWidth", w.maxWidth }, { "opacity", g_settings.opacity },
                 { "font", g_settings.textSize }, { "bold", g_settings.boldText }, { "panes", panes } };
    }

    void publishLayout() {
        if (!g_server) return;
        std::vector<AxisServer::Window> windows;
        for (const auto& w : g_windows) windows.push_back({ w.key, w.name, describe(w).dump(-1, ' ', false, json::error_handler_t::replace) });
        g_server->publishLayout(windows);
        if (g_windows.empty()) g_server->publishValues({});
    }

    POINT defaultPosition(int ordinal) {
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        return POINT{ mi.rcWork.left + 40 + ordinal * 30, mi.rcWork.top + 120 + ordinal * 30 };
    }

    void closeAll(bool notify) {
        if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
        g_dragging = nullptr;
        for (auto& w : g_windows) {
            freeBitmap(w);
            if (w.hwnd) DestroyWindow(w.hwnd);
        }
        g_windows.clear();
        KillTimer(g_control, kFrameTimer);
        KillTimer(g_control, kTopmostTimer);
        publishLayout();
        if (notify) publishStatus();
    }

    void reportPosition(const Window& w) {
        RECT r;
        if (!GetWindowRect(w.hwnd, &r) || !g_callbacks.moved) return;
        std::vector<std::pair<int, POINT>> moved;
        for (int index : w.paneIndices) moved.push_back({ index, POINT{ r.left, r.top } });
        g_callbacks.moved(moved);
    }

    Window* windowAt(POINT p) {
        for (auto& w : g_windows) {
            RECT r;
            if (w.hwnd && IsWindowVisible(w.hwnd) && GetWindowRect(w.hwnd, &r) && PtInRect(&r, p)) return &w;
        }
        return nullptr;
    }

    // Il tasto centrale sposta (e il doppio click chiude); sinistro e destro passano al gioco.
    LRESULT CALLBACK mouseHook(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION) {
            const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (wp == WM_MOUSEWHEEL) Axis::addMouseWheel(GET_WHEEL_DELTA_WPARAM(m->mouseData) / (double)WHEEL_DELTA);
            if (!g_locked && !g_obsOnly && g_visible) {
                if (wp == WM_MBUTTONDOWN) {
                    if (Window* w = windowAt(m->pt)) {
                        const DWORD now = m->time;
                        const bool doubleClick = now - g_lastMiddleDown <= GetDoubleClickTime() && std::abs(m->pt.x - g_lastMiddlePoint.x) < 8 &&
                                                 std::abs(m->pt.y - g_lastMiddlePoint.y) < 8;
                        g_lastMiddleDown = now;
                        g_lastMiddlePoint = m->pt;
                        if (doubleClick) {
                            PostMessageW(g_control, WM_AXIS_CLOSE, 0, 0);
                            return 1;
                        }
                        if (!w->fixed) {
                            RECT r;
                            GetWindowRect(w->hwnd, &r);
                            g_dragging = w;
                            g_dragOffset = POINT{ m->pt.x - r.left, m->pt.y - r.top };
                        }
                        return 1;
                    }
                } else if (wp == WM_MOUSEMOVE && g_dragging) {
                    SetWindowPos(g_dragging->hwnd, nullptr, m->pt.x - g_dragOffset.x, m->pt.y - g_dragOffset.y, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                } else if (wp == WM_MBUTTONUP && (g_dragging || windowAt(m->pt))) {
                    if (g_dragging) reportPosition(*g_dragging);
                    g_dragging = nullptr;
                    return 1;
                }
            } else if (g_dragging) {
                g_dragging = nullptr;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    void openOverlay(const Axis::Settings& settings, bool obsOnly) {
        closeAll(false);
        g_settings = settings;
        g_obsOnly = obsOnly;
        g_visible = true;
        g_locked = settings.locked;
        const Axis::Profile& profile = g_settings.active();
        int ordinal = 0;
        for (const auto& group : Axis::windowGroups(profile)) {
            Window w;
            w.key = group.key;
            w.name = group.name;
            w.paneIndices = group.panes;
            for (int index : group.panes) {
                const Axis::Pane& pane = profile.panes[index];
                w.panes.push_back(std::make_unique<AxisDraw::PaneView>(pane, g_settings, Axis::describe(pane.deviceId).name));
                w.fixed = w.fixed || pane.locked;
            }
            layoutWindow(w);
            // La posizione del primo modulo del gruppo che ne ha una (se cade ancora su uno schermo).
            POINT pos = defaultPosition(ordinal++);
            for (int index : group.panes) {
                const Axis::Pane& pane = profile.panes[index];
                if (!pane.left || !pane.top) continue;
                const POINT saved{ (LONG)std::lround(*pane.left), (LONG)std::lround(*pane.top) };
                if (MonitorFromPoint(POINT{ saved.x + 20, saved.y + 20 }, MONITOR_DEFAULTTONULL)) pos = saved;
                break;
            }
            w.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, kWindowClass,
                                     L"Iris - Axis Visualizer", WS_POPUP, pos.x, pos.y, w.width, w.height, nullptr, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
            if (!w.hwnd) continue;
            g_windows.push_back(std::move(w));
        }
        if (g_windows.empty()) {
            publishStatus();
            return;
        }
        for (auto& w : g_windows) {
            if (!obsOnly) {
                ShowWindow(w.hwnd, SW_SHOWNOACTIVATE);
                render(w);
            }
        }
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHook, GetModuleHandleW(nullptr), 0);
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_lastFrame);
        SetTimer(g_control, kFrameTimer, 15, nullptr);
        SetTimer(g_control, kTopmostTimer, 3000, nullptr);
        publishLayout();
        Log::info("Axis: overlay aperto (" + std::to_string(g_windows.size()) + " finestre" + (obsOnly ? ", solo OBS" : "") + ")");
        publishStatus();
    }

    void frame() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = g_frequency.QuadPart ? (double)(now.QuadPart - g_lastFrame.QuadPart) / g_frequency.QuadPart : 1.0 / 60;
        g_lastFrame = now;
        if (dt <= 0 || dt > 0.25) dt = 1.0 / 60;
        bool relayout = false;
        for (auto& w : g_windows) {
            for (auto& p : w.panes) {
                const float before = p->width() * 10000 + p->height();
                p->tick(dt);
                if (p->width() * 10000 + p->height() != before) relayout = true;
            }
        }
        if (relayout) {
            // Una periferica collegata adesso: i moduli hanno cambiato misura.
            for (auto& w : g_windows) {
                layoutWindow(w);
                SetWindowPos(w.hwnd, nullptr, 0, 0, w.width, w.height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            publishLayout();
        }
        if (!g_obsOnly && g_visible) {
            for (auto& w : g_windows) render(w);
        }
        if (g_server && g_server->running() && g_server->hasViewers()) {
            std::map<std::string, std::string> values;
            std::vector<double> v;
            for (const auto& w : g_windows) {
                json all = json::array();
                for (const auto& p : w.panes) {
                    p->values(v);
                    all.push_back(v);
                }
                values[w.key] = all.dump();
            }
            g_server->publishValues(std::move(values));
        }
    }

    LRESULT CALLBACK controlProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_AXIS_OPEN: {
            std::unique_ptr<Axis::Settings> s(reinterpret_cast<Axis::Settings*>(lp));
            if (s) openOverlay(*s, wp != 0);
            return 0;
        }
        case WM_AXIS_CLOSE:
            if (!g_windows.empty()) {
                closeAll(true);
                Log::info("Axis: overlay chiuso");
            }
            return 0;
        case WM_AXIS_LOCK:
            g_locked = wp != 0;
            g_dragging = nullptr;
            publishStatus();
            return 0;
        case WM_AXIS_VISIBLE:
            g_visible = !g_visible;
            for (auto& w : g_windows) {
                if (!g_obsOnly) ShowWindow(w.hwnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            }
            publishStatus();
            return 0;
        case WM_TIMER:
            if (wp == kFrameTimer) frame();
            else if (wp == kTopmostTimer) {
                // Alcuni giochi si mettono "sempre in primo piano" anche loro: si torna sopra.
                for (auto& w : g_windows) {
                    if (IsWindowVisible(w.hwnd)) SetWindowPos(w.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void threadMain() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        WNDCLASSW cc{};
        cc.lpfnWndProc = controlProc;
        cc.hInstance = GetModuleHandleW(nullptr);
        cc.lpszClassName = L"IrisAxisControl";
        RegisterClassW(&cc);
        g_control = CreateWindowExW(0, cc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cc.hInstance, nullptr);
        g_threadId = GetCurrentThreadId();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        closeAll(false);
        if (g_control) DestroyWindow(g_control);
        g_control = nullptr;
    }
}

void start(AxisServer* server, Callbacks callbacks) {
    if (g_thread.joinable()) return;
    g_server = server;
    g_callbacks = std::move(callbacks);
    g_thread = std::thread(threadMain);
    for (int i = 0; i < 200 && !g_control; ++i) Sleep(5); // la finestra di controllo nasce subito
}

void stop() {
    if (!g_thread.joinable()) return;
    if (const DWORD id = g_threadId.load()) PostThreadMessageW(id, WM_QUIT, 0, 0);
    g_thread.join();
    g_callbacks = Callbacks{};
}

void open(const Axis::Settings& settings, bool obsOnly) {
    if (!g_control) return;
    auto* copy = new Axis::Settings(settings);
    if (!PostMessageW(g_control, WM_AXIS_OPEN, obsOnly ? 1 : 0, (LPARAM)copy)) delete copy;
}

void close() {
    if (g_control) PostMessageW(g_control, WM_AXIS_CLOSE, 0, 0);
}

void setLocked(bool locked) {
    if (g_control) PostMessageW(g_control, WM_AXIS_LOCK, locked ? 1 : 0, 0);
}

void toggleVisible() {
    if (g_control) PostMessageW(g_control, WM_AXIS_VISIBLE, 0, 0);
}

Status status() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

} // namespace AxisOverlay
