#include "telemetry/cpu_math.h"

#include <gtest/gtest.h>

using sentinel::telemetry::cpuUsagePercent;
using sentinel::telemetry::fileTimeToU64;
using sentinel::telemetry::RawCpuSample;

TEST(FileTimeToU64, CombinesHighAndLowDwords) {
    EXPECT_EQ(fileTimeToU64(0, 0), 0u);
    EXPECT_EQ(fileTimeToU64(0, 1), 1u);
    EXPECT_EQ(fileTimeToU64(1, 0), 1ull << 32);
    EXPECT_EQ(fileTimeToU64(0x89ABCDEF, 0x01234567), 0x89ABCDEF01234567ull);
}

TEST(CpuUsagePercent, ZeroTotalDeltaIsZero) {
    const RawCpuSample sample{10, 20, 30};
    EXPECT_DOUBLE_EQ(cpuUsagePercent(sample, sample), 0.0);
}

TEST(CpuUsagePercent, IdleOnlyIntervalIsZero) {
    const RawCpuSample previous{0, 0, 0};
    // Kernel includes idle: idle +100, kernel +100, user unchanged.
    const RawCpuSample current{100, 100, 0};
    EXPECT_DOUBLE_EQ(cpuUsagePercent(previous, current), 0.0);
}

TEST(CpuUsagePercent, BusyOnlyIntervalIsOneHundred) {
    const RawCpuSample previous{0, 0, 0};
    const RawCpuSample current{0, 40, 60};
    EXPECT_DOUBLE_EQ(cpuUsagePercent(previous, current), 100.0);
}

TEST(CpuUsagePercent, MixedLoad) {
    const RawCpuSample previous{100, 400, 200};
    const RawCpuSample current{150, 500, 250};
    // Δidle=50, Δkernel=100, Δuser=50, total=150, busy=100 → 66.6...%
    EXPECT_NEAR(cpuUsagePercent(previous, current), 100.0 * 100.0 / 150.0, 1e-9);
}

TEST(CpuUsagePercent, BackwardsCountersAreZero) {
    const RawCpuSample previous{100, 200, 300};
    const RawCpuSample current{50, 200, 300};
    EXPECT_DOUBLE_EQ(cpuUsagePercent(previous, current), 0.0);
}

TEST(CpuUsagePercent, IdleExceedingTotalClampsToZero) {
    const RawCpuSample previous{0, 0, 0};
    const RawCpuSample current{200, 10, 0};
    EXPECT_DOUBLE_EQ(cpuUsagePercent(previous, current), 0.0);
}
