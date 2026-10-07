#include "FMMLooseFileBridge.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>

#include <spdlog/spdlog.h>
#include "third_party/miniz/miniz_tinfl.h"
#include "third_party/miniz/miniz_zip.h"

namespace fs = std::filesystem;

namespace fmm_loose_file_bridge {
namespace {

enum class SourceKind {
    Directory,
    Zip,
    Unsupported,
};

struct Source {
    SourceKind kind{SourceKind::Unsupported};
    fs::path path{};
};

struct State {
    std::mutex mutex{};
    bool initialized{false};
    fs::path game_root{};
    fs::path fluffy_root{};
    std::unordered_map<std::string, std::string> owner_by_file{};
    std::unordered_map<std::string, Source> source_by_owner{};
    std::unordered_set<std::string> failed_requests{};
    size_t request_logs{0};
};

State g_state{};

static std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

static std::string lowercase_ascii(std::string value) {
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

static std::string compact_name(std::string value) {
    value = lowercase_ascii(std::move(value));

    std::string out;
    out.reserve(value.size());

    for (const auto c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out.push_back(c);
        }
    }

    return out;
}

static bool contains_ascii_case_insensitive(std::wstring value, std::wstring_view needle) {
    for (auto& c : value) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }

    std::wstring lowered_needle{needle};
    for (auto& c : lowered_needle) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }

    return value.find(lowered_needle) != std::wstring::npos;
}

static std::string normalize_native_relative(std::string value) {
    value = lowercase_ascii(std::move(value));
    std::replace(value.begin(), value.end(), '\\', '/');

    while (value.rfind("./", 0) == 0) {
        value.erase(0, 2);
    }

    const auto pos = value.find("natives/");
    if (pos == std::string::npos) {
        return {};
    }

    return value.substr(pos);
}

static bool parse_modinfo_name_text(const std::string& text, std::string& name_out) {
    size_t begin = 0;

    while (begin < text.size()) {
        const auto end = text.find('\n', begin);
        const auto line_end = end == std::string::npos ? text.size() : end;

        auto line = text.substr(begin, line_end - begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        line = trim(line);

        if (line.size() >= 5) {
            const auto key = lowercase_ascii(line.substr(0, 5));

            if (key == "name=") {
                name_out = trim(line.substr(5));
                return !name_out.empty();
            }
        }

        if (end == std::string::npos) {
            break;
        }

        begin = end + 1;
    }

    return false;
}

static bool read_file_text(const fs::path& path, std::string& text_out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    text_out.assign(
        std::istreambuf_iterator<char>{file},
        std::istreambuf_iterator<char>{}
    );

    return true;
}

static bool read_modinfo_name_from_directory(const fs::path& directory, std::string& name_out) {
    std::string text;
    const auto path = directory / "modinfo.ini";

    if (!read_file_text(path, text)) {
        return false;
    }

    return parse_modinfo_name_text(text, name_out);
}

static bool read_zip_entry_name(mz_zip_archive* zip, mz_uint index, std::string& name_out) {
    char stack_buffer[8192]{};
    const auto required = mz_zip_reader_get_filename(zip, index, stack_buffer, sizeof(stack_buffer));

    if (required < sizeof(stack_buffer)) {
        name_out.assign(stack_buffer, required);
        if (!name_out.empty() && name_out.back() == '\0') {
            name_out.pop_back();
        }
        return true;
    }

    std::string dynamic_buffer;
    dynamic_buffer.resize(static_cast<size_t>(required) + 1);

    const auto written = mz_zip_reader_get_filename(
        zip,
        index,
        dynamic_buffer.data(),
        static_cast<mz_uint>(dynamic_buffer.size())
    );

    if (written >= dynamic_buffer.size()) {
        return false;
    }

    dynamic_buffer.resize(written);
    if (!dynamic_buffer.empty() && dynamic_buffer.back() == '\0') {
        dynamic_buffer.pop_back();
    }
    name_out = std::move(dynamic_buffer);
    return true;
}

static bool read_modinfo_name_from_zip(const fs::path& zip_path, std::string& name_out) {
    mz_zip_archive zip{};
    const auto zip_filename = zip_path.string();

    if (!mz_zip_reader_init_file(&zip, zip_filename.c_str(), 0)) {
        return false;
    }

    const auto file_count = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < file_count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        std::string entry_name;
        if (!read_zip_entry_name(&zip, i, entry_name)) {
            continue;
        }

        auto lowered = lowercase_ascii(entry_name);
        std::replace(lowered.begin(), lowered.end(), '\\', '/');

        const auto slash = lowered.find_last_of('/');
        const auto basename = slash == std::string::npos
            ? lowered
            : lowered.substr(slash + 1);

        if (basename != "modinfo.ini") {
            continue;
        }

        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);

        if (data == nullptr) {
            break;
        }

        std::string text(
            static_cast<const char*>(data),
            static_cast<const char*>(data) + size
        );
        MZ_FREE(data);

        if (parse_modinfo_name_text(text, name_out)) {
            mz_zip_reader_end(&zip);
            return true;
        }
    }

    mz_zip_reader_end(&zip);
    return false;
}

