#pragma once

#include "../axis_config.h"

#include <windows.h>

#include <functional>
#include <utility>
#include <vector>

class AxisServer;

// L'overlay dell'Axis Visualizer dentro Iris: una finestra per gruppo di moduli,
// sempre in primo piano, che lascia passare i clic (sinistro e destro vanno al
// gioco). Come in Axis Visualizer:
//  - trascinamento col tasto CENTRALE (se non è bloccato), doppio click centrale = chiudi;
//  - "solo OBS": le finestre non compaiono, ma i numeri vanno lo stesso al server.
// Tutto gira in un thread suo (60 fotogrammi al secondo, l'aggancio del mouse),
// così la finestra di Iris non rallenta mai l'overlay e viceversa.
namespace AxisOverlay {

    struct Callbacks {
        // Una finestra spostata: (indice del modulo nel profilo, posizione in pixel). Dal thread dell'overlay.
        std::function<void(const std::vector<std::pair<int, POINT>>&)> moved;
        // Aperto, chiuso, mostrato, bloccato. Dal thread dell'overlay.
        std::function<void()> changed;
    };

    void start(AxisServer* server, Callbacks callbacks);
    void stop();

    void open(const Axis::Settings& settings, bool obsOnly);
    void close();
    void setLocked(bool locked);
    void toggleVisible();

    struct Status {
        bool open = false, obsOnly = false, visible = false, locked = false;
        int windows = 0;
    };
    Status status();

} // namespace AxisOverlay
