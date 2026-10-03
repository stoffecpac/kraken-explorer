#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string_view>

#include "ui/depth_gauge.h"

// Zone limits: NOAA pelagic zones; Challenger Deep 10 994 m (GEBCO/Five Deeps 2019).
TEST_CASE("draws inside a frame")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {800, 600};
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    ImGui::Begin("gauge");
    draw_depth_gauge(0.0f, 0.383f, {-1.0f, 0.0f}, false);
    CHECK(ImGui::GetItemRectSize().y == doctest::Approx(depth_gauge_min_height()));
    CHECK(depth_gauge_min_height() >= ImGui::GetFontSize() * 1.5f); // room for the percent text
    ImGui::End();
    ImGui::EndFrame();
    ImGui::DestroyContext();
}