static std::optional<fs::path> get_game_root() {
    std::wstring buffer(32768, L'\0');

    const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );

    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }

    buffer.resize(length);

    const auto slash = buffer.find_last_of(L"\\/");

    if (slash == std::wstring::npos) {
        return std::nullopt;
    }

    buffer.resize(slash);
    return fs::path{buffer};
}

static std::wstring get_current_windows_user() {
    wchar_t buffer[256]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));

    if (GetUserNameW(buffer, &size) == 0 || size == 0) {
        return {};
    }

    return std::wstring{buffer};
}

static void append_existing_root(std::vector<fs::path>& roots, const fs::path& root) {
    if (root.empty()) {
        return;
    }

    std::error_code ec;

    if (!fs::is_directory(root, ec)) {
        return;
    }

    if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
        roots.push_back(root);
    }
}

static void append_matching_children(
    std::vector<fs::path>& roots,
    const fs::path& parent
) {
    std::error_code ec;

    if (!fs::is_directory(parent, ec)) {
        return;
    }

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (ec) {
            break;
        }

        if (!entry.is_directory(ec)) {
            ec.clear();
            continue;
        }

        const auto name = entry.path().filename().wstring();

        if (
            contains_ascii_case_insensitive(name, L"fluffy") ||
            contains_ascii_case_insensitive(name, L"modmanager")
        ) {
            append_existing_root(roots, entry.path());
        }

        ec.clear();
    }
}

static std::optional<fs::path> locate_fluffy_root(const fs::path& game_root) {
    std::vector<fs::path> candidates{};

    append_existing_root(candidates, fs::path{L"C:\\modmanager"});
    append_existing_root(candidates, fs::path{L"C:\\Fluffy Mod Manager"});

    const auto user = get_current_windows_user();

    const std::vector<std::wstring> relative_parents{
        L"Downloads",
        L"Desktop",
        L"Documents",
    };

    if (!user.empty()) {

        for (const auto& parent_name : relative_parents) {
            const fs::path parent = fs::path{L"Z:\\Users"} / user / parent_name;
            append_existing_root(candidates, parent);
            append_matching_children(candidates, parent);
        }

        const fs::path user_root = fs::path{L"Z:\\Users"} / user;
        append_matching_children(candidates, user_root);
    }

    // CrossOver's Windows account name is not necessarily the macOS account
    // name. Enumerate one level of Z:\Users so the bridge can discover the
    // host user's Downloads/Desktop/Documents regardless of the bottle user.
    {
        const fs::path z_users{L"Z:\\Users"};
        std::error_code ec;

        if (fs::is_directory(z_users, ec)) {
            for (const auto& user_entry : fs::directory_iterator(z_users, ec)) {
                if (ec) {
                    break;
                }

                if (!user_entry.is_directory(ec)) {
                    ec.clear();
                    continue;
                }

                const auto host_user_root = user_entry.path();

                for (const auto& parent_name : relative_parents) {
                    const auto parent = host_user_root / parent_name;
                    append_existing_root(candidates, parent);
                    append_matching_children(candidates, parent);
                }

                append_matching_children(candidates, host_user_root);
                ec.clear();
            }
        }
    }

    append_matching_children(candidates, fs::path{L"C:\\"});

    if (!game_root.empty()) {
        append_existing_root(candidates, game_root.parent_path());
    }

    for (const auto& candidate : candidates) {
        const auto ini = candidate / "Games" / "RE9" / "installed.ini";

        if (fs::is_regular_file(ini)) {
            return candidate;
        }
    }

    return std::nullopt;
}

