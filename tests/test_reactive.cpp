#include <gtest/gtest.h>

#include "core/curve.hpp"
#include "core/reactive.hpp"
#include "core/thread_pool.hpp"

#include <atomic>

using namespace mf;

TEST(Reactive, PropagatesAndCaches) {
    Graph g;
    auto a = g.addSource("a", 1.0);
    int runs = 0;
    auto b = g.addComputed("b", Graph::Kind::Signal, [&](Graph& gr) {
        ++runs;
        return Value(asNumber(gr.get(a)) * 2);
    });
    EXPECT_EQ(asNumber(g.get(b)), 2.0);
    EXPECT_EQ(asNumber(g.get(b)), 2.0);
    EXPECT_EQ(runs, 1);
    g.set(a, 5.0);
    EXPECT_EQ(asNumber(g.get(b)), 10.0);
    EXPECT_EQ(runs, 2);
    EXPECT_FALSE(g.set(a, 5.0));
    g.get(b);
    EXPECT_EQ(runs, 2);
}

TEST(Reactive, DiamondEvaluatesOncePerChange) {
    Graph g;
    auto t = g.addSource("t", 0.0);
    auto l = g.addComputed("l", Graph::Kind::Signal, [&](Graph& gr) { return Value(asNumber(gr.get(t)) + 1); });
    auto r = g.addComputed("r", Graph::Kind::Signal, [&](Graph& gr) { return Value(asNumber(gr.get(t)) * 10); });
    int runs = 0;
    auto out = g.addComputed("out", Graph::Kind::Output, [&](Graph& gr) {
        ++runs;
        return Value(asNumber(gr.get(l)) + asNumber(gr.get(r)));
    });
    EXPECT_EQ(asNumber(g.get(out)), 1.0);
    g.set(t, 2.0);
    EXPECT_EQ(asNumber(g.get(out)), 23.0);
    EXPECT_EQ(runs, 2);
    EXPECT_EQ(g.node(out).deps.size(), 2u);
}

TEST(Reactive, UnchangedIntermediateStopsPropagation) {
    Graph g;
    auto t = g.addSource("t", 0.2);
    auto floor = g.addComputed("floor", Graph::Kind::Signal, [&](Graph& gr) {
        return Value(std::floor(asNumber(gr.get(t))));
    });
    int runs = 0;
    auto out = g.addComputed("out", Graph::Kind::Output, [&](Graph& gr) {
        ++runs;
        return Value(asNumber(gr.get(floor)) + 100);
    });
    g.get(out);
    g.set(t, 0.7);
    EXPECT_EQ(asNumber(g.get(out)), 100.0);
    EXPECT_EQ(runs, 1);
    g.set(t, 1.1);
    EXPECT_EQ(asNumber(g.get(out)), 101.0);
    EXPECT_EQ(runs, 2);
}

TEST(Reactive, DynamicDependencies) {
    Graph g;
    auto flag = g.addSource("flag", 1.0);
    auto x = g.addSource("x", 3.0);
    auto y = g.addSource("y", 4.0);
    auto pick = g.addComputed("pick", Graph::Kind::Signal, [&](Graph& gr) {
        return asNumber(gr.get(flag)) > 0 ? gr.get(x) : gr.get(y);
    });
    EXPECT_EQ(asNumber(g.get(pick)), 3.0);
    EXPECT_EQ(g.node(pick).deps.size(), 2u);
    g.set(flag, 0.0);
    EXPECT_EQ(asNumber(g.get(pick)), 4.0);
    // x is no longer a dependency, so changing it does not dirty pick.
    g.set(x, 99.0);
    EXPECT_EQ(g.node(pick).state, Graph::Node::State::Clean);
    EXPECT_TRUE(g.node(x).dependents.empty());
}

TEST(Reactive, CycleIsReported) {
    Graph g;
    Graph::Index a = 0, b = 0;
    a = g.addComputed("a", Graph::Kind::Signal, [&](Graph& gr) { return Value(asNumber(gr.get(b)) + 1); });
    b = g.addComputed("b", Graph::Kind::Signal, [&](Graph& gr) { return Value(asNumber(gr.get(a)) + 1); });
    g.get(a);
    EXPECT_NE(g.node(a).error.find("cycle"), std::string::npos);
    EXPECT_NE(g.node(b).error.find("cycle"), std::string::npos);
}

TEST(Reactive, ExceptionsBecomeNodeErrors) {
    Graph g;
    auto n = g.addComputed("bad", Graph::Kind::Signal, [](Graph&) -> Value { throw std::runtime_error("boom"); });
    EXPECT_EQ(typeOf(g.get(n)), ValueType::None);
    EXPECT_EQ(g.node(n).error, "boom");
}

TEST(Curve, LinearHoldAndClamp) {
    Curve c;
    c.setKey(0, 0);
    c.setKey(2, 10);
    EXPECT_DOUBLE_EQ(c.sample(-1), 0);
    EXPECT_DOUBLE_EQ(c.sample(1), 5);
    EXPECT_DOUBLE_EQ(c.sample(3), 10);
    c.keys[0].interp = Interp::Hold;
    EXPECT_DOUBLE_EQ(c.sample(1.9), 0);
}

TEST(Curve, SetKeyKeepsOrderAndReplaces) {
    Curve c;
    c.setKey(2, 1);
    c.setKey(0, 0);
    c.setKey(1, 5);
    c.setKey(1, 6);
    ASSERT_EQ(c.keys.size(), 3u);
    EXPECT_DOUBLE_EQ(c.keys[1].v, 6);
    EXPECT_TRUE(c.removeKeyAt(1));
    EXPECT_EQ(c.keys.size(), 2u);
}

TEST(Curve, BezierEasing) {
    EXPECT_NEAR(cubicBezier(0.5, 0.42f, 0, 0.58f, 1), 0.5, 1e-4);
    EXPECT_LT(cubicBezier(0.25, 0.42f, 0, 0.58f, 1), 0.25);
    EXPECT_GT(cubicBezier(0.75, 0.42f, 0, 0.58f, 1), 0.75);
    Curve r = Curve::ramp("ease-in-out");
    EXPECT_NEAR(r.sample(0), 0, 1e-9);
    EXPECT_NEAR(r.sample(1), 1, 1e-9);
    EXPECT_NEAR(r.sample(0.5), 0.5, 1e-4);
    Curve back = Curve::ramp("ease-in-back");
    EXPECT_LT(back.sample(0.2), 0.0);
}

TEST(ThreadPool, CoversRangeOnce) {
    ThreadPool pool(4);
    std::vector<std::atomic<int>> hits(10000);
    pool.parallelFor(0, hits.size(), [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) hits[i]++;
    });
    for (auto& h : hits) ASSERT_EQ(h.load(), 1);
}
