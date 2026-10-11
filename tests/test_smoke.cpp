#include <gtest/gtest.h>

#include "core/value.hpp"

TEST(Smoke, ColorRoundTrip) {
    auto c = mf::parseColor("#808080");
    ASSERT_TRUE(c);
    EXPECT_EQ(mf::formatColor(*c), "#808080");
}
