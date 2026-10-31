#include "neural_voice.h"
#include "gpu.h"
#include "log.h"

#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace {
    // Come llama-tts (tools/tts): campionamento dei codici "semantici" del modello.
    constexpr float kTemperature = 0.8f;
    constexpr int kTopK = 40;
    constexpr float kTopP = 0.95f;
    constexpr float kMinP = 0.05f;
    constexpr float kRepeatPenalty = 1.05f;
    constexpr int kContext = 4096;
    constexpr int kBatch = 512;
    // 12,5 fotogrammi al secondo di voce: il massimo per una frase, per non
    // generare all'infinito se il modello non trova la fine.
    constexpr double kFramesPerSecond = 12.5;
}

NeuralVoice::~NeuralVoice() {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();
}

void NeuralVoice::release() {
    m_ready = false;
    if (m_gen) mtmd_helper_gen_audio_free(static_cast<mtmd_helper_gen_audio*>(m_gen));
    if (m_sampler) llama_sampler_free(static_cast<llama_sampler*>(m_sampler));
    if (m_mtmd) mtmd_free(static_cast<mtmd_context*>(m_mtmd));
    if (m_ctx) llama_free(static_cast<llama_context*>(m_ctx));
    if (m_model) llama_model_free(static_cast<llama_model*>(m_model));
    m_gen = m_sampler = m_mtmd = m_ctx = m_model = nullptr;
    m_gpuBytes = 0;
}

void NeuralVoice::unload() {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();
}

