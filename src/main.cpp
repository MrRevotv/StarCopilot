#include "app_paths.h"
#include "autostart.h"
#include "assistant.h"
#include "joystick_input.h"
#include "log.h"
#include "mining_overlay.h"
#include "ocr_overlay.h"
#include "overlay.h"
#include "ui/main_window.h"
#include "ui/ui.h"
#include "updater.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <cstdio>

namespace {
    // Se il programma si chiude per un errore grave, nel registro resta almeno cosa e dove.
    LONG WINAPI logCrash(EXCEPTION_POINTERS* info) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Chiusura imprevista: eccezione 0x%08lX all'indirizzo %p",
                 (unsigned long)info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
        Log::error(buf);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    constexpr const wchar_t* kMutexName = L"Local\\Iris.UnaCopiaSola";
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --cartella-dati "C:\..." = impostazioni, memoria e registro altrove (per le prove).
    // --dopo-aggiornamento = avviata dalla versione di prima, che si sta chiudendo.
    bool afterUpdate = false;
    bool fromWindows = false; // --avvio-windows: avviata con Windows, parte nascosta nell'area di notifica
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--cartella-dati") == 0 && i + 1 < argc) AppPaths::setDataDirOverride(argv[i + 1]);
            if (lstrcmpiW(argv[i], L"--dopo-aggiornamento") == 0) afterUpdate = true;
            if (lstrcmpiW(argv[i], Autostart::kArgument) == 0) fromWindows = true;
        }
        LocalFree(argv);
    }

    // Una copia sola: due Iris ascolterebbero e risponderebbero due volte.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        bool free = false;
        if (afterUpdate) {
            // La versione di prima si sta chiudendo: si aspetta che liberi il posto (al massimo un minuto).
            for (int i = 0; i < 240 && !free; ++i) {
                CloseHandle(mutex);
                Sleep(250);
                mutex = CreateMutexW(nullptr, FALSE, kMutexName);
                free = mutex && GetLastError() != ERROR_ALREADY_EXISTS;
            }
        }
        if (!free) {
            // Iris è già aperta (magari nascosta nell'area di notifica): le si chiede di mostrarsi.
            HWND other = FindWindowW(MainWindow::kClassName, nullptr);
            if (other && !fromWindows) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(other, RegisterWindowMessageW(MainWindow::kShowMessage), 0, 0);
            }
            if (mutex) CloseHandle(mutex);
            return 0;
        }
    }

    AppPaths::ensureDirectoriesExist();
    Log::info("Iris " + Updater::currentVersion() + (afterUpdate ? " (appena aggiornata)" : ""));
    Updater::cleanup();
    // Una versione nuova già scaricata e controllata: si installa subito, prima di
    // aprire la finestra e caricare i modelli (la nuova riparte da sola).
    if (!afterUpdate) {
        std::string version;
        const std::wstring pending = Updater::pendingUpdate(&version);
        if (!pending.empty()) {
            std::string error;
            if (Updater::install(pending, error)) {
                if (mutex) CloseHandle(mutex);
                return 0;
            }
            Log::warn("Aggiornamento " + version + " non installato all'avvio: " + error);
        }
    }

    SetUnhandledExceptionFilter(logCrash);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    Ui::initialize();
    JoystickInput::initialize();

    int exitCode = 0;
    {
        Assistant assistant;
        UpdateManager updates;
        Overlay::create(instance);
        OcrOverlay::create(instance);
        MiningOverlay::create(instance);
        HWND window = MainWindow::create(instance, assistant, updates, fromWindows ? SW_HIDE : showCommand);
        if (!window) {
            Log::error("Impossibile creare la finestra principale");
            MessageBoxW(nullptr, L"Impossibile creare la finestra del programma.", L"Iris", MB_OK | MB_ICONERROR);
            exitCode = 1;
        } else {
            assistant.start();
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = (int)msg.wParam;
        }
        updates.stop();
        assistant.shutdown();
        MiningOverlay::destroy();
        OcrOverlay::destroy();
        Overlay::destroy();
    }
    JoystickInput::shutdown();
    Ui::shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return exitCode;
}
