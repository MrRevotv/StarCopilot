#pragma once

#include <windows.h>

class Assistant;
class UpdateManager;

// La finestra principale: dentro c'è l'interfaccia (HTML/CSS/JS in WebView2,
// stile HUD), servita dall'exe stesso (https://iris.example/...). Riceve gli
// avvisi del motore e li gira alla pagina (WebBridge); tiene aggiornati i
// riquadri sopra il gioco (sfera, chat tradotta, mining).
//
// Per lavorare sull'interfaccia senza ricompilare:
//   iris.exe --interfaccia "C:\...\src\ui\web"   pagine lette dal disco (F5 ricarica)
//   iris.exe --sviluppo                           strumenti per sviluppatori (F12)
namespace MainWindow {

    inline constexpr const wchar_t* kClassName = L"IrisWindow";
    // Messaggio registrato: una seconda copia di Iris chiede alla prima di mostrare la finestra.
    inline constexpr const wchar_t* kShowMessage = L"Iris.MostraFinestra";

    // Crea e mostra la finestra (SW_HIDE = avviata con Windows: solo l'icona nell'area di notifica).
    // L'assistente e gli aggiornamenti devono vivere più a lungo della finestra.
    HWND create(HINSTANCE instance, Assistant& engine, UpdateManager& updates, int showCommand);

} // namespace MainWindow
