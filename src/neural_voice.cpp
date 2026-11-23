#include "neural_voice.h"
#include "app_paths.h"
#include "gpu.h"
#include "log.h"
#include "text_utils.h"

#include "llama.h"
#include "mtmd-helper.h"
#include "mtmd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
    constexpr int kContext = 4096;
    constexpr int kBatch = 512;
    // 12,5 fotogrammi al secondo di voce: il massimo per una frase, per non
    // generare all'infinito se il modello non trova la fine.
    constexpr double kFramesPerSecond = 12.5;
    // I codici del modello: 2048 suoni + 1024 speciali (inizio, pausa, lingua...).
    // Qwen3-TTS non lascia mai uscire gli speciali tranne la fine: qui uguale.
    constexpr int kCodecSounds = 2048;
    constexpr int kCodecAll = 3072;

    // Il testo con cui il modello genera le voci predefinite (circa 10 s): frasi
    // tranquille, con i suoni dell'italiano e qualche nome del gioco.
    const char* const kReferenceText =
        "Buongiorno, comandante. Sono Iris, l'assistente di bordo. I sistemi della nave sono a posto: "
        "scudi, motori e carburante funzionano bene. Dimmi pure dove vuoi andare, e io preparo la rotta.";

    llama_token findToken(const llama_vocab* vocab, const char* piece) {
        const int32_t n = llama_vocab_n_tokens(vocab);
        for (llama_token t = n - 1; t >= 0; --t) { // i codici sono in fondo al vocabolario
            const char* text = llama_vocab_get_text(vocab, t);
            if (text && std::strcmp(text, piece) == 0) return t;
        }
        return LLAMA_TOKEN_NULL;
    }

    std::string seconds1(double s) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.1f", s);
        return b;
    }
}

const std::vector<NeuralVoice::Preset>& NeuralVoice::presets() {
    // Voci femminili e calme, scelte fra 30 semi misurando (iris_tests --voce-semi e
    // --voce-coerenza): F0 da donna, timbro uguale in tutte le frasi (somiglianza 0,993
    // come la voce di Windows), poca variazione dentro le frasi, whisper capisce tutto.
    static const std::vector<Preset> list = {
        { "aurora", "Aurora (calma, la più stabile)", 28 }, // F0 ~190 Hz, la meno "cantilenante"
        { "vega", "Vega (chiara, decisa)", 5 },             // F0 ~200 Hz
        { "lyra", "Lyra (più bassa, nitida)", 30 },         // F0 ~175 Hz, timbro brillante
    };
    return list;
}

bool NeuralVoice::isPreset(const std::string& id) {
    for (const auto& p : presets()) {
        if (id == p.id) return true;
    }
    return false;
}

std::wstring NeuralVoice::presetPath(const std::string& id) {
    // "-1": la ricetta (testo, semi, campionamento). Se cambia, si cambia il numero e la voce si rigenera.
    return AppPaths::getAppDataDir() + L"\\voci\\" + TextUtils::utf8ToWide(id) + L"-1.wav";
}

NeuralVoice::~NeuralVoice() {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();
}

