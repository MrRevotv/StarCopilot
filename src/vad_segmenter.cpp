#include "vad_segmenter.h"

#include <algorithm>
#include <cmath>

VadSegmenter::VadSegmenter(const VadParams& params) : m_params(params) {}

void VadSegmenter::setParams(const VadParams& params) {
    m_params = params;
}

void VadSegmenter::reset() {
    m_preRoll.clear();
    m_current.clear();
    m_ready.clear();
    m_inSpeech = false;
    m_startRun = m_speechFrames = m_silenceRun = m_totalFrames = 0;
    m_lastProbability = 0.0f;
}

int VadSegmenter::speechMs() const {
    return m_inSpeech ? m_totalFrames * frameMs() : 0;
}

bool VadSegmenter::push(const float* frame, float probability) {
    const int n = m_params.frameSamples;
    const int fm = std::max(1, frameMs());

    // Il volume del blocco, confrontato col rumore di fondo. Silero, con un
    // microfono quasi muto, dà al rumore probabilità di 0,2-0,5: senza questo
    // controllo la frase non finiva mai.
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += (double)frame[i] * frame[i];
    const float rms = (float)std::sqrt(sum / std::max(1, n));
    const bool audible = rms >= m_params.minSpeechRms;                                    // per cominciare
    const bool aboveNoise = audible && rms >= m_params.noiseFactor * m_floor;             // per continuare
    m_lastProbability = aboveNoise ? probability : 0.0f;

    if (!m_inSpeech) {
        const bool candidate = audible && probability >= m_params.startThreshold;
        // Il rumore di fondo si stima solo fuori dalle frasi: scende in fretta, sale piano.
        if (!candidate) {
            if (m_floor <= 0.0f) m_floor = rms;
            else if (rms < m_floor) m_floor += (rms - m_floor) * 0.1f;
            else m_floor += (rms - m_floor) * 0.005f;
        }
        m_preRoll.emplace_back(frame, frame + n);
        const size_t keep = (size_t)(m_params.preRollMs / fm + m_params.startFrames);
        while (m_preRoll.size() > keep) m_preRoll.pop_front();

        m_startRun = candidate ? m_startRun + 1 : 0;
        if (m_startRun < m_params.startFrames) return false;

        // Comincia: il pre-ascolto (che contiene già i blocchi di voce) apre la frase.
        m_inSpeech = true;
        m_current.clear();
        for (const auto& f : m_preRoll) m_current.insert(m_current.end(), f.begin(), f.end());
        m_preRoll.clear();
        m_speechFrames = m_startRun;
        m_silenceRun = 0;
        m_totalFrames = (int)(m_current.size() / n);
        m_startRun = 0;
        return false;
    }

    m_current.insert(m_current.end(), frame, frame + n);
    m_totalFrames++;
    if (probability >= m_params.endThreshold && aboveNoise) {
        m_speechFrames++;
        m_silenceRun = 0;
    } else {
        m_silenceRun++;
    }

    if (m_silenceRun * fm >= m_params.endSilenceMs || m_totalFrames * fm >= m_params.maxUtteranceMs) {
        finish();
        return !m_ready.empty();
    }
    return false;
}

void VadSegmenter::finish() {
    const int n = m_params.frameSamples;
    const int fm = std::max(1, frameMs());
    // Il silenzio in fondo si accorcia: a whisper serve poco.
    const int tailFrames = m_params.tailMs / fm;
    const int drop = std::max(0, m_silenceRun - tailFrames);
    const size_t dropSamples = std::min(m_current.size(), (size_t)drop * n);
    m_current.resize(m_current.size() - dropSamples);

    if (m_speechFrames * fm >= m_params.minSpeechMs) m_ready.swap(m_current);
    m_current.clear();
    m_inSpeech = false;
    m_speechFrames = m_silenceRun = m_totalFrames = m_startRun = 0;
}

std::vector<float> VadSegmenter::takeUtterance() {
    std::vector<float> out;
    out.swap(m_ready);
    return out;
}
