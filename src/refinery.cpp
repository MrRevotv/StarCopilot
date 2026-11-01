#include "refinery.h"
#include "app_paths.h"
#include "text_utils.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace Refinery {

namespace {
    using json = nlohmann::json;

    std::wstring filePath() { return AppPaths::getAppDataDir() + L"\\raffineria.json"; }

    // Numeri a parole, quelli che servono per le durate dette a voce.
    int wordNumber(const std::string& w) {
        static const std::pair<const char*, int> kWords[] = {
            { "un", 1 }, { "uno", 1 }, { "una", 1 }, { "due", 2 }, { "tre", 3 }, { "quattro", 4 }, { "cinque", 5 },
            { "sei", 6 }, { "sette", 7 }, { "otto", 8 }, { "nove", 9 }, { "dieci", 10 }, { "undici", 11 }, { "dodici", 12 },
            { "tredici", 13 }, { "quattordici", 14 }, { "quindici", 15 }, { "sedici", 16 }, { "diciassette", 17 }, { "diciotto", 18 },
            { "diciannove", 19 }, { "venti", 20 }, { "trenta", 30 }, { "quaranta", 40 }, { "quarantacinque", 45 }, { "cinquanta", 50 },
            { "venticinque", 25 }, { "trentacinque", 35 }, { "quindici", 15 },
        };
        for (const auto& [k, v] : kWords) {
            if (w == k) return v;
        }
        return -1;
    }

    long long unitSeconds(const std::string& u) {
        if (u.empty()) return 0;
        if (u[0] == 'g' || u[0] == 'd') return 86400;                 // giorni, days
        if (u[0] == 'h' || TextUtils::startsWith(u, "or")) return 3600; // h, ore, ora
        if (u[0] == 'm') return 60;                                    // m, min, minuti
        if (u[0] == 's') return 1;                                     // s, sec, secondi
        return 0;
    }
}

std::optional<long long> parseDuration(const std::string& raw) {
    std::string t = TextUtils::toLower(TextUtils::trim(raw));
    if (t.empty()) return std::nullopt;
    // "3:20" (ore:minuti) o "1:02:30"
    if (t.find(':') != std::string::npos && t.find_first_not_of("0123456789: ") == std::string::npos) {
        std::vector<long long> parts;
        std::stringstream ss(t);
        std::string p;
        while (std::getline(ss, p, ':')) parts.push_back(p.empty() ? 0 : std::stoll(TextUtils::trim(p)));
        if (parts.size() == 2) return parts[0] * 3600 + parts[1] * 60;
        if (parts.size() == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
        return std::nullopt;
    }
    // Solo un numero: minuti.
    if (t.find_first_not_of("0123456789 ") == std::string::npos) {
        const long long m = std::stoll(t);
        return m > 0 ? std::optional<long long>(m * 60) : std::nullopt;
    }
    // Numeri e unità: "3h20", "3 ore e 20 minuti", "un'ora e mezza", "1g 2h".
    long long total = 0;
    long long pending = -1; // numero in attesa della sua unità
    bool any = false;
    size_t i = 0;
    auto flushAsMinutes = [&]() {
        if (pending > 0) total += pending * 60 / 1000;
        pending = -1;
    };
    while (i < t.size()) {
        const unsigned char c = (unsigned char)t[i];
        if (std::isdigit(c)) {
            size_t j = i;
            while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == ',' || t[j] == '.')) ++j;
            std::string num = t.substr(i, j - i);
            std::replace(num.begin(), num.end(), ',', '.');
            if (pending >= 0) flushAsMinutes();
            pending = (long long)(std::stod(num) * 1000); // millesimi, per "1,5 ore"
            i = j;
            continue;
        }
        if (std::isalpha(c) || c == '\'') {
            size_t j = i;
            while (j < t.size() && (std::isalpha((unsigned char)t[j]) || t[j] == '\'')) ++j;
            const std::string word = t.substr(i, j - i);
            i = j;
            if (word == "e" || word == "and" || word == "tra" || word == "fra" || word == "in") continue;
            if (word == "mezza" || word == "mezzo") { // "un'ora e mezza"
                total += 1800;
                any = true;
                continue;
            }
            const long long unit = unitSeconds(word);
            if (unit > 0 && pending >= 0) {
                total += pending * unit / 1000;
                pending = -1;
                any = true;
                continue;
            }
            if (unit > 0 && pending < 0 && (word == "ora" || word == "un'ora" || word == "giorno" || word == "minuto")) {
                total += unit; // "un'ora", "ora" da sola
                any = true;
                continue;
            }
            if (TextUtils::startsWith(word, "mezz'or")) { // "mezz'ora"
                total += 1800;
                any = true;
                continue;
            }
            const int n = wordNumber(word);
            if (n > 0) {
                if (pending >= 0) flushAsMinutes();
                pending = (long long)n * 1000;
                continue;
            }
            if (TextUtils::startsWith(word, "un'or")) {
                total += 3600;
                any = true;
            }
            continue;
        }
        ++i;
    }
    if (pending > 0) { // numero finale senza unità: "3 ore e 20" = minuti
        total += pending * 60 / 1000;
        any = true;
    }
    if (!any || total <= 0) return std::nullopt;
    return total;
}

