#pragma once

#include <mutex>
#include <string>
#include <vector>

// Il rilevatore di voce Silero (lo stesso file che whisper usa già), usato in
// continuo: per ogni blocco di 32 ms dice quanto è probabile che qualcuno
// stia parlando. Sul processore: è minuscolo (meno di 1 MB) e veloce.
// Se il modello non si carica, si va a volume (meno preciso: anche un rumore forte "parla").
class VoiceDetector {
public:
    static constexpr int kFrameSamples = 512; // 32 ms a 16 kHz

    VoiceDetector() = default;
    ~VoiceDetector();
    VoiceDetector(const VoiceDetector&) = delete;
    VoiceDetector& operator=(const VoiceDetector&) = delete;

    bool init(const std::string& modelPath, int threads);
    bool usesModel() const { return m_ctx != nullptr; }

    // samples: n campioni, n multiplo di kFrameSamples. probs: una probabilità per blocco.
    void process(const float* samples, int n, std::vector<float>& probs);
    // Tra una frase e l'altra (la memoria interna di Silero si azzera).
    void reset();

    // Soglia di volume per quando il modello manca.
    void setEnergyThreshold(float t) { m_energyThreshold = t; }

private:
    std::mutex m_mutex;
    void* m_ctx = nullptr; // whisper_vad_context*
    float m_energyThreshold = 0.01f;
};