static void parse_installed_ini(const fs::path& installed_ini) {
    std::ifstream file(installed_ini, std::ios::binary);

    if (!file) {
        return;
    }

    std::string current_section;

    std::string line;

    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        line = trim(line);

        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            current_section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        if (current_section.empty()) {
            continue;
        }

        const auto lowered = lowercase_ascii(line);

        if (lowered.rfind("file=", 0) != 0) {
            continue;
        }

        const auto relative = normalize_native_relative(line.substr(5));

        if (!relative.empty()) {
            // installed.ini is ordered in deployment order. The last claim
            // wins, matching how Fluffy installs conflicting loose files.
            g_state.owner_by_file[relative] = current_section;
        }
    }
}

static std::string source_key(const std::string& value) {
    return lowercase_ascii(trim(value));
}

static void add_source_alias(
    const std::string& owner,
    const Source& source
) {
    if (owner.empty()) {
        return;
    }

    g_state.source_by_owner[source_key(owner)] = source;

    const auto compact = compact_name(owner);

    if (!compact.empty()) {
        g_state.source_by_owner["#compact:" + compact] = source;
    }
}

static void build_mod_source_index() {
    const auto mods_root = g_state.fluffy_root / "Games" / "RE9" / "Mods";

    std::error_code ec;

    if (!fs::is_directory(mods_root, ec)) {
        spdlog::warn(
            "[LooseFileLoader][FMM] Mods directory not found: {}",
            mods_root.string()
        );
        return;
    }

    for (const auto& entry : fs::directory_iterator(mods_root, ec)) {
        if (ec) {
            break;
        }

        const auto path = entry.path();

        if (entry.is_directory(ec)) {
            std::string name;

            if (!read_modinfo_name_from_directory(path, name)) {
                name = path.filename().string();
            }

            add_source_alias(
                name,
                Source{SourceKind::Directory, path}
            );

            continue;
        }

        ec.clear();

        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        auto extension = lowercase_ascii(path.extension().string());

        if (extension == ".zip") {
            std::string name;

            if (!read_modinfo_name_from_zip(path, name)) {
                name = path.stem().string();
            }

            add_source_alias(
                name,
                Source{SourceKind::Zip, path}
            );
        } else if (extension == ".7z" || extension == ".rar") {
            add_source_alias(
                path.stem().string(),
                Source{SourceKind::Unsupported, path}
            );
        }

        ec.clear();
    }
}

static std::optional<Source> find_source_for_owner(const std::string& owner) {
    const auto exact = g_state.source_by_owner.find(source_key(owner));

    if (exact != g_state.source_by_owner.end()) {
        return exact->second;
    }

    const auto compact = compact_name(owner);

    if (!compact.empty()) {
        const auto compact_it =
            g_state.source_by_owner.find("#compact:" + compact);

        if (compact_it != g_state.source_by_owner.end()) {
            return compact_it->second;
        }
    }

    return std::nullopt;
}