void NeuralVoice::release() {
    m_ready = false;
    if (m_reference) mtmd_bitmap_free(static_cast<mtmd_bitmap*>(m_reference));
    if (m_gen) mtmd_helper_gen_audio_free(static_cast<mtmd_helper_gen_audio*>(m_gen));
    if (m_sampler) llama_sampler_free(static_cast<llama_sampler*>(m_sampler));
    if (m_mtmd) mtmd_free(static_cast<mtmd_context*>(m_mtmd));
    if (m_ctx) llama_free(static_cast<llama_context*>(m_ctx));
    if (m_model) llama_model_free(static_cast<llama_model*>(m_model));
    m_reference = m_gen = m_sampler = m_mtmd = m_ctx = m_model = nullptr;
    m_gpuBytes = 0;
    m_suppressed.clear();
    m_activeVoice.clear();
    m_activeName.clear();
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
    if (!mtmd_support_audio(mtmd)) Log::warn("Voce neurale: il mmproj non ha il codificatore di parlante: la voce può cambiare tra una frase e l'altra");

    // I codici speciali (in fondo al vocabolario, dopo i 2048 suoni) non devono uscire, tranne la fine.
    const llama_vocab* vocab = llama_model_get_vocab(model);
    const int32_t nVocab = llama_vocab_n_tokens(vocab);
    const llama_token codec0 = findToken(vocab, "<|codec_0|>");
    const llama_token codecEos = findToken(vocab, "<|codec_eos_token|>");
    if (codec0 != LLAMA_TOKEN_NULL && codec0 + kCodecAll == nVocab && codecEos >= codec0 + kCodecSounds) {
        for (llama_token t = codec0 + kCodecSounds; t < nVocab; ++t) {
            if (t != codecEos) m_suppressed.push_back(t);
        }
    } else {
        Log::warn("Voce neurale: vocabolario diverso dal previsto, codici speciali non filtrati");
    }
    buildSampler(m_sampling.seed);
    m_ready = true;
    m_gpuBytes = usedBefore >= 0 ? std::max(0LL, Gpu::processMemory() - usedBefore) : 0;
    Log::info("Voce neurale pronta (Qwen3-TTS, " + std::to_string(m_gpuBytes >> 20) + " MB sulla scheda video, " +
              std::to_string(m_sampleRate) + " Hz)");
    return true;
}

void NeuralVoice::buildSampler(uint32_t seed) {
    if (m_sampler) llama_sampler_free(static_cast<llama_sampler*>(m_sampler));
    m_sampler = nullptr;
    if (!m_model) return;
    const llama_vocab* vocab = llama_model_get_vocab(static_cast<llama_model*>(m_model));
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler* sampler = llama_sampler_chain_init(sp);
    if (!m_suppressed.empty()) {
        std::vector<llama_logit_bias> bias;
        for (llama_token t : m_suppressed) bias.push_back({ t, -INFINITY });
        llama_sampler_chain_add(sampler, llama_sampler_init_logit_bias(llama_vocab_n_tokens(vocab), (int32_t)bias.size(), bias.data()));
    }
    if (m_sampling.repeatPenalty != 1.0f) {
        llama_sampler_chain_add(sampler, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), kContext, m_sampling.repeatPenalty, 0.0f, 0.0f));
    }
    if (m_sampling.topK > 0) llama_sampler_chain_add(sampler, llama_sampler_init_top_k(m_sampling.topK));
    if (m_sampling.topP < 1.0f) llama_sampler_chain_add(sampler, llama_sampler_init_top_p(m_sampling.topP, 1));
    if (m_sampling.minP > 0.0f) llama_sampler_chain_add(sampler, llama_sampler_init_min_p(m_sampling.minP, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(m_sampling.temperature));
    // Seme fisso: llama_sampler_reset lo rimette a ogni frase, quindi stesso testo = stessi suoni.
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(seed));
    m_sampler = sampler;
    m_samplerSeed = seed;
}

void NeuralVoice::setSampling(const Sampling& s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sampling = s;
    buildSampler(s.seed);
}

void NeuralVoice::setVoice(const std::string& id, const std::string& wavPath) {
    std::lock_guard<std::mutex> lock(m_wantMutex);
    m_wantedId = id;
    m_wantedFile = wavPath;
    m_wantChanged = true;
}

std::string NeuralVoice::voiceName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeName;
}

bool NeuralVoice::useReferenceLocked(const std::vector<float>& pcm, int rate) {
    if (m_reference) mtmd_bitmap_free(static_cast<mtmd_bitmap*>(m_reference));
    m_reference = nullptr;
    if (pcm.empty()) return true;
    auto* mtmd = static_cast<mtmd_context*>(m_mtmd);
    if (!mtmd || !mtmd_support_audio(mtmd)) return false;
    const int wanted = mtmd_get_audio_sample_rate(mtmd) > 0 ? mtmd_get_audio_sample_rate(mtmd) : m_sampleRate;
    const std::vector<float> s = VoiceMeasure::resample(pcm.data(), pcm.size(), rate, wanted);
    m_reference = mtmd_bitmap_init_from_audio(s.size(), s.data());
    return m_reference != nullptr;
}

