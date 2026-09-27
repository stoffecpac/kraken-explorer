#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/png.h"

namespace
{
uint32_t be32(const std::vector<uint8_t>& v, std::size_t i)
{
    return (uint32_t{v[i]} << 24) | (uint32_t{v[i + 1]} << 16) | (uint32_t{v[i + 2]} << 8) | v[i + 3];
}
} // namespace

TEST_CASE("png_encode: signature, IHDR, one stored zlib block, IEND")
{
    const std::vector<uint8_t> rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}; // 2x2
    const std::vector<uint8_t> png = png_encode(2, 2, rgba);
    REQUIRE(png.size() > 8 + 25 + 12);
    CHECK(std::string(png.begin(), png.begin() + 8) == std::string("\x89PNG\r\n\x1a\n", 8));
    CHECK(be32(png, 8) == 13);
    CHECK(std::string(png.begin() + 12, png.begin() + 16) == "IHDR");
    CHECK(be32(png, 16) == 2);
    CHECK(be32(png, 20) == 2);
    CHECK(png[24] == 8); // bit depth
    CHECK(png[25] == 6); // RGBA
    // IDAT: zlib header (2) + stored block header (5) + 2 rows of (1 filter + 8 bytes) + adler (4)
    const std::size_t idat = 8 + 25;
    CHECK(be32(png, idat) == 2 + 5 + 18 + 4);
    CHECK(std::string(png.begin() + static_cast<std::ptrdiff_t>(idat + 4), png.begin() + static_cast<std::ptrdiff_t>(idat + 8)) == "IDAT");
    CHECK(png[idat + 8] == 0x78);
    CHECK(png[idat + 10] == 1);  // final stored block
    CHECK(png[idat + 11] == 18); // LEN
    CHECK(png[idat + 13] == static_cast<uint8_t>(~18));
    CHECK(png[idat + 15] == 0);   // filter None
    CHECK(png[idat + 16] == 255); // first pixel R
    CHECK(std::string(png.end() - 8, png.end() - 4) == "IEND");
    CHECK(png.size() == idat + 12 + 29 + 12);
}

TEST_CASE("png_write_file writes the bytes and reports an unwritable path")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "kraken_png_test.png";
    const std::vector<uint8_t> rgba(4 * 3 * 4, 0x80);
    std::string error;
    REQUIRE(png_write_file(path, 4, 3, rgba, &error));
    CHECK(std::filesystem::file_size(path) == png_encode(4, 3, rgba).size());
    std::filesystem::remove(path);
    CHECK_FALSE(png_write_file("/nonexistent-dir/x.png", 4, 3, rgba, &error));
    CHECK(!error.empty());
}