static int find_zip_member(mz_zip_archive* zip, const std::string& requested_relative) {
    const auto file_count = mz_zip_get_num_files(zip);

    for (mz_uint i = 0; i < file_count; ++i) {
        if (mz_zip_reader_is_file_a_directory(zip, i)) {
            continue;
        }

        std::string entry_name;

        if (!read_zip_entry_name(zip, i, entry_name)) {
            continue;
        }

        if (normalize_native_relative(entry_name) == requested_relative) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

static bool extract_from_zip(
    const Source& source,
    const std::string& requested_relative,
    const fs::path& destination
) {
    mz_zip_archive zip{};
    const auto zip_filename = source.path.string();

    if (!mz_zip_reader_init_file(&zip, zip_filename.c_str(), 0)) {
        spdlog::warn(
            "[LooseFileLoader][FMM] Failed to open ZIP: {}",
            source.path.string()
        );
        return false;
    }

    const auto member = find_zip_member(&zip, requested_relative);

    if (member < 0) {
        mz_zip_reader_end(&zip);
        return false;
    }

    std::error_code ec;
    fs::create_directories(destination.parent_path(), ec);

    if (ec) {
        mz_zip_reader_end(&zip);
        return false;
    }

    const auto destination_name = destination.string();

    const auto extracted = mz_zip_reader_extract_to_file(
        &zip,
        static_cast<mz_uint>(member),
        destination_name.c_str(),
        0
    );

    mz_zip_reader_end(&zip);

    return extracted != 0;
}

static bool copy_from_directory(
    const Source& source,
    const std::string& requested_relative,
    const fs::path& destination
) {
    auto relative_path = requested_relative;
    std::replace(
        relative_path.begin(),
        relative_path.end(),
        '/',
        '\\'
    );

    const auto source_path = source.path / fs::path{relative_path};

    std::error_code ec;

    if (!fs::is_regular_file(source_path, ec)) {
        return false;
    }

    fs::create_directories(destination.parent_path(), ec);

    if (ec) {
        return false;
    }

    fs::copy_file(
        source_path,
        destination,
        fs::copy_options::overwrite_existing,
        ec
    );

    return !ec;
}

static bool initialize_locked() {
    if (g_state.initialized) {
        return !g_state.fluffy_root.empty();
    }

    g_state.initialized = true;

    const auto game_root = get_game_root();

    if (!game_root) {
        spdlog::warn("[LooseFileLoader][FMM] Could not determine game root.");
        return false;
    }

    g_state.game_root = *game_root;

    const auto fluffy_root = locate_fluffy_root(*game_root);

    if (!fluffy_root) {
        spdlog::info(
            "[LooseFileLoader][FMM] Fluffy Mod Manager RE9 manifest not found."
        );
        return false;
    }

    g_state.fluffy_root = *fluffy_root;

    const auto installed_ini =
        g_state.fluffy_root / "Games" / "RE9" / "installed.ini";

    parse_installed_ini(installed_ini);

    build_mod_source_index();

    spdlog::info(
        "[LooseFileLoader][FMM] Compatibility bridge active: root={} active_native_entries={} sources={}",
        g_state.fluffy_root.string(),
        g_state.owner_by_file.size(),
        g_state.source_by_owner.size()
    );

    return true;
}

}

bool ensure_file(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }

    fs::path target{path};

    std::error_code ec;

    if (!target.is_absolute()) {
        if (const auto game_root = get_game_root()) {
            target = *game_root / target;
        }
    }

    if (fs::is_regular_file(target, ec)) {
        return true;
    }

    auto normalized_path = target.wstring();
    std::replace(
        normalized_path.begin(),
        normalized_path.end(),
        L'\\',
        L'/'
    );

    std::string normalized_utf8;
    normalized_utf8.reserve(normalized_path.size());

    for (const auto c : normalized_path) {
        if (c <= 0x7F) {
            normalized_utf8.push_back(static_cast<char>(c));
        } else {
            // RE9 Fluffy relative paths are ASCII in practice. Do not attempt
            // lossy encoding for a non-ASCII path here.
            normalized_utf8.clear();
            break;
        }
    }

    if (normalized_utf8.empty()) {
        return false;
    }

    const auto relative = normalize_native_relative(normalized_utf8);

    if (relative.empty()) {
        return false;
    }

    std::scoped_lock lock{g_state.mutex};

    if (!initialize_locked()) {
        return false;
    }

    if (g_state.failed_requests.contains(relative)) {
        return false;
    }

    const auto owner_it = g_state.owner_by_file.find(relative);

    if (owner_it == g_state.owner_by_file.end()) {
        if (g_state.request_logs < 12) {
            ++g_state.request_logs;
            spdlog::info(
                "[LooseFileLoader][FMM] No installed.ini owner for {}",
                relative
            );
        }

        g_state.failed_requests.insert(relative);
        return false;
    }

    const auto source = find_source_for_owner(owner_it->second);

    if (!source) {
        spdlog::warn(
            "[LooseFileLoader][FMM] Active mod has no source archive/folder: owner={} file={}",
            owner_it->second,
            relative
        );
        g_state.failed_requests.insert(relative);
        return false;
    }

    bool success = false;

    switch (source->kind) {
    case SourceKind::Directory:
        success = copy_from_directory(*source, relative, target);
        break;

    case SourceKind::Zip:
        success = extract_from_zip(*source, relative, target);
        break;

    case SourceKind::Unsupported:
        spdlog::warn(
            "[LooseFileLoader][FMM] 7z/RAR source is not supported by the compatibility bridge yet: {}",
            source->path.string()
        );
        g_state.failed_requests.insert(relative);
        return false;
    }

    if (success && fs::is_regular_file(target, ec)) {
        spdlog::info(
            "[LooseFileLoader][FMM] materialized loose file: owner={} {} -> {}",
            owner_it->second,
            relative,
            target.string()
        );
        return true;
    }

    g_state.failed_requests.insert(relative);
    return false;
}

}