bool NeuralVoice::setReferenceAudio(const std::vector<float>& pcm, int rate) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return false;
    m_activeVoice = "prova";
    m_activeName = pcm.empty() ? "nessun riferimento" : "riferimento di prova";
    return useReferenceLocked(pcm, rate);
}

bool NeuralVoice::makePresetReference(const Preset& preset, std::vector<float>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out.clear();
    if (!m_ready) return false;
    // Senza riferimento il modello si inventa una voce, decisa dal seme.
    void* saved = m_reference;
    m_reference = nullptr;
    const bool ok = generateLocked(kReferenceText, [&](const float* p, size_t n) {
        out.insert(out.end(), p, p + n);
        return true;
    }, {}, preset.seed, false, nullptr);
    m_reference = saved;
    return ok && !out.empty();
}

bool NeuralVoice::prepare() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return false;
    const bool ok = prepareLocked();
    // La prima frase sulla scheda video è lenta (prepara i suoi programmi): una a vuoto adesso,
    // con la voce vera (anche il codificatore di parlante si scalda).
    generateLocked("Pronta.", [](const float*, size_t) { return true; }, {}, m_sampling.seed, true, nullptr);
    return ok;
}

bool NeuralVoice::prepareLocked() {
    std::string id, file;
    {
        std::lock_guard<std::mutex> lock(m_wantMutex);
        id = m_wantedId;
        file = m_wantedFile;
        // Un riferimento di prova (setReferenceAudio) resta finché non si sceglie una voce.
        if (m_activeVoice == "prova" && !m_wantChanged) return m_reference != nullptr;
        m_wantChanged = false;
    }
    if (id != kFileVoice && !isPreset(id)) id = presets().front().id;
    const std::string key = id == kFileVoice ? "file:" + file : id;
    if (key == m_activeVoice) return m_reference != nullptr;
    m_activeVoice = key; // anche se non riesce: non si riprova a ogni frase

    std::vector<float> pcm;
    int rate = 0;
    std::string error;
    if (id == kFileVoice) {
        const std::wstring path = TextUtils::utf8ToWide(file);
        if (readWav(path, pcm, rate, &error) && cleanReference(pcm, rate, &error) && useReferenceLocked(pcm, rate)) {
            m_activeName = "il tuo file " + TextUtils::wideToUtf8(std::filesystem::path(path).filename().wstring());
            Log::info("Voce neurale: " + m_activeName + " (" + seconds1(pcm.size() / (double)rate) + " s)");
            return true;
        }
        Log::warn("Voce neurale: il file \"" + file + "\" non va (" + error + "): uso la voce " + presets().front().label);
        id = presets().front().id;
    }
    const Preset* preset = &presets().front();
    for (const auto& p : presets()) {
        if (id == p.id) preset = &p;
    }
    m_activeName = preset->label;
    const std::wstring path = presetPath(preset->id);
    if (readWav(path, pcm, rate) && cleanReference(pcm, rate) && useReferenceLocked(pcm, rate)) {
        Log::info("Voce neurale: " + m_activeName);
        return true;
    }
    // La prima volta: il modello genera la voce dal suo seme e la si salva.
    const auto t0 = std::chrono::steady_clock::now();
    pcm.clear();
    void* saved = m_reference;
    m_reference = nullptr;
    const bool made = generateLocked(kReferenceText, [&](const float* p, size_t n) {
        pcm.insert(pcm.end(), p, p + n);
        return true;
    }, {}, preset->seed, false, nullptr);
    m_reference = saved;
    if (made && cleanReference(pcm, m_sampleRate, &error)) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        // Si usa quello salvato (16 bit), così la voce di oggi è identica a quella dei giorni dopo.
        std::vector<float> saved16;
        int savedRate = 0;
        if (writeWav(path, pcm, m_sampleRate) && readWav(path, saved16, savedRate) && cleanReference(saved16, savedRate)) {
            pcm.swap(saved16);
        }
        if (useReferenceLocked(pcm, m_sampleRate)) {
            Log::info("Voce neurale: " + m_activeName + ", preparata in " +
                      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()) + " ms e salvata");
            return true;
        }
        error = "codificatore di parlante non disponibile";
    }
    Log::error("Voce neurale: voce di riferimento non preparata (" + error + "): la voce può cambiare tra una frase e l'altra");
    useReferenceLocked({}, 0);
    return false;
}

