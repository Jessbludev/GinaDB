// GinaDB Lite 0.7 — fts.cpp
#include "fts.hpp"

#include <algorithm>

namespace gina::fts {

void tokenize(const std::string& text, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            cur.push_back(static_cast<char>(
                (c >= 'A' && c <= 'Z') ? c + 32 : c));
        } else if (!cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
}

void InvertedIndex::index(const std::string& key, const std::string& text) {
    std::vector<std::string> toks;
    tokenize(text, toks);
    for (auto& t : toks) postings_[t].insert(key);
}

void InvertedIndex::remove(const std::string& key, const std::string& text) {
    std::vector<std::string> toks;
    tokenize(text, toks);
    for (auto& t : toks) {
        auto it = postings_.find(t);
        if (it == postings_.end()) continue;
        it->second.erase(key);
        if (it->second.empty()) postings_.erase(it);
    }
}

std::vector<std::string> InvertedIndex::query_and(
    const std::vector<std::string>& terms) const {
    if (terms.empty()) return {};
    auto it = postings_.find(terms[0]);
    if (it == postings_.end()) return {};
    std::vector<std::string> keys(it->second.begin(), it->second.end());
    for (std::size_t i = 1; i < terms.size() && !keys.empty(); i++) {
        auto jt = postings_.find(terms[i]);
        if (jt == postings_.end()) return {};
        const auto& allowed = jt->second;
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [&](const std::string& k) {
                                      return allowed.find(k) == allowed.end();
                                  }),
                   keys.end());
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace gina::fts
