#pragma once

#include <algorithm>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

// Paths that the game rejected while parsing a loose resource. The loader
// keeps them disabled for the current run so later requests fall back to the
// packaged resource instead of retrying the invalid loose file forever.
class LooseFileQuarantine {
public:
    void add(std::wstring path) {
        if (path.empty()) {
            return;
        }

        std::unique_lock lock{m_mutex};
        m_paths.insert(canonicalize(path));
    }

    [[nodiscard]] bool contains(std::wstring_view path) const {
        if (path.empty()) {
            return false;
        }

        std::shared_lock lock{m_mutex};

        return m_paths.contains(canonicalize(path));
    }

private:
    [[nodiscard]] static std::wstring canonicalize(std::wstring_view path) {
        std::wstring value{path};
        std::replace(value.begin(), value.end(), L'\\', L'/');

        for (auto& character : value) {
            if (character >= L'A' && character <= L'Z') {
                character = static_cast<wchar_t>(character - L'A' + L'a');
            }
        }

        if (value.size() >= 2 && value[1] == L':') {
            value.erase(0, 2);
        }

        const auto game_root = value.find(L"/re9/");
        if (game_root != std::wstring::npos) {
            value.erase(0, game_root + 5);
        }

        const auto natives = value.find(L"natives/");
        if (natives != std::wstring::npos) {
            value.erase(0, natives + 8);
        }

        if (value.starts_with(L"stm/")) {
            value.erase(0, 4);
        }

        const auto last_slash = value.find_last_of(L'/');
        const auto last_dot = value.find_last_of(L'.');
        if (last_dot != std::wstring::npos && last_dot > last_slash) {
            const auto suffix = value.substr(last_dot + 1);
            if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](wchar_t character) {
                    return character >= L'0' && character <= L'9';
                })) {
                value.erase(last_dot);
            }
        }

        return value;
    }
    mutable std::shared_mutex m_mutex{};
    std::unordered_set<std::wstring> m_paths{};
};
