#include "mining_log.h"
#include "app_paths.h"
#include "log.h"
#include "text_utils.h"

#include <windows.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace MiningLog {

namespace {
    using json = nlohmann::json;
    using ojson = nlohmann::ordered_json; // le chiavi restano nell'ordine scritto: il file si legge anche a occhio

    std::wstring statePath() { return AppPaths::getAppDataDir() + L"\\registro_mining.json"; }
    constexpr long long kSameRockSeconds = 45; // la stessa signature riletta entro 45 s è la stessa roccia
    constexpr size_t kMaxEntries = 5000;

    tm localTm(long long epoch) {
        tm t{};
        const time_t v = (time_t)epoch;
        localtime_s(&t, &v);
        return t;
    }

    json entryJson(const Entry& e) {
        json alts = json::array();
        for (const auto& a : e.alternatives) alts.push_back({ { "material", a.material }, { "label", a.label }, { "kind", a.kind }, { "quantity", a.quantity } });
        return { { "epoch", e.epoch },   { "value", e.value }, { "outcome", e.outcome },   { "material", e.material }, { "label", e.label },
                 { "kind", e.kind },     { "rarity", e.rarity }, { "quantity", e.quantity }, { "alternatives", alts },
                 { "source", e.source }, { "reads", e.reads }, { "last", e.last } };
    }

    Entry entryFrom(const json& j) {
        Entry e;
        e.epoch = j.value("epoch", 0LL);
        e.value = j.value("value", 0);
        e.outcome = j.value("outcome", std::string());
        e.material = j.value("material", std::string());
        e.label = j.value("label", std::string());
        e.kind = j.value("kind", std::string());
        e.rarity = j.value("rarity", std::string());
        e.quantity = j.value("quantity", 0);
        e.source = j.value("source", std::string());
        e.reads = std::max(1, j.value("reads", 1));
        e.last = std::max(e.epoch, j.value("last", 0LL)); // i registri di prima non l'avevano
        if (j.contains("alternatives") && j["alternatives"].is_array()) {
            for (const auto& a : j["alternatives"]) {
                if (a.is_object()) e.alternatives.push_back({ a.value("material", std::string()), a.value("label", std::string()), a.value("kind", std::string()), a.value("quantity", 0) });
            }
        }
        return e;
    }

    int outcomeRank(const std::string& o) { return o == "certo" ? 3 : o == "probabile" ? 2 : o == "ambiguo" ? 1 : 0; }

    // Quello che un rilevamento può essere ("Copper (Rame)|4"); vuoto = sconosciuto.
    std::vector<std::string> options(const Entry& e) {
        std::vector<std::string> out;
        if (e.outcome == "certo" || e.outcome == "probabile") out.push_back(e.label + "|" + std::to_string(e.quantity));
        for (const auto& a : e.alternatives) out.push_back(a.label + "|" + std::to_string(a.quantity));
        return out;
    }

    // Due letture vicine possono essere la stessa roccia solo se hanno almeno una possibilità in
    // comune (una sconosciuta va bene con tutto): Copper × 4 (16.960) e Iron × 4 (17.080) sono
    // due rocce diverse anche se i numeri distano meno del 2%.
    bool compatible(const Entry& a, const Entry& b) {
        const auto x = options(a), y = options(b);
        if (x.empty() || y.empty()) return true;
        return std::any_of(x.begin(), x.end(), [&](const std::string& o) { return std::find(y.begin(), y.end(), o) != y.end(); });
    }

    std::string csvField(std::string s) {
        // Un testo che comincia con = + - @ Excel lo eseguirebbe come formula: si scrive come testo.
        if (!s.empty() && (s[0] == '=' || s[0] == '+' || s[0] == '-' || s[0] == '@')) s = "'" + s;
        if (s.find_first_of(";\"\r\n") == std::string::npos) return s;
        std::string out = "\"";
        for (char c : s) out += c == '"' ? "\"\"" : std::string(1, c);
        return out + "\"";
    }

