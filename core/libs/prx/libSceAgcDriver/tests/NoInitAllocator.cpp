#include "prx/libSceAgcDriver/Graphics/include/NoInitAllocator.hpp"
#include <array>
#include <cstddef>
#include <cstdio>
#include <vector>

using AgcDriver::Graphics::NoInitAllocator;

namespace {
int failures = 0;
void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}
}

int main() {
    // construct() without arguments must not value-initialize (that is the zero pass the allocator exists to skip)...
    NoInitAllocator<std::byte> allocator;
    std::array<std::byte, 8> storage;
    storage.fill(std::byte{0xAB});
    allocator.construct(storage.data());
    Expect(storage[0] == std::byte{0xAB}, "default construct left the byte untouched");
    // ...but construct with an argument must still store it.
    allocator.construct(storage.data() + 1, std::byte{0x00});
    Expect(storage[1] == std::byte{0x00}, "construct with a value stores it");

    // A vector over it behaves like a vector: growth keeps what was there, explicit values are kept, shrink and swap work.
    std::vector<std::byte, NoInitAllocator<std::byte>> bytes(4, std::byte{7});
    Expect(bytes.size() == 4 && bytes[3] == std::byte{7}, "fill constructor stores its value");
    bytes.resize(1024);
    Expect(bytes.size() == 1024 && bytes[0] == std::byte{7} && bytes[3] == std::byte{7}, "growth keeps the existing bytes");
    bytes.resize(2);
    Expect(bytes.size() == 2, "shrink");
    decltype(bytes)().swap(bytes);
    Expect(bytes.capacity() == 0, "swap with an empty vector releases the storage");
    bytes.assign(16, std::byte{3});
    Expect(bytes.size() == 16 && bytes[15] == std::byte{3}, "assign");
    const NoInitAllocator<int> rebound(allocator);
    Expect(sizeof(rebound) > 0, "converting constructor");
    return failures == 0 ? 0 : 1;
}
