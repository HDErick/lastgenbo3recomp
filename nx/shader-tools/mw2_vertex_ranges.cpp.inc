// SPDX-License-Identifier: GPL-3.0-only
// Keep this mapping in agreement with DecodeVertexFormat's layout.word[] and
// half-unpack loop. It changes upload metadata, not generated shader code.
uint32_t words = 4;
switch (f.VertexFormat()) {
case 6: case 7: case 16: case 17: case 25: case 31: case 33: case 36:
    words = 1; break;
case 26: case 32: case 34: case 37:
    words = 2; break;
case 57:
    words = 3; break;
case 35: case 38:
    words = 4; break;
}
tail = std::max(tail, uint32_t(std::max(f.Offset(), 0)) + words);
