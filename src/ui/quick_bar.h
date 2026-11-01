#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// La barra rapida di Iris sopra il gioco (l'idea della barra di citizen-assistant):
// scende dall'alto dello schermo col suo tasto (Num / di base), con i pulsanti
// per leggere il mining, accendere la traduzione della chat, l'overlay degli assi,
// la sfera, e aprire i pannelli (Missioni, Raffineria, Diario). Nascondi tutto
// (Num *) toglie dallo schermo tutto quello di Iris, per un attimo.
//
// Finestre native (GDI+, alfa per pixel) che NON rubano mai il fuoco al gioco:
// si clicca con il cursore libero (mobiGlas, Alt...), il gioco resta in primo
// piano. Escluse dalle catture (OBS, letture OCR di Iris). Thread della finestra.
namespace QuickBar {

    struct Button {
        std::string id;
        std::wstring label;
        std::wstring hint; // scritto nella barra quando ci passi sopra
        bool on = false;   // acceso (interruttore) o pannello aperto
        bool enabled = true;
    };

    // Toni: 0 normale, 1 bene (verde), 2 attenzione (ambra), 3 pericolo (rosso), 4 spento, 5 titoletto, 6 accento (ciano).
    struct Row {
        std::wstring text;
        std::wstring right;   // a destra: ora, conto alla rovescia...
        int tone = 0;
        double progress = -1; // 0..1: barretta sotto la riga (raffineria)
    };

    struct Callbacks {
        std::function<void(const std::string& id)> clicked;                  // pulsante della barra
        std::function<void(const std::string& panel, int x, int y)> moved;   // pannello spostato (pixel dello schermo)
        std::function<void(const std::string& panel)> closed;                // pannello chiuso dalla sua X
    };

    void create(HINSTANCE instance, Callbacks callbacks);
    void destroy();

    // La barra: pulsanti, suggerimento sui tasti, stato di Iris (tono come le righe).
    void setButtons(const std::vector<Button>& buttons, const std::wstring& keyHint, const std::wstring& status, int statusTone);
    void show();
    void hide();
    void toggle();
    bool visible();

    // I pannelli ("missions", "refinery", "log"): contenuto, apertura (x, y = -1: sotto la barra), chiusura.
    void setPanel(const std::string& id, const std::wstring& title, const std::vector<Row>& rows);
    void openPanel(const std::string& id, int x, int y);
    void closePanel(const std::string& id);
    bool panelOpen(const std::string& id);

    // Nascondi tutto (barra e pannelli) senza dimenticare cosa era aperto; false = rimetti com'era.
    void setHiddenAll(bool hidden);

    // Per le prove: barra (con il mouse sul pulsante hoverIndex) e pannelli disegnati in PNG su
    // uno sfondo scuro, con lo stesso codice dello schermo (le finestre vere sono escluse dalle catture).
    bool savePreview(const std::wstring& folder, int hoverIndex);

} // namespace QuickBar
