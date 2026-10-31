#include "llm_engine.h"
#include "gpu.h"
#include "log.h"
#include "text_utils.h"

#include "gguf.h"
#include "llama.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {
    constexpr int kContextTokens = 8192;
    constexpr int kBatch = 512;
    // Traduzioni (chat di gioco): due contesti piccoli, come in Voice to Chat.
    constexpr int kTranslateContextTokens = 2048;
    constexpr int kTranslateMicroBatch = 128;
    constexpr int kTranslateMaxNewTokens = 220; // una riga di chat lunga sta in 120-150 token
    // Memoria della scheda oltre agli strati del modello (calcoli, driver),
    // con un contesto da 8192 token e blocchi da 512, più i due delle traduzioni.
    constexpr long long kGpuWorkBytes = 1150LL << 20;

    void quietLog(ggml_log_level level, const char* text, void*) {
        if (level == GGML_LOG_LEVEL_ERROR && text) Log::error(std::string("llama: ") + text);
    }

    long long u32Key(const gguf_context* g, const std::string& key, long long fallback) {
        const int64_t id = gguf_find_key(g, key.c_str());
        if (id < 0 || gguf_get_kv_type(g, id) != GGUF_TYPE_UINT32) return fallback;
        return gguf_get_val_u32(g, id);
    }

    // Quanti strati del modello entrano in "budget" byte della scheda video
    // (stessa logica di Voice to Chat, con il contesto di Iris).
    int layersThatFit(const std::string& path, long long budget, int& totalLayers) {
        totalLayers = 0;
        gguf_init_params params{};
        params.no_alloc = true;
        params.ctx = nullptr;
        gguf_context* g = gguf_init_from_file(path.c_str(), params);
        if (!g) return 0;
        std::vector<long long> layers;
        long long output = 0;
        for (int64_t i = 0; i < gguf_get_n_tensors(g); ++i) {
            const char* name = gguf_get_tensor_name(g, i);
            const long long size = (long long)gguf_get_tensor_size(g, i);
            if (std::strncmp(name, "blk.", 4) == 0) {
                const int n = std::atoi(name + 4);
                if (n < 0 || n >= 1000) continue;
                if ((size_t)n >= layers.size()) layers.resize(n + 1, 0);
                layers[n] += size;
            } else if (std::strncmp(name, "output", 6) == 0) {
                output += size;
            }
        }
        long long kv = 32LL << 20;
        const int64_t archKey = gguf_find_key(g, "general.architecture");
        if (archKey >= 0 && gguf_get_kv_type(g, archKey) == GGUF_TYPE_STRING) {
            const std::string arch = gguf_get_val_str(g, archKey);
            const long long embd = u32Key(g, arch + ".embedding_length", 0);
            const long long heads = u32Key(g, arch + ".attention.head_count", 0);
            const long long kvHeads = u32Key(g, arch + ".attention.head_count_kv", heads);
            // Chiavi e valori (f16) di uno strato: il contesto di Iris e i due delle traduzioni.
            if (embd > 0 && heads > 0 && kvHeads > 0) kv = 2LL * (kContextTokens + 2 * kTranslateContextTokens) * (embd / heads * kvHeads) * 2;
        }
        gguf_free(g);

        totalLayers = (int)layers.size();
        long long left = budget - kGpuWorkBytes;
        int fit = 0;
        for (int i = totalLayers - 1; i >= 0 && left >= layers[i] + kv; --i) {
            left -= layers[i] + kv;
            fit++;
        }
        if (fit == totalLayers && left >= output) fit++;
        return fit;
    }

    std::vector<int> tokenize(const llama_vocab* vocab, const std::string& text, bool addSpecial) {
        int n = -llama_tokenize(vocab, text.c_str(), (int)text.size(), nullptr, 0, addSpecial, true);
        if (n <= 0) return {};
        std::vector<int> tokens(n);
        if (llama_tokenize(vocab, text.c_str(), (int)text.size(), tokens.data(), n, addSpecial, true) < 0) return {};
        return tokens;
    }
}

