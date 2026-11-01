#pragma once

#include "../axis_config.h"
#include "../axis_server.h"

#include <windows.h>

// Quello che serve all'Axis Visualizer dentro Iris e vive quanto la finestra:
// le impostazioni (axis.json), il server per OBS, chi ha aperto l'overlay.
struct AxisHost {
    Axis::Settings settings;
    AxisServer server;
    bool overlayFromGame = false; // aperto perché è partito un gioco: si chiude con lui
};

// Messaggi dal thread dell'overlay alla finestra principale.
inline constexpr UINT WM_APP_AXIS_CHANGED = WM_APP + 50; // aperto/chiuso/bloccato
inline constexpr UINT WM_APP_AXIS_MOVED = WM_APP + 51;   // lParam = std::vector<std::pair<int, POINT>>*
