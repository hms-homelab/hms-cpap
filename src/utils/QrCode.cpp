#include "utils/QrCode.h"

#include <cstdint>
#include <cstdlib>
#include <limits>

namespace hms_cpap::qr {

namespace {

constexpr int kMaxVersion = 10;

// Level M, per version (index 0 unused): error correction codewords per
// block, and the number of blocks.
constexpr int kEccPerBlock[kMaxVersion + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
constexpr int kBlocks[kMaxVersion + 1]      = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

// Level M's two format bits.
constexpr int kFormatBitsM = 0;

int rawDataModules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        const int align = ver / 7 + 2;
        result -= (25 * align - 10) * align - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

int dataCodewords(int ver) { return rawDataModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

// GF(256) with the standard's polynomial 0x11D.
uint8_t gfMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

std::vector<uint8_t> rsDivisor(int degree) {
    std::vector<uint8_t> result(static_cast<size_t>(degree), 0);
    result.back() = 1;
    uint8_t root  = 1;
    for (int i = 0; i < degree; i++) {
        for (size_t j = 0; j < result.size(); j++) {
            result[j] = gfMul(result[j], root);
            if (j + 1 < result.size()) result[j] ^= result[j + 1];
        }
        root = gfMul(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> rsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (const uint8_t b : data) {
        const uint8_t factor = b ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); i++) result[i] ^= gfMul(divisor[i], factor);
    }
    return result;
}

std::vector<int> alignmentPositions(int ver, int size) {
    if (ver == 1) return {};
    const int align = ver / 7 + 2;
    const int step  = (ver * 4 + align * 2 + 1) / (align * 2 - 2) * 2;
    std::vector<int> result(static_cast<size_t>(align));
    result[0] = 6;
    for (int i = align - 1, pos = size - 7; i >= 1; i--, pos -= step) result[static_cast<size_t>(i)] = pos;
    return result;
}

struct Grid {
    int size;
    std::vector<std::vector<bool>> dark;
    std::vector<std::vector<bool>> fixed;   // a function module: no data, no mask

    explicit Grid(int n)
        : size(n), dark(n, std::vector<bool>(n, false)), fixed(n, std::vector<bool>(n, false)) {}

    void set(int x, int y, bool d) {
        dark[static_cast<size_t>(y)][static_cast<size_t>(x)]  = d;
        fixed[static_cast<size_t>(y)][static_cast<size_t>(x)] = true;
    }
};

void drawFinder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= g.size || y >= g.size) continue;
            const int dist = std::max(std::abs(dx), std::abs(dy));
            g.set(x, y, dist != 2 && dist != 4);
        }
}

void drawAlignment(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) g.set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

void drawFormat(Grid& g, int mask) {
    const int data = kFormatBitsM << 3 | mask;
    int rem        = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    const int bits = (data << 10 | rem) ^ 0x5412;
    auto bit       = [bits](int i) { return ((bits >> i) & 1) != 0; };

    for (int i = 0; i <= 5; i++) g.set(8, i, bit(i));
    g.set(8, 7, bit(6));
    g.set(8, 8, bit(7));
    g.set(7, 8, bit(8));
    for (int i = 9; i < 15; i++) g.set(14 - i, 8, bit(i));

    for (int i = 0; i < 8; i++) g.set(g.size - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; i++) g.set(8, g.size - 15 + i, bit(i));
    g.set(8, g.size - 8, true);   // the dark module
}

void drawVersion(Grid& g, int ver) {
    if (ver < 7) return;
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    const long bits = static_cast<long>(ver) << 12 | rem;
    for (int i = 0; i < 18; i++) {
        const bool d = ((bits >> i) & 1) != 0;
        const int  a = g.size - 11 + i % 3, b = i / 3;
        g.set(a, b, d);
        g.set(b, a, d);
    }
}

void drawFunctionPatterns(Grid& g, int ver) {
    for (int i = 0; i < g.size; i++) {
        g.set(6, i, i % 2 == 0);
        g.set(i, 6, i % 2 == 0);
    }
    drawFinder(g, 3, 3);
    drawFinder(g, g.size - 4, 3);
    drawFinder(g, 3, g.size - 4);
    const auto pos = alignmentPositions(ver, g.size);
    const size_t n = pos.size();
    for (size_t i = 0; i < n; i++)
        for (size_t j = 0; j < n; j++) {
            if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
            drawAlignment(g, pos[i], pos[j]);
        }
    drawFormat(g, 0);   // reserves the cells; redrawn with the chosen mask
    drawVersion(g, ver);
}

void placeData(Grid& g, const std::vector<uint8_t>& codewords) {
    size_t i = 0;
    for (int right = g.size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < g.size; vert++)
            for (int j = 0; j < 2; j++) {
                const int  x      = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int  y      = upward ? g.size - 1 - vert : vert;
                if (g.fixed[static_cast<size_t>(y)][static_cast<size_t>(x)]) continue;
                if (i < codewords.size() * 8) {
                    g.dark[static_cast<size_t>(y)][static_cast<size_t>(x)] =
                        ((codewords[i >> 3] >> (7 - static_cast<int>(i & 7))) & 1) != 0;
                    i++;
                }
            }
    }
}

