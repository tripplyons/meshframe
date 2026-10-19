#include "core/work_counter.hpp"
#include <gtest/gtest.h>
#include <thread>
using namespace mf;

TEST(WorkCounter, JoinedThreadsRemainSafeToReadAndReset) {
    auto before = workCounts().decodeFrames;
    std::thread([] { work::addDecode(7); }).join();
    EXPECT_EQ(workCounts().decodeFrames, before + 7);
    resetWorkCounts();
    EXPECT_EQ(workCounts().decodeFrames, 0u);
    std::thread([] { work::addDecode(3); }).join();
    EXPECT_EQ(workCounts().decodeFrames, 3u);
}
