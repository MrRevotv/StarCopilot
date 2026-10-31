#pragma once

#include <deque>
#include <vector>

// Dall'audio continuo alle frasi: dove comincia e dove finisce ognuna.
// Riceve blocchi di audio (32 ms) con la probabilità che dentro ci sia voce
// (calcolata da Silero, vedi voice_detector.h) e ritaglia le frasi intere,
// con un pezzetto prima dell'inizio (per non perdere la prima sillaba).
// Niente thread e niente Windows: provato in tests/tests.cpp.
struct VadParams {
    float startThreshold = 0.5f; // probabilità per dire "comincia a parlare"
    float endThreshold = 0.35f;  // sotto, è silenzio
    int startFrames = 2;         // blocchi di voce di fila per cominciare (64 ms: un clic non basta)
    int minSpeechMs = 250;       // frasi più corte (colpi, tosse) si scartano
    int endSilenceMs = 700;      // silenzio che chiude la frase
    int maxUtteranceMs = 15000;  // oltre si chiude comunque
    int preRollMs = 320;         // audio tenuto prima dell'inizio
    int tailMs = 240;            // silenzio lasciato in fondo
    int frameSamples = 512;
    int sampleRate = 16000;
    // Silero, con un microfono quasi muto, dà al rumore di fondo probabilità
    // di 0,2-0,5: la frase non si chiudeva mai. Per cominciare una frase serve
    // un volume minimo; per continuarla, anche il doppio del rumore di fondo
    // (il silenzio dopo che hai parlato conta come silenzio, qualunque cosa dica Silero).
    float minSpeechRms = 0.0015f; // volume minimo della voce
    float noiseFactor = 2.0f;     // la voce sta almeno al doppio del rumore di fondo
};

class VadSegmenter {
public:
    explicit VadSegmenter(const VadParams& params = {});

    void setParams(const VadParams& params);
    const VadParams& params() const { return m_params; }

    // Un blocco di frameSamples campioni. Ritorna true se una frase è finita:
    // la si prende con takeUtterance().
    bool push(const float* frame, float probability);
    std::vector<float> takeUtterance();

    // Qualcuno sta parlando adesso (per l'animazione e per interrompere Iris).
    bool inSpeech() const { return m_inSpeech; }
    // Da quanto dura la frase in corso (0 se nessuno parla).
    int speechMs() const;
    // Il rumore di fondo stimato (RMS) e l'ultima probabilità di voce usata.
    float noiseFloor() const { return m_floor; }
    float lastProbability() const { return m_lastProbability; }

    void reset();

private:
    int frameMs() const { return m_params.frameSamples * 1000 / m_params.sampleRate; }
    void finish();

    VadParams m_params;
    std::deque<std::vector<float>> m_preRoll;
    std::vector<float> m_current;
    std::vector<float> m_ready;
    bool m_inSpeech = false;
    int m_startRun = 0;
    int m_speechFrames = 0;
    int m_silenceRun = 0;
    int m_totalFrames = 0;
    float m_floor = 0.0f;
    float m_lastProbability = 0.0f;
};