std::string formatRemaining(long long s) {
    if (s <= 0) return "pronto";
    const long long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60, sec = s % 60;
    char buf[48];
    if (d > 0) snprintf(buf, sizeof(buf), "%lld g %lld h", d, h);
    else if (h > 0) snprintf(buf, sizeof(buf), "%lld h %02lld min", h, m);
    else if (m > 0) snprintf(buf, sizeof(buf), "%lld min %02lld s", m, sec);
    else snprintf(buf, sizeof(buf), "%lld s", sec);
    return buf;
}

std::string spokenDuration(long long s) {
    const long long h = s / 3600, m = (s % 3600 + 30) / 60;
    std::string out;
    if (h == 1) out = "un'ora";
    else if (h > 1) out = std::to_string(h) + " ore";
    if (m > 0 && m < 60) {
        if (!out.empty()) out += " e ";
        out += m == 1 ? "un minuto" : std::to_string(m) + " minuti";
    }
    if (out.empty()) out = "meno di un minuto";
    return out;
}

void Timers::load() {
    std::ifstream f(filePath(), std::ios::binary);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_jobs.clear();
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_array()) return;
    for (const auto& x : j) {
        if (!x.is_object()) continue;
        Job job;
        job.id = x.value("id", std::string());
        job.place = x.value("place", std::string());
        job.note = x.value("note", std::string());
        job.start = x.value("start", 0LL);
        job.end = x.value("end", 0LL);
        job.announced = x.value("announced", false);
        if (!job.id.empty() && job.end > 0) m_jobs.push_back(job);
    }
}

bool Timers::save() const {
    json j = json::array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& x : m_jobs) {
            j.push_back({ { "id", x.id }, { "place", x.place }, { "note", x.note }, { "start", x.start }, { "end", x.end }, { "announced", x.announced } });
        }
    }
    const std::wstring path = filePath(), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump(1);
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string Timers::add(const std::string& place, const std::string& note, long long seconds, long long now) {
    Job job;
    job.id = std::to_string(now) + "-" + std::to_string(GetTickCount64() % 100000);
    job.place = TextUtils::trim(place);
    job.note = TextUtils::trim(note);
    job.start = now;
    job.end = now + std::max(1LL, seconds);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.push_back(job);
        std::sort(m_jobs.begin(), m_jobs.end(), [](const Job& a, const Job& b) { return a.end < b.end; });
    }
    save();
    return job.id;
}

bool Timers::remove(const std::string& id) {
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const Job& j) { return j.id == id; });
        found = it != m_jobs.end();
        m_jobs.erase(it, m_jobs.end());
    }
    if (found) save();
    return found;
}

void Timers::clearDone(long long now) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const Job& j) { return j.done(now); }), m_jobs.end());
    }
    save();
}

std::vector<Job> Timers::jobs() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jobs;
}

std::vector<Job> Timers::takeFinished(long long now) {
    std::vector<Job> out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& j : m_jobs) {
            if (!j.announced && j.done(now)) {
                j.announced = true;
                out.push_back(j);
            }
        }
    }
    if (!out.empty()) save();
    return out;
}

} // namespace Refinery
