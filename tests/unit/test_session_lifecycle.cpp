/**
 * @file tests/unit/test_session_lifecycle.cpp
 * @brief Game Mode GDS reconnect contract: /launch must not wait on teardown.
 */
#include "../tests_common.h"

#include <chrono>
#include <thread>

#include <src/rtsp.h>

using namespace std::chrono_literals;

TEST(SessionLifecycle, SessionCountDoesNotBlock) {
  // rtsp_stream::session_count used to call clear(false) which joined Pulse
  // sample() on the nvhttp thread (Moonlight stuck on Starting Desktop, no log).
  const auto start = std::chrono::steady_clock::now();
  (void) rtsp_stream::session_count();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}
