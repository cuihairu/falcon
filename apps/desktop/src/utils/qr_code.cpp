/**
 * @file qr_code.cpp
 * @brief 轻量 QR Code 生成器实现（字节模式 / 纠错级别 M / 版本 1-3）
 *
 * 算法要点：
 *  - GF(256) 用本原多项式 0x11D，Reed-Solomon 生成 ECC
 *  - 版本 1-3 / 级别 M 均为单块，无需交错
 *  - 8 种掩码按罚分选最优，掩码号写入格式信息
 */

#include "qr_code.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <vector>

namespace falcon::desktop {
namespace {

using Byte = std::uint8_t;

/** GF(256) 乘法（本原多项式 x^8+x^4+x^3+x^2+1 = 0x11D） */
Byte gf_mul(Byte x, Byte y)
{
    Byte z = 0;
    for (int i = 7; i >= 0; --i) {
        z = static_cast<Byte>((z << 1) ^ ((z >> 7) * 0x11D));
        z ^= static_cast<Byte>(((y >> i) & 1) * x);
    }
    return z;
}

/** Reed-Solomon 除式（生成多项式，首一，长度 degree） */
std::vector<Byte> rs_divisor(int degree)
{
    std::vector<Byte> result(static_cast<std::size_t>(degree) - 1, 0);
    result.push_back(1);
    Byte root = 1;
    for (int i = 0; i < degree; ++i) {
        for (std::size_t j = 0; j < result.size(); ++j) {
            result[j] = gf_mul(result[j], root);
            if (j + 1 < result.size()) {
                result[j] ^= result[j + 1];
            }
        }
        root = gf_mul(root, 2);
    }
    return result;
}

/** Reed-Solomon 余数（ECC 码字） */
std::vector<Byte> rs_remainder(const std::vector<Byte>& data,
                               const std::vector<Byte>& divisor)
{
    std::vector<Byte> result(divisor.size(), 0);
    for (Byte b : data) {
        const Byte factor = static_cast<Byte>(b ^ result.front());
        result.erase(result.begin());
        result.push_back(0);
        for (std::size_t i = 0; i < divisor.size(); ++i) {
            result[i] ^= gf_mul(divisor[i], factor);
        }
    }
    return result;
}

/** 版本 v 的 ECC 码字数（级别 M，单块） */
int ecc_codewords(int version)
{
    static const int kEcc[] = {0, 10, 16, 26};
    return kEcc[version];
}

/** 版本 v 的数据码字数（级别 M，单块） */
int data_codewords(int version)
{
    static const int kData[] = {0, 16, 28, 44};
    return kData[version];
}

/** 字节模式最大可编码字节数（扣掉模式位 4 + 计数位 8） */
int byte_capacity(int version)
{
    return data_codewords(version) - 2;
}

struct BitBuffer {
    std::vector<bool> bits;

    void append(int value, int length)
    {
        for (int i = length - 1; i >= 0; --i) {
            bits.push_back(((value >> i) & 1) != 0);
        }
    }
};

struct QrMatrix {
    int size = 0;
    std::vector<std::vector<bool>> modules;
    std::vector<std::vector<bool>> is_function;

    explicit QrMatrix(int dimension)
        : size(dimension)
        , modules(static_cast<std::size_t>(dimension),
                  std::vector<bool>(static_cast<std::size_t>(dimension), false))
        , is_function(static_cast<std::size_t>(dimension),
                      std::vector<bool>(static_cast<std::size_t>(dimension), false))
    {
    }

    void set_function(int x, int y, bool dark)
    {
        modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = dark;
        is_function[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = true;
    }

    bool function_at(int x, int y) const
    {
        return is_function[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
    }
};

void draw_finder(QrMatrix& qr, int x, int y)
{
    for (int dy = -4; dy <= 4; ++dy) {
        for (int dx = -4; dx <= 4; ++dx) {
            const int px = x + dx;
            const int py = y + dy;
            if (px < 0 || px >= qr.size || py < 0 || py >= qr.size) {
                continue;
            }
            const int dist = std::max(std::abs(dx), std::abs(dy));
            qr.set_function(px, py, dist != 2 && dist != 4);
        }
    }
}

void draw_alignment(QrMatrix& qr, int x, int y)
{
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int dist = std::max(std::abs(dx), std::abs(dy));
            qr.set_function(x + dx, y + dy, dist != 1);
        }
    }
}

void draw_format_bits(QrMatrix& qr, int mask)
{
    // 纠错级别 M 的格式位 = 0b00；数据 = (ecl << 3) | mask = mask
    int data = mask;
    int rem = data;
    for (int i = 0; i < 10; ++i) {
        rem = ((rem << 1) ^ ((rem >> 9) * 0x537)) & 0x7FF;
    }
    const int bits = ((data << 10) | rem) ^ 0x5412;
    auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };

