#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "crypto/crypto.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

template <size_t Size>
std::array<std::byte, Size> pattern(uint32_t seed) {
    std::array<std::byte, Size> out{};
    for (size_t i = 0; i < out.size(); ++i) {
        seed = seed * 1664525U + 1013904223U;
        out[i] = static_cast<std::byte>((seed >> 24U) & 0xffU);
    }
    return out;
}

}  // namespace

int main() {
    bool ok = true;

    const auto keymsg = pattern<164>(0x12345678U);
    const auto ekey = pattern<72>(0x90abcdefU);

    const auto first = mirage::crypto::fairplay_decrypt_key(keymsg, ekey);
    const auto second = mirage::crypto::fairplay_decrypt_key(keymsg, ekey);
    ok &= expect(first == second, "fairplay sap output should be deterministic");
    ok &= expect(std::ranges::any_of(first, [](std::byte value) { return value != std::byte{0}; }),
                 "fairplay sap output should not be all zero");

    return ok ? 0 : 1;
}
