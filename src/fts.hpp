// GinaDB Lite 0.7 — minimal full-text search: tokenizer + inverted index.
// Tokens: maximal runs of [A-Za-z0-9] or any byte >= 0x80 (UTF-8 letters and
// accents are kept intact). ASCII case-insensitive; no stemming.
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gina::fts {

void tokenize(const std::string& text, std::vector<std::string>& out);

class InvertedIndex {
public:
    void index(const std::string& key, const std::string& text);
    void remove(const std::string& key, const std::string& text);
    // Keys containing ALL terms (AND). Sorted for determinism.
    [[nodiscard]] std::vector<std::string> query_and(
        const std::vector<std::string>& terms) const;
    [[nodiscard]] bool empty() const { return postings_.empty(); }

private:
    std::unordered_map<std::string, std::unordered_set<std::string>> postings_;
};

} // namespace gina::fts