    for (int i = 0; i <= 5; ++i) {
        qr.set_function(8, i, bit(i));
    }
    qr.set_function(8, 7, bit(6));
    qr.set_function(8, 8, bit(7));
    qr.set_function(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) {
        qr.set_function(14 - i, 8, bit(i));
    }
    for (int i = 0; i < 8; ++i) {
        qr.set_function(qr.size - 1 - i, 8, bit(i));
    }
    for (int i = 8; i < 15; ++i) {
        qr.set_function(8, qr.size - 15 + i, bit(i));
    }
    qr.set_function(8, qr.size - 8, true); // 固定黑点
}

void draw_function_patterns(QrMatrix& qr, int version)
{
    const int size = qr.size;
    draw_finder(qr, 3, 3);
    draw_finder(qr, size - 4, 3);
    draw_finder(qr, 3, size - 4);

    if (version >= 2) {
        const int pos = version == 2 ? 18 : 22;
        draw_alignment(qr, pos, pos);
    }

    for (int i = 8; i < size - 8; ++i) {
        if (!qr.function_at(6, i)) {
            qr.set_function(i, 6, i % 2 == 0);
        }
        if (!qr.function_at(i, 6)) {
            qr.set_function(6, i, i % 2 == 0);
        }
    }

    // 预留格式信息区（值稍后由 draw_format_bits 写入）
    for (int i = 0; i < 9; ++i) {
        if (!qr.function_at(8, i)) {
            qr.set_function(8, i, false);
        }
        if (!qr.function_at(i, 8)) {
            qr.set_function(i, 8, false);
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (!qr.function_at(size - 1 - i, 8)) {
            qr.set_function(size - 1 - i, 8, false);
        }
        if (!qr.function_at(8, size - 1 - i)) {
            qr.set_function(8, size - 1 - i, false);
        }
    }
}

void draw_codewords(QrMatrix& qr, const std::vector<Byte>& codewords)
{
    int bit_index = 0;
    const int total_bits = static_cast<int>(codewords.size()) * 8;
    for (int right = qr.size - 1; right >= 1; right -= 2) {
        if (right == 6) {
            right = 5;
        }
        for (int vert = 0; vert < qr.size; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? qr.size - 1 - vert : vert;
                if (!qr.function_at(x, y) && bit_index < total_bits) {
                    const bool dark = ((codewords[static_cast<std::size_t>(bit_index / 8)]
                                        >> (7 - (bit_index % 8))) & 1) != 0;
                    qr.modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] = dark;
                    ++bit_index;
                }
            }
        }
    }
}

bool mask_bit(int mask, int x, int y)
{
    switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        case 7: return ((x + y) % 2 + x * y % 3) % 2 == 0;
        default: return false;
    }
}

void apply_mask(QrMatrix& qr, int mask)
{
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.function_at(x, y) && mask_bit(mask, x, y)) {
                qr.modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] =
                    !qr.modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            }
        }
    }
}

bool finder_like(const std::vector<bool>& line, int pos)
{
    static const int kPattern[] = {1, 0, 1, 1, 1, 0, 1};
    if (pos < 0 || pos + 7 > static_cast<int>(line.size())) {
        return false;
    }
    for (int k = 0; k < 7; ++k) {
        if ((line[static_cast<std::size_t>(pos + k)] ? 1 : 0) != kPattern[k]) {
            return false;
        }
    }
    const bool left_light = pos >= 4 && !line[static_cast<std::size_t>(pos - 1)]
                            && !line[static_cast<std::size_t>(pos - 2)]
                            && !line[static_cast<std::size_t>(pos - 3)]
                            && !line[static_cast<std::size_t>(pos - 4)];
    const bool right_light = pos + 11 <= static_cast<int>(line.size())
                             && !line[static_cast<std::size_t>(pos + 7)]
                             && !line[static_cast<std::size_t>(pos + 8)]
                             && !line[static_cast<std::size_t>(pos + 9)]
                             && !line[static_cast<std::size_t>(pos + 10)];
    return left_light || right_light;
}

