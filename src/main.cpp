#include "app_paths.h"
#include "assistant.h"
#include "joystick_input.h"
#include "log.h"
#include "ocr_overlay.h"
#include "overlay.h"
#include "ui/main_window.h"
#include "ui/ui.h"

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
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --cartella-dati "C:\..." = impostazioni, memoria e registro altrove (per le prove).
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--cartella-dati") == 0) AppPaths::setDataDirOverride(argv[i + 1]);
        }
        LocalFree(argv);
    }

    // Una copia sola: due Iris ascolterebbero e risponderebbero due volte.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Iris.UnaCopiaSola");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(MainWindow::kClassName, nullptr);
        if (other) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        CloseHandle(mutex);
        return 0;
    }

    SetUnhandledExceptionFilter(logCrash);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    Ui::initialize();
    AppPaths::ensureDirectoriesExist();
    JoystickInput::initialize();

    int exitCode = 0;
    {
        Assistant assistant;
        Overlay::create(instance);
        OcrOverlay::create(instance);
        HWND window = MainWindow::create(instance, assistant, showCommand);
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
        assistant.shutdown();
        OcrOverlay::destroy();
        Overlay::destroy();
    }
    JoystickInput::shutdown();
    Ui::shutdown();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return exitCode;
}
