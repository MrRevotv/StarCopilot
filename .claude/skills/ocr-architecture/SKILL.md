---
name: ocr-architecture
description: Architettura OCR di Iris - OCR di Windows (Windows.Media.Ocr), PP-OCRv3 con ONNX Runtime, pre-elaborazione delle immagini, letture in thread separati. Usala quando tocchi il mining (signature dello scanner), la traduzione della chat letta a schermo o la mappa stellare.
---

# OCR in Iris

## Motori (locali, gratuiti)
- **OCR di Windows** (`ocr_engine.*`, `screen_ocr.*`): multilingua, con la posizione delle parole.
  Chat letta (`game_chat`), mappa stellare (`game_controller`), riserva del mining.
- **PP-OCRv3** (`ppocr.*`): riconoscitore di PaddleOCR via ONNX Runtime caricato al volo
  (`OrtGetApiBase`), decodifica CTC. Primo tentativo sui badge del mining. File scaricati al primo
  avvio con impronta SHA-256 fissa in `%appdata%\VoiceToChat\models\ppocr`. Se manca, si torna a Windows.

## Pre-elaborazione (`mining_image.*`, senza OpenCV)
Trova il badge, toglie l'icona, scala di grigi, CLAHE, Otsu, versioni invertite/contrastate.
Si provano più versioni e si tiene quella che "torna" con i dati (tabella dei materiali), con il punteggio
di confidenza del motore.

## Regole
- Le letture pesanti girano in thread loro (scanner del mining, chat letta): l'interfaccia non si blocca mai.
- Zone dello schermo scelte dall'utente (anche ruotate: l'HUD segue la curva del casco); gli overlay
  di Iris sono esclusi dalle catture quando coprono la zona letta.
- Correzioni solo se univoche; altrimenti si mostrano le alternative ("probabile").
- Immagini di debug solo se l'utente le chiede, e solo l'ultima lettura.
- Ogni cambio al riconoscimento ha una prova in `tests/tests.cpp` (immagini sintetiche o testi OCR reali).
