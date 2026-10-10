#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace re9_pe_header_pattern {

inline bool is_candidate(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 112) {
        return false;
    }

    // Stable bytes from "4C 89 ? 24 40 00 00 00 41 ?".
    if (data[0] != 0x4C || data[1] != 0x89 || data[3] != 0x24 ||
        data[4] != 0x40 || data[5] != 0x00 || data[6] != 0x00 ||
        data[7] != 0x00 || data[8] != 0x41) {
        return false;
    }

    const auto find_from = [&](const std::uint8_t* needle, std::size_t needle_size, std::size_t from) -> std::size_t {
        if (needle == nullptr || needle_size == 0 || from >= size || needle_size > size - from) {
            return size;
        }

        const auto end = data + size;
        const auto found = std::search(data + from, end, needle, needle + needle_size);
        return found == end ? size : static_cast<std::size_t>(found - data);
    };

    constexpr std::uint8_t sub_rcx_20[] = {0x48, 0x2B, 0x59, 0x20};
    constexpr std::uint8_t xor_rcx_28[] = {0x4C, 0x33, 0x61, 0x28};
    constexpr std::uint8_t sub_rsp_90[] = {0x4C, 0x2B, 0xAC, 0x24, 0x90, 0x00, 0x00, 0x00};

    const auto sub_pos = find_from(sub_rcx_20, sizeof(sub_rcx_20), 10);
    if (sub_pos == size) {
        return false;
    }

    const auto xor_pos = find_from(xor_rcx_28, sizeof(xor_rcx_28), sub_pos + sizeof(sub_rcx_20));
    if (xor_pos == size) {
        return false;
    }

    const auto rsp_pos = find_from(sub_rsp_90, sizeof(sub_rsp_90), xor_pos + sizeof(xor_rcx_28));
    return rsp_pos != size;
}

} // namespace re9_pe_header_pattern