LlmEngine::~LlmEngine() {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();
}

void LlmEngine::release() {
    m_ready = false;
    if (m_greedy) llama_sampler_free(static_cast<llama_sampler*>(m_greedy));
    m_greedy = nullptr;
    for (Slot& s : m_slots) {
        if (s.ctx) llama_free(static_cast<llama_context*>(s.ctx));
        s = Slot{};
    }
    if (m_ctx) llama_free(static_cast<llama_context*>(m_ctx));
    if (m_model) llama_model_free(static_cast<llama_model*>(m_model));
    m_ctx = m_model = nullptr;
    m_cached.clear();
    m_gpuLayers = 0;
    m_gpuBytes = 0;
}

void LlmEngine::unload() {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();
}

bool LlmEngine::init(const std::string& modelPath, int threads, long long gpuBudget) {
    std::lock_guard<std::mutex> lock(m_mutex);
    release();

    static bool backendReady = false;
    if (!backendReady) {
        Gpu::prepare();
        llama_log_set(quietLog, nullptr);
        llama_backend_init();
        ggml_backend_load_all();
        backendReady = true;
    }

    void* gpu = Gpu::bestDevice();
    int layers = -1;
    if (gpu && gpuBudget >= 0) {
        layers = gpuBudget > 0 ? layersThatFit(modelPath, gpuBudget, m_totalLayers) : 0;
        if (layers <= 0) gpu = nullptr;
        else if (layers > m_totalLayers) layers = -1;
    }
    if (gpu && load(modelPath, threads, gpu, layers)) return true;
    if (gpu) Log::warn("LLM: non entra nella scheda video, uso il processore");
    return load(modelPath, threads, nullptr, 0);
}

