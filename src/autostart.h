#pragma once

#include <string>

// Avvio di Iris con Windows: valore "Iris" nella chiave dell'utente
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run (niente permessi da
// amministratore). Avviata così, Iris parte nascosta nell'area di notifica.
namespace Autostart {

    inline constexpr const wchar_t* kArgument = L"--avvio-windows";

    // Il comando da registrare per questo exe: "C:\...\iris.exe" --avvio-windows
    std::wstring commandFor(const std::wstring& exePath);
    // C'è il valore? (command = quello scritto nel registro)
    bool enabled(std::wstring* command = nullptr);
    // Scritto per QUESTO exe? (se l'exe è stato spostato, il valore punta altrove)
    bool pointsHere();
    bool set(bool on, std::string* error = nullptr);

} // namespace Autostart