bool masked(int mask, int x, int y) {
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void applyMask(Grid& g, int mask) {
    for (int y = 0; y < g.size; y++)
        for (int x = 0; x < g.size; x++)
            if (!g.fixed[static_cast<size_t>(y)][static_cast<size_t>(x)] && masked(mask, x, y))
                g.dark[static_cast<size_t>(y)][static_cast<size_t>(x)] =
                    !g.dark[static_cast<size_t>(y)][static_cast<size_t>(x)];
}

// The four penalty rules, over a line read as a function of position.
long linePenalty(const std::vector<bool>& line) {
    long penalty = 0;
    // Rule 1: runs of five or more of one colour.
    int run = 1;
    for (size_t i = 1; i <= line.size(); i++) {
        if (i < line.size() && line[i] == line[i - 1]) {
            run++;
        } else {
            if (run >= 5) penalty += 3 + (run - 5);
            run = 1;
        }
    }
    // Rule 3: a finder-like 1:1:3:1:1 with four light modules on a side
    // (outside the code counts as light).
    std::vector<bool> padded(4, false);
    padded.insert(padded.end(), line.begin(), line.end());
    padded.insert(padded.end(), 4, false);
    static const bool kA[11] = {true, false, true, true, true, false, true, false, false, false, false};
    static const bool kB[11] = {false, false, false, false, true, false, true, true, true, false, true};
    for (size_t i = 0; i + 11 <= padded.size(); i++) {
        bool a = true, b = true;
        for (size_t k = 0; k < 11; k++) {
            if (padded[i + k] != kA[k]) a = false;
            if (padded[i + k] != kB[k]) b = false;
        }
        if (a) penalty += 40;
        if (b) penalty += 40;
    }
    return penalty;
}

long penalty(const Grid& g) {
    long total = 0;
    for (int y = 0; y < g.size; y++) total += linePenalty(g.dark[static_cast<size_t>(y)]);
    for (int x = 0; x < g.size; x++) {
        std::vector<bool> col(static_cast<size_t>(g.size));
        for (int y = 0; y < g.size; y++) col[static_cast<size_t>(y)] = g.dark[static_cast<size_t>(y)][static_cast<size_t>(x)];
        total += linePenalty(col);
    }
    // Rule 2: 2x2 blocks of one colour.
    for (int y = 0; y + 1 < g.size; y++)
        for (int x = 0; x + 1 < g.size; x++) {
            const bool c = g.dark[static_cast<size_t>(y)][static_cast<size_t>(x)];
            if (c == g.dark[static_cast<size_t>(y)][static_cast<size_t>(x + 1)] &&
                c == g.dark[static_cast<size_t>(y + 1)][static_cast<size_t>(x)] &&
                c == g.dark[static_cast<size_t>(y + 1)][static_cast<size_t>(x + 1)])
                total += 3;
        }
    // Rule 4: the share of dark modules away from half.
    long dark = 0;
    for (const auto& row : g.dark)
        for (const bool d : row) dark += d ? 1 : 0;
    const long all = static_cast<long>(g.size) * g.size;
    const long k   = (std::labs(dark * 20 - all * 10) + all - 1) / all - 1;
    total += std::max(0L, k) * 10;
    return total;
}

}  // namespace