/** 掩码罚分（越低越好），四条规定 N1-N4 */
int penalty(const QrMatrix& qr)
{
    int score = 0;
    const int size = qr.size;

    for (int i = 0; i < size; ++i) {
        std::vector<bool> row(static_cast<std::size_t>(size));
        std::vector<bool> col(static_cast<std::size_t>(size));
        for (int j = 0; j < size; ++j) {
            row[static_cast<std::size_t>(j)] = qr.modules[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
            col[static_cast<std::size_t>(j)] = qr.modules[static_cast<std::size_t>(j)][static_cast<std::size_t>(i)];
        }
        for (const auto* line : {&row, &col}) {
            int run = 1;
            for (int j = 1; j < size; ++j) {
                if ((*line)[static_cast<std::size_t>(j)] == (*line)[static_cast<std::size_t>(j - 1)]) {
                    ++run;
                } else {
                    if (run >= 5) {
                        score += run - 2;
                    }
                    run = 1;
                }
            }
            if (run >= 5) {
                score += run - 2;
            }
            for (int j = 0; j + 7 <= size; ++j) {
                if (finder_like(*line, j)) {
                    score += 40;
                }
            }
        }
    }

    for (int y = 0; y + 1 < size; ++y) {
        for (int x = 0; x + 1 < size; ++x) {
            const bool c = qr.modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)];
            if (c == qr.modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x + 1)]
                && c == qr.modules[static_cast<std::size_t>(y + 1)][static_cast<std::size_t>(x)]
                && c == qr.modules[static_cast<std::size_t>(y + 1)][static_cast<std::size_t>(x + 1)]) {
                score += 3;
            }
        }
    }

    int dark = 0;
    for (const auto& row : qr.modules) {
        dark += static_cast<int>(std::count(row.begin(), row.end(), true));
    }
    const int total = size * size;
    score += (std::abs(dark * 20 - total * 10) / total) * 10;
    return score;
}

QrMatrix build_matrix(const QByteArray& payload, int version)
{
    QrMatrix qr(version * 4 + 17);

    // 1) 数据编码：字节模式 + 字符计数 + 数据 + 终止符 + 对齐 + 填充
    BitBuffer buffer;
    buffer.append(4, 4); // 字节模式 0100
    buffer.append(payload.size(), 8);
    for (char ch : payload) {
        buffer.append(static_cast<unsigned char>(ch), 8);
    }
    const int capacity_bits = data_codewords(version) * 8;
    buffer.append(0, std::min(4, capacity_bits - static_cast<int>(buffer.bits.size())));
    while (buffer.bits.size() % 8 != 0) {
        buffer.append(0, 1);
    }

    std::vector<Byte> data;
    for (std::size_t i = 0; i < buffer.bits.size(); i += 8) {
        int value = 0;
        for (int j = 0; j < 8; ++j) {
            value = (value << 1) | (buffer.bits[i + static_cast<std::size_t>(j)] ? 1 : 0);
        }
        data.push_back(static_cast<Byte>(value));
    }
    int pad_index = 0;
    while (static_cast<int>(data.size()) < data_codewords(version)) {
        data.push_back(pad_index++ % 2 == 0 ? 0xEC : 0x11);
    }

    // 2) 纠错码字（级别 M，版本 1-3 单块，无需交错）
    const auto divisor = rs_divisor(ecc_codewords(version));
    const auto ecc = rs_remainder(data, divisor);
    std::vector<Byte> codewords = data;
    codewords.insert(codewords.end(), ecc.begin(), ecc.end());

    // 3) 功能图形 + 数据摆放
    draw_function_patterns(qr, version);
    draw_codewords(qr, codewords);

    // 4) 选最优掩码
    int best_mask = 0;
    int best_score = -1;
    for (int mask = 0; mask < 8; ++mask) {
        apply_mask(qr, mask);
        draw_format_bits(qr, mask);
        const int score = penalty(qr);
        if (best_score < 0 || score < best_score) {
            best_score = score;
            best_mask = mask;
        }
        apply_mask(qr, mask); // 撤销，恢复到未掩码状态
    }
    apply_mask(qr, best_mask);
    draw_format_bits(qr, best_mask);
    return qr;
}

} // namespace

QImage make_qr_image(const QString& text, int border, int scale)
{
    const QByteArray payload = text.toUtf8();
    if (payload.isEmpty() || border < 0 || scale < 1) {
        return {};
    }

    int version = 0;
    for (int v = 1; v <= 3; ++v) {
        if (payload.size() <= byte_capacity(v)) {
            version = v;
            break;
        }
    }
    if (version == 0) {
        return {};
    }

    const QrMatrix qr = build_matrix(payload, version);
    const int quiet = std::max(4, border);
    const int dimension = (qr.size + quiet * 2) * scale;
    QImage image(dimension, dimension, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < qr.size; ++y) {
        for (int x = 0; x < qr.size; ++x) {
            if (!qr.modules[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)]) {
                continue;
            }
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    image.setPixel((x + quiet) * scale + dx,
                                   (y + quiet) * scale + dy, 0xFF000000u);
                }
            }
        }
    }
    return image;
}

} // namespace falcon::desktop
