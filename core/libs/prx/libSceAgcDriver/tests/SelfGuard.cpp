// Unit tests for Execution/src/SelfGuard.cpp (no GPU needed).
#include "prx/libSceAgcDriver/Execution/include/SelfGuard.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace AgcDriver::SelfGuard;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
int main() {
    CHECK(ParseLimit(nullptr, 7) == 7);
    CHECK(ParseLimit("", 7) == 7);
    CHECK(ParseLimit("abc", 7) == 7);
    CHECK(ParseLimit("12x", 7) == 7);
    CHECK(ParseLimit("0", 7) == 0);
    CHECK(ParseLimit("18944", 7) == 18944);
    CHECK(!OverLimit(100, 0));
    CHECK(!OverLimit(18944, 18944));
    CHECK(OverLimit(18945, 18944));
    // budget: 3 pools, the 4th switches timestamps off for good
    TimestampBudget::ResetForTest(3);
    CHECK(TimestampBudget::TryAcquire("t") && TimestampBudget::TryAcquire("t") && TimestampBudget::TryAcquire("t"));
    CHECK(TimestampBudget::Live() == 3);
    TimestampBudget::Release();
    CHECK(TimestampBudget::Live() == 2 && !TimestampBudget::Disabled());
    CHECK(TimestampBudget::TryAcquire("t"));
    CHECK(!TimestampBudget::TryAcquire("t"));   // budget spent -> off
    CHECK(TimestampBudget::Disabled());
    TimestampBudget::Release();
    CHECK(!TimestampBudget::TryAcquire("t"));   // stays off even with room
    // a create failure switches off and uncounts
    TimestampBudget::ResetForTest(10);
    CHECK(TimestampBudget::TryAcquire("t"));
    TimestampBudget::NoteCreateFailure("t", -2);
    CHECK(TimestampBudget::Live() == 0 && TimestampBudget::Disabled());
    CHECK(!TimestampBudget::TryAcquire("t"));
    // the profile switch is off unless asked for with exactly "1", for every title
    CHECK(!GpuProfileAllowed(nullptr, true) && !GpuProfileAllowed("", true) && !GpuProfileAllowed("0", true) && !GpuProfileAllowed("yes", true) && !GpuProfileAllowed("11", true));
    CHECK(GpuProfileAllowed("1", true));
    CHECK(GpuProfileAllowed(nullptr, false) && GpuProfileAllowed("0", false));   // other platforms: no block
    // the default memory limit follows the machine
    const std::uint64_t GiB = 1ull << 30;
    CHECK(DefaultMemoryLimitMiB(0) == 0);
    CHECK(DefaultMemoryLimitMiB(4 * GiB) == 0);
    CHECK(DefaultMemoryLimitMiB(8 * GiB) == 8192 - 5632);
    CHECK(DefaultMemoryLimitMiB(16 * GiB) == 16384 - 5632);
    CHECK(DefaultMemoryLimitMiB(24 * GiB) == 18944);
    CHECK(DefaultMemoryLimitMiB(64 * GiB) == 65536 - 65536 / 5);
    CHECK(DefaultMemoryLimitMiB(~0ull) == 0xffffffffu);
    for (std::uint64_t g = 1; g <= 512; ++g) { const auto l = DefaultMemoryLimitMiB(g * GiB); CHECK(l == 0 || (l >= 1024 && l < g * 1024)); }
#if defined(__APPLE__)
    CHECK(PhysicalMemoryBytes() >= GiB);
#endif
    // release never underflows
    TimestampBudget::ResetForTest(10);
    TimestampBudget::Release();
    CHECK(TimestampBudget::Live() == 0);
#if defined(__APPLE__)
    { const std::size_t n = 128u << 20; char* m = static_cast<char*>(std::malloc(n)); std::memset(m, 1, n); CHECK(FootprintMiB() >= 100); volatile char sink = m[n / 2]; (void)sink; std::free(m); }
#endif
    std::printf("selfguard tests: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
