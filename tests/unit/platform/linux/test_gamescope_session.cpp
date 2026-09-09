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

TEST(GamescopeSessionTest, MapsAbsoluteMousePixelsOntoUnitSquare) {
  EXPECT_EQ(platf::gamescope::abs_to_unit(0.0F, 0.0F, 0, 0, 1920, 1080), std::make_pair(0.0F, 0.0F));
  EXPECT_EQ(platf::gamescope::abs_to_unit(960.0F, 540.0F, 0, 0, 1920, 1080), std::make_pair(0.5F, 0.5F));
  EXPECT_EQ(platf::gamescope::abs_to_unit(1920.0F, 1080.0F, 0, 0, 1920, 1080), std::make_pair(1.0F, 1.0F));
  EXPECT_EQ(platf::gamescope::abs_to_unit(2880.0F, 540.0F, 1920, 0, 1920, 1080), std::make_pair(0.5F, 0.5F));
  EXPECT_EQ(platf::gamescope::abs_to_unit(100.0F, 100.0F, 0, 0, 0, 0), std::make_pair(0.0F, 0.0F));
}

TEST(GamescopeSessionTest, AbsoluteMousePixelsAreNotClampedAsUnitCoords) {
  // Passing desktop pixels through touch_to_window_xy would clamp 400 to 1.0.
  const auto unit = platf::gamescope::abs_to_unit(384.0F, 216.0F, 0, 0, 1920, 1080);
  EXPECT_NEAR(unit.first, 0.2F, 0.0001F);
  EXPECT_NEAR(unit.second, 0.2F, 0.0001F);
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(unit.first, unit.second, 1920, 1080), std::make_pair(384, 216));
  EXPECT_EQ(platf::gamescope::touch_to_window_xy(384.0F, 216.0F, 1920, 1080), std::make_pair(1919, 1079));
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

TEST(GamescopeSessionTest, SessionX11IsGamescopeZero) {
  EXPECT_STREQ(platf::gamescope::session_x11_name(), ":0");
}

TEST(GamescopeSessionTest, DetectsSecondDisplayPointerBit) {
  EXPECT_FALSE(platf::gamescope::touch_is_second_display(3));
  EXPECT_TRUE(platf::gamescope::touch_is_second_display(3U | platf::gamescope::TOUCH_SECOND_DISPLAY_POINTER));
  EXPECT_TRUE(platf::gamescope::touch_is_second_display(platf::gamescope::TOUCH_SECOND_DISPLAY_POINTER));
}

#endif
