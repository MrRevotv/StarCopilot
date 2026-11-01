#include "main_window.h"
#include "axis_host.h"
#include "axis_overlay.h"
#include "hud_host.h"
#include "orb.h"
#include "ui.h"
#include "web_bridge.h"
#include "../app_paths.h"
#include "../assistant.h"
#include "../hotkey_listener.h"
#include "../log.h"
#include "../mining_overlay.h"
#include "../ocr_overlay.h"
#include "../overlay.h"
#include "../screen_capture.h"
#include "../text_utils.h"
#include "../updater.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlwapi.h>
#include <wrl.h>

#include <WebView2.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
    constexpr UINT WM_APP_ENGINE = WM_APP + 1;  // dall'assistente: wParam = Assistant::Event
    constexpr UINT WM_APP_UPDATE = WM_APP + 2;  // lo stato dell'aggiornamento è cambiato
    constexpr UINT WM_APP_TRAY = WM_APP + 60;   // l'icona nell'area di notifica
    constexpr UINT kTrayShow = 1, kTrayBar = 2, kTrayQuit = 3;
    constexpr UINT_PTR kMeterTimer = 1;
    constexpr int kDefaultWidth = 1220, kDefaultHeight = 820; // a 96 DPI
    constexpr int kMinWidth = 940, kMinHeight = 620;
    constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    constexpr const wchar_t* kHost = L"https://iris.example/";
    constexpr COLORREF kCaption = RGB(5, 9, 15);       // come la barra laterale della pagina
    constexpr COLORREF kCaptionText = RGB(150, 214, 230);
    constexpr COLORREF kBorder = RGB(18, 64, 82);

    struct State {
        Assistant* engine = nullptr;
        UpdateManager* updates = nullptr;
        HWND hwnd = nullptr;
        ComPtr<ICoreWebView2Environment> env;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        std::unique_ptr<WebBridge> bridge;
        AxisHost axis; // l'Axis Visualizer: impostazioni, server per OBS
        std::wstring webDir; // --interfaccia: le pagine dal disco
        bool devTools = false;
        bool failed = false;
        NOTIFYICONDATAW tray{};
        bool trayAdded = false;
        bool trayHintShown = false; // il fumetto "Iris resta qui" una volta sola
    };
    UINT g_msgTaskbarCreated = 0; // Explorer riavviato: l'icona va rimessa
    UINT g_msgShowMe = 0;         // una seconda copia di Iris chiede di mostrare la finestra
    State* g = nullptr;

    // --- I file della pagina (dentro l'exe, o dal disco con --interfaccia) -------------------

    struct WebFile {
        const wchar_t* path;     // dopo https://iris.example/
        const wchar_t* resource; // nome della risorsa in iris.rc
        const wchar_t* type;
    };
    constexpr WebFile kFiles[] = {
        { L"index.html", L"INDEX_HTML", L"text/html; charset=utf-8" },
        { L"app.css", L"APP_CSS", L"text/css; charset=utf-8" },
        { L"app.js", L"APP_JS", L"text/javascript; charset=utf-8" },
        { L"axis.js", L"AXIS_JS", L"text/javascript; charset=utf-8" },
        { L"log.js", L"LOG_JS", L"text/javascript; charset=utf-8" },
        { L"commands.js", L"COMMANDS_JS", L"text/javascript; charset=utf-8" },
    };

    bool loadWebFile(const WebFile& f, std::string& data) {
        if (!g->webDir.empty()) {
            std::ifstream in(g->webDir + L"\\" + f.path, std::ios::binary);
            if (!in) return false;
            std::stringstream ss;
            ss << in.rdbuf();
            data = ss.str();
            return true;
        }
        HRSRC res = FindResourceW(nullptr, f.resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL handle = res ? LoadResource(nullptr, res) : nullptr;
        const void* bytes = handle ? LockResource(handle) : nullptr;
        if (!bytes) return false;
        data.assign(static_cast<const char*>(bytes), SizeofResource(nullptr, res));
        return true;
    }

    std::string loadResourceText(const wchar_t* name) {
        HRSRC res = FindResourceW(nullptr, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL handle = res ? LoadResource(nullptr, res) : nullptr;
        const void* bytes = handle ? LockResource(handle) : nullptr;
        return bytes ? std::string(static_cast<const char*>(bytes), SizeofResource(nullptr, res)) : std::string();
    }

    // L'Axis Visualizer di Iris: impostazioni (la prima volta quelle di Axis Visualizer, se c'è),
    // server per OBS e il thread dell'overlay.
    void startAxis(HWND hwnd) {
        AxisHost& axis = g->axis;
        if (Axis::exists()) {
            axis.settings = Axis::load();
        } else {
            std::string message;
            Axis::importFromAxisVisualizer(axis.settings, message); // (lo scrive lei nel registro)
            axis.settings.sanitize();
            Axis::save(axis.settings);
        }
        axis.server.setPages(loadResourceText(L"AXIS_OVERLAY_HTML"), loadResourceText(L"AXIS_INDEX_HTML"));
        if (axis.settings.serverEnabled) {
            std::string error;
            if (!axis.server.start(axis.settings.serverPort, error)) Log::warn("Axis: server per OBS non avviato: " + error);
        }
        AxisOverlay::Callbacks callbacks;
        callbacks.changed = [hwnd]() { PostMessageW(hwnd, WM_APP_AXIS_CHANGED, 0, 0); };
        callbacks.moved = [hwnd](const std::vector<std::pair<int, POINT>>& moved) {
            auto* copy = new std::vector<std::pair<int, POINT>>(moved);
            if (!PostMessageW(hwnd, WM_APP_AXIS_MOVED, 0, (LPARAM)copy)) delete copy;
        };
        AxisOverlay::start(&axis.server, callbacks);
    }

    HRESULT serveResource(ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) {
        if (!g || !g->env) return S_OK;
        ComPtr<ICoreWebView2WebResourceRequest> request;
        args->get_Request(&request);
        LPWSTR uriRaw = nullptr;
        request->get_Uri(&uriRaw);
        std::wstring uri = uriRaw ? uriRaw : L"";
        CoTaskMemFree(uriRaw);
        std::wstring path = uri.size() > wcslen(kHost) ? uri.substr(wcslen(kHost)) : L"";
        path = path.substr(0, path.find_first_of(L"?#"));
        if (path.empty()) path = L"index.html";
        std::string data;
        const WebFile* found = nullptr;
        for (const auto& f : kFiles) {
            if (_wcsicmp(f.path, path.c_str()) == 0 && loadWebFile(f, data)) found = &f;
        }
        ComPtr<ICoreWebView2WebResourceResponse> response;
        if (!found) {
            g->env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
        } else {
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), (UINT)data.size()));
            const std::wstring headers = std::wstring(L"Content-Type: ") + found->type + L"\r\nCache-Control: no-store";
            g->env->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
        }
        args->put_Response(response.Get());
        return S_OK;
    }

    // --- Riquadri sopra il gioco ----------------------------------------------------------------

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
        Overlay::setVisible(s.overlayEnabled && !HudHost::hiddenAll());
        OcrOverlay::setOpacity(s.ocrOverlayOpacity);
        OcrOverlay::setPosition(s.ocrOverlayX, s.ocrOverlayY);
        // Mining: riquadro e contorno della zona letta (fuori dalla zona, mai nella cattura).
        MiningOverlay::setOpacity(s.miningOverlayOpacity, s.miningOverlayTextOpacity);
        MiningOverlay::setPosition(s.miningOverlayX, s.miningOverlayY);
        const RECT region{ s.miningRegionX, s.miningRegionY, s.miningRegionX + s.miningRegionW, s.miningRegionY + s.miningRegionH };
        MiningOverlay::setBorder(region, s.miningEnabled && s.miningShowBorder && s.miningRegionW > 0 && s.miningRegionH > 0 && !HudHost::hiddenAll());
        if (!s.miningEnabled) MiningOverlay::hide();
    }

    std::wstring keyLabel(const std::vector<TalkTrigger>& triggers) {
        if (triggers.empty()) return L"";
        return TextUtils::utf8ToWide(HotkeyListener::describeTrigger(triggers.front()));
    }

    // Il risultato del mining (o l'inserimento manuale) nel riquadro sopra il gioco.
    void showMining() {
        const AppSettings s = g->engine->settings();
        if (!s.miningEnabled) {
            MiningOverlay::hide();
            return;
        }
        const RECT region{ s.miningRegionX, s.miningRegionY, s.miningRegionX + s.miningRegionW, s.miningRegionY + s.miningRegionH };
        const bool hasRegion = s.miningRegionW > 0 && s.miningRegionH > 0;
        using MiningOverlay::Line;
        using MiningOverlay::Size;
        std::vector<Line> lines;

        const MiningScanner::ManualState manual = g->engine->mining().manualState();
        if (manual.active) {
            lines.push_back({ L"MINING · INSERIMENTO MANUALE", Ui::Color::TextDim, Size::Small });
            lines.push_back({ (manual.buffer.empty() ? L"" : TextUtils::utf8ToWide(Mining::formatValue(std::atoi(manual.buffer.c_str())))) + L"_",
                              Ui::Color::Busy, Size::Big });
            lines.push_back({ L"Cifre dal tastierino · Invio conferma · Esc annulla", Ui::Color::TextDim, Size::Small });
            MiningOverlay::show(region, hasRegion, lines, 0);
            return;
        }

        const MiningScanner::Reading r = g->engine->mining().lastReading();
        // Niente lettura recente (es. inserimento manuale annullato): il riquadro si toglie.
        if (!r.valid || GetTickCount64() - r.tick > (ULONGLONG)s.miningOverlaySeconds * 1000ULL) {
            MiningOverlay::hide();
            return;
        }
        const Mining::Decoded& d = r.decoded;
        std::wstring header = L"MINING";
        if (d.hasValue) header += L" · " + TextUtils::utf8ToWide(Mining::formatValue(d.value));
        lines.push_back({ header, Ui::Color::TextDim, Size::Small });
        if (!d.hasValue) {
            lines.push_back({ L"Signature non letta", Ui::Color::Error, Size::Big });
            std::wstring hint = r.error.empty() ? L"" : TextUtils::utf8ToWide(r.error);
            const std::wstring manualKey = keyLabel(s.miningManualTriggers);
            if (!manualKey.empty()) hint += (hint.empty() ? L"" : L" · ") + manualKey + L" per scriverla a mano";
            if (!hint.empty()) lines.push_back({ hint, Ui::Color::TextDim, Size::Small });
        } else if (d.matches.empty()) {
            lines.push_back({ L"Sconosciuta", Ui::Color::Error, Size::Big });
            lines.push_back({ d.note.empty() ? L"Non torna con nessun materiale: identificala nella pagina Mining" : TextUtils::utf8ToWide(d.note),
                              Ui::Color::TextDim, Size::Small });
        } else {
            for (const auto& m : d.matches) {
                const COLORREF color = m.kind == "salvage" ? Ui::Color::Busy : m.kind == "minerale" ? Ui::Color::Ok : Ui::Color::Accent;
                lines.push_back({ TextUtils::utf8ToWide(Mining::describeMatch(m)), color, Size::Big });
            }
            // Rarità e minerali secondari (dalla tabella di Citizen Assistant), se la roccia è una sola.
            if (d.matches.size() == 1) {
                if (const Mining::OreInfo* info = Mining::oreInfo(d.matches.front().name)) {
                    std::string extra = Mining::rarityName(info->rarity);
                    if (*info->secondary) extra += std::string("  ·  con ") + info->secondary;
                    lines.push_back({ TextUtils::utf8ToWide(extra), Ui::Color::TextDim, Size::Small });
                } else if (d.matches.front().kind == "roc" || d.matches.front().kind == "fps") {
                    lines.push_back({ L"Il minerale dipende dal posto: la signature dice solo quanti depositi", Ui::Color::TextDim, Size::Small });
                }
            }
            std::wstring detail;
            for (const auto& m : d.matches) {
                detail += (detail.empty() ? L"" : L" · ") + std::wstring(L"base ") + TextUtils::utf8ToWide(Mining::formatValue(m.base)) +
                          (m.kind == "salvage" ? L" (salvage)" : L"");
            }
            if (d.matches.size() > 1) detail = L"Più possibilità: " + detail;
            lines.push_back({ detail, Ui::Color::TextDim, Size::Small });
            if (d.corrected) lines.push_back({ L"Probabile: " + TextUtils::utf8ToWide(d.note), Ui::Color::Busy, Size::Small });
        }
        // Informazioni base: solo le righe grandi (materiale × quantità, o "Sconosciuta").
        if (s.miningOverlayBasic) {
            lines.erase(std::remove_if(lines.begin(), lines.end(), [](const Line& l) { return l.size != Size::Big; }), lines.end());
        }
        MiningOverlay::show(region, hasRegion, lines, s.miningOverlaySeconds);
    }

    void showChatTranslation() {
        // La chat degli altri, tradotta, nel riquadro sotto la chat del gioco.
        GameChat::ScreenView view = g->engine->chat().screenView();
        if (view.visible && !view.lines.empty()) {
            RECT region{ view.regionX, view.regionY, view.regionX + view.regionW, view.regionY + view.regionH };
            std::vector<OcrOverlay::Line> lines;
            for (const auto& l : view.lines) lines.push_back({ TextUtils::utf8ToWide(l.prefix), TextUtils::utf8ToWide(l.text), l.pending });
            OcrOverlay::show(region, lines);
        } else {
            OcrOverlay::hide();
        }
    }

    // --- Area di notifica ------------------------------------------------------------------------------

    void showMain() {
        if (!g) return;
        if (!IsWindowVisible(g->hwnd)) ShowWindow(g->hwnd, SW_SHOW);
        if (IsIconic(g->hwnd)) ShowWindow(g->hwnd, SW_RESTORE);
        SetForegroundWindow(g->hwnd);
    }

    void addTrayIcon() {
        if (!g) return;
        NOTIFYICONDATAW& n = g->tray;
        n = NOTIFYICONDATAW{};
        n.cbSize = sizeof(n);
        n.hWnd = g->hwnd;
        n.uID = 1;
        n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        n.uCallbackMessage = WM_APP_TRAY;
        n.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        wcscpy_s(n.szTip, L"Iris \u2014 copilota di bordo");
        g->trayAdded = Shell_NotifyIconW(NIM_ADD, &n) != FALSE;
        n.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &n);
    }

    void removeTrayIcon() {
        if (!g || !g->trayAdded) return;
        Shell_NotifyIconW(NIM_DELETE, &g->tray);
        g->trayAdded = false;
    }

    void trayBalloon(const wchar_t* title, const wchar_t* text) {
        if (!g || !g->trayAdded) return;
        NOTIFYICONDATAW n = g->tray;
        n.uFlags = NIF_INFO;
        n.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
        wcscpy_s(n.szInfoTitle, title);
        wcscpy_s(n.szInfo, text);
        Shell_NotifyIconW(NIM_MODIFY, &n);
    }

    void trayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kTrayShow, L"Apri Iris");
        AppendMenuW(menu, MF_STRING, kTrayBar, L"Barra rapida");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayQuit, L"Esci da Iris");
        SetMenuDefaultItem(menu, kTrayShow, FALSE);
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(g->hwnd); // se no il menu non si chiude cliccando fuori
        const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g->hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == kTrayShow) showMain();
        else if (cmd == kTrayBar) HudHost::toggleBar();
        else if (cmd == kTrayQuit) PostMessageW(g->hwnd, WM_CLOSE, 0, 0);
    }

    void onEngineEvent(Assistant::Event e) {
        HudHost::onEngineEvent(e);
        switch (e) {
        case Assistant::Event::Screen: showChatTranslation(); break;
        case Assistant::Event::Settings: applyOverlaySettings(); break;
        case Assistant::Event::Mining: showMining(); break;
        default: break;
        }
        if (g->bridge) g->bridge->onEngineEvent((int)e);
    }

    // --- WebView2 ---------------------------------------------------------------------------------

    void resizeWebView() {
        if (!g || !g->controller) return;
        RECT rc;
        GetClientRect(g->hwnd, &rc);
        g->controller->put_Bounds(rc);
    }

    void webViewFailed(HRESULT hr) {
        if (!g || g->failed) return;
        g->failed = true;
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX", (unsigned long)hr);
        Log::error(std::string("WebView2 non disponibile: ") + code);
        const int answer = MessageBoxW(g->hwnd,
                                       L"Per mostrare la sua interfaccia Iris usa Microsoft Edge WebView2, che c'è già in Windows 11 e "
                                       L"in quasi tutti i Windows 10, ma su questo PC non si avvia.\n\n"
                                       L"Vuoi scaricarlo adesso da Microsoft? (Poi riapri Iris.)",
                                       L"Iris", MB_YESNO | MB_ICONWARNING);
        if (answer == IDYES) {
            ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr, SW_SHOWNORMAL);
        }
        DestroyWindow(g->hwnd);
    }

    bool isOurPage(const std::wstring& uri) { return uri.rfind(kHost, 0) == 0; }

    void openExternally(const std::wstring& uri) {
        if (uri.rfind(L"https://", 0) == 0 || uri.rfind(L"http://", 0) == 0) {
            ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void setupWebView(ICoreWebView2Controller* controller) {
        g->controller = controller;
        controller->get_CoreWebView2(&g->webview);
        if (!g->webview) return webViewFailed(E_FAIL);

        // Nessun lampo bianco mentre la pagina si carica: lo stesso fondo scuro.
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
            COREWEBVIEW2_COLOR background{ 255, 6, 10, 17 };
            controller2->put_DefaultBackgroundColor(background);
        }

        ComPtr<ICoreWebView2Settings> settings;
        g->webview->get_Settings(&settings);
        if (settings) {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDevToolsEnabled(g->devTools ? TRUE : FALSE);
            settings->put_IsBuiltInErrorPageEnabled(FALSE);
            // F5, Ctrl+P, Ctrl+F... sono cose da browser, non da programma (con --interfaccia o --sviluppo restano).
            ComPtr<ICoreWebView2Settings3> settings3;
            if (SUCCEEDED(settings.As(&settings3))) settings3->put_AreBrowserAcceleratorKeysEnabled(g->devTools || !g->webDir.empty() ? TRUE : FALSE);
            ComPtr<ICoreWebView2Settings5> settings5;
            if (SUCCEEDED(settings.As(&settings5))) settings5->put_IsPinchZoomEnabled(FALSE);
            ComPtr<ICoreWebView2Settings4> settings4;
            if (SUCCEEDED(settings.As(&settings4))) {
                settings4->put_IsPasswordAutosaveEnabled(FALSE);
                settings4->put_IsGeneralAutofillEnabled(FALSE);
            }
        }

        // Le pagine arrivano dall'exe (o dal disco con --interfaccia).
        g->webview->AddWebResourceRequestedFilter((std::wstring(kHost) + L"*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        EventRegistrationToken token;
        g->webview->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(serveResource).Get(), &token);

        // I messaggi della pagina vanno al ponte.
        g->webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                               [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                   LPWSTR text = nullptr;
                                                   if (SUCCEEDED(args->TryGetWebMessageAsString(&text)) && text && g && g->bridge) {
                                                       const std::string message = TextUtils::wideToUtf8(text);
                                                       g->bridge->onMessage(message);
                                                   }
                                                   CoTaskMemFree(text);
                                                   return S_OK;
                                               })
                                               .Get(),
                                           &token);

        // I collegamenti esterni (pagina della release...) si aprono nel browser, mai dentro Iris.
        g->webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                               [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                                   LPWSTR uri = nullptr;
                                                   args->get_Uri(&uri);
                                                   const std::wstring target = uri ? uri : L"";
                                                   CoTaskMemFree(uri);
                                                   if (!isOurPage(target)) {
                                                       args->put_Cancel(TRUE);
                                                       openExternally(target);
                                                   }
                                                   return S_OK;
                                               })
                                               .Get(),
                                           &token);
        g->webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                               [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                                   LPWSTR uri = nullptr;
                                                   args->get_Uri(&uri);
                                                   openExternally(uri ? uri : L"");
                                                   CoTaskMemFree(uri);
                                                   args->put_Handled(TRUE);
                                                   return S_OK;
                                               })
                                               .Get(),
                                           &token);
        // Il menu del tasto destro solo dove si scrive (copia, incolla...).
        ComPtr<ICoreWebView2_11> webview11;
        if (!g->devTools && SUCCEEDED(g->webview.As(&webview11))) {
            webview11->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                                                    [](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                                                        ComPtr<ICoreWebView2ContextMenuTarget> target;
                                                        BOOL editable = FALSE;
                                                        if (SUCCEEDED(args->get_ContextMenuTarget(&target)) && target) target->get_IsEditable(&editable);
                                                        if (!editable) args->put_Handled(TRUE);
                                                        return S_OK;
                                                    })
                                                    .Get(),
                                                &token);
        }
        g->webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                          [](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                              COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                                              args->get_ProcessFailedKind(&kind);
                                              Log::warn("WebView2: processo terminato (tipo " + std::to_string((int)kind) + "), ricarico la pagina");
                                              if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                                                  kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                                                  sender->Reload();
                                              }
                                              return S_OK;
                                          })
                                          .Get(),
                                      &token);

        g->bridge = std::make_unique<WebBridge>(*g->engine, *g->updates, g->axis, g->hwnd, [](const std::string& json) {
            if (g && g->webview) g->webview->PostWebMessageAsJson(TextUtils::utf8ToWide(json).c_str());
        });

        resizeWebView();
        controller->put_IsVisible(TRUE);
        g->webview->Navigate((std::wstring(kHost) + L"index.html").c_str());
        if (GetForegroundWindow() == g->hwnd) controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    void createWebView() {
        // I dati di WebView2 (cache, impostazioni) con quelli di Iris, non accanto all'exe.
        const std::wstring userData = AppPaths::getAppDataDir() + L"\\WebView2";
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, userData.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (!g) return S_OK;
                if (FAILED(result) || !env) {
                    webViewFailed(result);
                    return S_OK;
                }
                g->env = env;
                const HRESULT created = env->CreateCoreWebView2Controller(
                    g->hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                     if (!g) return S_OK;
                                     if (FAILED(result) || !controller) webViewFailed(result);
                                     else setupWebView(controller);
                                     return S_OK;
                                 })
                                 .Get());
                if (FAILED(created)) webViewFailed(created);
                return S_OK;
            }).Get());
        if (FAILED(hr)) webViewFailed(hr);
    }

    // --- La finestra -------------------------------------------------------------------------------

    void styleTitleBar(HWND hwnd) {
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)))) DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
        // Windows 11: barra del titolo e bordo nei colori dell'HUD.
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &kCaption, sizeof(kCaption));
        DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &kCaptionText, sizeof(kCaptionText));
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &kBorder, sizeof(kBorder));
    }

    void saveGeometry(HWND hwnd) {
        WINDOWPLACEMENT wp{ sizeof(wp) };
        if (!GetWindowPlacement(hwnd, &wp)) return;
        const RECT r = wp.rcNormalPosition; // anche se la finestra è ridotta o ingrandita
        const UINT dpi = Ui::dpiOf(hwnd);
        g->engine->updateSettings([&](AppSettings& s) {
            s.windowX = r.left;
            s.windowY = r.top;
            s.windowW = MulDiv(r.right - r.left, 96, (int)dpi);
            s.windowH = MulDiv(r.bottom - r.top, 96, (int)dpi);
        });
    }

    LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT dpi = Ui::dpiOf(hwnd);
            mmi->ptMinTrackSize.x = Ui::px(kMinWidth, dpi);
            mmi->ptMinTrackSize.y = Ui::px(kMinHeight, dpi);
            return 0;
        }
        case WM_SIZE:
            resizeWebView();
            if (g && g->controller) g->controller->put_IsVisible(wp != SIZE_MINIMIZED);
            return 0;
        case WM_MOVE:
        case WM_MOVING:
            if (g && g->controller) g->controller->NotifyParentWindowPositionChanged();
            break;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_ACTIVATE:
            if (g && g->controller && (msg == WM_SETFOCUS || LOWORD(wp) != WA_INACTIVE)) {
                g->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            break;
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, Ui::brush(RGB(6, 10, 17)));
            return 1;
        }
        case WM_TIMER:
            if (g && wp == kMeterTimer) {
                // La sfera fluttuante si muove sempre (anche con la finestra ridotta: è lì per il gioco).
                Overlay::animate(orbMode(g->engine->status().state), g->engine->micLevel(), g->engine->voiceLevel(), g->engine->userSpeaking());
                HudHost::onTimer();
                if (g->bridge) g->bridge->onTimer();
            }
            return 0;
        case WM_APP_ENGINE:
            if (g) onEngineEvent((Assistant::Event)wp);
            return 0;
        case WM_APP_UPDATE:
            if (g && g->bridge) g->bridge->onUpdateChanged();
            return 0;
        case WM_APP_AXIS_CHANGED:
            if (g && g->bridge) g->bridge->onAxisChanged();
            HudHost::refresh();
            return 0;
        case WM_APP_TRAY:
            if (g) {
                const UINT event = LOWORD(lp);
                if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT) showMain();
                else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) trayMenu();
            }
            return 0;
        case WM_SYSCOMMAND:
            // La X (e Alt+F4): con l'opzione attiva Iris resta nell'area di notifica. WM_CLOSE mandato dal
            // programma (aggiornamento, "Esci" dal menu) chiude davvero.
            if ((wp & 0xFFF0) == SC_CLOSE && g && g->trayAdded && g->engine->settings().closeToTray) {
                saveGeometry(hwnd);
                ShowWindow(hwnd, SW_HIDE);
                if (!g->trayHintShown) {
                    g->trayHintShown = true;
                    trayBalloon(L"Iris resta attiva", L"È qui nell'area di notifica: clic per riaprirla, tasto destro \u203A Esci per chiuderla.");
                }
                return 0;
            }
            break;
        case WM_APP_AXIS_MOVED:
            if (g && g->bridge) {
                g->bridge->onAxisMoved(reinterpret_cast<void*>(lp)); // lo libera lui
            } else if (g) {
                // Pagina non ancora pronta: la posizione si salva lo stesso.
                std::unique_ptr<std::vector<std::pair<int, POINT>>> moved(reinterpret_cast<std::vector<std::pair<int, POINT>>*>(lp));
                auto& panes = g->axis.settings.active().panes;
                for (const auto& [index, pt] : *moved) {
                    if (index < 0 || index >= (int)panes.size()) continue;
                    panes[index].left = (double)pt.x;
                    panes[index].top = (double)pt.y;
                }
                Axis::save(g->axis.settings);
            } else {
                delete reinterpret_cast<std::vector<std::pair<int, POINT>>*>(lp);
            }
            return 0;
        case WM_CLOSE:
            if (g) saveGeometry(hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kMeterTimer);
            HudHost::stop();
            removeTrayIcon();
            AxisOverlay::stop();
            if (g) {
                g->axis.server.stop();
                g->engine->setListener(nullptr);
                g->bridge.reset();
                if (g->controller) g->controller->Close();
                g->webview.Reset();
                g->controller.Reset();
                g->env.Reset();
            }
            Overlay::setOnMoved(nullptr);
            OcrOverlay::setOnMoved(nullptr);
            MiningOverlay::setOnMoved(nullptr);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            delete g;
            g = nullptr;
            break;
        }
        if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
            if (g) addTrayIcon();
            return 0;
        }
        if (g_msgShowMe && msg == g_msgShowMe) {
            showMain();
            return 0;
        }
        if (msg >= WM_APP_BRIDGE_FIRST && msg <= WM_APP_BRIDGE_LAST) {
            if (g && g->bridge) {
                g->bridge->onAppMessage(msg, wp, lp);
            } else if (msg == WM_APP_BRIDGE_SEND) {
                delete reinterpret_cast<std::string*>(lp);
            } else if (msg == WM_APP_BRIDGE_CAPTURED) {
                delete reinterpret_cast<TalkTrigger*>(lp);
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void readArguments(State& s) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv) return;
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--interfaccia") == 0 && i + 1 < argc) s.webDir = argv[++i];
            else if (lstrcmpiW(argv[i], L"--sviluppo") == 0) s.devTools = true;
        }
        LocalFree(argv);
        if (!s.webDir.empty()) s.devTools = true;
    }
}

