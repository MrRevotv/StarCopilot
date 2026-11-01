---
name: ui-ux-design
description: Regole di design per le interfacce di Iris (pagine WebView2 in src/ui/web e finestre GDI+ sopra il gioco). Usala ogni volta che crei o ritocchi una pagina, un pannello, la barra rapida o un overlay, e per fare un controllo completo dell'interfaccia (spaziature, tipografia, stati dei controlli).
---

# Interfaccia visiva e UX di Iris

Iris ha uno stile **HUD sci-fi** (scelto dall'utente): cabina di Star Citizen, angoli tagliati,
filetti luminosi ciano su fondo blu notte. Tutto il testo è in **italiano**. Le pagine sono servite
dall'exe, offline: si usano solo caratteri già presenti in Windows.

## Tipografia
- Titoli, pulsanti, numeri: `Bahnschrift SemiCondensed` / `Bahnschrift` (variabile `--display`),
  maiuscolo con spaziatura larga (`letter-spacing: .12em`–`.2em`).
- Testo: `Segoe UI Variable Text` / `Segoe UI` (`--body`). Codice, tasti, valori: `Cascadia Mono` / `Consolas` (`--mono`).
- Niente Arial/Inter, niente font scaricati da internet.

## Colori (token in `:root` di `app.css`, non scrivere colori a mano se c'è il token)
- Fondo `--bg #05080e`, pannelli `--panel`, linee `--line` / `--line-2` / `--line-3`.
- Accento `--cyan #3fd8f5`; stati: `--green` (ok), `--amber` (attenzione, in corso), `--red` (errore, pericolo).
- Testo `--text`, secondario `--text-2`, spento `--text-3`.
- Sopra il gioco (GDI+): stessi colori; fondo scuro semitrasparente, testo sempre leggibile anche senza fondo.

## Spaziature e layout
- Griglia da **8 px**: margini e distanze 4, 8, 12, 16, 24, 32 (4 solo dentro i controlli piccoli).
- Pannelli `.panel` con angoli tagliati; titoli `.panel-title`; righe `.row` (testo a sinistra, controllo a destra).
- Una pagina = intestazione (`h1` + una riga di spiegazione + chip di stato) + griglia di pannelli.
- Larghezza minima della finestra 940 px: niente scorrimento orizzontale, testi lunghi con ellissi.

## Stati dei controlli (obbligatori)
Ogni elemento interattivo ha: `default`, `:hover`, `:active`, `:focus-visible`, `:disabled`, e dove serve
`busy`/caricamento (`.btn.busy`). Pulsanti pericolosi `.danger`, azione principale `.primary` (una per gruppo).
Transizioni **150–200 ms** (colore, bordo, ombra, opacità); niente animazioni lunghe o che spostano il layout.

## Comportamento
- Azioni irreversibili (svuota, elimina, attiva funzioni a rischio): conferma con `ask()`, pulsante `danger`.
- Esito delle azioni: `toast()` breve; errori persistenti in un `.callout.bad` nella pagina.
- Mentre l'utente scrive in una casella non ridisegnare quella parte (perderebbe il fuoco).
- Overlay sopra il gioco: non rubano mai il fuoco (WS_EX_NOACTIVATE), esclusi dalle catture quando
  coprono una zona letta dall'OCR, posizioni ricordate, DPI per monitor.

## Verifica
Dopo una modifica: anteprima con Edge headless (dati finti) o con Iris vero, controlla allineamenti,
testi tagliati, stati hover/disabled, contrasto. Le pagine e `app.css` finiscono nell'exe: ricompila.