bool LlmEngine::load(const std::string& modelPath, int threads, void* gpu, int gpuLayers) {
    llama_model_params mp = llama_model_default_params();
    ggml_backend_dev_t devices[2] = { static_cast<ggml_backend_dev_t>(gpu), nullptr };
    mp.devices = devices;
    mp.n_gpu_layers = !gpu ? 0 : gpuLayers < 0 ? 999 : gpuLayers;
    const long long usedBefore = gpu ? Gpu::processMemory() : -1;
    llama_model* model = llama_model_load_from_file(modelPath.c_str(), mp);
    if (!model) {
        Log::error("LLM: impossibile caricare il modello " + modelPath);
        return false;
    }
    m_model = model;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = kContextTokens;
    cp.n_batch = kBatch;
    cp.n_ubatch = kBatch;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = true;
    cp.op_offload = false;
    llama_context* ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        Log::error("LLM: impossibile creare il contesto");
        release();
        return false;
    }
    m_ctx = ctx;
    m_nCtx = (int)llama_n_ctx(ctx);
    m_nBatch = (int)cp.n_batch;
    m_cached.clear();

    // I due canali delle traduzioni (voce, chat letta): contesti piccoli sullo stesso modello.
    llama_context_params tp = cp;
    tp.n_ctx = kTranslateContextTokens;
    tp.n_ubatch = kTranslateMicroBatch;
    for (Slot& s : m_slots) {
        llama_context* tctx = llama_init_from_model(model, tp);
        if (!tctx) {
            Log::error("LLM: impossibile creare il contesto delle traduzioni");
            release();
            return false;
        }
        s.ctx = tctx;
    }
    llama_sampler_chain_params gp = llama_sampler_chain_default_params();
    gp.no_perf = true;
    llama_sampler* greedy = llama_sampler_chain_init(gp);
    // Greedy: per tradurre non serve fantasia, e la stessa frase dà sempre la stessa traduzione.
    llama_sampler_chain_add(greedy, llama_sampler_init_greedy());
    m_greedy = greedy;

    char name[256] = {};
    if (llama_model_meta_val_str(model, "general.name", name, sizeof(name)) > 0) m_modelName = name;
    else m_modelName = modelPath.substr(modelPath.find_last_of("\\/") + 1);
    char arch[64] = {};
    llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch));
    m_qwen3 = TextUtils::startsWith(arch, "qwen3");

    // La prima risposta sulla scheda video è lenta (prepara i suoi programmi):
    // se ne fa una a vuoto adesso.
    if (gpu) {
        const llama_vocab* vocab = llama_model_get_vocab(model);
        std::vector<int> tokens = tokenize(vocab, "<|im_start|>user\nciao<|im_end|>\n<|im_start|>assistant\n", true);
        if (!tokens.empty() && evalTokens(tokens, nullptr)) {
            std::vector<int> one = { tokens.back() };
            evalTokens(one, nullptr);
        }
        llama_memory_clear(llama_get_memory(ctx), true);
        // E una traduzione a vuoto (blocchi più piccoli: altri programmi della scheda).
        TranslationRequest warm;
        warm.text = "ok";
        warm.sourceLanguage = "English";
        warm.targetLanguage = "Italian";
        auto* tctx = static_cast<llama_context*>(m_slots[0].ctx);
        std::vector<int> wt = tokenize(vocab, TranslationPrompt::prefix(warm) + TranslationPrompt::request(warm), true);
        if (!wt.empty() && (int)wt.size() < kTranslateContextTokens && evalTokensIn(tctx, wt, nullptr)) {
            std::vector<int> one = { wt.back() };
            evalTokensIn(tctx, one, nullptr);
        }
        llama_memory_clear(llama_get_memory(tctx), true);
    }

    m_totalLayers = llama_model_n_layer(model);
    m_gpuLayers = !gpu ? 0 : gpuLayers < 0 ? m_totalLayers : gpuLayers;
    m_gpuBytes = gpu && usedBefore >= 0 ? std::max(0LL, Gpu::processMemory() - usedBefore) : 0;
    m_ready = true;
    Log::info("LLM pronto: " + m_modelName + " (" +
              (gpu ? "scheda video " + Gpu::bestDeviceName() + ", " + std::to_string(m_gpuLayers) + " strati su " +
                         std::to_string(m_totalLayers) + ", " + std::to_string(m_gpuBytes >> 20) + " MB"
                   : std::to_string(threads) + " thread") +
              ", contesto " + std::to_string(m_nCtx) + ")");
    return true;
}

bool LlmEngine::evalTokens(const std::vector<int>& tokens, const std::atomic<bool>* cancel) {
    return evalTokensIn(m_ctx, tokens, cancel);
}

bool LlmEngine::evalTokensIn(void* ctxPtr, const std::vector<int>& tokens, const std::atomic<bool>* cancel) {
    auto* ctx = static_cast<llama_context*>(ctxPtr);
    for (size_t i = 0; i < tokens.size(); i += (size_t)m_nBatch) {
        if (cancel && cancel->load()) return false;
        int n = (int)std::min<size_t>((size_t)m_nBatch, tokens.size() - i);
        llama_batch batch = llama_batch_get_one(const_cast<int*>(tokens.data() + i), n);
        if (llama_decode(ctx, batch) != 0) return false;
    }
    return true;
}

int LlmEngine::countTokens(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return (int)text.size() / 3;
    return (int)tokenize(llama_model_get_vocab(static_cast<llama_model*>(m_model)), text, true).size();
}