std::optional<Code> encode(const std::string& text) {
    if (text.empty()) return std::nullopt;
    int ver = 0;
    for (int v = 1; v <= kMaxVersion; v++) {
        const int count_bits = v <= 9 ? 8 : 16;
        const long need      = 4 + count_bits + static_cast<long>(text.size()) * 8;
        if (need <= dataCodewords(v) * 8L) {
            ver = v;
            break;
        }
    }
    if (ver == 0) return std::nullopt;

    // The bit stream: byte mode, the count, the bytes, then the terminator
    // and padding up to the data capacity.
    std::vector<bool> bits;
    auto append = [&bits](uint32_t val, int len) {
        for (int i = len - 1; i >= 0; i--) bits.push_back(((val >> i) & 1) != 0);
    };
    append(0x4, 4);
    append(static_cast<uint32_t>(text.size()), ver <= 9 ? 8 : 16);
    for (const char c : text) append(static_cast<uint8_t>(c), 8);
    const size_t capacity = static_cast<size_t>(dataCodewords(ver)) * 8;
    append(0, static_cast<int>(std::min<size_t>(4, capacity - bits.size())));
    append(0, static_cast<int>((8 - bits.size() % 8) % 8));
    for (uint32_t pad = 0xEC; bits.size() < capacity; pad ^= 0xEC ^ 0x11) append(pad, 8);

    std::vector<uint8_t> data(bits.size() / 8, 0);
    for (size_t i = 0; i < bits.size(); i++)
        if (bits[i]) data[i >> 3] |= static_cast<uint8_t>(1 << (7 - (i & 7)));

    // Blocks, their error correction, interleaved.
    const int blocks     = kBlocks[ver];
    const int ecc_len    = kEccPerBlock[ver];
    const int raw        = rawDataModules(ver) / 8;
    const int short_n    = blocks - raw % blocks;
    const int short_len  = raw / blocks;
    const auto divisor   = rsDivisor(ecc_len);
    std::vector<std::vector<uint8_t>> parts;
    size_t at = 0;
    for (int i = 0; i < blocks; i++) {
        const size_t len = static_cast<size_t>(short_len - ecc_len + (i < short_n ? 0 : 1));
        std::vector<uint8_t> block(data.begin() + static_cast<long>(at), data.begin() + static_cast<long>(at + len));
        at += len;
        const auto ecc = rsRemainder(block, divisor);
        if (i < short_n) block.push_back(0);   // a gap, skipped when interleaving
        block.insert(block.end(), ecc.begin(), ecc.end());
        parts.push_back(std::move(block));
    }
    std::vector<uint8_t> codewords;
    for (size_t i = 0; i < parts[0].size(); i++)
        for (size_t j = 0; j < parts.size(); j++)
            if (i != static_cast<size_t>(short_len - ecc_len) || j >= static_cast<size_t>(short_n))
                codewords.push_back(parts[j][i]);

    const int size = ver * 4 + 17;
    Grid base(size);
    drawFunctionPatterns(base, ver);
    placeData(base, codewords);

    int  best_mask = 0;
    long best      = std::numeric_limits<long>::max();
    for (int m = 0; m < 8; m++) {
        Grid g = base;
        applyMask(g, m);
        drawFormat(g, m);
        const long p = penalty(g);
        if (p < best) {
            best      = p;
            best_mask = m;
        }
    }
    Grid g = base;
    applyMask(g, best_mask);
    drawFormat(g, best_mask);

    Code c;
    c.version = ver;
    c.size    = size;
    c.mask    = best_mask;
    c.modules = g.dark;
    return c;
}

std::vector<std::string> rows(const Code& code) {
    std::vector<std::string> out;
    for (const auto& row : code.modules) {
        std::string s;
        for (const bool d : row) s.push_back(d ? '1' : '0');
        out.push_back(s);
    }
    return out;
}

}  // namespace hms_cpap::qr
