#pragma once

#include "mining_image.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Il riconoscitore di testo di PaddleOCR (PP-OCRv3, "ch_PP-OCRv3_rec_infer"),
// lo stesso che Citizen Assistant usa tramite RapidOCR per leggere la
// signature del mining: sulle cifre dell'HUD di Star Citizen sbaglia molto
// meno dell'OCR di Windows. Solo RICONOSCIMENTO, niente ricerca del testo: la
// zona l'ha già ritagliata il mining (badge senza icona), e la ricerca
// spezzerebbe il numero perdendo cifre.
//
// Gira con ONNX Runtime (Microsoft, licenza MIT), caricato al volo da
// onnxruntime.dll: niente da installare. Runtime e modello (circa 24 MB)
// si scaricano una volta sola nella cartella dei modelli, controllati con la
// loro impronta SHA-256. Se mancano o non si caricano (per esempio senza
// Visual C++ Redistributable) il mining usa l'OCR di Windows come prima.
namespace PpOcr {

    struct Line {
        std::string text;
        float confidence = 0.0f; // media delle probabilità dei caratteri tenuti (0..1)
    };

    std::wstring folder();      // %appdata%\VoiceToChat\models\ppocr
    std::wstring runtimePath(); // ...\onnxruntime.dll
    std::wstring modelPath();   // ...\ch_PP-OCRv3_rec_infer.onnx
    bool filesPresent();

    // Scarica (e controlla) quello che manca. Va chiamata da un thread di lavoro.
    bool ensureFiles(const std::function<void(long long done, long long total)>& onProgress, const std::atomic<bool>* cancel,
                     std::string& error);

    // Acceso di solito; spento = il mining legge solo con l'OCR di Windows (per i confronti).
    void setEnabled(bool on);
    bool enabled();

    // Carica runtime e modello (una volta sola; poi è un controllo veloce). Thread-safe.
    bool load(std::string& error);
    bool ready();

    // Una riga di testo (immagine in grigi, testo chiaro o scuro). Thread-safe.
    bool recognize(const MiningImage::Gray& image, Line& out, std::string& error);

    // --- Parti pure (provate in tests/tests.cpp) -------------------------------------

    // L'ingresso del modello: altezza 48, larghezza in proporzione (al massimo maxWidth),
    // 3 canali uguali, valori (x/255 - 0.5) / 0.5, ordine CHW. width = colonne del tensore.
    std::vector<float> toTensor(const MiningImage::Gray& image, int& width, int maxWidth = 960);

    // Decodifica CTC "golosa": per ogni passo la classe più probabile, si saltano i
    // vuoti (classe 0) e le ripetizioni. characters[i] = il carattere della classe i.
    Line ctcDecode(const float* probabilities, int steps, int classes, const std::vector<std::string>& characters);

    // Il ritaglio stretto attorno alle cifre chiare (come Citizen Assistant):
    // soglia = media + 1,1 × deviazione (tra 70 e 235), margine del 6% / 10%.
    MiningImage::Gray tight(const MiningImage::Gray& image);

} // namespace PpOcr
