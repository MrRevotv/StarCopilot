#pragma once

#include "../assistant.h"

#include <windows.h>

#include <functional>

struct AxisHost;

// La barra rapida e i suoi pannelli collegati a Iris: cosa mostrano (stato,
// mining, traduzione, assi, sfera, missioni dal diario di bordo, raffineria),
// cosa fanno i pulsanti, il tasto "nascondi tutto". Thread della finestra.
namespace HudHost {

    struct Context {
        HWND window = nullptr;
        Assistant* engine = nullptr;
        AxisHost* axis = nullptr;
        std::function<void()> showMain;       // apri la finestra di Iris
        std::function<void()> applyOverlays;  // rimetti sfera e riquadri come dicono le impostazioni
        std::function<void()> askLogClose;    // chiudi il registro del mining: Iris chiede luogo e qualità
    };

    void start(HINSTANCE instance, Context context);
    void stop();

    void onEngineEvent(Assistant::Event e);
    void onTimer();          // dal timer della finestra (~30 al secondo)
    void refresh();          // ridisegna barra e pannelli (es. overlay degli assi aperto/chiuso)
    void toggleBar();
    void toggleHideAll();
    bool hiddenAll();        // "nascondi tutto" attivo: niente sfera né riquadri finché non lo ripremi

} // namespace HudHost
