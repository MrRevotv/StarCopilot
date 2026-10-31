#include "speech_utils.h"
#include "text_utils.h"

#include <algorithm>
#include <cmath>

namespace SpeechUtils {

double rms(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0;
    double sum = 0.0;
    for (float s : samples) sum += (double)s * s;
    return std::sqrt(sum / samples.size());
}

namespace {
    double frameRms(const std::vector<float>& samples, size_t start) {
        size_t end = std::min(samples.size(), start + kFrameSamples);
        if (end <= start) return 0.0;
        double sum = 0.0;
        for (size_t i = start; i < end; ++i) sum += (double)samples[i] * samples[i];
        return std::sqrt(sum / (end - start));
    }
}

double voicedSeconds(const std::vector<float>& samples, double threshold) {
    size_t voiced = 0;
    for (size_t i = 0; i < samples.size(); i += kFrameSamples) {
        if (frameRms(samples, i) >= threshold) voiced++;
    }
    return voiced * (double)kFrameSamples / kSampleRate;
}

std::vector<float> trimSilence(const std::vector<float>& samples, double threshold, int marginMs) {
    if (samples.empty()) return samples;
    size_t first = samples.size(), last = 0;
    for (size_t i = 0; i < samples.size(); i += kFrameSamples) {
        if (frameRms(samples, i) >= threshold) {
            if (first == samples.size()) first = i;
            last = std::min(samples.size(), i + kFrameSamples);
        }
    }
    if (first == samples.size()) return {}; // solo silenzio
    size_t margin = (size_t)marginMs * kSampleRate / 1000;
    size_t start = first > margin ? first - margin : 0;
    size_t end = std::min(samples.size(), last + margin);
    return std::vector<float>(samples.begin() + start, samples.begin() + end);
}

bool isLikelyHallucination(const std::string& text) {
    std::string n = TextUtils::normalizeForMatch(text);
    if (n.empty()) return true;
    // Senza accenti: "comunità" deve combaciare con "comunita" dell'elenco.
    {
        std::wstring folded;
        for (wchar_t c : TextUtils::utf8ToWide(n)) {
            std::wstring plain = c < 128 ? std::wstring() : TextUtils::transliterate(c);
            folded += plain.empty() ? std::wstring(1, c) : plain;
        }
        n = TextUtils::wideToUtf8(folded);
    }
    // Frasi intere o pezzi tipici dei sottotitoli: se la trascrizione è solo
    // questo (o quasi), è inventata.
    static const char* kPhrases[] = {
        "sottotitoli creati dalla comunita amara org", "sottotitoli e revisione a cura di qtss",
        "sottotitoli a cura di qtss", "sottotitoli a cura di", "sottotitoli creati dalla comunita",
        "amara org", "qtss", "grazie per la visione", "grazie a tutti per la visione",
        "iscriviti al canale", "ciao a tutti e benvenuti", "thanks for watching", "thank you for watching",
        "please subscribe", "subtitles by the amara org community", "subtitles by",
        "untertitel der amara org community", "untertitel im auftrag des zdf",
        "sous titres realises par la communaute d amara org", "sous titrage st 501",
        "subtitulos realizados por la comunidad de amara org", "musica", "music", "applausi",
        "applause", "risate", "laughter",
    };
    for (const char* p : kPhrases) {
        std::string phrase = p;
        if (n == phrase) return true;
        // Quasi tutto il testo è la frase inventata (es. "Grazie per la visione!").
        if (n.find(phrase) != std::string::npos && phrase.size() * 10 >= n.size() * 7) return true;
    }
    // Una parola ripetuta all'infinito ("no no no no no no no no").
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < n.size()) {
        size_t sp = n.find(' ', pos);
        if (sp == std::string::npos) sp = n.size();
        words.push_back(n.substr(pos, sp - pos));
        pos = sp + 1;
    }
    if (words.size() >= 6) {
        std::vector<std::string> sorted = words;
        std::sort(sorted.begin(), sorted.end());
        size_t best = 1, run = 1;
        for (size_t i = 1; i < sorted.size(); ++i) {
            run = sorted[i] == sorted[i - 1] ? run + 1 : 1;
            best = std::max(best, run);
        }
        if (best * 10 >= words.size() * 7) return true;
    }
    return false;
}

std::string cleanTranscript(const std::string& text) {
    std::string out;
    int depthSquare = 0, depthRound = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '[') { depthSquare++; continue; }
        if (c == ']') { if (depthSquare > 0) depthSquare--; continue; }
        if (c == '(') { depthRound++; continue; }
        if (c == ')') { if (depthRound > 0) depthRound--; continue; }
        if (c == '*') continue;
        if (depthSquare > 0 || depthRound > 0) continue;
        // Note musicali (U+266A, U+266B in UTF-8: E2 99 AA / E2 99 AB).
        if ((unsigned char)c == 0xE2 && i + 2 < text.size() && (unsigned char)text[i + 1] == 0x99 &&
            ((unsigned char)text[i + 2] == 0xAA || (unsigned char)text[i + 2] == 0xAB)) {
            i += 2;
            continue;
        }
        out += c;
    }
    out = TextUtils::collapseSpaces(out);
    // Whisper a volte mette "..." o "-" iniziali.
    while (!out.empty() && (out.front() == '-' || out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    out = TextUtils::trim(out);
    // Ci deve essere almeno una lettera o cifra.
    std::string n = TextUtils::normalizeForMatch(out);
    if (n.empty()) return {};
    return out;
}

} // namespace SpeechUtils