LlmEngine::Result LlmEngine::generate(const std::string& prompt, const Sampling& sampling,
                                      const std::function<bool(const std::string&)>& onPiece, std::string& out,
                                      const std::atomic<bool>* cancel) {
    out.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return Result::Error;
    auto* model = static_cast<llama_model*>(m_model);
    auto* ctx = static_cast<llama_context*>(m_ctx);
    const llama_vocab* vocab = llama_model_get_vocab(model);
    llama_memory_t mem = llama_get_memory(ctx);

    std::vector<int> tokens = tokenize(vocab, prompt, true);
    if (tokens.empty()) return Result::Error;
    const int maxNew = std::min(sampling.maxTokens, m_nCtx - (int)tokens.size() - 4);
    if (maxNew < 32) {
        Log::warn("LLM: conversazione troppo lunga (" + std::to_string(tokens.size()) + " token)");
        return Result::TooLong;
    }

    // 1) Quello che è già nella memoria del modello (stesso inizio) non si rifà.
    size_t common = 0;
    while (common < m_cached.size() && common < tokens.size() && m_cached[common] == tokens[common]) ++common;
    if (common == tokens.size()) common--; // almeno un token da elaborare, per avere la prossima parola
    if (common < m_cached.size() && !llama_memory_seq_rm(mem, 0, (llama_pos)common, -1)) {
        llama_memory_clear(mem, true);
        common = 0;
    }
    m_cached.resize(common);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<int> fresh(tokens.begin() + common, tokens.end());
    if (!evalTokens(fresh, cancel)) {
        // Interrotto a metà: la memoria non corrisponde più a m_cached.
        llama_memory_clear(mem, true);
        m_cached.clear();
        return cancel && cancel->load() ? Result::Cancelled : Result::Error;
    }
    m_cached.insert(m_cached.end(), fresh.begin(), fresh.end());
    const auto t1 = std::chrono::steady_clock::now();

    // 2) La risposta, una parola alla volta.
    llama_sampler_chain_params sp = llama_sampler_chain_default_params();
    sp.no_perf = true;
    llama_sampler* sampler = llama_sampler_chain_init(sp);
    llama_sampler_chain_add(sampler, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, sampling.repeatPenalty, 0.0f, 0.0f));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(sampling.topK));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(sampling.topP, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(sampling.temperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist((uint32_t)std::random_device{}()));
    struct FreeSampler {
        llama_sampler* s;
        ~FreeSampler() { llama_sampler_free(s); }
    } freeSampler{ sampler };

    Result result = Result::Ok;
    int generated = 0;
    std::string pending; // byte di un carattere UTF-8 non ancora completo
    for (; generated < maxNew; ++generated) {
        if (cancel && cancel->load()) { result = Result::Cancelled; break; }
        llama_token token = llama_sampler_sample(sampler, ctx, -1);
        if (llama_vocab_is_eog(vocab, token)) break;

        char piece[256];
        int n = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
        if (n > 0) {
            pending.append(piece, n);
            // Si passa avanti solo fino all'ultimo carattere completo.
            size_t complete = pending.size();
            for (size_t back = 1; back <= 3 && back <= pending.size(); ++back) {
                unsigned char c = (unsigned char)pending[pending.size() - back];
                if ((c & 0xC0) == 0x80) continue;              // byte di continuazione
                size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
                if (len > back) complete = pending.size() - back; // l'ultimo carattere non è finito
                break;
            }
            if (complete > 0) {
                std::string text = pending.substr(0, complete);
                pending.erase(0, complete);
                out += text;
                if (onPiece && !onPiece(text)) {
                    // Il token si mette comunque in memoria: la prossima volta il prompt combacia.
                    llama_batch batch = llama_batch_get_one(&token, 1);
                    if (llama_decode(ctx, batch) == 0) m_cached.push_back(token);
                    break;
                }
            }
        }

        llama_batch batch = llama_batch_get_one(&token, 1);
        if (llama_decode(ctx, batch) != 0) {
            llama_memory_clear(mem, true);
            m_cached.clear();
            return Result::Error;
        }
        m_cached.push_back(token);
    }
    if (!pending.empty()) out += pending;

    const auto t2 = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
    Log::info("LLM: " + std::to_string(fresh.size()) + " token nuovi su " + std::to_string(tokens.size()) + " in " + std::to_string(ms(t0, t1)) +
              " ms, risposta " + std::to_string(generated) + " token in " + std::to_string(ms(t1, t2)) + " ms");
    return result;
}

// --- Traduzioni (chat di gioco, da Voice to Chat) ---------------------------------------