bool NeuralVoice::speak(const std::string& text, const std::function<bool(const float*, size_t)>& onChunk,
                        const std::function<bool()>& shouldStop, int attempt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready || TextUtils::trim(text).empty()) return false;
    prepareLocked(); // la voce scelta (niente da fare se è già pronta)
    return generateLocked(text, onChunk, shouldStop, m_sampling.seed + (uint32_t)attempt * 7919u, true, nullptr);
}

bool NeuralVoice::generateLocked(const std::string& text, const std::function<bool(const float*, size_t)>& onChunk,
                                 const std::function<bool()>& shouldStop, uint32_t seed, bool withReference, int* framesOut) {
    auto* ctx = static_cast<llama_context*>(m_ctx);
    auto* gen = static_cast<mtmd_helper_gen_audio*>(m_gen);
    if (seed != m_samplerSeed) buildSampler(seed);
    auto* sampler = static_cast<llama_sampler*>(m_sampler);
    const auto t0 = std::chrono::steady_clock::now();

    // Ogni frase da capo, con gli stessi semi.
    llama_memory_clear(llama_get_memory(ctx), true);
    mtmd_helper_gen_audio_reset(gen);
    llama_sampler_reset(sampler);

    mtmd_helper_gen_audio_inp inp = mtmd_helper_gen_audio_inp_default();
    inp.stream = true; // l'audio arriva a pezzi: le frasi lunghe cominciano a suonare prima
    inp.seq_id = 0;
    inp.prompt = text.c_str();
    inp.prompt_len = text.size();
    inp.speaker_ref = withReference ? static_cast<mtmd_bitmap*>(m_reference) : nullptr;
    inp.lang = "it";
    inp.top_k = 50; // il "code predictor" (gli altri 15 codici di ogni fotogramma): valori di Qwen3-TTS
    inp.top_p = 1.0f;
    inp.seed = seed; // seme fisso anche per lui (di base sarebbe a caso)
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
    const long long promptMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    // Quanto può durare al massimo: circa 13 caratteri al secondo di parlato, con margine.
    const int maxFrames = std::clamp((int)(text.size() / 13.0 * kFramesPerSecond * 2.0) + 40, 60, 1500);
    size_t delivered = 0;
    auto deliver = [&]() -> bool {
        int32_t rate = 0;
        const char* data = nullptr;
        size_t len = 0;
        if (mtmd_helper_gen_audio_get_output(gen, &rate, &data, &len, nullptr) != 0) return false;
        if (len == 0) return true;
        delivered += len / sizeof(float);
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
        // Il decodificatore trasforma i codici in audio a blocchi di 72 fotogrammi (5,8 s). Chiedere
        // prima il primo pezzo (firstChunk) farebbe partire prima le frasi lunghe, ma misurato
        // (iris_tests --voce-cuciture) cambia l'audio e lascia un clic al confine: di base no.
        if (m_sampling.firstChunk > 0 && frames == m_sampling.firstChunk && mtmd_helper_gen_audio_flush(gen) != 0) return false;
        if (!deliver()) {
            cancelled = true;
            break;
        }
        hState = hNext;
        sampled = llama_sampler_sample(sampler, ctx, -1);
    }
    if (!cancelled) {
        if (mtmd_helper_gen_audio_flush(gen) != 0) return false;
        deliver();
    }
    if (framesOut) *framesOut = frames;
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    Log::info("Voce neurale: " + std::to_string(frames) + " fotogrammi (" + seconds1(frames / kFramesPerSecond) + " s di voce) in " +
              std::to_string(ms) + " ms (testo e timbro " + std::to_string(promptMs) + " ms)" + (cancelled ? ", interrotta" : "") +
              (frames >= maxFrames ? ", troppo lunga: tagliata" : ""));
    // Nessun suono (il modello ha chiuso subito): non è riuscita, si riprova con un altro seme.
    if (!cancelled && delivered == 0) return false;
    return true;
}

