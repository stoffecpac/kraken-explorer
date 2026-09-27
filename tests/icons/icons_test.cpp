#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/icons.h"

// Every embedded SVG must parse and put ink in its own cell, at both DPI scales.
TEST_CASE("every icon rasterises into its own cell")
{
    for (int scale : {1, 2})
    {
        CAPTURE(scale);
        const IconAtlasPixels px = icons_rasterize(scale);
        REQUIRE(px.cell == 16 * scale);
        REQUIRE(px.rgba.size() == static_cast<size_t>(px.width) * px.height * 4);
        const int cols = px.width / px.cell;
        for (int i = 0; i < static_cast<int>(Icon::Count); ++i)
        {
            CAPTURE(i);
            int ink = 0;
            for (int y = 0; y < px.cell; ++y)
            {
                for (int x = 0; x < px.cell; ++x)
                {
                    const size_t at = (static_cast<size_t>((i / cols) * px.cell + y) * px.width + (i % cols) * px.cell + x) * 4;
                    ink += px.rgba[at + 3] > 128 ? 1 : 0;
                }
            }
            CHECK(ink > px.cell); // more than a stray line of pixels
            CHECK(ink < px.cell * px.cell);
        }
    }
}
