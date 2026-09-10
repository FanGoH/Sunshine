/**
 * @file tests/unit/test_session_lifecycle.cpp
 * @brief Game Mode GDS reconnect contract: /launch must not wait on teardown.
 */
#include "../tests_common.h"

#include <chrono>
#include <thread>

#include <src/rtsp.h>
#include <src/stream.h>

using namespace std::chrono_literals;

TEST(SessionLifecycle, SessionCountDoesNotBlock) {
  // rtsp_stream::session_count used to call clear(false) which joined Pulse
  // sample() on the nvhttp thread (Moonlight stuck on Starting Desktop, no log).
  const auto start = std::chrono::steady_clock::now();
  (void) rtsp_stream::session_count();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}

TEST(SessionLifecycle, StoppingSessionDoesNotAcceptControlPeer) {
  using stream::session::state_e;
  EXPECT_FALSE(stream::session::accepts_control_peer(state_e::STOPPING));
  EXPECT_FALSE(stream::session::accepts_control_peer(state_e::STOPPED));
  EXPECT_TRUE(stream::session::accepts_control_peer(state_e::STARTING));
  EXPECT_TRUE(stream::session::accepts_control_peer(state_e::RUNNING));
}

TEST(SessionLifecycle, ControlLoopStaysUpWhileSessionsJoin) {
  // App gone + leftover join (Pulse hang) must keep ENet serviced.
  EXPECT_FALSE(stream::session::control_loop_may_exit(false, false, 1));
  EXPECT_FALSE(stream::session::control_loop_may_exit(true, false, 0));
  EXPECT_FALSE(stream::session::control_loop_may_exit(false, true, 0));
  EXPECT_TRUE(stream::session::control_loop_may_exit(false, false, 0));
}
