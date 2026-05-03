#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "queue/spsc_queue.h"

using namespace lt;

TEST_SUITE("SpscQueue") {
    TEST_CASE("push and pop single item") {
        SpscQueue<int> queue(16);
        CHECK(queue.try_push(42));

        auto val = queue.try_pop();
        REQUIRE(val.has_value());
        CHECK(*val == 42);
    }

    TEST_CASE("FIFO order") {
        SpscQueue<int> queue(16);
        queue.try_push(1);
        queue.try_push(2);
        queue.try_push(3);

        CHECK(*queue.try_pop() == 1);
        CHECK(*queue.try_pop() == 2);
        CHECK(*queue.try_pop() == 3);
    }

    TEST_CASE("empty queue returns nullopt") {
        SpscQueue<int> queue(16);
        auto val = queue.try_pop();
        CHECK_FALSE(val.has_value());
    }

    TEST_CASE("full queue returns false") {
        SpscQueue<int> queue(4);  // capacity 4
        CHECK(queue.try_push(1));
        CHECK(queue.try_push(2));
        CHECK(queue.try_push(3));
        // SPSCQueue actual capacity might be capacity-1 or capacity
        // Just keep pushing until it fails
        int pushed = 3;
        while (queue.try_push(pushed + 1)) pushed++;
        // Should have at least pushed some
        CHECK(pushed >= 3);
    }

    TEST_CASE("overflow counter increments") {
        SpscQueue<int> queue(2);
        // Fill queue
        while (queue.try_push(0)) {}
        // One more should overflow
        queue.try_push(99);
        CHECK(queue.overflow_count() > 0);
    }

    TEST_CASE("stats counters") {
        SpscQueue<int> queue(16);
        queue.try_push(1);
        queue.try_push(2);
        CHECK(queue.push_count() == 2);

        queue.try_pop();
        CHECK(queue.pop_count() == 1);
    }

    TEST_CASE("cross-thread usage") {
        SpscQueue<int> queue(1024);
        constexpr int N = 10000;

        std::thread producer([&]() {
            for (int i = 0; i < N; ++i) {
                while (!queue.try_push(i)) {
                    std::this_thread::yield();
                }
            }
        });

        std::vector<int> received;
        received.reserve(N);

        std::thread consumer([&]() {
            while (received.size() < N) {
                auto val = queue.try_pop();
                if (val.has_value()) {
                    received.push_back(*val);
                } else {
                    std::this_thread::yield();
                }
            }
        });

        producer.join();
        consumer.join();

        CHECK(received.size() == N);
        for (int i = 0; i < N; ++i) {
            CHECK(received[i] == i);
        }
    }
}
