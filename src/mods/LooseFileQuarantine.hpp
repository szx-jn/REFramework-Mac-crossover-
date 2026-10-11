#pragma once

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
        m_paths.insert(std::move(path));
    }

    [[nodiscard]] bool contains(std::wstring_view path) const {
        if (path.empty()) {
            return false;
        }

        std::shared_lock lock{m_mutex};

        for (const auto& candidate : m_paths) {
            if (candidate == path) {
                return true;
            }
        }

        return false;
    }

private:
    mutable std::shared_mutex m_mutex{};
    std::unordered_set<std::wstring> m_paths{};
};
