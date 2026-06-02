#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>
#include "gui/GUIApp.hpp"

using namespace aether;

// ============================================================================
// PlotRingBuffer — lock-free SPSC ring buffer
// ============================================================================

TEST_CASE("PlotRingBuffer starts empty", "[RingBuffer]") {
    PlotRingBuffer rb;
    REQUIRE(rb.available() == 0);
    auto drained = rb.drain();
    REQUIRE(drained.empty());
}

TEST_CASE("PlotRingBuffer push-drain round-trip", "[RingBuffer]") {
    PlotRingBuffer rb;

    PlotPoint p1{ 1.0, 10.0f, 20.0f, 8.0f, 18.0f };
    PlotPoint p2{ 2.0, 15.0f, 25.0f, 13.0f, 23.0f };
    PlotPoint p3{ 3.0, 12.0f, 22.0f, 10.0f, 20.0f };

    rb.push(p1);
    rb.push(p2);
    rb.push(p3);

    REQUIRE(rb.available() == 3);

    auto drained = rb.drain();
    REQUIRE(drained.size() == 3);
    REQUIRE(drained[0].time == 1.0);
    REQUIRE(drained[0].rawX == 10.0f);
    REQUIRE(drained[1].time == 2.0);
    REQUIRE(drained[2].time == 3.0);

    // After drain, should be empty
    REQUIRE(rb.available() == 0);
}

TEST_CASE("PlotRingBuffer wraps around correctly", "[RingBuffer]") {
    PlotRingBuffer rb;

    // Fill past CAPACITY (2048)
    for (int i = 0; i < 3000; ++i) {
        PlotPoint p{};
        p.time = static_cast<double>(i);
        p.rawX = static_cast<float>(i);
        rb.push(p);
    }

    // Drain — should get exactly 3000 items
    auto drained = rb.drain();
    REQUIRE(drained.size() == 3000);
    REQUIRE(drained[0].time == 0.0);
    REQUIRE(drained[2999].time == 2999.0);
    REQUIRE(drained[1500].rawX == 1500.0f);
}

TEST_CASE("PlotRingBuffer reset clears state", "[RingBuffer]") {
    PlotRingBuffer rb;

    for (int i = 0; i < 100; ++i) {
        rb.push(PlotPoint{});
    }
    REQUIRE(rb.available() == 100);

    rb.reset();
    REQUIRE(rb.available() == 0);
}

TEST_CASE("PlotRingBuffer concurrent push-drain", "[RingBuffer]") {
    PlotRingBuffer rb;
    std::atomic<bool> running{true};
    std::atomic<int> drainedCount{0};

    // Producer thread: push 10000 items
    std::thread producer([&]() {
        for (int i = 0; i < 10000; ++i) {
            PlotPoint p{};
            p.time = static_cast<double>(i);
            rb.push(p);
        }
        running.store(false);
    });

    // Consumer thread: drain periodically
    std::thread consumer([&]() {
        while (running.load() || rb.available() > 0) {
            auto batch = rb.drain();
            drainedCount.fetch_add(static_cast<int>(batch.size()));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        // Final drain
        auto batch = rb.drain();
        drainedCount.fetch_add(static_cast<int>(batch.size()));
    });

    producer.join();
    consumer.join();

    REQUIRE(drainedCount.load() == 10000);
}

TEST_CASE("PlotRingBuffer handles rapid push-drain interleaving", "[RingBuffer]") {
    PlotRingBuffer rb;

    for (int cycle = 0; cycle < 10; ++cycle) {
        // Push 5 items
        for (int i = 0; i < 5; ++i) {
            PlotPoint p{};
            p.time = static_cast<double>(cycle * 5 + i);
            rb.push(p);
        }

        // Drain
        auto batch = rb.drain();
        REQUIRE(batch.size() == 5);
        REQUIRE(rb.available() == 0);
    }
}