bool NeuralVoice::speakerEmbedding(const float* pcm, size_t count, int rate, std::vector<float>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out.clear();
    if (!m_ready || !pcm || count == 0) return false;
    auto* mtmd = static_cast<mtmd_context*>(m_mtmd);
    if (!mtmd_support_audio(mtmd)) return false;
    const int wanted = mtmd_get_audio_sample_rate(mtmd) > 0 ? mtmd_get_audio_sample_rate(mtmd) : m_sampleRate;
    const std::vector<float> s = VoiceMeasure::resample(pcm, count, rate, wanted);
    mtmd_bitmap* bitmap = mtmd_bitmap_init_from_audio(s.size(), s.data());
    if (!bitmap) return false;
    // Come encode_speaker in mtmd-helper-gen.cpp: il segnaposto dell'audio, poi il codificatore.
    const char* marker = mtmd_get_marker(mtmd);
    mtmd_input_text text{ marker, std::strlen(marker), false, true };
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    const mtmd_bitmap* bitmaps[1] = { bitmap };
    if (mtmd_tokenize(mtmd, chunks, &text, bitmaps, 1) == 0) {
        for (size_t i = 0; i < mtmd_input_chunks_size(chunks); ++i) {
            const mtmd_input_chunk* chunk = mtmd_input_chunks_get(chunks, i);
            if (mtmd_input_chunk_get_type(chunk) != MTMD_INPUT_CHUNK_TYPE_AUDIO) continue;
            if (mtmd_encode_chunk(mtmd, chunk) == 0) {
                const float* embd = mtmd_get_output_embd(mtmd);
                const size_t n = (size_t)llama_model_n_embd_inp(static_cast<llama_model*>(m_model)) * mtmd_input_chunk_get_n_tokens(chunk);
                out.assign(embd, embd + n);
            }
            break;
        }
    }
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bitmap);
    return !out.empty();
}

// --- File WAV -----------------------------------------------------------------------------

bool NeuralVoice::readWav(const std::wstring& path, std::vector<float>& mono, int& rate, std::string* error) {
    mono.clear();
    rate = 0;
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return fail("file non trovato");
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < (200u << 20)) data.insert(data.end(), buf, buf + n);
    fclose(f);
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) return fail("non è un file WAV");
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sampleRate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    for (size_t pos = 12; pos + 8 <= data.size();) {
        uint32_t size = 0;
        std::memcpy(&size, data.data() + pos + 4, 4);
        if (std::memcmp(data.data() + pos, "fmt ", 4) == 0 && pos + 24 <= data.size()) {
            std::memcpy(&format, data.data() + pos + 8, 2);
            std::memcpy(&channels, data.data() + pos + 10, 2);
            std::memcpy(&sampleRate, data.data() + pos + 12, 4);
            std::memcpy(&bits, data.data() + pos + 22, 2);
            if (format == 0xFFFE && size >= 40 && pos + 34 <= data.size()) std::memcpy(&format, data.data() + pos + 32, 2); // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(data.data() + pos, "data", 4) == 0) {
            pcm = data.data() + pos + 8;
            pcmBytes = std::min<size_t>(size, data.size() - pos - 8);
            break;
        }
        pos += 8 + (size_t)size + (size & 1);
    }
    if (!pcm || channels == 0 || sampleRate < 8000) return fail("formato del WAV non riconosciuto");
    const bool isFloat = format == 3 && bits == 32;
    if (!isFloat && !(format == 1 && (bits == 16 || bits == 24 || bits == 32))) return fail("serve un WAV PCM a 16, 24 o 32 bit");
    const size_t bytes = bits / 8, frames = pcmBytes / (bytes * channels);
    mono.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (size_t c = 0; c < channels; ++c) {
            const uint8_t* p = pcm + (i * channels + c) * bytes;
            double v = 0;
            if (isFloat) {
                float x;
                std::memcpy(&x, p, 4);
                v = x;
            } else if (bits == 16) {
                int16_t x;
                std::memcpy(&x, p, 2);
                v = x / 32768.0;
            } else if (bits == 24) {
                const int32_t x = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
                v = x / 8388608.0;
            } else {
                int32_t x;
                std::memcpy(&x, p, 4);
                v = x / 2147483648.0;
            }
            sum += v;
        }
        mono[i] = (float)(sum / channels);
    }
    rate = (int)sampleRate;
    return !mono.empty() || fail("il WAV è vuoto");
}

