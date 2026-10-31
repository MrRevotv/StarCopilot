#pragma once

#include <windows.h>

class Assistant;

// La finestra principale: il menu delle pagine a sinistra, la pagina aperta a
// destra (che scorre se lo schermo è basso), lo stato in basso a sinistra.
// Riceve gli avvisi del motore e aggiorna pagine e overlay.
namespace MainWindow {

    inline constexpr const wchar_t* kClassName = L"IrisWindow";

    // Crea e mostra la finestra. L'assistente deve vivere più a lungo della finestra.
    HWND create(HINSTANCE instance, Assistant& engine, int showCommand);

} // namespace MainWindow
