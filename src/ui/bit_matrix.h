#pragma once

struct CanDbMessage;

// The Layout View grid (old BitMatrixWidget): bytes as rows, bits 7..0 as columns, each
// signal a coloured block per byte. cell_size is the row height in logical pixels.
// msg may be null (empty 8-byte grid).
void draw_bit_matrix(const CanDbMessage* msg, float cell_size);
