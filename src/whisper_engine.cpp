#include "whisper_engine.h"
#include "gpu.h"
#include "log.h"
#include "speech_utils.h"
#include "text_utils.h"

#include "whisper.h"

#include <algorithm>

namespace {
    void quietLog(ggml_log_level level, const char* text, void*) {
        if (level == GGML_LOG_LEVEL_ERROR && text) Log::error(std::string("whisper: ") + text);
    }
}

WhisperEngine::~WhisperEngine() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready = false;
    if (m_ctx) whisper_free(static_cast<whisper_context*>(m_ctx));
    m_ctx = nullptr;
}

void WhisperEngine::unload() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ready = false;
    if (m_ctx) whisper_free(static_cast<whisper_context*>(m_ctx));
    m_ctx = nullptr;
    m_onGpu = false;
    m_gpuBytes = 0;
}

bool WhisperEngine::init(const std::string& modelPath, const std::string& vadModelPath, int threads, bool useGpu) {
    std::lock_guard<std::mutex> lock(m_mutex);
    whisper_log_set(quietLog, nullptr);
    if (m_ctx) {
        whisper_free(static_cast<whisper_context*>(m_ctx));
        m_ctx = nullptr;
        m_ready = false;
    }
    whisper_context_params cp = whisper_context_default_params();
    // Sulla scheda video se c'è (vedi gpu.h); se non va, sul processore.
    cp.use_gpu = useGpu && Gpu::bestDevice() != nullptr;
    cp.gpu_device = std::max(0, Gpu::bestDeviceIndex());
    // flash_attn: con la build solo-CPU dava crash al caricamento.
    cp.flash_attn = false;
    const long long usedBefore = cp.use_gpu ? Gpu::processMemory() : -1;
    whisper_context* ctx = whisper_init_from_file_with_params(modelPath.c_str(), cp);
    if (!ctx && cp.use_gpu) {
        Log::warn("Whisper: non parte sulla scheda video, uso il processore");
        cp.use_gpu = false;
        ctx = whisper_init_from_file_with_params(modelPath.c_str(), cp);
    }
    if (!ctx) {
        Log::error("Whisper: impossibile caricare il modello " + modelPath);
        return false;
    }
    m_ctx = ctx;
    m_vadPath = vadModelPath;
    m_threads = threads;
    m_onGpu = cp.use_gpu;
    m_gpuBytes = cp.use_gpu && usedBefore >= 0 ? std::max(0LL, Gpu::processMemory() - usedBefore) : 0;
    m_ready = true;
    Log::info("Whisper pronto (" +
              (cp.use_gpu ? "scheda video " + Gpu::bestDeviceName() + ", " + std::to_string(m_gpuBytes >> 20) + " MB"
                          : std::to_string(threads) + " thread") +
              ")");
    return true;
}

std::string WhisperEngine::transcribe(const std::vector<float>& samples, const std::string& languageCode, const std::string& hint) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || samples.empty()) return {};
    auto* ctx = static_cast<whisper_context*>(m_ctx);

    // Whisper lavora a blocchi di 30 s e con meno di 1 s di audio sbaglia più
    // spesso: allunghiamo con un po' di silenzio le frasi cortissime ("sì", "ok").
    std::vector<float> audio = samples;
    const size_t minSamples = SpeechUtils::kSampleRate * 11 / 10;
    if (audio.size() < minSamples) audio.resize(minSamples, 0.0f);

    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.print_progress = false;
    p.print_special = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.translate = false;          // traduce l'LLM, non whisper
    p.language = languageCode.c_str();
    p.detect_language = false;
    p.n_threads = m_threads;
    p.no_context = true;          // ogni frase da sola: niente "trascinamento" dalla precedente
    p.single_segment = false;     // frasi lunghe (oltre 30 s) restano intere
    p.suppress_blank = true;
    p.suppress_nst = true;        // niente "[Musica]", "(applausi)"...
    p.no_speech_thold = 0.6f;
    p.temperature = 0.0f;
    p.temperature_inc = 0.2f;     // se la prima prova è incerta, riprova (meno frasi inventate)
    // Solo i pezzi in cui qualcuno parla: su silenzio, tosse o un clic i modelli
    // grandi inventano frasi intere ("Grazie a tutti."). Senza parlato, niente testo.
    if (!m_vadPath.empty()) {
        p.vad = true;
        p.vad_model_path = m_vadPath.c_str();
    }
    if (!hint.empty()) p.initial_prompt = hint.c_str();

    if (whisper_full(ctx, p, audio.data(), (int)audio.size()) != 0) {
        Log::error("Whisper: trascrizione non riuscita");
        return {};
    }

    std::string text;
    const int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        // Segmento che whisper stesso giudica "non parlato": lo saltiamo.
        if (whisper_full_get_segment_no_speech_prob(ctx, i) > 0.85f) continue;
        const char* seg = whisper_full_get_segment_text(ctx, i);
        if (seg) text += seg;
    }

    std::string cleaned = SpeechUtils::cleanTranscript(text);
    if (cleaned.empty() || SpeechUtils::isLikelyHallucination(cleaned)) {
        if (!TextUtils::trim(text).empty()) Log::info("Whisper: scartato \"" + TextUtils::trim(text) + "\" (non parlato)");
        return {};
    }
    // Il suggerimento a volte "esce" nel testo: se la trascrizione è solo quello, è inventata.
    if (!hint.empty() && TextUtils::normalizeForMatch(hint).find(TextUtils::normalizeForMatch(cleaned)) != std::string::npos &&
        cleaned.size() > 12) {
        Log::info("Whisper: scartato \"" + cleaned + "\" (ripete il suggerimento)");
        return {};
    }
    return cleaned;
}