    // "2026-11-01 21:40:17", ora locale: Excel (anche in italiano) la riconosce come data e ora.
    std::string excelLocal(long long epoch) {
        const std::string iso = isoLocal(epoch); // "2026-11-01T21:40:17+01:00"
        return iso.substr(0, 10) + " " + iso.substr(11, 8);
    }

    bool writeAll(const std::wstring& path, const std::string& data) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        return (bool)f;
    }

    bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
}

std::string gameName(const std::string& label) {
    const size_t p = label.find(" (");
    return TextUtils::trim(p == std::string::npos ? label : label.substr(0, p));
}

Summary summarize(const std::vector<Entry>& entries) {
    Summary s;
    std::map<std::string, MaterialTotal> totals;
    for (const auto& e : entries) {
        ++s.finds;
        s.reads += e.reads;
        if (e.outcome == "certo" || e.outcome == "probabile") {
            ++s.sure;
            MaterialTotal& t = totals[e.label];
            t.material = e.material;
            t.label = e.label;
            t.kind = e.kind;
            t.rarity = e.rarity;
            ++t.rocks;
            t.quantity += e.quantity;
        } else {
            ++s.toCheck;
        }
    }
    for (auto& [k, t] : totals) s.materials.push_back(t);
    std::sort(s.materials.begin(), s.materials.end(), [](const MaterialTotal& a, const MaterialTotal& b) {
        return a.quantity != b.quantity ? a.quantity > b.quantity : a.label < b.label;
    });
    return s;
}

std::string isoLocal(long long epoch) {
    const tm t = localTm(epoch);
    tm copy = t;
    const long long offset = (long long)_mkgmtime(&copy) - epoch; // secondi avanti rispetto a UTC (ora legale compresa)
    const long long mins = std::llabs(offset) / 60;
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02lld:%02lld", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
             offset < 0 ? '-' : '+', mins / 60, mins % 60);
    return buf;
}

