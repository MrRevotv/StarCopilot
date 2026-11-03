#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

// La ruota di Iris sopra il gioco e i suoi pannelli (Missioni, Raffineria, Diario).
//
// La ruota si apre al centro dello schermo del gioco col suo tasto (Shift+Tab di base)
// e porta Iris in PRIMO PIANO: il gioco perde il fuoco e libera il cursore, così si
// sceglie con il mouse senza Alt-Tab. Si sceglie dalla direzione del mouse rispetto al
// centro; il centro, Esc, il tasto destro o di nuovo il tasto riportano il fuoco al
// gioco. Cliccare fuori (sul gioco) la chiude e basta.
//
// I pannelli invece NON rubano mai il fuoco (si cliccano col cursore libero, il gioco
// resta davanti): si mettono in alto uno accanto all'altro, oppure dove li hai lasciati
// l'ultima volta che li hai trascinati. Finestre native (GDI+, alfa per pixel), escluse
// dalle catture (OBS, letture OCR di Iris). Thread della finestra.
namespace QuickBar {

    // Una voce della ruota. Toni come le righe (vedi Row).
    struct Button {
        std::string id;
        std::wstring label;
        std::wstring hint;  // sotto la ruota quando ci passi sopra
        bool on = false;    // acceso (interruttore) o pannello aperto
        bool enabled = true;
        std::wstring state; // al centro della ruota: "ACCESA", "APERTO", "DA PREPARARE"...
        int stateTone = 4;
    };

    // Toni: 0 normale, 1 bene (verde), 2 attenzione (ambra), 3 pericolo (rosso), 4 spento, 5 titoletto, 6 accento (ciano).
    struct Row {
        std::wstring text;
        std::wstring right;   // a destra: ora, conto alla rovescia...
        int tone = 0;
        double progress = -1; // 0..1: barretta sotto la riga (raffineria)
    };

    struct Callbacks {
        std::function<void(const std::string& id)> clicked;                  // voce della ruota scelta
        std::function<void(const std::string& panel, int x, int y)> moved;   // pannello spostato (pixel dello schermo)
        std::function<void(const std::string& panel)> closed;                // pannello chiuso dalla sua X
    };

    void create(HINSTANCE instance, Callbacks callbacks);
    void destroy();

    // Le voci della ruota (in senso orario dall'alto), i tasti per tornare al gioco, lo stato di Iris.
    void setButtons(const std::vector<Button>& buttons, const std::wstring& keyHint, const std::wstring& status, int statusTone);
    // Apre la ruota sul monitor della finestra in primo piano e prende il primo piano.
    // centerCursor: il mouse parte dal centro (aperta dal tasto, in gioco).
    void show(bool centerCursor);
    // Chiude la ruota; backToGame: ridà il primo piano alla finestra che c'era prima (il gioco).
    void hide(bool backToGame);
    bool visible();

    // I pannelli ("missions", "refinery", "log"): contenuto, apertura (x, y = -1: in alto, in fila), chiusura.
    void setPanel(const std::string& id, const std::wstring& title, const std::vector<Row>& rows);
    void openPanel(const std::string& id, int x, int y);
    void closePanel(const std::string& id);
    bool panelOpen(const std::string& id);

    // Nascondi tutto (ruota e pannelli) senza dimenticare cosa era aperto; false = rimetti i pannelli.
    void setHiddenAll(bool hidden);

    // La voce della ruota nella direzione (dx, dy) dal centro (y verso il basso): 0 in alto, poi in senso
    // orario; -1 se il punto è nel cerchio centrale (raggio deadZone).
    int sectorAt(double dx, double dy, int count, double deadZone);

    // Per le prove: la ruota (con il mouse sulla voce hoverIndex, -1 nessuna, -2 il centro) e i pannelli
    // disegnati in PNG su uno sfondo scuro, con lo stesso codice dello schermo (le finestre vere sono
    // escluse dalle catture).
    bool savePreview(const std::wstring& folder, int hoverIndex);

} // namespace QuickBar
