---
name: windows-system-integration
description: Integrazione di Iris con Windows - avvio automatico (chiave Run), icona nell'area di notifica, aggiornamento automatico da GitHub Releases, tasti globali, istanza unica. Usala quando tocchi avvio, chiusura, tray, aggiornamenti o tasti globali.
---

# Integrazione con Windows e aggiornamento automatico (Iris)

## Avvio con Windows
- Chiave utente `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, valore `Iris` =
  `"<percorso di iris.exe>" --avvio-windows`. Mai HKLM, mai privilegi da amministratore.
- Lo stato vero è il registro (non un'impostazione): l'interruttore della pagina lo legge e lo scrive.
- Se il percorso nel registro non è quello dell'exe attuale (exe spostato) l'interfaccia lo segnala e lo corregge
  quando riattivi l'opzione.
- `--avvio-windows`: niente finestra, solo l'icona nell'area di notifica; i modelli si caricano in sottofondo.

## Area di notifica (tray)
- Icona sempre presente: clic = mostra Iris, menu = Mostra Iris / Barra rapida / Esci.
- Chiudere la finestra (X, Alt+F4 = `SC_CLOSE`) la nasconde nell'area di notifica se l'opzione è attiva;
  `WM_CLOSE` mandato dal programma (aggiornamento, prove) chiude davvero.
- Ricreare l'icona su `TaskbarCreated` (Explorer riavviato).

## Istanza unica
- Mutex `Local\Iris.UnaCopiaSola`; la seconda copia chiede alla prima di mostrarsi (messaggio alla finestra
  `IrisWindow`, `AllowSetForegroundWindow`) ed esce.

## Aggiornamento automatico (già fatto: `updater.*`)
- GitHub Releases di `MrRevotv/StarCopilot`, confronto con `tag_name`, impronta SHA-256 dell'asset.
- Download in sottofondo, installazione al riavvio (o subito su richiesta): exe in uso rinominato `.old`,
  nuova copia, riavvio con `--dopo-aggiornamento`. La versione sta nel file `VERSION`.
- Pubblicare solo quando l'utente lo chiede (`pubblica.bat`).

## Tasti globali
- `HotkeyListener` (thread suo, `GetAsyncKeyState` + joystick): funziona anche col gioco in primo piano
  senza agganci di sistema. Tasti configurabili dalla pagina (tastiera o pulsanti del joystick).
- Mai mandare input al gioco senza che l'utente l'abbia chiesto.