LlmEngine::Result LlmEngine::translate(const TranslationRequest& request, std::string& out, Channel channel,
                                       const std::atomic<bool>* cancel) {
    out = request.text;
    if (request.text.empty()) return Result::Ok;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ready) return Result::Error;
    if (cancel && cancel->load()) return Result::Cancelled;

    Slot& slot = m_slots[(int)channel];
    auto* model = static_cast<llama_model*>(m_model);
    auto* ctx = static_cast<llama_context*>(slot.ctx);
    auto* sampler = static_cast<llama_sampler*>(m_greedy);
    const llama_vocab* vocab = llama_model_get_vocab(model);
    llama_memory_t mem = llama_get_memory(ctx);

    auto forgetPrefix = [&]() {
        llama_memory_clear(mem, true);
        slot.cachedPrefix.clear();
        slot.prefixTokens = 0;
    };

    // 1) Prefisso: se è lo stesso dell'ultima volta è già in memoria, basta
    //    togliere la frase precedente; se no (lingue/tono cambiati) si rifà.
    const std::string prefix = TranslationPrompt::prefix(request);
    if (prefix != slot.cachedPrefix || slot.prefixTokens == 0) {
        forgetPrefix();
        std::vector<int> prefixTokens = tokenize(vocab, prefix, true);
        if (prefixTokens.empty() || (int)prefixTokens.size() > kTranslateContextTokens / 2) {
            Log::error("LLM: prefisso della traduzione non valido");
            return Result::Error;
        }
        if (!evalTokensIn(ctx, prefixTokens, cancel)) {
            forgetPrefix();
            if (cancel && cancel->load()) return Result::Cancelled;
            Log::error("LLM: prefisso della traduzione non elaborato");
            return Result::Error;
        }
        slot.cachedPrefix = prefix;
        slot.prefixTokens = (int)prefixTokens.size();
    } else if (!llama_memory_seq_rm(mem, 0, slot.prefixTokens, -1)) {
        forgetPrefix();
        return Result::Error;
    }

    // 2) La frase da tradurre.
    std::vector<int> requestTokens = tokenize(vocab, TranslationPrompt::request(request), false);
    if (requestTokens.empty() || slot.prefixTokens + (int)requestTokens.size() + kTranslateMaxNewTokens > kTranslateContextTokens) {
        Log::warn("LLM: frase troppo lunga per la traduzione (" + std::to_string(requestTokens.size()) + " token)");
        return Result::TooLong;
    }
    if (!evalTokensIn(ctx, requestTokens, cancel)) {
        if (cancel && cancel->load()) return Result::Cancelled;
        forgetPrefix();
        return Result::Error;
    }

    // 3) La traduzione, una parola alla volta, fino a fine risposta o a capo.
    llama_sampler_reset(sampler);
    std::string result;
    std::vector<int> generated;
    for (int i = 0; i < kTranslateMaxNewTokens; ++i) {
        if (cancel && cancel->load()) return Result::Cancelled;
        llama_token token = llama_sampler_sample(sampler, ctx, -1);
        if (llama_vocab_is_eog(vocab, token)) break;

        char piece[256];
        int n = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, false);
        if (n > 0) result.append(piece, n);
        // Una riga sola: al primo a capo dopo del testo, la traduzione è finita.
        if (result.find('\n') != std::string::npos && result.find_first_not_of(" \n\r\t") < result.find('\n')) break;

        // Se il modello si incastra a ripetere sempre lo stesso pezzo, basta.
        generated.push_back(token);
        if (generated.size() >= 24) {
            bool loop = true;
            for (size_t k = 0; k < 12; ++k) {
                if (generated[generated.size() - 1 - k] != generated[generated.size() - 13 - k]) {
                    loop = false;
                    break;
                }
            }
            if (loop) break;
        }

        llama_token next = token;
        llama_batch batch = llama_batch_get_one(&next, 1);
        if (llama_decode(ctx, batch) != 0) {
            forgetPrefix();
            return Result::Error;
        }
    }

    out = TranslationPrompt::cleanOutput(result, request.text);
    return Result::Ok;
}
