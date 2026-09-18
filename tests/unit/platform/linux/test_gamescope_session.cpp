/**
 * @file tests/unit/platform/linux/test_gamescope_session.cpp
 * @brief Tests for Game Mode overlay title matching and GamePad / Azahar touch mapping.
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
  EXPECT_FALSE(platf::gamescope::title_is_gamepad_view("Azahar 2126.0 | SUPER MARIO 3D LAND | Secondary Window"));
}

TEST(GamescopeSessionTest, MatchesCemuTvAndSkipsHelper) {
  EXPECT_TRUE(platf::gamescope::title_is_cemu_tv("Cemu 2.6 - FPS: 30.00 [OpenGL] The Wind Waker HD"));
  EXPECT_FALSE(platf::gamescope::title_is_cemu_tv("GamePad View - FPS: 30.00"));
  EXPECT_FALSE(platf::gamescope::title_is_cemu_tv("Cemu_relwithdebinfo"));
}

TEST(GamescopeSessionTest, MatchesEdenHdmiSurface) {
  constexpr auto kLibrary = "Eden | v0.2.1 | Clang 22.1.6";
  constexpr auto kGame = "Eden | v0.2.1 | Clang 22.1.6 | The Legend of Zelda: Tears of the Kingdom (64-bit) | 1.4.3 | RADV";
  EXPECT_TRUE(platf::gamescope::title_is_eden(kLibrary));
  EXPECT_TRUE(platf::gamescope::title_is_eden(kGame));
  EXPECT_TRUE(platf::gamescope::title_is_hdmi_surface(kLibrary));
  EXPECT_TRUE(platf::gamescope::title_is_hdmi_surface(kGame));
  EXPECT_FALSE(platf::gamescope::title_is_eden("eden"));
  EXPECT_FALSE(platf::gamescope::title_is_eden("Qt Selection Owner for eden"));
  EXPECT_FALSE(platf::gamescope::title_is_hdmi_surface("eden"));
  EXPECT_FALSE(platf::gamescope::title_is_hdmi_surface("Qt Selection Owner for eden"));
  EXPECT_FALSE(platf::gamescope::title_is_touch_surface(kLibrary));
}

TEST(GamescopeSessionTest, MatchesAzaharTouchAndHdmiSurfaces) {
  constexpr auto kPrimary = "Azahar 2126.0 | SUPER MARIO 3D LAND | Primary Window";
  constexpr auto kSecondary = "Azahar 2126.0 | SUPER MARIO 3D LAND | Secondary Window";
  constexpr auto kLibrary = "Azahar 2126.0 | SUPER MARIO 3D LAND";
  EXPECT_TRUE(platf::gamescope::title_is_touch_surface("GamePad View - FPS: 30.00"));
  EXPECT_TRUE(platf::gamescope::title_is_touch_surface(kSecondary));
  EXPECT_FALSE(platf::gamescope::title_is_touch_surface(kPrimary));
  EXPECT_FALSE(platf::gamescope::title_is_touch_surface(kLibrary));
  EXPECT_TRUE(platf::gamescope::title_is_hdmi_surface("Cemu 2.6 - FPS: 30.00 [OpenGL] The Wind Waker HD"));
  EXPECT_TRUE(platf::gamescope::title_is_hdmi_surface(kPrimary));
  EXPECT_FALSE(platf::gamescope::title_is_hdmi_surface(kSecondary));
  EXPECT_TRUE(platf::gamescope::title_is_hdmi_surface(kLibrary));
  EXPECT_TRUE(platf::gamescope::title_is_azahar_stacked(kLibrary));
  EXPECT_FALSE(platf::gamescope::title_is_azahar_stacked(kPrimary));
  EXPECT_FALSE(platf::gamescope::title_is_azahar_stacked(kSecondary));
  EXPECT_FALSE(platf::gamescope::title_is_hdmi_surface("GamePad View - FPS: 30.00"));
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

TEST(GamescopeSessionTest, MapsGamepadPacketOntoUnitSquareWithoutLetterbox) {
  // Stream-sized ref (new Moonlight) is already 16:9 — no letterbox undo.
  const auto center_stream = platf::gamescope::packet_to_unit(960.0F, 540.0F, 1919.0F, 1079.0F);
  EXPECT_NEAR(center_stream.first, 960.0F / 1919.0F, 0.0001F);
  EXPECT_NEAR(center_stream.second, 540.0F / 1079.0F, 0.0001F);
  EXPECT_EQ(platf::gamescope::packet_to_unit(100.0F, 100.0F, 0.0F, 1080.0F), std::make_pair(0.0F, 0.0F));
}

TEST(GamescopeSessionTest, RewritesFocusDisplayMiddleWithoutStaleIds) {
  const auto live = platf::gamescope::focus_display_t {12602, 0, 68};
  const auto nested = platf::gamescope::focus_display_with_middle(live, 1);
  EXPECT_EQ(nested.server, 12602U);
  EXPECT_EQ(nested.nested, 1U);
  EXPECT_EQ(nested.token, 68U);
  const auto steam = platf::gamescope::focus_display_with_middle(nested, 0);
  EXPECT_EQ(steam.server, 12602U);
  EXPECT_EQ(steam.nested, 0U);
  EXPECT_EQ(steam.token, 68U);
}

TEST(GamescopeSessionTest, MapsThorStretchPanelRefLinearly) {
  // Live Thor Stretch packet: ref=1239x1079. y=190 is 18% down the
  // stretched 1920×1080 image, not a Fit letterbox bar.
  constexpr float kW = 1239.0F;
  constexpr float kH = 1079.0F;
  const auto mid = platf::gamescope::packet_to_unit(kW * 0.5F, kH * 0.5F, kW, kH);
  EXPECT_NEAR(mid.first, 0.5F, 0.0001F);
  EXPECT_NEAR(mid.second, 0.5F, 0.0001F);
  const auto near_top = platf::gamescope::packet_to_unit(70.0F, 190.0F, kW, kH);
  EXPECT_NEAR(near_top.first, 70.0F / kW, 0.0001F);
  EXPECT_NEAR(near_top.second, 190.0F / kH, 0.0001F);
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

TEST(GamescopeSessionTest, ParsesSecondScreenTouchSidecar) {
  const auto touch = platf::gamescope::parse_second_screen_touch("display=:0\nxid=0x60000a\n");
  ASSERT_TRUE(touch.has_value());
  EXPECT_EQ(touch->display, ":0");
  EXPECT_EQ(touch->xid, 0x60000aUL);
  EXPECT_FALSE(platf::gamescope::parse_second_screen_touch("display=:2\nxid=0x400015\n").has_value());
  EXPECT_FALSE(platf::gamescope::parse_second_screen_touch("display=:0\nxid=0\n").has_value());
  const auto decimal = platf::gamescope::parse_second_screen_touch("xid=6291466\n");
  ASSERT_TRUE(decimal.has_value());
  EXPECT_EQ(decimal->display, ":0");
  EXPECT_EQ(decimal->xid, 6291466UL);
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

TEST(GamescopeSessionTest, SessionX11TouchPrefersFocusDisplayOne) {
  EXPECT_EQ(platf::gamescope::session_x11_touch_count(), 2U);
  EXPECT_STREQ(platf::gamescope::session_x11_touch_name(0), ":1");
  EXPECT_STREQ(platf::gamescope::session_x11_touch_name(1), ":0");
  EXPECT_EQ(platf::gamescope::session_x11_touch_name(2), nullptr);
}

TEST(GamescopeSessionTest, DetectsSecondDisplayPointerBit) {
  EXPECT_FALSE(platf::gamescope::touch_is_second_display(3));
  EXPECT_TRUE(platf::gamescope::touch_is_second_display(3U | platf::gamescope::TOUCH_SECOND_DISPLAY_POINTER));
  EXPECT_TRUE(platf::gamescope::touch_is_second_display(platf::gamescope::TOUCH_SECOND_DISPLAY_POINTER));
}

TEST(GamescopeSessionTest, AbsTargetsGamepadViewForDualStreamAndGamepadOnly) {
  EXPECT_TRUE(platf::gamescope::abs_targets_gamepad_view(1, false));
  EXPECT_TRUE(platf::gamescope::abs_targets_gamepad_view(1, true));
  EXPECT_TRUE(platf::gamescope::abs_targets_gamepad_view(0, true));
  EXPECT_FALSE(platf::gamescope::abs_targets_gamepad_view(0, false));
}

TEST(GamescopeSessionTest, AbsTargetsHdmiSurfaceOnlyOnDisplay0WithoutOdinHack) {
  EXPECT_TRUE(platf::gamescope::abs_targets_hdmi_surface(0, false));
  EXPECT_FALSE(platf::gamescope::abs_targets_hdmi_surface(0, true));
  EXPECT_FALSE(platf::gamescope::abs_targets_hdmi_surface(1, false));
  EXPECT_FALSE(platf::gamescope::abs_targets_hdmi_surface(1, true));
}

TEST(GamescopeSessionTest, AbsTargetsSteamOverlayOnlyOnDisplay0WhenOverlayOn) {
  EXPECT_TRUE(platf::gamescope::abs_targets_steam_overlay(0, false, true));
  EXPECT_FALSE(platf::gamescope::abs_targets_steam_overlay(0, false, false));
  EXPECT_FALSE(platf::gamescope::abs_targets_steam_overlay(0, true, true));
  EXPECT_FALSE(platf::gamescope::abs_targets_steam_overlay(1, false, true));
}

#endif
