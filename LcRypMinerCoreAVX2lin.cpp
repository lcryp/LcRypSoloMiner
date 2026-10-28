// ============================================================
// LcRyp Solo Miner - CPU Core (AVX2 Linear)
// https://lcryp.com
// Version 2.2
// ============================================================

#include <stdint.h>
#include <immintrin.h>
#include <atomic>
#include <intrin.h>

// Optimizations for MSVC
#define ADD(a, b) _mm256_add_epi32(a, b)
#define ROR(x, n) _mm256_or_si256(_mm256_srli_epi32(x, n), _mm256_slli_epi32(x, 32 - n))
#define S0(x) _mm256_xor_si256(ROR(x, 2), _mm256_xor_si256(ROR(x, 13), ROR(x, 22)))
#define S1(x) _mm256_xor_si256(ROR(x, 6), _mm256_xor_si256(ROR(x, 11), ROR(x, 25)))
#define s0(x) _mm256_xor_si256(ROR(x, 7), _mm256_xor_si256(ROR(x, 18), _mm256_srli_epi32(x, 3)))
#define s1(x) _mm256_xor_si256(ROR(x, 17), _mm256_xor_si256(ROR(x, 19), _mm256_srli_epi32(x, 10)))
#define CH(x, y, z) _mm256_xor_si256(_mm256_and_si256(x, y), _mm256_andnot_si256(x, z))
#define MAJ(x, y, z) _mm256_xor_si256(_mm256_and_si256(x, y), _mm256_xor_si256(_mm256_and_si256(x, z), _mm256_and_si256(y, z)))

// Atomic counter for sequential enumeration
static std::atomic<uint32_t> global_nonce_counter(0);

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

__forceinline void sha256_round_8(__m256i* state, const __m256i* data) {
    __m256i w[64];
    for (int i = 0; i < 16; i++) w[i] = data[i];
    for (int i = 16; i < 64; i++) w[i] = ADD(s1(w[i - 2]), ADD(w[i - 7], ADD(s0(w[i - 15]), w[i - 16])));
    __m256i a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        __m256i t1 = ADD(h, ADD(S1(e), ADD(CH(e, f, g), ADD(_mm256_set1_epi32(K[i]), w[i]))));
        __m256i t2 = ADD(S0(a), MAJ(a, b, c));
        h = g; g = f; f = e; e = ADD(d, t1); d = c; c = b; b = a; a = ADD(t1, t2);
    }
    state[0] = ADD(state[0], a); state[1] = ADD(state[1], b); state[2] = ADD(state[2], c); state[3] = ADD(state[3], d);
    state[4] = ADD(state[4], e); state[5] = ADD(state[5], f); state[6] = ADD(state[6], g); state[7] = ADD(state[7], h);
}

extern "C" __declspec(dllexport) uint32_t run_mining_batch(
    uint32_t* header, 
    uint32_t start_nonce,  // Ignored
    uint32_t iterations, 
    uint32_t* target
) {
    if (!header || !target) return 0;

    // Ignore input start_nonce, always start from global counter
    uint32_t base_nonce = global_nonce_counter.fetch_add(iterations);

    // Pre-cache header
    __m256i h_data[16];
    for (int j = 0; j < 16; j++) h_data[j] = _mm256_set1_epi32(_byteswap_ulong(header[j]));
    __m256i h_ext[3];
    for (int j = 0; j < 3; j++) h_ext[j] = _mm256_set1_epi32(_byteswap_ulong(header[16 + j]));

    uint32_t groups = iterations / 8;
    for (uint32_t g = 0; g < groups; g++) {
        uint32_t n[8];
        
        // SEQUENTIAL INCREMENT: 0,1,2,3,4,5,6,7 for each group of 8
        for (int i = 0; i < 8; i++) {
            n[i] = base_nonce + g * 8 + i;
        }

        __m256i v_nonces = _mm256_set_epi32(
            _byteswap_ulong(n[7]), _byteswap_ulong(n[6]), _byteswap_ulong(n[5]), _byteswap_ulong(n[4]),
            _byteswap_ulong(n[3]), _byteswap_ulong(n[2]), _byteswap_ulong(n[1]), _byteswap_ulong(n[0])
        );

        // SHA256 Pass 1
        __m256i state[8] = { 
            _mm256_set1_epi32(0x6a09e667), _mm256_set1_epi32(0xbb67ae85), _mm256_set1_epi32(0x3c6ef372), _mm256_set1_epi32(0xa54ff53a), 
            _mm256_set1_epi32(0x510e527f), _mm256_set1_epi32(0x9b05688c), _mm256_set1_epi32(0x1f83d9ab), _mm256_set1_epi32(0x5be0cd19) 
        };
        sha256_round_8(state, h_data);

        __m256i data_ext[16];
        data_ext[0] = h_ext[0]; data_ext[1] = h_ext[1]; data_ext[2] = h_ext[2];
        data_ext[3] = v_nonces;
        data_ext[4] = _mm256_set1_epi32(0x80000000);
        for (int j = 5; j < 15; j++) data_ext[j] = _mm256_setzero_si256();
        data_ext[15] = _mm256_set1_epi32(640);
        sha256_round_8(state, data_ext);

        // SHA256 Pass 2
        __m256i state2[8] = { 
            _mm256_set1_epi32(0x6a09e667), _mm256_set1_epi32(0xbb67ae85), _mm256_set1_epi32(0x3c6ef372), _mm256_set1_epi32(0xa54ff53a), 
            _mm256_set1_epi32(0x510e527f), _mm256_set1_epi32(0x9b05688c), _mm256_set1_epi32(0x1f83d9ab), _mm256_set1_epi32(0x5be0cd19) 
        };
        __m256i data2[16];
        for (int j = 0; j < 8; j++) data2[j] = state[j];
        data2[8] = _mm256_set1_epi32(0x80000000);
        for (int j = 9; j < 15; j++) data2[j] = _mm256_setzero_si256();
        data2[15] = _mm256_set1_epi32(256);
        sha256_round_8(state2, data2);

        alignas(32) uint32_t res[8][8];
        for (int j = 0; j < 8; j++) _mm256_store_si256((__m256i*)res[j], state2[j]);

        for (int i = 0; i < 8; i++) {
            bool matched = true;
            for (int j = 7; j >= 0; j--) {
                uint32_t h_val = _byteswap_ulong(res[j][i]);
                if (h_val > target[j]) { matched = false; break; }
                if (h_val < target[j]) break;
            }
            if (matched) return n[i]; // Return found nonce immediately
        }
    }
    return 0; // Nothing found
}