bool NeuralVoice::load(const std::string& modelPath, const std::string& mmprojPath, int threads) {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();
    Gpu::prepare();
    llama_backend_init();

    // Solo sulla scheda video: sul processore sarebbe più lenta del parlato.
    void* gpu = Gpu::bestDevice();
    if (!gpu) {
        Log::warn("Voce neurale: nessuna scheda video compatibile, resta la voce di Windows");
        return false;
    }
    const long long usedBefore = Gpu::processMemory();

    llama_model_params mp = llama_model_default_params();
    ggml_backend_dev_t devices[2] = { static_cast<ggml_backend_dev_t>(gpu), nullptr };
    mp.devices = devices;
    mp.n_gpu_layers = 999;
    llama_model* model = llama_model_load_from_file(modelPath.c_str(), mp);
    if (!model) {
        Log::error("Voce neurale: modello non caricato " + modelPath);
        return false;
    }
    m_model = model;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = kContext;
    cp.n_batch = kBatch;
    cp.n_ubatch = kBatch;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.embeddings = true; // lo stato nascosto del modello serve a generare l'audio
    cp.no_perf = true;
    llama_context* ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        Log::error("Voce neurale: contesto non creato");
        release();
        return false;
    }
    m_ctx = ctx;

    mtmd_context_params mtp = mtmd_context_params_default();
    mtp.use_gpu = true;
    mtp.device = static_cast<ggml_backend_dev_t>(gpu);
    mtp.print_timings = false;
    mtp.n_threads = threads;
    mtmd_context* mtmd = mtmd_init_from_file(mmprojPath.c_str(), model, mtp);
    if (!mtmd) {
        Log::error("Voce neurale: decodificatore audio non caricato " + mmprojPath);
        release();
        return false;
    }
    m_mtmd = mtmd;
    const mtmd_gen_audio_info info = mtmd_gen_audio_get_info(mtmd);
    if (info.type == MTMD_GEN_AUDIO_TYPE_NONE) {
        Log::error("Voce neurale: il file mmproj non genera audio");
        release();
        return false;
    }
    m_sampleRate = info.sample_rate > 0 ? info.sample_rate : 24000;
    m_gen = mtmd_helper_gen_audio_init(ctx, mtmd);
    if (!m_gen) {
        release();
        return false;
    }

    const llama_vocab* vocab = llama_model_get_vocab(model);
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler* sampler = llama_sampler_chain_init(sp);
    llama_sampler_chain_add(sampler, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), kContext, kRepeatPenalty, 0.0f, 0.0f));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(kTopK));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(kTopP, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_min_p(kMinP, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(kTemperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist((uint32_t)std::random_device{}()));
    m_sampler = sampler;
    m_ready = true;
    m_gpuBytes = usedBefore >= 0 ? std::max(0LL, Gpu::processMemory() - usedBefore) : 0;
    Log::info("Voce neurale pronta (Qwen3-TTS, " + std::to_string(m_gpuBytes >> 20) + " MB sulla scheda video, " +
              std::to_string(m_sampleRate) + " Hz)");
    return true;
}

bool NeuralVoice::speak(const std::string& text, const std::function<bool(const float*, size_t)>& onChunk,
                        const std::function<bool()>& shouldStop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || text.empty()) return false;
    auto* ctx = static_cast<llama_context*>(m_ctx);
    auto* gen = static_cast<mtmd_helper_gen_audio*>(m_gen);
    auto* sampler = static_cast<llama_sampler*>(m_sampler);
    const auto t0 = std::chrono::steady_clock::now();

    // Ogni frase da capo.
    llama_memory_clear(llama_get_memory(ctx), true);
    mtmd_helper_gen_audio_reset(gen);
    llama_sampler_reset(sampler);

    mtmd_helper_gen_audio_inp inp = mtmd_helper_gen_audio_inp_default();
    inp.stream = true; // l'audio arriva a pezzi: le frasi lunghe cominciano a suonare prima
    inp.seq_id = 0;
    inp.prompt = text.c_str();
    inp.prompt_len = text.size();
    inp.speaker_ref = nullptr;
    inp.lang = "it";
    inp.out_type = MTMD_HELPER_GEN_AUDIO_OUTTYPE_PCM;
    if (mtmd_helper_gen_audio_set_input(gen, &inp) != 0) {
        Log::error("Voce neurale: testo non accettato");
        return false;
    }
    for (;;) {
        const int32_t left = mtmd_helper_gen_audio_step_prompt(gen, kBatch);
        if (left < 0) {
            Log::error("Voce neurale: elaborazione del testo non riuscita");
            return false;
        }
        if (left == 0) break;
    }

    // Quanto può durare al massimo: circa 13 caratteri al secondo di parlato, con margine.
    const int maxFrames = std::clamp((int)(text.size() / 13.0 * kFramesPerSecond * 2.0) + 40, 60, 1500);
    auto deliver = [&]() -> bool {
        int32_t rate = 0;
        const char* data = nullptr;
        size_t len = 0;
        if (mtmd_helper_gen_audio_get_output(gen, &rate, &data, &len, nullptr) != 0) return false;
        if (len == 0) return true;
        return onChunk(reinterpret_cast<const float*>(data), len / sizeof(float));
    };

    llama_token sampled = llama_sampler_sample(sampler, ctx, -1);
    const float* hState = llama_get_embeddings_ith(ctx, -1);
    int frames = 0;
    bool stop = false, cancelled = false;
    while (!stop && frames < maxFrames) {
        if (shouldStop && shouldStop()) {
            cancelled = true;
            break;
        }
        const float* hNext = nullptr;
        if (mtmd_helper_gen_audio_step_gen(gen, sampled, hState, &hNext, &stop) != 0) {
            Log::error("Voce neurale: generazione interrotta al fotogramma " + std::to_string(frames));
            return false;
        }
        if (!hNext) break;
        frames++;
        if (!deliver()) {
            cancelled = true;
            break;
        }
        hState = hNext;
        sampled = llama_sampler_sample(sampler, ctx, -1);
    }
    if (!cancelled) {
        mtmd_helper_gen_audio_flush(gen);
        deliver();
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    Log::info("Voce neurale: " + std::to_string(frames) + " fotogrammi (" + std::to_string((int)(frames / kFramesPerSecond * 10) / 10.0).substr(0, 4) +
              " s di voce) in " + std::to_string(ms) + " ms" + (cancelled ? ", interrotta" : "") + (frames >= maxFrames ? ", troppo lunga: tagliata" : ""));
    return true;
}
