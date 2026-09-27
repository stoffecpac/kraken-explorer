#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string_view>

#include "ui/depth_gauge.h"

// Zone limits: NOAA pelagic zones; Challenger Deep 10 994 m (GEBCO/Five Deeps 2019).
TEST_CASE("fraction maps to Mariana Trench depth")
{
    CHECK(depth_m(0.0f) == 0.0f);
    CHECK(depth_m(1.0f) == doctest::Approx(10994.0f));
    CHECK(depth_m(0.5f) == doctest::Approx(5497.0f));
    CHECK(depth_m(-1.0f) == 0.0f);
    CHECK(depth_m(2.0f) == doctest::Approx(10994.0f));
}

TEST_CASE("pelagic zones")
{
    CHECK(std::string_view(depth_zone(0.0f)) == "Epipelagic");
    CHECK(std::string_view(depth_zone(199.0f)) == "Epipelagic");
    CHECK(std::string_view(depth_zone(200.0f)) == "Mesopelagic");
    CHECK(std::string_view(depth_zone(1000.0f)) == "Bathypelagic");
    CHECK(std::string_view(depth_zone(4213.0f)) == "Abyssopelagic");
    CHECK(std::string_view(depth_zone(6000.0f)) == "Hadal");
    CHECK(std::string_view(depth_zone(10994.0f)) == "Hadal");
}

TEST_CASE("draws inside a frame")
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {800, 600};
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::NewFrame();
    ImGui::Begin("gauge");
    draw_depth_gauge(0.383f, {-1.0f, 0.0f});
    CHECK(ImGui::GetItemRectSize().y == doctest::Approx(depth_gauge_min_height()));
    CHECK(depth_gauge_min_height() >= ImGui::GetFontSize() * 5.0f); // room for the 2.2x depth readout
    ImGui::End();
    ImGui::EndFrame();
    ImGui::DestroyContext();
}
