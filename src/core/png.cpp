#include "core/png.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <string_view>

namespace
{

constexpr std::array<uint32_t, 256> crc_table = []
{
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n)
    {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k)
        {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[n] = c;
    }
    return t;
}();

uint32_t crc32(std::span<const uint8_t> bytes, uint32_t crc = 0xFFFFFFFFu)
{
    for (const uint8_t b : bytes)
    {
        crc = crc_table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.insert(out.end(), {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                           static_cast<uint8_t>(v)});
}

void chunk(std::vector<uint8_t>& out, std::string_view type, std::span<const uint8_t> data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(std::span(out).subspan(start)) ^ 0xFFFFFFFFu);
}

} // namespace

std::vector<uint8_t> png_encode(int w, int h, std::span<const uint8_t> rgba)
{
    const std::size_t stride = static_cast<std::size_t>(w) * 4;
    // Filter byte 0 (None) in front of every row.
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<std::ptrdiff_t>(y * stride),
                   rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * stride));
    }
    // zlib stream of stored deflate blocks (max 65535 bytes each) + Adler-32.
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1;
    uint32_t b = 0;
    for (const uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (std::size_t pos = 0; pos < raw.size() || pos == 0;)
    {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.insert(z.end(), {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(~n),
                           static_cast<uint8_t>(~n >> 8)});
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
        pos += n;
        if (last)
        {
            break;
        }
    }
    put32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(w));
    put32(ihdr, static_cast<uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8 bit, RGBA, deflate, no filter method, no interlace
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

bool png_write_file(const std::filesystem::path& path, int w, int h, std::span<const uint8_t> rgba, std::string* error)
{
    const std::vector<uint8_t> bytes = png_encode(w, h, rgba);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        if (error != nullptr)
        {
            *error = std::format("cannot write {}", path.string());
        }
        return false;
    }
    return true;
}
