#include "voice_detector.h"
#include "log.h"

#include "whisper.h"

#include <algorithm>
#include <cmath>

VoiceDetector::~VoiceDetector() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ctx) whisper_vad_free(static_cast<whisper_vad_context*>(m_ctx));
    m_ctx = nullptr;
}

bool VoiceDetector::init(const std::string& modelPath, int threads) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ctx) {
        whisper_vad_free(static_cast<whisper_vad_context*>(m_ctx));
        m_ctx = nullptr;
    }
    whisper_vad_context_params p = whisper_vad_default_context_params();
    p.n_threads = std::max(1, threads);
    p.use_gpu = false;
    m_ctx = whisper_vad_init_from_file_with_params(modelPath.c_str(), p);
    if (!m_ctx) {
        Log::warn("Rilevatore di voce non caricato: uso il volume del microfono");
        return false;
    }
    Log::info("Rilevatore di voce pronto (Silero, processore)");
    return true;
}

void VoiceDetector::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ctx) whisper_vad_reset_state(static_cast<whisper_vad_context*>(m_ctx));
}

void VoiceDetector::process(const float* samples, int n, std::vector<float>& probs) {
    probs.clear();
    const int frames = n / kFrameSamples;
    if (frames <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto* ctx = static_cast<whisper_vad_context*>(m_ctx);
    if (ctx && whisper_vad_detect_speech_no_reset(ctx, samples, frames * kFrameSamples)) {
        const int count = std::min(frames, whisper_vad_n_probs(ctx));
        const float* p = whisper_vad_probs(ctx);
        probs.assign(p, p + count);
        while ((int)probs.size() < frames) probs.push_back(0.0f);
        return;
    }
    // Senza modello: voce = volume sopra la soglia.
    for (int f = 0; f < frames; ++f) {
        double sum = 0.0;
        for (int i = 0; i < kFrameSamples; ++i) {
            float s = samples[f * kFrameSamples + i];
            sum += (double)s * s;
        }
        const double rms = std::sqrt(sum / kFrameSamples);
        probs.push_back((float)std::clamp(rms / (2.0 * m_energyThreshold), 0.0, 1.0));
    }
}
