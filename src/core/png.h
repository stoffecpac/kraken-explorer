#pragma once

// Minimal PNG writer for screenshots: RGBA8, rows top-down, zlib "stored" blocks (no
// compression: a graph capture is a few MB at most and no extra dependency is needed).

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

// The encoded file for a w x h RGBA8 image (rgba.size() == w * h * 4).
[[nodiscard]] std::vector<uint8_t> png_encode(int w, int h, std::span<const uint8_t> rgba);
// Writes png_encode() to path; false with *error set when the file cannot be written.
[[nodiscard]] bool png_write_file(const std::filesystem::path& path, int w, int h, std::span<const uint8_t> rgba,
                                  std::string* error);