std::string fileStem(long long epoch) {
    const tm t = localTm(epoch);
    char buf[48];
    snprintf(buf, sizeof(buf), "Iris mining %04d-%02d-%02d %02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    return buf;
}

std::string toJson(const std::vector<Entry>& entries, const Closing& c) {
    const Summary s = summarize(entries);
    ojson materials = ojson::array();
    for (const auto& m : s.materials) {
        ojson x;
        x["materiale"] = m.material;
        x["nome"] = m.label;
        x["tipo"] = m.kind;
        if (!m.rarity.empty()) x["rarita"] = m.rarity;
        x["rocce"] = m.rocks;
        x["quantita"] = m.quantity;
        materials.push_back(x);
    }
    ojson finds = ojson::array();
    for (const auto& e : entries) {
        ojson x;
        x["ora"] = isoLocal(e.epoch);
        x["segnale"] = e.value;
        x["esito"] = e.outcome;
        if (e.outcome == "certo" || e.outcome == "probabile") {
            x["materiale"] = e.material;
            x["nome"] = e.label;
            x["quantita"] = e.quantity;
            x["tipo"] = e.kind;
            if (!e.rarity.empty()) x["rarita"] = e.rarity;
        }
        if (!e.alternatives.empty()) {
            ojson alts = ojson::array();
            for (const auto& a : e.alternatives) {
                ojson y;
                y["materiale"] = a.material;
                y["nome"] = a.label;
                y["quantita"] = a.quantity;
                y["tipo"] = a.kind;
                alts.push_back(y);
            }
            x["alternative"] = alts;
        }
        x["origine"] = e.source;
        x["letture"] = e.reads;
        finds.push_back(x);
    }
    ojson j;
    j["formato"] = "iris-registro-mining";
    j["versione"] = 1;
    j["programma"] = c.program;
    j["operatore"] = c.operatorName;
    j["luogo"] = c.place;
    if (c.quality >= 0) j["qualitaMedia"] = c.quality;
    else j["qualitaMedia"] = nullptr;
    j["inizio"] = isoLocal(c.start);
    j["fine"] = isoLocal(c.end);
    j["durataMinuti"] = (int)std::lround(std::max(0LL, c.end - c.start) / 60.0);
    ojson summary;
    summary["rilevamenti"] = s.finds;
    summary["letture"] = s.reads;
    summary["certi"] = s.sure;
    summary["daVerificare"] = s.toCheck;
    summary["materiali"] = materials;
    j["riepilogo"] = summary;
    j["rilevamenti"] = finds;
    return j.dump(2, ' ', false, ojson::error_handler_t::replace) + "\n";
}

std::string toCsv(const std::vector<Entry>& entries, const Closing& c) {
    std::ostringstream o;
    o << "\xEF\xBB\xBF"; // BOM: Excel capisce che è UTF-8 (accenti giusti)
    o << "Luogo;" << csvField(c.place) << "\r\n";
    o << "Qualità media;" << (c.quality >= 0 ? std::to_string(c.quality) : "") << "\r\n";
    o << "Operatore;" << csvField(c.operatorName) << "\r\n";
    o << "Inizio;" << excelLocal(c.start) << "\r\n";
    o << "Fine;" << excelLocal(c.end) << "\r\n";
    o << "\r\n";
    o << "Ora;Segnale;Esito;Materiale;Nome;Quantità;Tipo;Rarità;Alternative;Origine;Letture\r\n";
    for (const auto& e : entries) {
        std::string alts;
        for (const auto& a : e.alternatives) alts += (alts.empty() ? "" : ", ") + a.label + " x" + std::to_string(a.quantity);
        const bool sure = e.outcome == "certo" || e.outcome == "probabile";
        o << excelLocal(e.epoch) << ";" << e.value << ";" << e.outcome << ";" << csvField(sure ? e.material : "") << ";"
          << csvField(sure ? e.label : "") << ";" << (sure ? std::to_string(e.quantity) : "") << ";" << (sure ? e.kind : "") << ";"
          << (sure ? e.rarity : "") << ";" << csvField(alts) << ";" << csvField(e.source) << ";" << e.reads << "\r\n";
    }
    o << "\r\nMateriale;Nome;Tipo;Rocce;Quantità\r\n";
    for (const auto& m : summarize(entries).materials) {
        o << csvField(m.material) << ";" << csvField(m.label) << ";" << m.kind << ";" << m.rocks << ";" << m.quantity << "\r\n";
    }
    return o.str();
}

std::wstring desktopFolder() {
    // Con una cartella dati di prova (--cartella-dati) i file vanno lì: le prove non toccano il desktop vero.
    if (AppPaths::dataDirOverridden()) return AppPaths::getAppDataDir();
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path)) && path) out = path;
    if (path) CoTaskMemFree(path);
    return out;
}

// --- Il registro ------------------------------------------------------------------------------------

void Recorder::load() {
    std::ifstream f(statePath(), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_active = j.value("active", false);
    m_start = j.value("start", 0LL);
    m_lastFile = TextUtils::utf8ToWide(j.value("lastFile", std::string()));
    m_entries.clear();
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& e : j["entries"]) {
            if (e.is_object()) m_entries.push_back(entryFrom(e));
        }
    }
    m_places.clear();
    if (j.contains("places") && j["places"].is_array()) {
        for (const auto& p : j["places"]) {
            if (p.is_string()) m_places.push_back(p.get<std::string>());
        }
    }
    if (m_active) Log::info("Registro del mining: riprendo quello in corso (" + std::to_string(m_entries.size()) + " rilevamenti)");
}

void Recorder::save() const {
    json j;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        j["active"] = m_active;
        j["start"] = m_start;
        j["lastFile"] = TextUtils::wideToUtf8(m_lastFile);
        json list = json::array();
        for (const auto& e : m_entries) list.push_back(entryJson(e));
        j["entries"] = list;
        j["places"] = m_places;
    }
    const std::wstring path = statePath(), tmp = path + L".tmp";
    // Un nome della tabella con caratteri non UTF-8 non deve far cadere Iris: si sostituiscono.
    if (writeAll(tmp, j.dump(1, ' ', false, json::error_handler_t::replace))) MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Recorder::active() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_active;
}

