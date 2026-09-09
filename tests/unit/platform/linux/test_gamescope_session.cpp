/**
 * @file tests/unit/platform/linux/test_gamescope_session.cpp
 * @brief Tests for Game Mode overlay title matching and GamePad touch mapping.
 */
#ifdef __linux__
  // test includes
  #include "../../../tests_common.h"

  // standard includes
  #include <cstdint>
  #include <optional>
  #include <utility>

  // local includes
  #include "src/platform/linux/gamescope_session.h"

TEST(GamescopeSessionTest, MatchesSteamBigPictureTitle) {
  EXPECT_TRUE(platf::gamescope::title_is_steam_big_picture("Steam Big Picture Mode"));
  EXPECT_FALSE(platf::gamescope::title_is_steam_big_picture("Cemu 2.6 - FPS: 30.00"));
}

TEST(GamescopeSessionTest, MatchesGamepadViewTitle) {
  EXPECT_TRUE(platf::gamescope::title_is_gamepad_view("GamePad View - FPS: 30.00"));
  EXPECT_FALSE(platf::gamescope::title_is_gamepad_view("Cemu 2.6 - FPS: 30.00"));
}

TEST(GamescopeSessionTest, MatchesCemuTvAndSkipsHelper) {
  EXPECT_TRUE(platf::gamescope::title_is_cemu_tv("Cemu 2.6 - FPS: 30.00 [OpenGL] The Wind Waker HD"));
  EXPECT_FALSE(platf::gamescope::title_is_cemu_tv("GamePad View - FPS: 30.00"));
  EXPECT_FALSE(platf::gamescope::title_is_cemu_tv("Cemu_relwithdebinfo"));
}

TEST(GamescopeSessionTest, MapsNormalizedTouchOntoWindowPixels) {
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(0.0F, 0.0F, 1920, 1080), std::make_pair(0, 0));
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(1.0F, 1.0F, 1920, 1080), std::make_pair(1919, 1079));
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(0.5F, 0.5F, 1920, 1080), std::make_pair(960, 540));
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(-1.0F, 2.0F, 100, 100), std::make_pair(0, 99));
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(0.5F, 0.5F, 0, 0), std::make_pair(0, 0));
}

TEST(GamescopeSessionTest, TogglesOverlayAction) {
  EXPECT_EQ(platf::gamescope::overlay_toggle_action(false), platf::gamescope::overlay_action_e::show);
  EXPECT_EQ(platf::gamescope::overlay_toggle_action(true), platf::gamescope::overlay_action_e::hide);
}

TEST(GamescopeSessionTest, ResolvesOverlayAppid) {
  EXPECT_EQ(platf::gamescope::resolve_overlay_appid(2374129079U, 769U, 2374129079U), 2374129079U);
  EXPECT_EQ(platf::gamescope::resolve_overlay_appid(std::nullopt, 769U, 2374129079U), 2374129079U);
  EXPECT_EQ(platf::gamescope::resolve_overlay_appid(std::nullopt, 2374129079U, 769U), 2374129079U);
  EXPECT_EQ(platf::gamescope::resolve_overlay_appid(std::nullopt, 769U, 769U), 0U);
  EXPECT_EQ(platf::gamescope::resolve_overlay_appid(769U, 769U, 769U), 0U);
}

TEST(GamescopeSessionTest, DetectsSecondDisplayPointerBit) {
  EXPECT_FALSE(platf::gamescope::touch_is_second_display(3));
  EXPECT_TRUE(platf::gamescope::touch_is_second_display(3U | platf::gamescope::TOUCH_SECOND_DISPLAY_POINTER));
  EXPECT_TRUE(platf::gamescope::touch_is_second_display(platf::gamescope::TOUCH_SECOND_DISPLAY_POINTER));
}

#endif
