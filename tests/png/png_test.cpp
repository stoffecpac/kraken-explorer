#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <zlib.h>

#include "core/png.h"

namespace
{
uint32_t be32(const std::vector<uint8_t>& v, std::size_t i)
{
    return (uint32_t{v[i]} << 24) | (uint32_t{v[i + 1]} << 16) | (uint32_t{v[i + 2]} << 8) | v[i + 3];
}
} // namespace

TEST_CASE("png_encode: signature, IHDR, deflated IDAT that inflates to the filtered rows, IEND")
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
    const std::size_t idat = 8 + 25;
    const uint32_t len = be32(png, idat);
    CHECK(std::string(png.begin() + static_cast<std::ptrdiff_t>(idat + 4), png.begin() + static_cast<std::ptrdiff_t>(idat + 8)) == "IDAT");
    // CRC of type + data, as every reader checks.
    CHECK(be32(png, idat + 8 + len) == crc32(0, png.data() + idat + 4, len + 4));
    std::vector<uint8_t> raw(2 * 9);
    uLongf raw_len = static_cast<uLongf>(raw.size());
    REQUIRE(uncompress(raw.data(), &raw_len, png.data() + idat + 8, len) == Z_OK);
    REQUIRE(raw_len == 18);
    CHECK(raw[0] == 0);   // row 0: filter None
    CHECK(raw[1] == 255); // first pixel R
    CHECK(raw[9] == 2);   // row 1: filter Up, bytes are differences to row 0
    CHECK(static_cast<uint8_t>(raw[10] + rgba[0]) == rgba[8]);
    CHECK(std::string(png.end() - 8, png.end() - 4) == "IEND");
}

TEST_CASE("png_encode: a flat 4K image stays small")
{
    const std::vector<uint8_t> rgba(std::size_t{3840} * 2160 * 4, 0x20);
    CHECK(png_encode(3840, 2160, rgba).size() < 200'000);
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
