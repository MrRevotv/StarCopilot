#include "screen_match.h"
#include "text_utils.h"

#include <algorithm>

namespace {
    constexpr double kAlignMin = 0.7;

    size_t distance(const std::wstring& x, const std::wstring& y) {
        std::vector<size_t> prev(y.size() + 1), cur(y.size() + 1);
        for (size_t j = 0; j <= y.size(); ++j) prev[j] = j;
        for (size_t i = 1; i <= x.size(); ++i) {
            cur[0] = i;
            for (size_t j = 1; j <= y.size(); ++j) {
                const size_t cost = x[i - 1] == y[j - 1] ? 0 : 1;
                cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            }
            std::swap(prev, cur);
        }
        return prev[y.size()];
    }

    double ratio(const std::wstring& x, const std::wstring& y) {
        if (x.empty() || y.empty()) return 0.0;
        return 1.0 - (double)distance(x, y) / (double)std::max(x.size(), y.size());
    }
}

namespace ScreenMatch {

bool sameText(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    const std::wstring x = TextUtils::utf8ToWide(a), y = TextUtils::utf8ToWide(b);
    const size_t len = std::max(x.size(), y.size());
    const size_t allowed = len < 6 ? 0 : len < 12 ? 1 : len < 24 ? 2 : len * 12 / 100;
    if (len - std::min(x.size(), y.size()) > allowed) return false;
    return distance(x, y) <= allowed;
}

bool sameNumbers(const std::string& a, const std::string& b) {
    auto digits = [](const std::string& s) {
        std::string d;
        for (char c : s) if (c >= '0' && c <= '9') d += c;
        return d;
    };
    return digits(a) == digits(b);
}

double likeness(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;
    const std::wstring x = TextUtils::utf8ToWide(a), y = TextUtils::utf8ToWide(b);
    double best = ratio(x, y);
    const std::wstring& s = x.size() <= y.size() ? x : y;
    const std::wstring& l = x.size() <= y.size() ? y : x;
    // Solo pezzi di almeno due parole e otto lettere: "ok" è la fine di mille frasi.
    if (s.size() >= 8 && s.size() < l.size() && s.find(L' ') != std::wstring::npos) {
        const double part = std::max(ratio(s, l.substr(l.size() - s.size())), ratio(s, l.substr(0, s.size())));
        if (part >= 0.85) best = std::max(best, part * 0.9);
    }
    return best;
}

std::vector<int> align(const std::vector<std::string>& previous, const std::vector<std::string>& current) {
    const size_t n = previous.size(), m = current.size();
    std::vector<int> match(m, -1);
    if (n == 0 || m == 0) return match;

    // Gli abbinamenti in ordine con la somiglianza totale più alta.
    std::vector<std::vector<double>> like(n, std::vector<double>(m));
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < m; ++j) like[i][j] = likeness(previous[i], current[j]);
    std::vector<std::vector<double>> best(n + 1, std::vector<double>(m + 1, 0.0));
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            double b = std::max(best[i - 1][j], best[i][j - 1]);
            if (like[i - 1][j - 1] >= kAlignMin) b = std::max(b, best[i - 1][j - 1] + like[i - 1][j - 1]);
            best[i][j] = b;
        }
    }
    for (size_t i = n, j = m; i > 0 && j > 0;) {
        if (like[i - 1][j - 1] >= kAlignMin && best[i][j] == best[i - 1][j - 1] + like[i - 1][j - 1]) {
            match[j - 1] = (int)(i - 1);
            --i;
            --j;
        } else if (best[i][j] == best[i - 1][j]) {
            --i;
        } else {
            --j;
        }
    }

    // I buchi tra due abbinamenti: se hanno la stessa misura, sono gli stessi messaggi.
    int lastP = -1, lastC = -1;
    for (int c = 0; c < (int)m; ++c) {
        const int p = match[c];
        if (p < 0) continue;
        if (lastC >= 0 && p - lastP == c - lastC) {
            for (int k = 1; k < c - lastC; ++k) match[lastC + k] = lastP + k;
        }
        lastP = p;
        lastC = c;
    }
    return match;
}

} // namespace ScreenMatch