namespace MainWindow {

HWND create(HINSTANCE instance, Assistant& engine, UpdateManager& updates, int showCommand) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExW(&wc);

    g = new State();
    g->engine = &engine;
    g->updates = &updates;
    readArguments(*g);

    // Posizione e dimensione dell'ultima volta (se il monitor c'è ancora).
    const AppSettings s = engine.settings();
    POINT where{ s.windowX, s.windowY };
    HMONITOR monitor = MonitorFromPoint(where, MONITOR_DEFAULTTONULL);
    const bool remembered = s.windowX != -1 && monitor;
    if (!monitor) monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    UINT dpiX = 96, dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) dpiX = 96;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const RECT work = mi.rcWork;
    int width = Ui::px(s.windowW > 0 ? std::max(s.windowW, kMinWidth) : kDefaultWidth, dpiX);
    int height = Ui::px(s.windowH > 0 ? std::max(s.windowH, kMinHeight) : kDefaultHeight, dpiX);
    width = std::min(width, (int)(work.right - work.left));
    height = std::min(height, (int)(work.bottom - work.top));
    int x = remembered ? s.windowX : work.left + (work.right - work.left - width) / 2;
    int y = remembered ? s.windowY : work.top + (work.bottom - work.top - height) / 2;
    x = std::clamp(x, (int)work.left, std::max((int)work.left, (int)work.right - width));
    y = std::clamp(y, (int)work.top, std::max((int)work.top, (int)work.bottom - height));

    HWND hwnd = CreateWindowExW(0, kClassName, L"Iris", kStyle, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        delete g;
        g = nullptr;
        return nullptr;
    }
    g->hwnd = hwnd;
    styleTitleBar(hwnd);

    engine.setListener([hwnd](Assistant::Event e) { PostMessageW(hwnd, WM_APP_ENGINE, (WPARAM)e, 0); });
    updates.start([&engine]() { return engine.settings().autoUpdate; }, [hwnd]() { PostMessageW(hwnd, WM_APP_UPDATE, 0, 0); });
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
    MiningOverlay::setOnMoved([&engine](int ox, int oy) {
        engine.updateSettings([&](AppSettings& st) {
            st.miningOverlayX = ox;
            st.miningOverlayY = oy;
        });
    });
    applyOverlaySettings();
    startAxis(hwnd);

    // Area di notifica, barra rapida sopra il gioco.
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_msgShowMe = RegisterWindowMessageW(kShowMessage);
    ChangeWindowMessageFilterEx(hwnd, g_msgShowMe, MSGFLT_ALLOW, nullptr);
    addTrayIcon();
    HudHost::Context hud;
    hud.window = hwnd;
    hud.engine = &engine;
    hud.axis = &g->axis;
    hud.showMain = []() { showMain(); };
    hud.applyOverlays = []() { applyOverlaySettings(); };
    hud.askLogClose = []() {
        showMain();
        if (g && g->bridge) g->bridge->openMiningLogDialog();
    };
    HudHost::start(instance, std::move(hud));

    SetTimer(hwnd, kMeterTimer, 33, nullptr); // ~30 al secondo: la sfera si muove fluida
    // Avviata con Windows: niente finestra, solo l'icona nell'area di notifica.
    if (showCommand != SW_HIDE) ShowWindow(hwnd, showCommand == SW_SHOWMINIMIZED || showCommand == SW_MINIMIZE ? showCommand : SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    createWebView();
    return hwnd;
}

} // namespace MainWindow
