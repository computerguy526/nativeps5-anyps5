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
