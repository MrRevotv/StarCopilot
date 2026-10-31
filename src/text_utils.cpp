#include "text_utils.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace TextUtils {

std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), result.data(), size, nullptr, nullptr);
    return result;
}

std::string trim(const std::string& s) {
    const char* spaces = " \t\r\n\v\f";
    size_t start = s.find_first_not_of(spaces);
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(spaces);
    return s.substr(start, end - start + 1);
}

std::string collapseSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool lastSpace = false;
    for (char c : s) {
        bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
        if (space) {
            if (!lastSpace && !out.empty()) out += ' ';
            lastSpace = true;
        } else {
            out += c;
            lastSpace = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string toLower(const std::string& s) {
    std::wstring w = utf8ToWide(s);
    if (w.empty()) return s;
    CharLowerBuffW(w.data(), (DWORD)w.size());
    return wideToUtf8(w);
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

bool containsWord(const std::string& text, const std::string& term) {
    if (term.empty()) return false;
    std::wstring t = utf8ToWide(toLower(text));
    std::wstring w = utf8ToWide(toLower(term));
    if (w.empty()) return false;
    auto isWordChar = [](wchar_t c) { return std::iswalnum(c) != 0 || c == L'_'; };
    size_t pos = 0;
    while ((pos = t.find(w, pos)) != std::wstring::npos) {
        bool startOk = pos == 0 || !isWordChar(t[pos - 1]) || !isWordChar(w.front());
        size_t end = pos + w.size();
        bool endOk = end >= t.size() || !isWordChar(t[end]) || !isWordChar(w.back());
        if (startOk && endOk) return true;
        pos += 1;
    }
    return false;
}

std::string fixShoutingCase(const std::string& text) {
    std::wstring w = utf8ToWide(text);
    int letters = 0, upper = 0;
    for (wchar_t c : w) {
        if (IsCharAlphaW(c)) {
            letters++;
            if (IsCharUpperW(c)) upper++;
        }
    }
    // Meno del 70% di maiuscole, o frase cortissima ("OK", "GG"): resta com'è.
    if (letters < 6 || upper * 100 < letters * 70) return text;
    CharLowerBuffW(w.data(), (DWORD)w.size());
    for (wchar_t& c : w) {
        if (IsCharAlphaW(c)) {
            CharUpperBuffW(&c, 1);
            break;
        }
    }
    return wideToUtf8(w);
}

std::wstring transliterate(wchar_t c) {
    // Prima le lettere che si scompongono: "é" = "e" + accento combinante.
    switch (c) {
    case L'ß': return L"ss";
    case L'æ': return L"ae";
    case L'Æ': return L"AE";
    case L'œ': return L"oe";
    case L'Œ': return L"OE";
    case L'ø': return L"o";
    case L'Ø': return L"O";
    case L'đ': return L"d";
    case L'Đ': return L"D";
    case L'ł': return L"l";
    case L'Ł': return L"L";
    case L'ı': return L"i";
    case L'«': case L'»': case L'“': case L'”': case L'„': return L"\"";
    case L'‘': case L'’': case L'‚': return L"'";
    case L'–': case L'—': return L"-";
    case L'…': return L"...";
    case L'¿': return L"";
    case L'¡': return L"";
    case 0x00A0: return L" ";
    }
    wchar_t buf[8] = {};
    int n = FoldStringW(MAP_COMPOSITE, &c, 1, buf, 8);
    if (n > 0 && buf[0] < 128) return std::wstring(1, buf[0]);
    return {};
}

std::string normalizeForMatch(const std::string& s) {
    std::wstring w = utf8ToWide(s);
    CharLowerBuffW(w.data(), (DWORD)w.size());
    std::wstring out;
    bool lastSpace = true;
    for (wchar_t c : w) {
        if (IsCharAlphaNumericW(c)) {
            out += c;
            lastSpace = false;
        } else if (!lastSpace) {
            out += L' ';
            lastSpace = true;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return wideToUtf8(out);
}

double similarity(const std::string& a, const std::string& b) {
    if (a == b) return 1.0;
    std::wstring x = utf8ToWide(a), y = utf8ToWide(b);
    if (x.empty() || y.empty()) return 0.0;
    std::vector<size_t> prev(y.size() + 1), cur(y.size() + 1);
    for (size_t j = 0; j <= y.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= x.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= y.size(); ++j) {
            size_t cost = x[i - 1] == y[j - 1] ? 0 : 1;
            cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, cur);
    }
    double dist = (double)prev[y.size()];
    return 1.0 - dist / (double)std::max(x.size(), y.size());
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

} // namespace TextUtils