long long Recorder::startedAt() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_start;
}

std::vector<Entry> Recorder::entries() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

std::vector<std::string> Recorder::recentPlaces() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_places;
}

std::wstring Recorder::lastFile() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastFile;
}

void Recorder::start(long long now) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active) return;
        m_active = true;
        m_start = now;
        m_entries.clear();
    }
    save();
    Log::info("Registro del mining: iniziato");
}

void Recorder::cancel() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_entries.clear();
    }
    save();
    Log::info("Registro del mining: annullato");
}

bool Recorder::add(const Entry& e) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_active) return false;
        // La stessa roccia riletta entro 45 s dalla sua ultima lettura (anche con un'altra roccia in
        // mezzo: si passa dall'una all'altra) è una lettura in più, non un rilevamento nuovo. Stessa
        // signature, o vicina (2%, una cifra letta male) ma con lo stesso materiale possibile.
        // Se adesso la lettura è più sicura, si tiene questa.
        // Tra le ultime, quella con la signature più vicina (a pari distanza, la più recente).
        Entry* same = nullptr;
        for (size_t back = 0; back < m_entries.size() && back < 8; ++back) {
            Entry& x = m_entries[m_entries.size() - 1 - back];
            const long long seen = std::max(x.epoch, x.last);
            const int tolerance = std::max(2, (int)std::lround(std::abs(x.value) * 0.02));
            const int diff = std::abs(e.value - x.value);
            if (e.epoch < seen - 1 || e.epoch - seen > kSameRockSeconds || diff > tolerance || !compatible(e, x)) continue;
            if (!same || diff < std::abs(e.value - same->value) ||
                (diff == std::abs(e.value - same->value) && seen > std::max(same->epoch, same->last))) {
                same = &x;
            }
        }
        if (same) {
            const int reads = same->reads + 1;
            const long long first = same->epoch;
            if (outcomeRank(e.outcome) > outcomeRank(same->outcome)) *same = e;
            same->reads = reads;
            same->epoch = first;
            same->last = e.epoch;
        } else {
            if (m_entries.size() >= kMaxEntries) return false;
            m_entries.push_back(e);
            m_entries.back().last = e.epoch;
        }
    }
    save();
    return true;
}

bool Recorder::finish(const Closing& closing, const std::wstring& folder, bool csv, std::wstring& jsonPath, std::string& error) {
    std::vector<Entry> entries;
    long long start = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_active) {
            error = "Il registro non è attivo.";
            return false;
        }
        entries = m_entries;
        start = m_start;
    }
    if (folder.empty()) {
        error = "Non trovo la cartella del desktop.";
        return false;
    }
    Closing c = closing;
    if (c.start == 0) c.start = start;
    // Un nome libero: "Iris mining 2026-11-01 21-40.json", poi " (2)"...
    const std::wstring stem = folder + L"\\" + TextUtils::utf8ToWide(fileStem(c.start));
    std::wstring base = stem;
    for (int n = 2; exists(base + L".json") || (csv && exists(base + L".csv")); ++n) base = stem + L" (" + std::to_wstring(n) + L")";
    if (!writeAll(base + L".json", toJson(entries, c))) {
        error = "Il file non si scrive sul desktop (cartella protetta o disco pieno?).";
        return false;
    }
    if (csv && !writeAll(base + L".csv", toCsv(entries, c))) Log::warn("Registro del mining: il CSV per Excel non si scrive");
    jsonPath = base + L".json";
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active = false;
        m_entries.clear();
        m_lastFile = jsonPath;
        const std::string place = TextUtils::trim(c.place);
        if (!place.empty()) {
            m_places.erase(std::remove(m_places.begin(), m_places.end(), place), m_places.end());
            m_places.insert(m_places.begin(), place);
            if (m_places.size() > 10) m_places.resize(10);
        }
    }
    save();
    Log::info("Registro del mining: salvato " + TextUtils::wideToUtf8(jsonPath) + " (" + std::to_string(entries.size()) + " rilevamenti)");
    return true;
}

} // namespace MiningLog