bool NeuralVoice::writeWav(const std::wstring& path, const std::vector<float>& mono, int rate) {
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") != 0 || !out) return false;
    const uint32_t r = (uint32_t)rate, dataBytes = (uint32_t)mono.size() * 2, riff = 36 + dataBytes, fmtSize = 16, byteRate = r * 2;
    const uint16_t fmtPcm = 1, channels = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, out);
    fwrite(&riff, 4, 1, out);
    fwrite("WAVEfmt ", 1, 8, out);
    fwrite(&fmtSize, 4, 1, out);
    fwrite(&fmtPcm, 2, 1, out);
    fwrite(&channels, 2, 1, out);
    fwrite(&r, 4, 1, out);
    fwrite(&byteRate, 4, 1, out);
    fwrite(&align, 2, 1, out);
    fwrite(&bits, 2, 1, out);
    fwrite("data", 1, 4, out);
    fwrite(&dataBytes, 4, 1, out);
    std::vector<int16_t> pcm(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) pcm[i] = (int16_t)std::clamp(mono[i] * 32767.0f, -32768.0f, 32767.0f);
    const bool ok = fwrite(pcm.data(), 2, pcm.size(), out) == pcm.size();
    return fclose(out) == 0 && ok;
}

bool NeuralVoice::cleanReference(std::vector<float>& s, int rate, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (rate <= 0 || s.empty()) return fail("niente audio");
    // Silenzi in testa e in coda: pezzi da 10 ms sotto 1/30 del picco.
    float peak = 0;
    for (float x : s) peak = std::max(peak, std::fabs(x));
    if (peak < 0.01f) return fail("è quasi muto");
    const size_t hop = (size_t)rate / 100;
    auto loud = [&](size_t at) {
        double e = 0;
        const size_t end = std::min(s.size(), at + hop);
        for (size_t i = at; i < end; ++i) e += (double)s[i] * s[i];
        return std::sqrt(e / std::max<size_t>(1, end - at)) > peak / 30.0;
    };
    size_t first = 0, last = s.size();
    while (first < s.size() && !loud(first)) first += hop;
    while (last > first + hop && !loud(last - hop)) last -= hop;
    const size_t keep = hop * 10; // 0,1 s di respiro ai lati
    first = first > keep ? first - keep : 0;
    last = std::min(s.size(), last + keep);
    if (last - first < (size_t)rate * 3) return fail("troppo corto: servono almeno 3 secondi di voce");
    last = std::min(last, first + (size_t)rate * 15);
    s = std::vector<float>(s.begin() + first, s.begin() + last);
    // Volume pareggiato: picco a -3 dB.
    peak = 0;
    for (float x : s) peak = std::max(peak, std::fabs(x));
    const float gain = 0.708f / peak;
    for (float& x : s) x *= gain;
    return true;
}

// --- Misure -------------------------------------------------------------------------------

