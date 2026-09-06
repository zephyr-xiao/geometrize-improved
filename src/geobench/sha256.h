// 轻量 SHA-256 实现(公有领域算法,自写避免引入 OpenSSL 依赖)
// 仅用于 A/B 对拍的指纹计算,非性能敏感路径
#ifndef GEO_SHA256_H
#define GEO_SHA256_H

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace geobench {

class Sha256 {
public:
    Sha256() : m_state{0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
                      0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL},
               m_bitLen(0), m_bufferLen(0)
    {}

    void update(const std::uint8_t* data, std::size_t len)
    {
        m_bitLen += static_cast<std::uint64_t>(len) * 8U;
        while(len > 0) {
            const std::size_t take = (std::min)(len, sizeof(m_buffer) - m_bufferLen);
            std::memcpy(m_buffer + m_bufferLen, data, take);
            m_bufferLen += take;
            data += take;
            len -= take;
            if(m_bufferLen == sizeof(m_buffer)) {
                processBlock(m_buffer);
                m_bufferLen = 0;
            }
        }
    }

    std::string hex()
    {
        // 补齐填充:0x80 + 零 + 64 位大端长度
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while(m_bufferLen != 56) {
            update(&pad, 1);
        }
        std::uint8_t lengthBytes[8];
        const std::uint64_t bits = m_bitLen - 64U; // 减去本函数已计入的填充位数
        for(int i = 0; i < 8; i++) {
            lengthBytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }
        // 直接塞块,不再走 update(避免再次累计 bitLen)
        std::memcpy(m_buffer + 56, lengthBytes, 8);
        processBlock(m_buffer);

        std::string out;
        out.reserve(64);
        char buf[3];
        for(std::uint32_t word : m_state) {
            for(int shift = 24; shift >= 0; shift -= 8) {
                std::snprintf(buf, sizeof(buf), "%02x", (word >> shift) & 0xffU);
                out += buf;
            }
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void processBlock(const std::uint8_t* block)
    {
        static constexpr std::uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::uint32_t w[64];
        for(int i = 0; i < 16; i++) {
            w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24)
                 | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[4 * i + 2]) << 8)
                 | static_cast<std::uint32_t>(block[4 * i + 3]);
        }
        for(int i = 16; i < 64; i++) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a{m_state[0]}, b{m_state[1]}, c{m_state[2]}, d{m_state[3]};
        std::uint32_t e{m_state[4]}, f{m_state[5]}, g{m_state[6]}, h{m_state[7]};

        for(int i = 0; i < 64; i++) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + S1 + ch + K[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;

        m_bufferLen = 0;
    }

    std::uint32_t m_state[8];
    std::uint64_t m_bitLen;
    std::size_t m_bufferLen;
    std::uint8_t m_buffer[64];
};

inline std::string sha256Hex(const std::vector<std::uint8_t>& data)
{
    Sha256 h;
    h.update(data.data(), data.size());
    return h.hex();
}

}

#endif
