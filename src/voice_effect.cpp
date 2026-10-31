#include "voice_effect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Filtro del secondo ordine (formule "Audio EQ Cookbook" di R. Bristow-Johnson).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    float run(float x) {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
    void clear() { z1 = z2 = 0; }

    static Biquad make(double nb0, double nb1, double nb2, double a0, double na1, double na2) {
        Biquad q;
        q.b0 = nb0 / a0;
        q.b1 = nb1 / a0;
        q.b2 = nb2 / a0;
        q.a1 = na1 / a0;
        q.a2 = na2 / a0;
        return q;
    }
    static Biquad highpass(double f, double q, double sr) {
        const double w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        return make((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    static Biquad lowpass(double f, double q, double sr) {
        const double w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        return make((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
    static Biquad bandpass(double f, double q, double sr) { // guadagno 0 dB al centro
        const double w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        return make(al, 0, -al, 1 + al, -2 * c, 1 - al);
    }
    static Biquad peak(double f, double q, double gainDb, double sr) {
        const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        return make(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
};

// Linea di ritardo circolare con lettura frazionaria.
struct Delay {
    std::vector<float> buf;
    size_t pos = 0;

    void init(size_t n) {
        buf.assign(std::max<size_t>(n, 2), 0.0f);
        pos = 0;
    }
    void clear() {
        std::fill(buf.begin(), buf.end(), 0.0f);
        pos = 0;
    }
    // Il campione di "d" passi fa (d >= 1; 1 = l'ultimo spinto).
    float tap(double d) const {
        double rp = (double)pos - d;
        while (rp < 0) rp += (double)buf.size();
        const size_t i0 = (size_t)rp % buf.size(), i1 = (i0 + 1) % buf.size();
        const double fr = rp - std::floor(rp);
        return (float)(buf[i0] * (1 - fr) + buf[i1] * fr);
    }
    void push(float x) {
        buf[pos] = x;
        pos = (pos + 1) % buf.size();
    }
};

// Pettine con retroazione: la risonanza "metallica", da tubo.
struct Comb {
    Delay line;
    double delay = 1, feedback = 0, wet = 0;

    void init(double sr, double ms, double fb, double w) {
        delay = std::max(1.0, sr * ms / 1000.0);
        feedback = fb;
        wet = w;
        line.init((size_t)delay + 4);
    }
    float run(float x) {
        const float y = x + (float)feedback * line.tap(delay);
        line.push(y);
        return (float)(x * (1 - wet) + y * (1 - feedback) * wet);
    }
};

// Due copie leggermente ritardate e "ondeggianti": voce sdoppiata, sintetica.
struct Chorus {
    Delay line;
    double sr = 24000, base1 = 0, base2 = 0, depth = 0, rate1 = 0, rate2 = 0, wet = 0, ph1 = 0, ph2 = 1.7;

    void init(double s, double baseMs1, double baseMs2, double depthMs, double r1, double r2, double w) {
        sr = s;
        base1 = s * baseMs1 / 1000;
        base2 = s * baseMs2 / 1000;
        depth = s * depthMs / 1000;
        rate1 = r1;
        rate2 = r2;
        wet = w;
        line.init((size_t)(std::max(base1, base2) + depth) + 8);
        ph1 = 0;
        ph2 = 1.7;
    }
    float run(float x) {
        line.push(x);
        const float a = line.tap(base1 + depth * std::sin(ph1) + 1);
        const float b = line.tap(base2 + depth * std::sin(ph2) + 1);
        ph1 += 2 * kPi * rate1 / sr;
        ph2 += 2 * kPi * rate2 / sr;
        if (ph1 > 2 * kPi) ph1 -= 2 * kPi;
        if (ph2 > 2 * kPi) ph2 -= 2 * kPi;
        return (float)(x * (1 - wet) + (a + b) * 0.5 * wet);
    }
};

// Riverbero piccolo (stile Freeverb): la "stanza" della plancia.
struct Reverb {
    struct DampedComb {
        Delay line;
        double d = 1, fb = 0.75, damp = 0.3, filt = 0;
        float run(float x) {
            const float out = line.tap(d);
            filt = out * (1 - damp) + filt * damp;
            line.push((float)(x + filt * fb));
            return out;
        }
    };
    struct Allpass {
        Delay line;
        double d = 1, g = 0.5;
        float run(float x) {
            const float b = line.tap(d);
            line.push((float)(x + b * g));
            return -x + b;
        }
    };
    DampedComb combs[4];
    Allpass allpasses[2];
    double wet = 0;

    void init(double sr, double roomFeedback, double damping, double w) {
        static const int combLen[4] = { 1116, 1188, 1277, 1356 }; // a 44,1 kHz
        static const int apLen[2] = { 556, 441 };
        const double k = sr / 44100.0 * 0.8; // un po' più piccola: plancia, non cattedrale
        for (int i = 0; i < 4; ++i) {
            combs[i].d = combLen[i] * k;
            combs[i].fb = roomFeedback;
            combs[i].damp = damping;
            combs[i].filt = 0;
            combs[i].line.init((size_t)combs[i].d + 4);
        }
        for (int i = 0; i < 2; ++i) {
            allpasses[i].d = apLen[i] * k;
            allpasses[i].line.init((size_t)allpasses[i].d + 4);
        }
        wet = w;
    }
    float run(float x) {
        float s = 0;
        for (auto& c : combs) s += c.run(x * 0.15f);
        for (auto& a : allpasses) s = a.run(s);
        return (float)(x + s * wet);
    }
};

// Vocoder a canali: l'inviluppo della voce, banda per banda, applicato a un'onda a
// dente di sega a nota fissa (più un fruscio per le consonanti): il robot classico,
// monotono, da film di fantascienza.
struct Vocoder {
    struct Band {
        Biquad m1, m2, c1, c2;
        double env = 0;
        bool noisy = false;
    };
    std::vector<Band> bands;
    double sr = 24000, phase = 0, dt = 0, attack = 0, release = 0, dry = 0;
    bool subUp = false; // onda quadra un'ottava sotto: cambia segno a ogni giro del dente di sega
    uint32_t noiseState = 22222;
    Biquad dryHp;

    void init(double s, double pitchHz, int count, double dryMix) {
        sr = s;
        dt = pitchHz / s;
        phase = 0;
        dry = dryMix;
        attack = std::exp(-1.0 / (s * 0.003));
        release = std::exp(-1.0 / (s * 0.025));
        const double lo = 110, hi = std::min(8000.0, s * 0.42);
        const double r = std::pow(hi / lo, 1.0 / (count - 1));
        const double q = 1.0 / (std::sqrt(r) - 1.0 / std::sqrt(r)) * 0.85;
        bands.clear();
        for (int i = 0; i < count; ++i) {
            const double f = lo * std::pow(r, i);
            Band b;
            b.m1 = b.m2 = b.c1 = b.c2 = Biquad::bandpass(f, q, s);
            b.noisy = f > 2600;
            bands.push_back(b);
        }
        dryHp = Biquad::highpass(900, 0.7, s); // della voce vera restano solo le consonanti, per capire le parole
    }
    float saw() {
        double v = 2 * phase - 1;
        if (phase < dt) {
            const double x = phase / dt;
            v -= x + x - x * x - 1;
        } else if (phase > 1 - dt) {
            const double x = (phase - 1) / dt;
            v -= x * x + x + x + 1;
        }
        phase += dt;
        if (phase >= 1) {
            phase -= 1;
            subUp = !subUp;
        }
        return (float)v;
    }
    float noise() {
        noiseState = noiseState * 1664525u + 1013904223u;
        return (float)((noiseState >> 8) / 8388608.0 - 1.0);
    }
    float run(float x) {
        const float s = saw();
        const float sub = subUp ? 0.3f : -0.3f; // un'ottava sotto, appena accennata: più corpo
        const float n = noise();
        float out = 0;
        for (auto& b : bands) {
            const float mod = b.m2.run(b.m1.run(x));
            const double a = std::fabs(mod);
            b.env = a > b.env ? a + (b.env - a) * attack : a + (b.env - a) * release;
            const float carrier = b.noisy ? s * 0.45f + n * 0.9f : s + sub;
            out += b.c2.run(b.c1.run(carrier)) * (float)b.env;
        }
        return out * 6.0f + dryHp.run(x) * (float)dry;
    }
};

// Pareggia il volume con quello della voce originale (gli effetti lo cambiano).
// Si adatta solo mentre la voce c'è: nelle pause (e nella coda del riverbero)
// il guadagno resta fermo, così l'eco non viene schiacciata.
struct LevelMatch {
    double inE = 0, outE = 0, a = 0, g = 1, smooth = 0;
    void init(double sr) {
        a = std::exp(-1.0 / (sr * 0.15));
        smooth = 1.0 / (sr * 0.04);
        inE = outE = 0;
        g = 1;
    }
    float run(float in, float out) {
        inE = inE * a + (double)in * in * (1 - a);
        outE = outE * a + (double)out * out * (1 - a);
        if (inE > 1e-6 && outE > 1e-9) {
            const double target = std::clamp(std::sqrt(inE / outE), 0.2, 6.0);
            g += (target - g) * smooth;
        }
        return (float)(out * g);
    }
};

float softClip(float x) {
    const float t = 0.8f, ax = std::fabs(x);
    if (ax <= t) return x;
    const float y = t + (1 - t) * std::tanh((ax - t) / (1 - t));
    return x < 0 ? -y : y;
}

struct RingMod {
    double sr = 24000, freq = 0, depth = 0, ph = 0;
    float run(float x) {
        const double m = (1 - depth) + depth * std::sin(ph);
        ph += 2 * kPi * freq / sr;
        if (ph > 2 * kPi) ph -= 2 * kPi;
        return (float)(x * m);
    }
};
} // namespace

struct VoiceEffect::Impl {
    double sr = 24000;
    std::string id = "nessuno";
    bool on = false;
    bool useRing = false, useComb = false, useChorus = false, useVocoder = false, useReverb = false;
    Biquad hp, presence;
    RingMod ring;
    Comb comb;
    Chorus chorus;
    Vocoder vocoder;
    Reverb reverb;
    LevelMatch level;

    void configure(const std::string& name) {
        id = isValid(name) ? name : "nessuno";
        on = id != "nessuno";
        useRing = useComb = useChorus = useVocoder = useReverb = false;
        level.init(sr);
        presence = Biquad::peak(3200, 0.9, 3.0, sr);
        if (id == "ia") {
            // IA di bordo: la voce resta naturale, con una risonanza metallica, sdoppiata, in una plancia.
            hp = Biquad::highpass(130, 0.7, sr);
            comb.init(sr, 2.8, 0.40, 0.30);
            chorus.init(sr, 9, 13, 1.2, 0.35, 0.5, 0.25);
            reverb.init(sr, 0.74, 0.35, 0.9);
            useComb = useChorus = useReverb = true;
        } else if (id == "androide") {
            // Androide: ronzio elettrico (modulazione ad anello) e tubo metallico ben evidente.
            hp = Biquad::highpass(150, 0.7, sr);
            ring.sr = sr;
            ring.freq = 62;
            ring.depth = 0.45;
            ring.ph = 0;
            comb.init(sr, 5.0, 0.60, 0.50);
            chorus.init(sr, 8, 11, 1.0, 0.3, 0.45, 0.20);
            reverb.init(sr, 0.72, 0.40, 0.8);
            useRing = useComb = useChorus = useReverb = true;
        } else if (id == "robot") {
            // Robot: vocoder a nota fissa (monotono), un filo di voce vera per le consonanti.
            hp = Biquad::highpass(90, 0.7, sr);
            vocoder.init(sr, 140, 22, 0.35);
            comb.init(sr, 3.5, 0.30, 0.20);
            reverb.init(sr, 0.72, 0.40, 0.8);
            useVocoder = useComb = useReverb = true;
        }
    }

    float runOne(float x) {
        float y = hp.run(x);
        if (useRing) y = ring.run(y);
        if (useVocoder) y = vocoder.run(y);
        if (useComb) y = comb.run(y);
        if (useChorus) y = chorus.run(y);
        y = presence.run(y);
        if (useReverb) y = reverb.run(y);
        return softClip(level.run(x, y));
    }
};

const std::vector<VoiceEffect::PresetInfo>& VoiceEffect::presets() {
    static const std::vector<PresetInfo> list = {
        { "nessuno", "Nessuno (voce così com'è)" },
        { "ia", "IA di bordo (metallica, leggera)" },
        { "androide", "Androide (ronzio elettrico e metallo)" },
        { "robot", "Robot (vocoder monotono, da film)" },
    };
    return list;
}

bool VoiceEffect::isValid(const std::string& id) {
    for (const auto& p : presets()) {
        if (id == p.id) return true;
    }
    return false;
}

VoiceEffect::VoiceEffect(int sampleRate) : m(std::make_unique<Impl>()) {
    m->sr = sampleRate > 0 ? sampleRate : 24000;
    m->configure("nessuno");
}

VoiceEffect::~VoiceEffect() = default;

void VoiceEffect::setPreset(const std::string& id) {
    m->configure(id);
}

const std::string& VoiceEffect::preset() const {
    return m->id;
}

bool VoiceEffect::active() const {
    return m->on;
}

void VoiceEffect::reset() {
    m->configure(m->id);
}

void VoiceEffect::process(float* samples, size_t count) {
    if (!m->on || !samples) return;
    for (size_t i = 0; i < count; ++i) samples[i] = m->runOne(samples[i]);
}

std::vector<float> VoiceEffect::tail() {
    if (!m->on || !m->useReverb) return {};
    std::vector<float> out((size_t)(m->sr * 0.18), 0.0f);
    process(out.data(), out.size());
    // Sfuma in fondo: niente "clic" se poi non arriva altro.
    const size_t fade = out.size() / 3;
    for (size_t i = 0; i < fade; ++i) out[out.size() - fade + i] *= 1.0f - (float)i / fade;
    return out;
}