namespace VoiceMeasure {

namespace {
    // FFT complessa radice 2, sul posto (n potenza di 2).
    void fft(std::vector<std::complex<double>>& a) {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const double ang = -2 * 3.14159265358979323846 / (double)len;
            const std::complex<double> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<double> w(1);
                for (size_t k = 0; k < len / 2; ++k) {
                    const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    // Volume (RMS) di ogni pezzo da 10 ms, e la soglia sotto cui è silenzio:
    // 20 dB sotto i pezzi più forti (il 95° percentile).
    std::vector<double> frameLevels(const float* s, size_t n, int rate, size_t window, double& threshold) {
        const size_t hop = (size_t)rate / 100;
        std::vector<double> levels;
        for (size_t start = 0; start + window <= n; start += hop) {
            double e = 0;
            for (size_t j = 0; j < window; ++j) e += (double)s[start + j] * s[start + j];
            levels.push_back(std::sqrt(e / window));
        }
        std::vector<double> sorted = levels;
        std::sort(sorted.begin(), sorted.end());
        const double loud = sorted.empty() ? 0.0 : sorted[(size_t)(sorted.size() * 0.95) < sorted.size() ? (size_t)(sorted.size() * 0.95) : sorted.size() - 1];
        threshold = std::max(loud * 0.1, 1e-4);
        return levels;
    }
}

std::vector<float> pitch(const float* s, size_t n, int rate) {
    std::vector<float> f0;
    if (!s || rate <= 0) return f0;
    const size_t hop = (size_t)rate / 100;
    const size_t window = (size_t)rate * 25 / 1000;
    const size_t minLag = (size_t)rate / 450, maxLag = (size_t)rate / 65;
    if (n < window + maxLag + 2) return f0;
    double threshold = 0;
    const std::vector<double> levels = frameLevels(s, n, rate, window + maxLag + 2, threshold);
    std::vector<double> d(maxLag + 2), cm(maxLag + 2);
    for (size_t frame = 0; frame < levels.size(); ++frame) {
        if (levels[frame] < threshold) {
            f0.push_back(0.0f);
            continue;
        }
        const float* x = s + frame * hop;
        double running = 0;
        cm[0] = 1;
        for (size_t tau = 1; tau <= maxLag + 1; ++tau) {
            double sum = 0;
            for (size_t j = 0; j < window; ++j) {
                const double diff = (double)x[j] - x[j + tau];
                sum += diff * diff;
            }
            d[tau] = sum;
            running += sum;
            cm[tau] = running > 0 ? sum * tau / running : 1.0;
        }
        size_t best = 0;
        for (size_t tau = minLag; tau <= maxLag; ++tau) {
            if (cm[tau] < 0.15) {
                while (tau + 1 <= maxLag && cm[tau + 1] < cm[tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best == 0) {
            size_t argmin = minLag;
            for (size_t tau = minLag; tau <= maxLag; ++tau) {
                if (cm[tau] < cm[argmin]) argmin = tau;
            }
            if (cm[argmin] < 0.3) best = argmin;
        }
        if (best == 0) {
            f0.push_back(0.0f);
            continue;
        }
        double tau = (double)best;
        if (best > 1 && best < maxLag) { // interpolazione parabolica intorno al minimo
            const double a = cm[best - 1], b = cm[best], c = cm[best + 1];
            const double den = a - 2 * b + c;
            if (std::fabs(den) > 1e-12) tau += 0.5 * (a - c) / den;
        }
        f0.push_back((float)(rate / tau));
    }
    return f0;
}

double centroid(const float* s, size_t n, int rate) {
    const size_t size = 1024;
    if (!s || n < size || rate <= 0) return 0.0;
    double threshold = 0;
    const std::vector<double> levels = frameLevels(s, n, rate, size, threshold);
    const size_t hop = (size_t)rate / 100;
    std::vector<float> values;
    std::vector<std::complex<double>> buf(size);
    for (size_t frame = 0; frame < levels.size(); ++frame) {
        if (levels[frame] < threshold) continue;
        for (size_t j = 0; j < size; ++j) {
            const double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * j / (size - 1));
            buf[j] = std::complex<double>(s[frame * hop + j] * w, 0.0);
        }
        fft(buf);
        double num = 0, den = 0;
        for (size_t k = 1; k < size / 2; ++k) {
            const double mag = std::abs(buf[k]);
            num += mag * k * rate / (double)size;
            den += mag;
        }
        if (den > 0) values.push_back((float)(num / den));
    }
    return median(values);
}

double median(std::vector<float> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](float v) { return !(v > 0.0f); }), values.end());
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t m = values.size() / 2;
    return values.size() % 2 ? values[m] : 0.5 * (values[m - 1] + values[m]);
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0;
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    return na > 0 && nb > 0 ? dot / std::sqrt(na * nb) : 0.0;
}

std::vector<float> resample(const float* s, size_t n, int fromRate, int toRate) {
    if (!s || n == 0) return {};
    if (fromRate == toRate || fromRate <= 0 || toRate <= 0) return std::vector<float>(s, s + n);
    const double ratio = (double)fromRate / toRate;
    std::vector<float> out((size_t)(n / ratio));
    for (size_t i = 0; i < out.size(); ++i) {
        const double src = i * ratio;
        const size_t a = (size_t)src;
        const float s0 = s[std::min(a, n - 1)], s1 = s[std::min(a + 1, n - 1)];
        out[i] = (float)(s0 + (s1 - s0) * (src - a));
    }
    return out;
}

} // namespace VoiceMeasure
