// engine3p.hpp - the 3P compression engine (portable C++17, header-only).
//
//  .3p file layout
//    [0xB3] [varint nameLen] [name, UTF-8] [varint originalSize] [u8 tableBits] [varint pixelStride] ([u8 bytesPerPixel])
//    then one FRAME per 8 MiB segment of the original:
//    [varint payloadLen*2 + modeled] [crc32 of the original segment, 4 bytes LE] [payload]
//
//  * Segments are independent, so compression AND extraction run on every CPU core.
//  * Inside a modeled segment: 16 KB blocks, each coded by the model, stored raw, or skipped without
//    touching the model (long incompressible runs run ~15x faster than modeled data).
//  * Integer-only model (context mixing + match + SSE): bit-identical on every machine and compiler.
//  * Memory is bounded: one model (~110 MB) + two 8 MB buffers per worker thread; tables are
//    lazily committed (calloc), so small or incompressible inputs touch very little RAM.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   // Windows 7+: condition variables
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wchar.h>
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace p3 {
namespace fs = std::filesystem;
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t i16; typedef int32_t i32; typedef int64_t i64;

constexpr u8 kMagic = 0xB3;
constexpr size_t kSegment = (size_t)8 << 20;    // independent unit of parallel work
constexpr size_t kBlock = 1u << 14;             // model / raw / skip decision granularity
constexpr size_t kMaxName = 1000;
inline const char* extension() { return ".3p"; }

// ------------------------------------------------------------------ public API
enum class Status { Ok, Cancelled, IoError, BadFormat, Corrupt, OutOfMemory, DiskFull };
struct Result {
    Status status = Status::Ok;
    std::string message;
    u64 inSize = 0, outSize = 0;     // compress: original / .3p size.  restore: .3p / restored size
    fs::path outPath;
    std::string storedName;          // original file name (UTF-8) kept in the metadata
};
struct Options {
    unsigned threads = 0;            // 0 = automatic
    u64 memoryBudget = 0;            // bytes the engine may use for worker models; 0 = no limit
    bool verify = false;             // decode every segment right after encoding it
    bool dedup = true;               // find long-distance duplicates (any distance) before compressing
    bool detect = true;              // detect headerless PCM audio and fixed-size records / image rows
    fs::path outDir;                 // empty = next to the source file
};
// progress(fractionDone 0..1, outputBytesSoFar). Return false to cancel. Called ~10x per second.
typedef std::function<bool(double, u64)> ProgressFn;

// ------------------------------------------------------------------ platform threading (no std::thread on Windows,
// so there is no dependency on the MinGW threading model)
namespace sys {
#ifdef _WIN32
class Mutex { CRITICAL_SECTION cs_; public:
    Mutex() { InitializeCriticalSection(&cs_); } ~Mutex() { DeleteCriticalSection(&cs_); }
    Mutex(const Mutex&) = delete; Mutex& operator=(const Mutex&) = delete;
    void lock() { EnterCriticalSection(&cs_); } void unlock() { LeaveCriticalSection(&cs_); } CRITICAL_SECTION* native() { return &cs_; } };
class CondVar { CONDITION_VARIABLE cv_; public:
    CondVar() { InitializeConditionVariable(&cv_); }
    CondVar(const CondVar&) = delete; CondVar& operator=(const CondVar&) = delete;
    void notify_all() { WakeAllConditionVariable(&cv_); }
    void wait(Mutex& m) { SleepConditionVariableCS(&cv_, m.native(), INFINITE); }
    void wait_ms(Mutex& m, unsigned ms) { SleepConditionVariableCS(&cv_, m.native(), ms); } };
class Thread { HANDLE h_ = nullptr; std::function<void()> fn_;
    static DWORD WINAPI tramp(LPVOID p) { static_cast<Thread*>(p)->fn_(); return 0; }
public:
    Thread() {} Thread(const Thread&) = delete; Thread& operator=(const Thread&) = delete;
    bool start(std::function<void()> f) { fn_ = std::move(f); h_ = CreateThread(nullptr, 0, tramp, this, 0, nullptr); return h_ != nullptr; }
    void join() { if (h_) { WaitForSingleObject(h_, INFINITE); CloseHandle(h_); h_ = nullptr; } }
    ~Thread() { join(); } };
inline unsigned hardware_threads() { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors ? (unsigned)si.dwNumberOfProcessors : 1u; }
#else
class Mutex { std::mutex m_; public: void lock() { m_.lock(); } void unlock() { m_.unlock(); } };
class CondVar { std::condition_variable_any cv_; public:
    void notify_all() { cv_.notify_all(); }
    void wait(Mutex& m) { cv_.wait(m); }
    void wait_ms(Mutex& m, unsigned ms) { cv_.wait_for(m, std::chrono::milliseconds(ms)); } };
class Thread { std::thread t_; public:
    bool start(std::function<void()> f) { try { t_ = std::thread(std::move(f)); return true; } catch (...) { return false; } }
    void join() { if (t_.joinable()) t_.join(); } ~Thread() { join(); } };
inline unsigned hardware_threads() { unsigned n = std::thread::hardware_concurrency(); return n ? n : 1u; }
#endif
struct Lock { Mutex& m; explicit Lock(Mutex& mm) : m(mm) { m.lock(); } ~Lock() { m.unlock(); } };
}  // namespace sys

// ------------------------------------------------------------------ internals
namespace detail {

struct Failure { Status status; std::string msg; };
struct Cancelled {};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int ceil_log2(u64 x) { int k = 0; while (k < 63 && ((u64)1 << k) < x) k++; return k; }

template <class T> struct Arr {                  // zero-initialised, lazily committed by the OS
    T* p = nullptr;
    Arr() {} Arr(const Arr&) = delete; Arr& operator=(const Arr&) = delete;
    void alloc(size_t n) { std::free(p); p = (T*)std::calloc(n ? n : 1, sizeof(T)); if (!p) throw std::bad_alloc(); }
    ~Arr() { std::free(p); }
    T& operator[](size_t i) { return p[i]; }
};

struct Tables {
    short stretch_[4096]; int dt_[1024]; double lg1_[4097];
    static int squash(int d) {
        static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,2047,
            2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
        if (d > 2047) return 4095;
        if (d < -2047) return 0;
        int w = d & 127; d = (d >> 7) + 16;
        return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
    }
    Tables() {
        int pi = 0;
        for (int x = -2047; x <= 2047; x++) { int v = squash(x); for (int i = pi; i <= v; i++) stretch_[i] = (short)x; pi = v + 1; }
        for (int i = pi; i < 4096; i++) stretch_[i] = 2047;
        for (int i = 0; i < 1024; i++) dt_[i] = 16384 / (i + i + 3);
        for (int i = 1; i < 4096; i++) lg1_[i] = -std::log((double)i / 4096.0) / std::log(2.0);   // encoder-only decision aid
        lg1_[0] = lg1_[4096] = 12.0;
    }
};
inline const Tables g_tables;
inline const Tables& tables() { return g_tables; }
inline int squash(int d) { return Tables::squash(d); }

constexpr u32 kInit = 0x80000000u;               // counters are stored XOR kInit so zeroed memory means p=0.5, n=0
inline void sm_up(u32* t, int y, int limit) {
    const Tables& T = tables();
    int n = (int)(*t & 1023), p = (int)(*t >> 10);
    if (n < limit) ++*t; else *t = (*t & 0xfffffc00u) | (u32)limit;
    i64 d = (((i64)(y << 22) - p) >> 3) * T.dt_[n];
    *t = (u32)((i64)*t + (d & ~(i64)1023));
}
inline u32 hmix(u32 h, u32 v) {
    h ^= v + 0x9E3779B9u + (h << 6) + (h >> 2);
    h *= 0x85EBCA6Bu; h ^= h >> 15; h *= 0xC2B2AE35u; h ^= h >> 13;
    return h;
}

struct APM {
    std::vector<u16> t; int idx = 0;
    void init(int n) {
        t.resize((size_t)n * 24); idx = 0;
        for (int i = 0; i < n; i++) for (int j = 0; j < 24; j++) t[(size_t)i * 24 + j] = (u16)(squash(((j * 2 + 1) * 4096) / 48 - 2048) * 16);
    }
    int pp(int pr, int cx) {
        int s = (tables().stretch_[pr] + 2048) * 23, wt = s & 0xfff;
        cx = cx * 24 + (s >> 12); idx = cx + (wt >> 11);
        return (t[cx] * (4096 - wt) + t[cx + 1] * wt) >> 16;
    }
    void up(int y) { int g = (y << 16) + (y << 7) - y - y; t[idx] = (u16)(t[idx] + ((g - t[idx]) >> 7)); }
};

// ------------------------------------------------------------------ what the model is told about the data
// PCM WAV audio (8/16/24-bit, 1-8 channels): the "data" chunk is turned into prediction residuals before modelling.
struct Audio {
    u64 dataOff = 0, dataLen = 0; int channels = 0, width = 0;
    bool on() const { return channels > 0; }
    int frame() const { return channels * width; }
};
// whole audio frames of segment [segStart, segStart+L) as local offsets [ts, te); empty region -> ts == te == 0
inline void audio_region(const Audio& a, u64 segStart, size_t L, size_t& ts, size_t& te) {
    ts = te = 0; if (!a.on()) return;
    u64 f = (u64)a.frame(), e = segStart + L;
    u64 lo = std::max(segStart, a.dataOff), hi = std::min(e, a.dataOff + a.dataLen);
    if (hi <= lo) return;
    u64 rel = (lo - a.dataOff) % f; if (rel) lo += f - rel;
    if (lo >= hi) return;
    hi = lo + ((hi - lo) / f) * f; if (hi <= lo) return;
    ts = (size_t)(lo - segStart); te = (size_t)(hi - segStart);
}
struct Hint {                                       // per-segment hints for the model contexts
    int stride = 0, bpp = 0;                        // BMP pixel geometry
    u64 ats = 0, ate = 0; int channels = 0, width = 0;   // audio residual region (local offsets)
};

// ------------------------------------------------------------------ the model
class Model {
public:
    static constexpr int NBIT = 5, NBYTE = 9, NCTX = NBIT + NBYTE, N_IN = NCTX + 2;
    static constexpr int MINLEN = 4, LIMIT = 30, LR1 = 32, LR2 = 4, BIAS = 77;

    Model(u64 len, int tbMax, const Hint& hint) {
        byteTb_ = clampi(ceil_log2(len ? len * 8 : 1) + 1, 16, tbMax);
        mtBits_ = clampi(ceil_log2(len ? len : 1) + 1, 12, tbMax);
        u64 w = 4096; while (w < len && w < ((u64)1 << 24)) w <<= 1;
        ring_ = (size_t)w; mask_ = w - 1;
        static const int ords[NBIT] = {2, 4, 8, 12, 16};
        for (int i = 0; i < NCTX; i++) tab_[i].alloc((size_t)1 << (i < NBIT ? ords[i] : byteTb_));
        buf_.alloc(ring_); mtab_.alloc((size_t)1 << mtBits_);
        for (int i = 0; i < 32; i++) mm_[i] = kInit;
        mix1_.assign((size_t)256 * N_IN, 9830); mix2_.assign((size_t)216 * N_IN, 9830); mix3_.assign((size_t)256 * N_IN, 9830);
        for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) mixf_[i][j] = 21845;
        a1_.init(256); a2_.init(65536);
        if (hint.stride > 0 && hint.bpp > 0 && (u64)hint.stride + (u64)hint.bpp + 8 < ring_ / 2) { stride_ = hint.stride; bpp_ = hint.bpp; }
        if (hint.channels > 0 && hint.width > 0 && hint.ate > hint.ats) { ats_ = hint.ats; ate_ = hint.ate; ach_ = hint.channels; aw_ = hint.width; }
        setByteCtx();
    }

    int predict() {
        const Tables& T = tables();
        static const int ords[NBIT] = {2, 4, 8, 12, 16};
        for (int i = 0; i < NBIT; i++) slot_[i] = &tab_[i][hist_ & ((1u << ords[i]) - 1)];
        if (pos_ == 0 || pos_ == 4)                       // one cache line (16 counters) per nibble context
            for (int i = 0; i < NBYTE; i++) base_[i] = (hmix(bh_[i], pos_ == 0 ? 0u : c0_) >> (32 - byteTb_)) & ~15u;
        int j = pos_ & 3;
        u32 nib = j == 0 ? 1u : ((1u << j) | (c0_ & ((1u << j) - 1)));
        for (int i = 0; i < NBYTE; i++) slot_[NBIT + i] = &tab_[NBIT + i][base_[i] + nib];
        for (int i = 0; i < NCTX; i++) {
            u32 t = *slot_[i] ^ kInit;
            st_[i] = (t & 1023) ? T.stretch_[clampi((int)(t >> 20), 4, 4091)] : 0;
        }
        mctx_ = 0; mexp_ = -1; st_[NCTX] = 0;
        if (mptr_) {
            u32 pb = (u32)buf_[(size_t)(mptr_ & mask_)] | 256u;
            if ((pb >> (8 - pos_)) == c0_) {
                mexp_ = (int)((pb >> (7 - pos_)) & 1);
                mctx_ = 1 + (int)(mlen_ < 30 ? mlen_ : 30);
                int s = T.stretch_[clampi((int)(mm_[mctx_] >> 20), 41, 4055)];
                st_[NCTX] = mexp_ ? s : -s;
            }
        }
        st_[NCTX + 1] = BIAS;
        int mb = mexp_ < 0 ? 0 : 1 + (int)((mlen_ < 15 ? mlen_ : 15) / 2);
        wsel_[0] = &mix1_[(size_t)c0_ * N_IN];
        wsel_[1] = &mix2_[(size_t)(((mb * 3) + (mexp_ + 1)) * 8 + pos_) * N_IN];
        wsel_[2] = &mix3_[(size_t)c1_ * N_IN];
        for (int k = 0; k < 3; k++) {
            i64 s = 0;
            for (int i = 0; i < N_IN; i++) s += (i64)wsel_[k][i] * st_[i];
            int d = clampi((int)(s >> 16), -2047, 2047);
            p1_[k] = squash(d); st2_[k] = d;
        }
        st2_[3] = BIAS;
        i64 s = 0;
        for (int i = 0; i < 4; i++) s += (i64)mixf_[pos_][i] * st2_[i];
        int d = clampi((int)(s >> 16), -2047, 2047);
        pf_ = clampi(squash(d), 1, 4095);
        int a1 = a1_.pp(pf_, (int)c0_), a2 = a2_.pp(pf_, (int)((c1_ << 8) | c0_));
        return clampi((2 * pf_ + a1 + a2 + 2) >> 2, 1, 4095);
    }

    void update(int y) {
        for (int k = 0; k < 3; k++) {
            int e0 = (y << 12) - p1_[k];
            if (e0 > 6 || e0 < -6) {
                int err = e0 * LR1;
                for (int i = 0; i < N_IN; i++) wsel_[k][i] += (st_[i] * err + 0x8000) >> 16;
            }
        }
        int err = ((y << 12) - pf_) * LR2;
        for (int i = 0; i < 4; i++) mixf_[pos_][i] += (st2_[i] * err + 0x8000) >> 16;
        a1_.up(y); a2_.up(y);
        for (int i = 0; i < NCTX; i++) { u32 t = *slot_[i] ^ kInit; sm_up(&t, y, LIMIT); *slot_[i] = t ^ kInit; }
        if (mexp_ >= 0) sm_up(&mm_[mctx_], y == mexp_, 60);
        hist_ = (hist_ << 1) | (u32)y;
        c0_ = (c0_ << 1) | (u32)y;
        if (++pos_ == 8) byteDone(c0_ & 255u);
    }

private:
    static u32 bitlen(u32 v) { u32 n = 0; while (v) { n++; v >>= 1; } return n; }
    int resAt(u64 idx) const {                            // signed residual stored at absolute index idx (aw_ bytes, little endian)
        u32 v = 0; for (int i = aw_ - 1; i >= 0; i--) v = (v << 8) | buf_.p[(size_t)((idx + (u64)i) & mask_)];
        return (int)v;                                   // zigzag value: already a magnitude
    }
    int back(u32 k) const { return (k && blen_ >= k) ? buf_.p[(size_t)((blen_ - k) & mask_)] : 0; }

    void setByteCtx() {
        u32 h;
        bh_[0] = hmix(0xA0, 0);
        h = hmix(0xA1, c1_);     bh_[1] = h;
        h = hmix(h ^ 0xA2, c2_); bh_[2] = h;
        h = hmix(h ^ 0xA3, c3_); bh_[3] = h;
        h = hmix(h ^ 0xA4, c4_); bh_[4] = h;
        bh_[5] = hmix(0xA5, word_);
        if (blen_ >= ats_ && blen_ < ate_ && ach_) {      // audio residuals: magnitude contexts of neighbouring samples
            u64 f = (u64)(ach_ * aw_), rel = (blen_ - ats_) % f, base = blen_ - (rel % (u64)aw_);
            int bi = (int)(rel % (u64)aw_);
            int p1 = base >= ats_ + f ? resAt(base - f) : 0, p2 = base >= ats_ + 2 * f ? resAt(base - 2 * f) : 0;
            int ot = base >= ats_ + (u64)aw_ ? resAt(base - (u64)aw_) : 0;
            u32 q1 = bitlen(p1 < 0 ? -p1 : p1), q2 = bitlen(p2 < 0 ? -p2 : p2), qo = bitlen(ot < 0 ? -ot : ot);
            u32 known = bi ? (u32)back(1) | (bi > 1 ? (u32)back(2) << 8 : 0u) : 0u;       // bytes of this sample already coded
            u32 avg = bitlen((u32)(((p1 < 0 ? -p1 : p1) + (p2 < 0 ? -p2 : p2) + (ot < 0 ? -ot : ot)) / 3));
            bh_[6] = hmix(hmix(0xD0, (u32)rel), q1 | (q2 << 6) | (known >> 4 << 12));
            bh_[7] = hmix(hmix(hmix(0xD1, (u32)rel), qo), known);
            bh_[8] = hmix(hmix(0xD2, (u32)rel), avg | (q1 << 6) | (qo << 12));
        } else if (stride_) {                             // BMP pixels: left / above / above-left neighbours
            int l = back((u32)bpp_), a = back((u32)stride_), al = back((u32)(stride_ + bpp_));
            bh_[6] = hmix(hmix(0xB0, (u32)a), (u32)l);
            bh_[7] = hmix(0xB1, (u32)clampi(l + a - al, 0, 255));
            bh_[8] = hmix(hmix(0xB2, (u32)a), c1_);
        } else {                                          // sparse "skip" contexts
            bh_[6] = hmix(0xC0, c2_);
            bh_[7] = hmix(hmix(0xC1, c2_), c3_);
            bh_[8] = hmix(hmix(0xC2, c1_), c3_);
        }
    }

    void byteDone(u32 c) {
        pos_ = 0; c0_ = 1;
        c4_ = c3_; c3_ = c2_; c2_ = c1_; c1_ = c;
        if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57)) word_ = (word_ * 263u + c + 1u) & 0xFFFFFFu; else word_ = 0;
        buf_[(size_t)(blen_ & mask_)] = (u8)c; blen_++;
        if (mptr_ && buf_[(size_t)(mptr_ & mask_)] == c) { mlen_++; mptr_++; } else { mlen_ = 0; mptr_ = 0; }
        if (blen_ >= (u64)MINLEN) {
            u32 h = hmix(hmix(hmix((u32)back(1), (u32)back(2)), (u32)back(3)), (u32)back(4)) >> (32 - mtBits_);
            if (mptr_ == 0) {
                u32 cand = mtab_[h];
                if (cand) {
                    u32 dist = (u32)blen_ - cand;
                    if (dist > 0 && (u64)dist + 40 < ring_) {
                        u64 candAbs = blen_ - dist; u32 l = 0;
                        while (l < 32 && candAbs > l && buf_[(size_t)((candAbs - 1 - l) & mask_)] == buf_[(size_t)((blen_ - 1 - l) & mask_)]) l++;
                        if (l >= (u32)MINLEN) { mptr_ = candAbs; mlen_ = l; }
                    }
                }
            }
            mtab_[h] = (u32)blen_;
        }
        setByteCtx();
    }

    int byteTb_ = 22, mtBits_ = 22;
    size_t ring_ = 4096; u64 mask_ = 4095;
    Arr<u32> tab_[NCTX]; u32* slot_[NCTX] = {}; u32 base_[NBYTE] = {};
    u32 hist_ = 0, c0_ = 1, c1_ = 0, c2_ = 0, c3_ = 0, c4_ = 0, word_ = 0, bh_[NBYTE] = {};
    int pos_ = 0, stride_ = 0, bpp_ = 0;
    u64 ats_ = 0, ate_ = 0; int ach_ = 0, aw_ = 0;
    Arr<u8> buf_; u64 blen_ = 0; Arr<u32> mtab_; u64 mptr_ = 0; u32 mlen_ = 0; u32 mm_[32]; int mexp_ = -1, mctx_ = 0;
    std::vector<i32> mix1_, mix2_, mix3_; i32 mixf_[8][4]; i32* wsel_[3] = {};
    int st_[N_IN] = {}, st2_[4] = {}, p1_[3] = {}, pf_ = 2048;
    APM a1_, a2_;
};

// ------------------------------------------------------------------ audio predictor (integer only, bit-exact everywhere)
// Per channel: stage A = normalised LMS over the last `na` sample differences (+`mc` differences of the other channel;
// for the 2nd channel that includes the current sample of the 1st), stage B = a small LMS over stage A's past errors.
// Three presets trade adaptation speed against precision; the encoder picks the best one per segment.
// forward(): samples -> residuals (same byte count).   inverse(): residuals -> samples.
struct AudioPreset { int na, mc, sha, shb; };
inline const AudioPreset& audio_preset(int i) {
    static const AudioPreset t[3] = {{32, 16, 8, 6}, {32, 8, 7, 5}, {16, 8, 6, 5}};
    return t[i < 0 ? 0 : (i > 2 ? 2 : i)];
}
class AudioCodec {
public:
    static constexpr int NAX = 32, MCX = 16, NB = 16;
    AudioCodec(int channels, int width, int preset) : C_(channels), W_(width), P_(audio_preset(preset)), ch_((size_t)channels) {}
    void forward(u8* p, size_t frames) {
        for (size_t f = 0; f < frames; f++) for (int c = 0; c < C_; c++) {
            int x = getSample(p), pr = predict(c);
            putRes(p, zig(wrap(x - pr))); update(c, x); p += W_;
        }
    }
    void inverse(u8* p, size_t frames) {
        for (size_t f = 0; f < frames; f++) for (int c = 0; c < C_; c++) {
            int r = unzig(getU(p)), pr = predict(c), x = wrap(r + pr);
            putSample(p, x); update(c, x); p += W_;
        }
    }
private:
    struct Ch {
        i32 dh[NAX] = {}, eh[NB] = {}, wa[NAX + MCX] = {}, wb[NB] = {}, ia[NAX + MCX] = {};
        i32 s1 = 0, pA = 0, pB = 0; i64 eA = 0, eB = 0;
    };
    int wrap(i64 v) const { int bits = 8 * W_; i64 m = ((i64)1 << bits) - 1; v &= m; if (v >= ((i64)1 << (bits - 1))) v -= ((i64)1 << bits); return (int)v; }
    int zig(int s) const {                                   // zigzag: small +/- residuals -> small unsigned values
        int bits = 8 * W_; u32 m = (bits >= 32) ? 0xffffffffu : ((1u << bits) - 1);
        return (int)((((u32)s << 1) ^ (u32)(s >> (bits - 1))) & m);
    }
    u32 getU(const u8* p) const { u32 v = 0; for (int i = W_ - 1; i >= 0; i--) v = (v << 8) | p[i]; return v; }
    int unzig(u32 u) const { return (int)(u >> 1) ^ -(int)(u & 1); }
    int getRes(const u8* p) const { u32 v = 0; for (int i = W_ - 1; i >= 0; i--) v = (v << 8) | p[i]; int bits = 8 * W_; return (int)((v ^ (1u << (bits - 1))) - (1u << (bits - 1))); }
    void putRes(u8* p, int r) const { u32 v = (u32)r; for (int i = 0; i < W_; i++) { p[i] = (u8)v; v >>= 8; } }
    int getSample(const u8* p) const { return W_ == 1 ? (int)p[0] - 128 : getRes(p); }
    void putSample(u8* p, int x) const { if (W_ == 1) p[0] = (u8)(x + 128); else putRes(p, x); }
    int predict(int c) {
        Ch& s = ch_[(size_t)c]; const int na = P_.na, m = C_ == 2 ? P_.mc : 0;
        const Ch* o = C_ == 2 ? &ch_[(size_t)(1 - c)] : nullptr;
        for (int i = 0; i < na; i++) s.ia[i] = s.dh[i];
        for (int i = 0; i < m; i++) s.ia[na + i] = o->dh[i];
        i64 sa = 0, ea = 0;
        for (int i = 0; i < na + m; i++) { sa += (i64)s.wa[i] * s.ia[i]; ea += (i64)s.ia[i] * s.ia[i]; }
        s.eA = ea; s.pA = (int)((sa + 32768) >> 16);
        i64 sb = 0, eb = 0;
        for (int i = 0; i < NB; i++) { sb += (i64)s.wb[i] * s.eh[i]; eb += (i64)s.eh[i] * s.eh[i]; }
        s.eB = eb; s.pB = (int)((sb + 32768) >> 16);
        i64 pr = (i64)s.s1 + s.pA + s.pB, lo = -((i64)1 << (8 * W_ - 1)), hi = ((i64)1 << (8 * W_ - 1)) - 1;
        return (int)(pr < lo ? lo : (pr > hi ? hi : pr));
    }
    void update(int c, int x) {
        Ch& s = ch_[(size_t)c]; const int na = P_.na, m = C_ == 2 ? P_.mc : 0;
        int d = x - s.s1, errA = d - s.pA, errB = errA - s.pB;
        const i64 kEps = 2048, kMaxW = (i64)1 << 24;
        i64 g = ((i64)errA * ((i64)1 << (32 - P_.sha))) / (s.eA + kEps);
        for (int i = 0; i < na + m; i++) { i64 w = s.wa[i] + ((g * s.ia[i]) >> 16); s.wa[i] = (i32)(w > kMaxW ? kMaxW : (w < -kMaxW ? -kMaxW : w)); }
        i64 g2 = ((i64)errB * ((i64)1 << (32 - P_.shb))) / (s.eB + kEps);
        for (int i = 0; i < NB; i++) { i64 w = s.wb[i] + ((g2 * s.eh[i]) >> 16); s.wb[i] = (i32)(w > kMaxW ? kMaxW : (w < -kMaxW ? -kMaxW : w)); }
        std::memmove(s.dh + 1, s.dh, (size_t)(na - 1) * sizeof(i32)); s.dh[0] = d;
        std::memmove(s.eh + 1, s.eh, (NB - 1) * sizeof(i32)); s.eh[0] = errA;
        s.s1 = x;
    }
    int C_, W_; AudioPreset P_; std::vector<Ch> ch_;
};

// ------------------------------------------------------------------ grid transform (tables, series, images)
// Words of `width` bytes are replaced by their difference to a prediction: mode 0 = the word one record (stride bytes) back,
// mode 1 = gradient left + above - above-left (images, stride = row bytes). zz = zigzag the signed difference.
struct Grid {
    int stride = 0, width = 0, mode = 0, zz = 0;
    bool on() const { return stride > 0; }
};
inline bool grid_region(const Grid& g, u64 segStart, size_t L, size_t& ts, size_t& te) {   // whole records only
    ts = te = 0; if (!g.on()) return false;
    u64 S = (u64)g.stride, first = ((segStart + S - 1) / S) * S;
    if (first >= segStart + L) return false;
    u64 lo = first - segStart, recs = ((u64)L - lo) / S;
    if (recs == 0) return false;
    ts = (size_t)lo; te = (size_t)(lo + recs * S); return true;
}
class GridCodec {
public:
    explicit GridCodec(const Grid& g) : g_(g), w_(g.width), cols_((size_t)(g.stride / g.width)), mask_(g.width == 4 ? 0xffffffffull : (((u64)1 << (8 * g.width)) - 1)) {}
    void forward(u8* p, size_t bytes) {            // descending: predictors (earlier words) are still the originals
        for (size_t k = bytes / (size_t)w_; k-- > 0;) { u64 x = get(p, k), pr = predict(p, k); put(p, k, enc((x - pr) & mask_)); }
    }
    void inverse(u8* p, size_t bytes) {            // ascending: predictors are already restored
        size_t n = bytes / (size_t)w_;
        for (size_t k = 0; k < n; k++) { u64 r = dec(get(p, k)), pr = predict(p, k); put(p, k, (r + pr) & mask_); }
    }
private:
    u64 get(const u8* p, size_t k) const { u64 v = 0; const u8* q = p + k * (size_t)w_; for (int i = w_ - 1; i >= 0; i--) v = (v << 8) | q[i]; return v; }
    void put(u8* p, size_t k, u64 v) const { u8* q = p + k * (size_t)w_; for (int i = 0; i < w_; i++) { q[i] = (u8)v; v >>= 8; } }
    u64 predict(const u8* p, size_t k) const {
        if (g_.mode == 0) return k >= cols_ ? get(p, k - cols_) : 0;
        size_t col = k % cols_; i64 l = col ? (i64)get(p, k - 1) : 0, a = k >= cols_ ? (i64)get(p, k - cols_) : 0, al = (k >= cols_ && col) ? (i64)get(p, k - cols_ - 1) : 0;
        i64 v = l + a - al; return (u64)(v < 0 ? 0 : (v > (i64)mask_ ? (i64)mask_ : v));
    }
    u64 enc(u64 r) const {                          // r = wrapped difference
        if (!g_.zz) return r;
        i64 sgn = (r >> (8 * w_ - 1)) ? (i64)r - ((i64)1 << (8 * w_)) : (i64)r;
        return (u64)((sgn << 1) ^ (sgn >> 63)) & mask_;
    }
    u64 dec(u64 u) const { if (!g_.zz) return u; return (u64)(((i64)(u >> 1)) ^ -(i64)(u & 1)) & mask_; }
    Grid g_; int w_; size_t cols_; u64 mask_;
};

// ------------------------------------------------------------------ executable filters (x86 / ARM64 / ARM32)
// Relative branch targets become absolute addresses, so the same function called from many places gives the same bytes.
// Each rewrite is addition modulo 2^k on a field that is detected from bytes the filter leaves unchanged, so it is
// exactly reversible by construction. base = file offset of the segment (instruction addresses).
//   kind 1: x86 CALL/JMP (E8/E9 rel32 whose top byte is 00/FF: a 25-bit signed value), kind 2: ARM64 BL, kind 3: ARM32 BL.
inline void exec_filter(u8* p, size_t L, u64 base, int kind, bool forward) {
    auto rd = [](const u8* q) { return (u32)q[0] | ((u32)q[1] << 8) | ((u32)q[2] << 16) | ((u32)q[3] << 24); };
    auto wr = [](u8* q, u32 v) { q[0] = (u8)v; q[1] = (u8)(v >> 8); q[2] = (u8)(v >> 16); q[3] = (u8)(v >> 24); };
    if (kind == 1) {
        for (size_t i = 0; i + 5 <= L;) {
            if (p[i] != 0xE8 && p[i] != 0xE9) { i++; continue; }
            u32 v = rd(p + i + 1), top = v >> 24;
            if (top == 0 || top == 0xFF) {
                u32 low = v & 0x1FFFFFFu, off = (u32)((base + i + 5) & 0x1FFFFFFu);
                low = forward ? ((low + off) & 0x1FFFFFFu) : ((low - off) & 0x1FFFFFFu);
                wr(p + i + 1, (low & 0x1000000u) ? (low | 0xFE000000u) : low);
            }
            i += 5;
        }
    } else if (kind == 2) {
        for (size_t i = 0; i + 4 <= L; i += 4) {
            u32 w = rd(p + i);
            if ((w >> 26) == 0x25) {
                u32 pc = (u32)((base + i) >> 2) & 0x3FFFFFFu, imm = w & 0x3FFFFFFu;
                imm = forward ? ((imm + pc) & 0x3FFFFFFu) : ((imm - pc) & 0x3FFFFFFu);
                wr(p + i, (w & 0xFC000000u) | imm);
            }
        }
    } else if (kind == 3) {
        for (size_t i = 0; i + 4 <= L; i += 4) {
            if (p[i + 3] != 0xEB) continue;
            u32 w = rd(p + i), pc = (u32)((base + i + 8) >> 2) & 0xFFFFFFu, imm = w & 0xFFFFFFu;
            imm = forward ? ((imm + pc) & 0xFFFFFFu) : ((imm - pc) & 0xFFFFFFu);
            wr(p + i, (w & 0xFF000000u) | imm);
        }
    }
}

// ------------------------------------------------------------------ arithmetic coder over memory buffers
class Enc {
public:
    explicit Enc(std::vector<u8>& o) : out_(o) {}
    void bit(int y, int p) { u32 xm = x1_ + ((x2_ - x1_) >> 12) * (u32)p; if (y) x2_ = xm; else x1_ = xm + 1; norm(); }
    void half(int y) { u32 xm = x1_ + ((x2_ - x1_) >> 1); if (y) x2_ = xm; else x1_ = xm + 1; norm(); }
    void finish() {                                         // shortest byte string (zero padded by the decoder) inside [x1,x2]
        if (x1_ == 0) return;
        for (int nb = 1; nb <= 4; nb++) {
            int shift = 32 - 8 * nb;
            u64 v = (((u64)x1_ + ((u64)1 << shift) - 1) >> shift) << shift;
            if (v <= x2_) { for (int k = 0; k < nb; k++) out_.push_back((u8)(v >> (24 - 8 * k))); return; }
        }
        for (int k = 0; k < 4; k++) out_.push_back((u8)(x1_ >> (24 - 8 * k)));
    }
private:
    void norm() { while (((x1_ ^ x2_) & 0xff000000u) == 0) { out_.push_back((u8)(x2_ >> 24)); x1_ <<= 8; x2_ = (x2_ << 8) | 255u; } }
    std::vector<u8>& out_; u32 x1_ = 0, x2_ = 0xffffffffu;
};
class Dec {
public:
    Dec(const u8* p, size_t n) : p_(p), n_(n) { for (int i = 0; i < 4; i++) x_ = (x_ << 8) | next(); }
    int bit(int p) { u32 xm = x1_ + ((x2_ - x1_) >> 12) * (u32)p; int y = x_ <= xm; if (y) x2_ = xm; else x1_ = xm + 1; norm(); return y; }
    int half() { u32 xm = x1_ + ((x2_ - x1_) >> 1); int y = x_ <= xm; if (y) x2_ = xm; else x1_ = xm + 1; norm(); return y; }
private:
    u8 next() { return pos_ < n_ ? p_[pos_++] : 0; }
    void norm() { while (((x1_ ^ x2_) & 0xff000000u) == 0) { x1_ <<= 8; x2_ = (x2_ << 8) | 255u; x_ = (x_ << 8) | next(); } }
    const u8* p_; size_t n_, pos_ = 0; u32 x1_ = 0, x2_ = 0xffffffffu, x_ = 0;
};

inline void put_varint_mem(std::vector<u8>& v, u64 n) { while (n >= 128) { v.push_back((u8)(n | 128)); n >>= 7; } v.push_back((u8)n); }

struct Counters { std::atomic<u64> done{0}, out{0}; std::atomic<bool> cancel{false}; };

// order-0 entropy of a block in bits per byte (encoder-side heuristic only, never affects decoding)
inline double block_entropy(const u8* p, size_t n) {
    u32 c[256]; std::memset(c, 0, sizeof c);
    for (size_t i = 0; i < n; i++) c[p[i]]++;
    double s = 0;
    for (int i = 0; i < 256; i++) if (c[i]) s += (double)c[i] * std::log2((double)c[i]);
    return std::log2((double)n) - s / (double)n;
}

// ------------------------------------------------------------------ segment codec
// Block types: 0 = coded by the model, 1 = stored raw inside the arithmetic stream (model still learns),
// 2 = SKIPPED: the model is not run at all and the bytes go to a plain side area (memcpy speed).
// After 4 raw blocks in a row the encoder skips 15 blocks, then probes one block with the model again;
// repeated failed probes lengthen the skip up to 255 blocks (4 MB); a block whose byte entropy drops below 7.7 bits ends it at once.
// payload = [varint arithLen] [arithmetic stream] [bytes of the skipped blocks, in order]
inline void encode_segment(const u8* data, size_t L, int tbMax, const Hint& hint, std::vector<u8>& out, Counters& cn) {
    const Tables& T = tables();
    std::vector<u8> arith, side; arith.reserve(L / 2 + 64);
    Enc e(arith);
    Model m(L, tbMax, hint);
    std::vector<u16> pk(kBlock * 8);
    u32 fcRaw[3] = {kInit, kInit, kInit}, fcSkip[2] = {kInit, kInit};
    int prevType = 0, rawRun = 0, skipLeft = 0, skipLen = 15; size_t off = 0, lastOut = 0;
    while (off < L) {
        if (cn.cancel.load(std::memory_order_relaxed)) throw Cancelled();
        size_t B = std::min(kBlock, L - off); int type; size_t k = 0;
        if (skipLeft > 0 && block_entropy(data + off, B) < 7.7) { skipLeft = 0; rawRun = 0; skipLen = 15; }   // obvious structure: stop skipping
        if (skipLeft > 0) { type = 2; skipLeft--; }
        else {
            double cost = 0;
            for (size_t i = 0; i < B; i++) {
                u32 byte = data[off + i];
                for (int b = 7; b >= 0; b--) {
                    int y = (byte >> b) & 1, p = m.predict();
                    cost += y ? T.lg1_[p] : T.lg1_[4096 - p];
                    pk[k++] = (u16)((p << 1) | y);
                    m.update(y);
                }
            }
            type = cost + 0.5 >= 8.0 * (double)B ? 1 : 0;
            if (type) { if (++rawRun >= 4) { skipLeft = skipLen; skipLen = std::min(skipLen * 2 + 1, 255); } }
            else { rawRun = 0; skipLen = 15; }
        }
        e.bit(type != 0, clampi((int)(fcRaw[prevType] >> 20), 16, 4080)); sm_up(&fcRaw[prevType], type != 0, 30);
        if (type) { int c = prevType == 2; e.bit(type == 2, clampi((int)(fcSkip[c] >> 20), 16, 4080)); sm_up(&fcSkip[c], type == 2, 30); }
        prevType = type;
        if (type == 0) for (size_t j = 0; j < k; j++) e.bit(pk[j] & 1, pk[j] >> 1);
        else if (type == 1) for (size_t j = 0; j < k; j++) e.half(pk[j] & 1);
        else side.insert(side.end(), data + off, data + off + B);
        off += B;
        cn.done.fetch_add(B, std::memory_order_relaxed);
        size_t now = arith.size() + side.size();
        cn.out.fetch_add(now - lastOut, std::memory_order_relaxed); lastOut = now;
    }
    e.finish();
    cn.out.fetch_add(arith.size() + side.size() - lastOut, std::memory_order_relaxed);
    out.clear(); put_varint_mem(out, arith.size());
    out.insert(out.end(), arith.begin(), arith.end()); out.insert(out.end(), side.begin(), side.end());
}

inline void decode_segment(const u8* payload, size_t plen, u8* out, size_t L, int tbMax, const Hint& hint, Counters* cn) {
    u64 alen = 0; size_t hdr = 0; int shift = 0;
    for (;;) { if (hdr >= plen || shift > 56) throw Failure{Status::Corrupt, "file is damaged (segment header)"};
               u8 c = payload[hdr++]; alen |= (u64)(c & 127) << shift; if (!(c & 128)) break; shift += 7; }
    if (alen > plen - hdr) throw Failure{Status::Corrupt, "file is damaged (segment size)"};
    Dec d(payload + hdr, (size_t)alen);
    const u8* side = payload + hdr + alen; size_t sideLeft = plen - hdr - (size_t)alen;
    Model m(L, tbMax, hint);
    u32 fcRaw[3] = {kInit, kInit, kInit}, fcSkip[2] = {kInit, kInit};
    int prevType = 0; size_t off = 0;
    while (off < L) {
        if (cn && cn->cancel.load(std::memory_order_relaxed)) throw Cancelled();
        size_t B = std::min(kBlock, L - off);
        int type = d.bit(clampi((int)(fcRaw[prevType] >> 20), 16, 4080)); sm_up(&fcRaw[prevType], type, 30);
        if (type) { int c = prevType == 2; int sk = d.bit(clampi((int)(fcSkip[c] >> 20), 16, 4080)); sm_up(&fcSkip[c], sk, 30); type = sk ? 2 : 1; }
        prevType = type;
        u8* o = out + off;
        if (type == 2) {
            if (sideLeft < B) throw Failure{Status::Corrupt, "file is damaged (missing data)"};
            std::memcpy(o, side, B); side += B; sideLeft -= B;
        } else for (size_t i = 0; i < B; i++) {
            int v = 0;
            for (int b = 0; b < 8; b++) { int p = m.predict(), y = type ? d.half() : d.bit(p); m.update(y); v = (v << 1) | y; }
            o[i] = (u8)v;
        }
        off += B;
        if (cn) { cn->done.fetch_add(B, std::memory_order_relaxed); cn->out.fetch_add(B, std::memory_order_relaxed); }
    }
}

// ------------------------------------------------------------------ helpers
struct Crc {
    u32 t[8][256];
    Crc() {
        for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[0][i] = c; }
        for (u32 i = 0; i < 256; i++) for (int k = 1; k < 8; k++) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 255];
    }
};
inline const Crc g_crc;
inline u32 crc32_update(u32 prev, const u8* p, size_t n) {
    const Crc& C = g_crc; u32 r = ~prev;
    while (n >= 8) {
        u32 a = r ^ ((u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24));
        r = C.t[7][a & 255] ^ C.t[6][(a >> 8) & 255] ^ C.t[5][(a >> 16) & 255] ^ C.t[4][a >> 24] ^
            C.t[3][p[4]] ^ C.t[2][p[5]] ^ C.t[1][p[6]] ^ C.t[0][p[7]];
        p += 8; n -= 8;
    }
    while (n--) r = C.t[0][(r ^ *p++) & 255] ^ (r >> 8);
    return ~r;
}
inline u32 crc32(const u8* p, size_t n) { return crc32_update(0, p, n); }

inline FILE* fopen_path(const fs::path& p, const char* mode) {
#ifdef _WIN32
    std::wstring m(mode, mode + std::strlen(mode)); return _wfopen(p.c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}
inline int seek64(FILE* f, u64 off) {
#ifdef _WIN32
    return _fseeki64(f, (__int64)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
struct File {
    FILE* f = nullptr; File() {} File(const File&) = delete; File& operator=(const File&) = delete;
    bool open(const fs::path& p, const char* mode) { close(); f = fopen_path(p, mode); return f != nullptr; }
    void close() { if (f) { std::fclose(f); f = nullptr; } } ~File() { close(); }
};
inline std::string to_utf8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }
inline fs::path from_utf8(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}
inline std::string safe_name(std::string n) {            // never trust a name that came out of a file
    size_t cut = n.find_last_of("/\\"); if (cut != std::string::npos) n = n.substr(cut + 1);
    for (char& c : n) if ((unsigned char)c < 32 || std::strchr("<>:\"|?*", c)) c = '_';
    while (!n.empty() && (n.back() == '.' || n.back() == ' ')) n.pop_back();
    if (n.empty()) n = "restored_file";
    std::string up; for (char c : n.substr(0, n.find('.'))) up += (char)std::toupper((unsigned char)c);
    static const char* bad[] = {"CON","PRN","AUX","NUL","COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9","LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9"};
    for (const char* b : bad) if (up == b) { n = "_" + n; break; }
    if (n.size() > 200) { size_t dot = n.rfind('.'); std::string ext = (dot != std::string::npos && n.size() - dot < 20) ? n.substr(dot) : ""; n = n.substr(0, 200 - ext.size()) + ext; }
    return n;
}
inline fs::path unique_path(const fs::path& dir, const std::string& name) {
    fs::path base = from_utf8(name), p = dir / base; std::error_code ec;
    if (!fs::exists(p, ec)) return p;
    std::string stem = to_utf8(base.stem()), ext = to_utf8(base.extension());
    for (int i = 1; i < 100000; i++) { p = dir / from_utf8(stem + " (" + std::to_string(i) + ")" + ext); if (!fs::exists(p, ec)) return p; }
    return dir / base;
}
inline void put_varint(std::vector<u8>& v, u64 n) { while (n >= 128) { v.push_back((u8)(n | 128)); n >>= 7; } v.push_back((u8)n); }
inline bool get_varint(FILE* f, u64& n, u64& used) {
    n = 0; int shift = 0;
    for (;;) { int c = std::fgetc(f); if (c == EOF || shift > 63) return false; used++; n |= (u64)(c & 127) << shift; if (!(c & 128)) return true; shift += 7; }
}
struct Match { u64 gap = 0, len = 0, dist = 0; };   // gap = literal bytes since the previous match; copy len bytes from dist back
struct Header { std::string name; u64 total = 0; int tbMax = 22, stride = 0, bpp = 0; Audio audio; Grid grid; int exec = 0; u64 bytes = 0;
                bool dedup = false; u32 fileCrc = 0; u64 litTotal = 0; std::vector<Match> matches;
                bool jpeg = false; std::vector<u8> jpegPayload;
                bool gen = false; std::vector<u8> genPrefix, genTail; };          // gen: whole file = Mersenne Twister output
inline std::vector<u8> make_header(const Header& h) {
    std::vector<u8> v; v.push_back(kMagic);
    put_varint(v, h.name.size()); v.insert(v.end(), h.name.begin(), h.name.end());
    put_varint(v, h.total); v.push_back((u8)h.tbMax);
    if (h.audio.on()) { v.push_back(2); put_varint(v, h.audio.dataOff); put_varint(v, h.audio.dataLen); v.push_back((u8)h.audio.channels); v.push_back((u8)h.audio.width); }
    else if (h.stride) { v.push_back(1); put_varint(v, (u64)h.stride); v.push_back((u8)h.bpp); }
    else if (h.dedup) {
        if (h.exec) { v.push_back(7); v.push_back((u8)h.exec); } else v.push_back(3);      // 7 = copy records + executable filter
        put_varint(v, h.matches.size()); for (int i = 0; i < 4; i++) v.push_back((u8)(h.fileCrc >> (8 * i)));
        for (const Match& m : h.matches) { put_varint(v, m.gap); put_varint(v, m.len); put_varint(v, m.dist); }
    }
    else if (h.exec) { v.push_back(6); v.push_back((u8)h.exec); }
    else if (h.grid.on()) { v.push_back(4); put_varint(v, (u64)h.grid.stride); v.push_back((u8)h.grid.width); v.push_back((u8)(h.grid.mode | (h.grid.zz << 1))); }
    else if (h.jpeg) { v.push_back(8); for (int i = 0; i < 4; i++) v.push_back((u8)(h.fileCrc >> (8 * i))); put_varint(v, h.jpegPayload.size()); v.insert(v.end(), h.jpegPayload.begin(), h.jpegPayload.end()); }
    else if (h.gen) { v.push_back(5); v.insert(v.end(), h.genPrefix.begin(), h.genPrefix.end()); for (int i = 0; i < 4; i++) v.push_back((u8)(h.fileCrc >> (8 * i))); v.push_back((u8)h.genTail.size()); v.insert(v.end(), h.genTail.begin(), h.genTail.end()); }
    else v.push_back(0);
    return v;
}
inline bool read_header(FILE* f, Header& h) {
    u64 used = 0, n = 0;
    if (std::fgetc(f) != kMagic) return false;
    used = 1;
    if (!get_varint(f, n, used) || n == 0 || n > kMaxName) return false;
    h.name.resize((size_t)n); if (std::fread(&h.name[0], 1, (size_t)n, f) != (size_t)n) return false; used += n;
    if (!get_varint(f, h.total, used)) return false;
    int tb = std::fgetc(f); if (tb < 16 || tb > 22) return false; h.tbMax = tb; used++;
    int kind = std::fgetc(f); used++;
    if (kind == 7) { int e = std::fgetc(f); used++; if (e < 1 || e > 3) return false; h.exec = e; kind = 3; }
    if (kind == 1) {
        u64 st = 0; if (!get_varint(f, st, used) || st == 0 || st > 0x7fffffff) return false; h.stride = (int)st;
        int b = std::fgetc(f); if (b < 1 || b > 8) return false; h.bpp = b; used++;
    } else if (kind == 2) {
        u64 off = 0, len = 0; if (!get_varint(f, off, used) || !get_varint(f, len, used)) return false;
        int ch = std::fgetc(f), w = std::fgetc(f); used += 2;
        if (ch < 1 || ch > 8 || w < 1 || w > 3 || off > h.total || len > h.total - off) return false;
        h.audio.dataOff = off; h.audio.dataLen = len; h.audio.channels = ch; h.audio.width = w;
    } else if (kind == 3) {
        u64 cnt = 0; if (!get_varint(f, cnt, used) || cnt > h.total / 16 + 1) return false;
        u8 c4[4]; if (std::fread(c4, 1, 4, f) != 4) return false; used += 4;
        h.fileCrc = (u32)c4[0] | ((u32)c4[1] << 8) | ((u32)c4[2] << 16) | ((u32)c4[3] << 24);
        h.matches.reserve((size_t)cnt); u64 cursor = 0, matched = 0;
        for (u64 i = 0; i < cnt; i++) {
            Match m; if (!get_varint(f, m.gap, used) || !get_varint(f, m.len, used) || !get_varint(f, m.dist, used)) return false;
            if (m.len == 0 || m.dist == 0 || m.gap > h.total || m.len > h.total) return false;
            cursor += m.gap; if (cursor > h.total || m.dist > cursor) return false;
            cursor += m.len; if (cursor > h.total) return false;
            matched += m.len; h.matches.push_back(m);
        }
        h.dedup = true; h.litTotal = h.total - matched;
    } else if (kind == 4) {
        u64 st = 0; if (!get_varint(f, st, used) || st == 0 || st > ((u64)1 << 26)) return false;
        int w = std::fgetc(f), fl = std::fgetc(f); used += 2;
        if ((w != 1 && w != 2 && w != 4) || fl < 0 || fl > 3 || st % (u64)w) return false;
        h.grid.stride = (int)st; h.grid.width = w; h.grid.mode = fl & 1; h.grid.zz = (fl >> 1) & 1;
    } else if (kind == 6) {
        int e = std::fgetc(f); used++; if (e < 1 || e > 3) return false; h.exec = e;
    } else if (kind == 8) {
        u8 c4[4]; u64 pl = 0; if (std::fread(c4, 1, 4, f) != 4) return false; used += 4;
        h.fileCrc = (u32)c4[0] | ((u32)c4[1] << 8) | ((u32)c4[2] << 16) | ((u32)c4[3] << 24);
        if (!get_varint(f, pl, used) || pl > h.total + ((u64)1 << 22)) return false;
        h.jpegPayload.resize((size_t)pl); if (pl && std::fread(h.jpegPayload.data(), 1, (size_t)pl, f) != (size_t)pl) return false;
        used += pl; h.jpeg = true;
    } else if (kind == 5) {
        h.genPrefix.resize(624 * 4); u8 c4[4];
        if (std::fread(h.genPrefix.data(), 1, h.genPrefix.size(), f) != h.genPrefix.size() || std::fread(c4, 1, 4, f) != 4) return false;
        h.fileCrc = (u32)c4[0] | ((u32)c4[1] << 8) | ((u32)c4[2] << 16) | ((u32)c4[3] << 24);
        int tl = std::fgetc(f); if (tl < 0 || tl > 3) return false;
        h.genTail.resize((size_t)tl); if (tl && std::fread(h.genTail.data(), 1, (size_t)tl, f) != (size_t)tl) return false;
        used += h.genPrefix.size() + 4 + 1 + (u64)tl;
        if (h.total < h.genPrefix.size() + (u64)tl || (h.total - (u64)tl) % 4) return false;
        h.gen = true;
    } else if (kind != 0) return false;
    h.bytes = used; return true;
}
inline void detect_geometry(const u8* b, size_t n, int& stride, int& bpp) {      // uncompressed BMP: 8/24/32 bit
    stride = bpp = 0;
    if (n < 30 || b[0] != 'B' || b[1] != 'M') return;
    u32 w = (u32)b[18] | ((u32)b[19] << 8) | ((u32)b[20] << 16); int bits = b[28];
    if ((bits == 8 || bits == 24 || bits == 32) && b[29] == 0 && w > 0 && w < 100000) { bpp = bits / 8; stride = (int)((((u64)w * (u64)bits + 31) / 32) * 4); }
}
// PCM WAV (RIFF/WAVE, 8/16/24-bit, up to 8 channels): finds the "data" chunk
inline bool detect_wav(FILE* f, u64 fileSize, Audio& a) {
    u8 h[12]; if (seek64(f, 0) != 0 || std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) return false;
    u64 pos = 12; int channels = 0, bits = 0, blockAlign = 0; bool pcm = false;
    for (int guard = 0; guard < 64 && pos + 8 <= fileSize; guard++) {
        u8 c[8]; if (seek64(f, pos) != 0 || std::fread(c, 1, 8, f) != 8) return false;
        u64 size = (u64)c[4] | ((u64)c[5] << 8) | ((u64)c[6] << 16) | ((u64)c[7] << 24);
        if (!std::memcmp(c, "fmt ", 4) && size >= 16) {
            u8 b[40]; size_t want = size >= 40 ? 40 : 16; if (std::fread(b, 1, want, f) != want) return false;
            int tag = b[0] | (b[1] << 8); channels = b[2] | (b[3] << 8); blockAlign = b[12] | (b[13] << 8); bits = b[14] | (b[15] << 8);
            if (tag == 0xFFFE && want == 40) tag = b[24] | (b[25] << 8);                 // WAVE_FORMAT_EXTENSIBLE sub-format
            pcm = (tag == 1);
        } else if (!std::memcmp(c, "data", 4)) {
            if (!pcm || channels < 1 || channels > 8 || (bits != 8 && bits != 16 && bits != 24) || blockAlign != channels * bits / 8) return false;
            u64 off = pos + 8, avail = fileSize > off ? fileSize - off : 0;
            a.dataOff = off; a.dataLen = (size == 0 || size > avail) ? avail : size; a.channels = channels; a.width = bits / 8;
            return a.dataLen >= (u64)a.frame();
        }
        pos += 8 + size + (size & 1);
    }
    return false;
}
// ------------------------------------------------------------------ long-distance duplicate finder
// Rolling hash over a 32-byte window; content-defined anchors (1 in 2^k positions) are remembered in a hash table.
// A hit is verified byte by byte, then extended backwards and forwards. The table is bounded (256 MB), so any
// file size works; the file is read in 16 MB steps and verified through two small block caches.
class FileCache {
public:
    FileCache(const fs::path& p, u64 size) : size_(size), blk_(kBlk) { ok_ = f_.open(p, "rb"); }
    bool ok() const { return ok_; }
    const u8* get(u64 pos, size_t& avail) {
        if (pos >= size_) { avail = 0; return nullptr; }
        if (pos < off_ || pos >= off_ + len_) {
            off_ = pos & ~(u64)(kBlk - 1);
            if (seek64(f_.f, off_) != 0) { len_ = 0; avail = 0; return nullptr; }
            len_ = std::fread(blk_.data(), 1, kBlk, f_.f);
            if (pos >= off_ + len_) { avail = 0; return nullptr; }
        }
        avail = (size_t)(off_ + len_ - pos); return blk_.data() + (pos - off_);
    }
    int at(u64 pos) { size_t a; const u8* p = get(pos, a); return p ? (int)*p : -1; }
private:
    static constexpr size_t kBlk = (size_t)1 << 22;
    File f_; bool ok_ = false; u64 size_, off_ = ~(u64)0; size_t len_ = 0; std::vector<u8> blk_;
};
inline u64 common_forward(FileCache& a, u64 pa, FileCache& b, u64 pb, u64 maxLen) {
    u64 total = 0;
    while (total < maxLen) {
        size_t aa, ab; const u8* x = a.get(pa + total, aa); const u8* y = b.get(pb + total, ab);
        if (!x || !y) break;
        size_t m = (size_t)std::min<u64>(std::min<u64>(aa, ab), maxLen - total), k = 0;
        while (k < m && x[k] == y[k]) k++;
        total += k; if (k < m) break;
    }
    return total;
}
// returns false on cancel/read error. cancelled is set when prog returned false.
inline bool find_duplicates(const fs::path& src, u64 n, u64 tableBudget, const std::function<bool(double)>& prog,
                            std::vector<Match>& out, u64& matched, bool& cancelled) {
    cancelled = false; out.clear(); matched = 0;
    const int k = std::max(7, ceil_log2(n) - 24);
    const u64 minm = (u64)4 << k;
    u64 anchors = (n >> k) + 1, cap = (u64)1 << 16;
    while (cap < anchors + anchors / 4 && cap < ((u64)1 << 26)) cap <<= 1;
    while (cap * 12 > tableBudget && cap > ((u64)1 << 16)) cap >>= 1;
    std::vector<u64> tpos((size_t)cap, 0); std::vector<u32> tchk((size_t)cap, 0);
    FileCache cs(src, n), cd(src, n); File in;
    if (!cs.ok() || !cd.ok() || !in.open(src, "rb")) return false;
    const size_t kBuf = (size_t)16 << 20; std::vector<u8> buf(kBuf);
    const u64 B = 0x100000001B3ull; u64 Bpow = 1; for (int i = 0; i < 32; i++) Bpow *= B;
    u64 pos = 0, litStart = 0, bufStart = 0, bufEnd = 0, h = 0; int fill = 0; u8 win[32]; unsigned wi = 0;
    const int shift = 64 - k;
    while (pos < n) {
        if (pos < bufStart || pos >= bufEnd) {
            if (prog && !prog((double)pos / (double)n)) { cancelled = true; return false; }
            if (seek64(in.f, pos) != 0) return false;
            size_t got = std::fread(buf.data(), 1, (size_t)std::min<u64>(kBuf, n - pos), in.f); if (got == 0) return false;
            bufStart = pos; bufEnd = pos + got;
        }
        size_t i = (size_t)(pos - bufStart), end = (size_t)(bufEnd - bufStart); bool jumped = false;
        for (; i < end; i++) {
            u8 c = buf[i];
            if (fill < 32) { h = h * B + c; win[wi] = c; wi = (wi + 1) & 31; if (++fill < 32) continue; }
            else { u8 o = win[wi]; win[wi] = c; wi = (wi + 1) & 31; h = h * B + c - (u64)o * Bpow; }
            u64 mix = h * 0x9E3779B97F4A7C15ull;
            if ((mix >> shift) != 0) continue;
            u64 p = bufStart + i + 1;
            size_t slot = (size_t)((mix >> 8) & (cap - 1)); u32 chk = (u32)((h * 0xC2B2AE3D27D4EB4Full) >> 32) | 1u;
            u64 q = tpos[slot];
            if (q && tchk[slot] == chk) {
                u64 dist = p - q, maxb = std::min<u64>(std::min<u64>(p - litStart, q), (u64)1 << 20), bk = 0;
                while (bk < maxb && cd.at(p - 1 - bk) == cs.at(q - 1 - bk)) bk++;
                u64 fw = common_forward(cd, p, cs, q, n - p);
                u64 len = bk + fw, start = p - bk;                                     // len > dist = repeating run / pattern
                if (len >= minm && start + len >= p) {
                    out.push_back(Match{start - litStart, len, dist}); matched += len; litStart = start + len;
                    pos = litStart; fill = 0; h = 0; wi = 0; jumped = true; break;
                }
            }
            tpos[slot] = p; tchk[slot] = chk;
        }
        if (!jumped) pos = bufEnd;
    }
    return true;
}
struct CrcJob { fs::path path; u32 crc = 0; bool ok = true; };
inline void crc_file(CrcJob* j) {
    File f; if (!f.open(j->path, "rb")) { j->ok = false; return; }
    std::vector<u8> b((size_t)4 << 20); size_t g;
    while ((g = std::fread(b.data(), 1, b.size(), f.f)) > 0) j->crc = crc32_update(j->crc, b.data(), g);
}
// literal stream = the file without the matched ranges
struct LitRun { u64 off, len, cum; };
inline std::vector<LitRun> make_runs(const std::vector<Match>& m, u64 n) {
    std::vector<LitRun> r; u64 cursor = 0, cum = 0;
    for (const Match& x : m) { if (x.gap) { r.push_back(LitRun{cursor, x.gap, cum}); cum += x.gap; } cursor += x.gap + x.len; }
    if (cursor < n) r.push_back(LitRun{cursor, n - cursor, cum});
    return r;
}
inline bool read_literals(FILE* f, const std::vector<LitRun>& runs, u64 vstart, size_t L, u8* dst) {
    if (runs.empty()) return L == 0;
    size_t lo = 0, hi = runs.size();
    while (lo + 1 < hi) { size_t mid = (lo + hi) / 2; if (runs[mid].cum <= vstart) lo = mid; else hi = mid; }
    u64 v = vstart; size_t need = L;
    for (size_t ri = lo; need && ri < runs.size(); ri++) {
        const LitRun& r = runs[ri]; u64 skip = v - r.cum, take = std::min<u64>(r.len - skip, need);
        if (seek64(f, r.off + skip) != 0 || std::fread(dst, 1, (size_t)take, f) != (size_t)take) return false;
        dst += take; v += take; need -= (size_t)take;
    }
    return need == 0;
}
// Output side: writes literal runs and executes the copy records in order. Matches within the last 64 MB are served
// from an in-memory window; older ones are read back from the output file.
class OutWriter {
public:
    OutWriter(FILE* out, const fs::path& tmp, const std::vector<Match>* m, Counters& cn) : out_(out), tmp_(tmp), m_(m), cn_(cn) {
        if (m_) { ring_.resize(kRing); chunk_.resize(kChunk); gapLeft_ = m_->empty() ? ~(u64)0 : (*m_)[0].gap; }
    }
    void append(const u8* p, size_t L) {
        if (!m_) { write(p, L); return; }
        size_t i = 0;
        for (;;) {
            applyReady();
            if (i >= L) break;
            size_t take = (size_t)std::min<u64>(gapLeft_, (u64)(L - i));
            write(p + i, take); i += take; if (gapLeft_ != ~(u64)0) gapLeft_ -= take;
        }
    }
    void finish() { if (m_) applyReady(); }
    u64 pos() const { return pos_; }
    u32 crc() const { return crc_; }
private:
    static constexpr size_t kRing = (size_t)64 << 20, kChunk = (size_t)4 << 20;
    void applyReady() {
        while (mi_ < m_->size() && gapLeft_ == 0) { copyMatch((*m_)[mi_]); mi_++; gapLeft_ = mi_ < m_->size() ? (*m_)[mi_].gap : ~(u64)0; }
    }
    void write(const u8* p, size_t L) {
        if (!L) return;
        if (std::fwrite(p, 1, L, out_) != L) throw Failure{Status::DiskFull, "write failed (disk full?)"};
        if (m_) {
            crc_ = crc32_update(crc_, p, L);
            size_t at = (size_t)(pos_ & (kRing - 1)), first = std::min(L, kRing - at);
            std::memcpy(ring_.data() + at, p, first); if (first < L) std::memcpy(ring_.data(), p + first, L - first);
        }
        pos_ += L;
    }
    void readBack(u64 src, size_t c, u64 dist, u8* dst) {                 // c bytes starting at output offset src
        if (dist <= kRing) {
            size_t at = (size_t)(src & (kRing - 1)), first = std::min(c, kRing - at);
            std::memcpy(dst, ring_.data() + at, first); if (first < c) std::memcpy(dst + first, ring_.data(), c - first);
        } else {
            if (std::fflush(out_) != 0) throw Failure{Status::DiskFull, "write failed (disk full?)"};
            if (!rd_.f && !rd_.open(tmp_, "rb")) throw Failure{Status::IoError, "cannot re-read the output file"};
            if (seek64(rd_.f, src) != 0 || std::fread(dst, 1, c, rd_.f) != c) throw Failure{Status::IoError, "cannot re-read the output file"};
        }
    }
    void copyMatch(const Match& m) {
        if (m.dist > pos_) throw Failure{Status::Corrupt, "file is damaged (bad copy record)"};
        u64 left = m.len;
        while (left) {
            size_t c = (size_t)std::min<u64>(left, kChunk);
            if (m.dist >= c) readBack(pos_ - m.dist, c, m.dist, chunk_.data());
            else {                                                       // overlapping copy: repeat the last `dist` bytes
                size_t d = (size_t)m.dist; readBack(pos_ - m.dist, d, m.dist, chunk_.data());
                for (size_t i = d; i < c; i++) chunk_[i] = chunk_[i - d];
            }
            write(chunk_.data(), c); cn_.out.fetch_add(c, std::memory_order_relaxed); left -= c;
        }
    }
    FILE* out_; fs::path tmp_; const std::vector<Match>* m_; Counters& cn_; File rd_;
    std::vector<u8> ring_, chunk_; u64 pos_ = 0, gapLeft_ = 0; size_t mi_ = 0; u32 crc_ = 0;
};

// ------------------------------------------------------------------ generator detector (MT19937)
// Files written by Python's random.randbytes / numpy RandomState.bytes are the raw 32-bit outputs of a Mersenne Twister.
// 624 outputs reveal its whole internal state, so the rest of the file can be regenerated exactly.
class Mt19937Stream {
public:
    explicit Mt19937Stream(const u32* first624) { for (int i = 0; i < 624; i++) mt_[i] = untemper(first624[i]); idx_ = 624; }
    u32 next() { if (idx_ == 624) { twist(); idx_ = 0; } return temper(mt_[idx_++]); }
private:
    static u32 temper(u32 y) { y ^= y >> 11; y ^= (y << 7) & 0x9D2C5680u; y ^= (y << 15) & 0xEFC60000u; y ^= y >> 18; return y; }
    static u32 untemper(u32 y) {
        y ^= y >> 18; y ^= (y << 15) & 0xEFC60000u;
        u32 t = y; for (int i = 0; i < 4; i++) t = y ^ ((t << 7) & 0x9D2C5680u); y = t;
        t = y; for (int i = 0; i < 2; i++) t = y ^ (t >> 11); return t;
    }
    void twist() {
        for (int i = 0; i < 624; i++) {
            u32 y = (mt_[i] & 0x80000000u) | (mt_[(i + 1) % 624] & 0x7fffffffu);
            mt_[i] = mt_[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0u);
        }
    }
    u32 mt_[624]; int idx_;
};
inline u32 rd32le(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
// true only if EVERY byte of the file matches the regenerated stream (the last 0-3 bytes are kept raw)
inline bool detect_mt19937(const fs::path& src, u64 n, Header& h) {
    if (n < ((u64)100 << 10)) return false;
    File f; if (!f.open(src, "rb")) return false;
    std::vector<u8> head(624 * 4); if (std::fread(head.data(), 1, head.size(), f.f) != head.size()) return false;
    u32 w[624]; for (int i = 0; i < 624; i++) w[i] = rd32le(&head[(size_t)i * 4]);
    Mt19937Stream mt(w); u32 crc = crc32_update(0, head.data(), head.size());
    u64 words = n / 4, done = 624; std::vector<u8> buf((size_t)4 << 20); u64 compared = 0;
    while (compared < words - 624) {
        size_t cnt = (size_t)std::min<u64>(buf.size() / 4, words - 624 - compared);
        if (std::fread(buf.data(), 1, cnt * 4, f.f) != cnt * 4) return false;
        for (size_t i = 0; i < cnt; i++) if (rd32le(&buf[i * 4]) != mt.next()) return false;
        crc = crc32_update(crc, buf.data(), cnt * 4); compared += cnt; done += cnt;
    }
    size_t tl = (size_t)(n - words * 4); std::vector<u8> tail(tl);
    if (tl && std::fread(tail.data(), 1, tl, f.f) != tl) return false;
    if (tl) crc = crc32_update(crc, tail.data(), tl);
    h.gen = true; h.genPrefix = head; h.genTail = tail; h.fileCrc = crc; return true;
}

// ------------------------------------------------------------------ JPEG recompression (baseline / extended-sequential Huffman)
// The entropy-coded scan is decoded to quantised DCT coefficients, which are coded with a context-mixing model that looks at
// the neighbouring blocks; extraction re-creates the exact original scan bytes. The compressor re-encodes and compares before
// it accepts the method, so anything unusual (progressive, odd padding, non-canonical Huffman use) just falls back.
namespace jpg {
struct Huff {
    bool present = false; u8 bits[17] = {}; u8 vals[256] = {}; int mincode[17] = {}, maxcode[18] = {}, valptr[17] = {}; u16 code[256] = {}; u8 len[256] = {};
};
struct Comp { int id = 0, h = 1, v = 1, td = 0, ta = 0, bw = 0, bh = 0; std::vector<i16> coef; std::vector<u8> nz; };
struct Info {
    int width = 0, height = 0, ncomp = 0, restart = 0, mcusX = 0, mcusY = 0, hmax = 1, vmax = 1;
    Comp c[4]; Huff dc[4], ac[4]; size_t scanStart = 0;
};
inline bool build(Huff& t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t.valptr[l] = k; t.mincode[l] = code;
        for (int i = 0; i < t.bits[l]; i++) { if (k >= 256 || code >= (1 << l)) return false; t.code[t.vals[k]] = (u16)code; t.len[t.vals[k]] = (u8)l; k++; code++; }
        t.maxcode[l] = t.bits[l] ? code - 1 : -1; code <<= 1;
    }
    t.present = true; return true;
}
// parses the markers up to and including the SOS header; false for anything that is not baseline / extended sequential 8-bit
inline bool parse_header(const u8* d, size_t n, Info& j) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
    size_t pos = 2; bool haveSof = false;
    while (pos + 4 <= n) {
        if (d[pos] != 0xFF) return false;
        while (pos + 1 < n && d[pos + 1] == 0xFF) pos++;
        int m = d[pos + 1]; pos += 2;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
        if (m == 0xD9) return false;
        if (pos + 2 > n) return false;
        size_t len = ((size_t)d[pos] << 8) | d[pos + 1]; if (len < 2 || pos + len > n) return false;
        const u8* s = d + pos + 2; size_t sl = len - 2;
        if (m == 0xC0 || m == 0xC1) {
            if (haveSof || sl < 6 || s[0] != 8) return false;
            j.height = (s[1] << 8) | s[2]; j.width = (s[3] << 8) | s[4]; j.ncomp = s[5];
            if (j.width < 1 || j.height < 1 || j.ncomp < 1 || j.ncomp > 4 || sl < (size_t)(6 + 3 * j.ncomp)) return false;
            for (int i = 0; i < j.ncomp; i++) { j.c[i].id = s[6 + 3 * i]; j.c[i].h = s[7 + 3 * i] >> 4; j.c[i].v = s[7 + 3 * i] & 15; if (j.c[i].h < 1 || j.c[i].h > 4 || j.c[i].v < 1 || j.c[i].v > 4) return false; }
            haveSof = true;
        } else if ((m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) || m == 0xC8 || m == 0xCC) return false;      // progressive / lossless / arithmetic
        else if (m == 0xC4) {
            size_t o = 0;
            while (o + 17 <= sl) {
                int tc = s[o] >> 4, th = s[o] & 15; if (tc > 1 || th > 3) return false;
                Huff& t = tc ? j.ac[th] : j.dc[th]; t = Huff(); size_t tot = 0;
                for (int l = 1; l <= 16; l++) { t.bits[l] = s[o + l]; tot += t.bits[l]; }
                if (tot > 256 || o + 17 + tot > sl) return false;
                for (size_t i = 0; i < tot; i++) t.vals[i] = s[o + 17 + i];
                if (!build(t)) return false;
                o += 17 + tot;
            }
        } else if (m == 0xDD) { if (sl < 2) return false; j.restart = (s[0] << 8) | s[1]; }
        else if (m == 0xDA) {
            if (!haveSof || sl < 1 || s[0] != j.ncomp || sl < (size_t)(1 + 2 * s[0] + 3)) return false;
            for (int i = 0; i < j.ncomp; i++) {
                int cid = s[1 + 2 * i], td = s[2 + 2 * i] >> 4, ta = s[2 + 2 * i] & 15; if (cid != j.c[i].id || td > 3 || ta > 3) return false;
                if (!j.dc[td].present || !j.ac[ta].present) return false;
                j.c[i].td = td; j.c[i].ta = ta;
            }
            size_t q = 1 + 2 * j.ncomp; if (s[q] != 0 || s[q + 1] != 63 || s[q + 2] != 0) return false;
            j.scanStart = pos + len;
            if (j.ncomp == 1 && (j.c[0].h != 1 || j.c[0].v != 1)) return false;
            for (int i = 0; i < j.ncomp; i++) { j.hmax = std::max(j.hmax, j.c[i].h); j.vmax = std::max(j.vmax, j.c[i].v); }
            j.mcusX = (j.width + 8 * j.hmax - 1) / (8 * j.hmax); j.mcusY = (j.height + 8 * j.vmax - 1) / (8 * j.vmax);
            u64 total = 0;
            for (int i = 0; i < j.ncomp; i++) { j.c[i].bw = j.mcusX * j.c[i].h; j.c[i].bh = j.mcusY * j.c[i].v; total += (u64)j.c[i].bw * j.c[i].bh * 64; }
            if (total > ((u64)200 << 20)) return false;
            for (int i = 0; i < j.ncomp; i++) { size_t nb = (size_t)j.c[i].bw * j.c[i].bh; j.c[i].coef.assign(nb * 64, 0); j.c[i].nz.assign(nb, 0); }
            return true;
        }
        pos += len;
    }
    return false;
}
struct BitReader {
    const u8* d; size_t n, pos; u8 cur = 0; int nb = 0; bool err = false;
    int get() {
        if (nb == 0) {
            if (pos >= n) { err = true; return 0; }
            u8 b = d[pos]; if (b == 0xFF) { if (pos + 1 < n && d[pos + 1] == 0x00) pos += 2; else { err = true; return 0; } } else pos++;
            cur = b; nb = 8;
        }
        nb--; return (cur >> nb) & 1;
    }
    int bits(int k) { int v = 0; while (k--) v = (v << 1) | get(); return v; }
    bool pad_ok() { if (nb > 0) { int rem = cur & ((1 << nb) - 1); if (rem != (1 << nb) - 1) return false; nb = 0; } return true; }
};
inline int hdecode(BitReader& br, const Huff& t) {
    int code = 0;
    for (int l = 1; l <= 16; l++) { code = (code << 1) | br.get(); if (t.maxcode[l] >= 0 && code <= t.maxcode[l] && code >= t.mincode[l]) return t.vals[t.valptr[l] + code - t.mincode[l]]; }
    return -1;
}
inline int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }
inline bool decode_scan(const u8* d, size_t n, Info& j, size_t& scanEnd) {
    BitReader br{d, n, j.scanStart}; int pred[4] = {0, 0, 0, 0}; size_t total = (size_t)j.mcusX * (size_t)j.mcusY, rst = 0;
    for (size_t m = 0; m < total; m++) {
        if (j.restart && m && m % (size_t)j.restart == 0) {
            if (!br.pad_ok() || br.pos + 2 > n || d[br.pos] != 0xFF || d[br.pos + 1] != 0xD0 + (int)(rst & 7)) return false;
            br.pos += 2; rst++; for (int i = 0; i < 4; i++) pred[i] = 0;
        }
        int mx = (int)(m % (size_t)j.mcusX), my = (int)(m / (size_t)j.mcusX);
        for (int ci = 0; ci < j.ncomp; ci++) {
            Comp& c = j.c[ci];
            for (int vy = 0; vy < c.v; vy++) for (int hx = 0; hx < c.h; hx++) {
                i16* blk = &c.coef[((size_t)(my * c.v + vy) * (size_t)c.bw + (size_t)(mx * c.h + hx)) * 64];
                int t = hdecode(br, j.dc[c.td]); if (t < 0 || t > 11) return false;
                int diff = t ? extend(br.bits(t), t) : 0; pred[ci] += diff; if (pred[ci] < -32000 || pred[ci] > 32000) return false; blk[0] = (i16)pred[ci];
                int k = 1;
                while (k < 64) {
                    int rs = hdecode(br, j.ac[c.ta]); if (rs < 0) return false;
                    int r = rs >> 4, s = rs & 15;
                    if (s == 0) { if (r == 15) { k += 16; continue; } break; }
                    if (s > 10) return false;
                    k += r; if (k > 63) return false; blk[k] = (i16)extend(br.bits(s), s); k++;
                }
                if (br.err) return false;
            }
        }
    }
    if (!br.pad_ok() || br.err) return false;
    scanEnd = br.pos; return true;
}
struct BitWriter {
    std::vector<u8>& out; u32 acc = 0; int n = 0;
    explicit BitWriter(std::vector<u8>& o) : out(o) {}
    void put(u32 code, int len) {
        acc = (acc << len) | code; n += len;
        while (n >= 8) { u8 b = (u8)(acc >> (n - 8)); out.push_back(b); if (b == 0xFF) out.push_back(0); n -= 8; }
        acc &= (n ? ((1u << n) - 1) : 0u);
    }
    void pad() { if (n) put((1u << (8 - n)) - 1, 8 - n); }
};
inline int bitlen(int v) { int n = 0; while (v) { n++; v >>= 1; } return n; }
inline bool encode_scan(const Info& j, std::vector<u8>& out) {
    BitWriter bw(out); int pred[4] = {0, 0, 0, 0}; size_t total = (size_t)j.mcusX * (size_t)j.mcusY, rst = 0;
    for (size_t m = 0; m < total; m++) {
        if (j.restart && m && m % (size_t)j.restart == 0) {
            bw.pad(); out.push_back(0xFF); out.push_back((u8)(0xD0 + (rst & 7))); rst++; for (int i = 0; i < 4; i++) pred[i] = 0;
        }
        int mx = (int)(m % (size_t)j.mcusX), my = (int)(m / (size_t)j.mcusX);
        for (int ci = 0; ci < j.ncomp; ci++) {
            const Comp& c = j.c[ci]; const Huff& dc = j.dc[c.td]; const Huff& ac = j.ac[c.ta];
            for (int vy = 0; vy < c.v; vy++) for (int hx = 0; hx < c.h; hx++) {
                const i16* blk = &c.coef[((size_t)(my * c.v + vy) * (size_t)c.bw + (size_t)(mx * c.h + hx)) * 64];
                int diff = blk[0] - pred[ci]; pred[ci] = blk[0]; int a = diff < 0 ? -diff : diff, t = bitlen(a);
                if (t > 11 || !dc.len[t]) return false;
                bw.put(dc.code[t], dc.len[t]); if (t) bw.put((u32)(diff < 0 ? diff + (1 << t) - 1 : diff), t);
                int r = 0;
                for (int k = 1; k < 64; k++) {
                    int v = blk[k]; if (v == 0) { r++; continue; }
                    while (r > 15) { if (!ac.len[0xF0]) return false; bw.put(ac.code[0xF0], ac.len[0xF0]); r -= 16; }
                    int av = v < 0 ? -v : v, s = bitlen(av), sym = (r << 4) | s;
                    if (s > 10 || !ac.len[sym]) return false;
                    bw.put(ac.code[sym], ac.len[sym]); bw.put((u32)(v < 0 ? v + (1 << s) - 1 : v), s); r = 0;
                }
                if (r > 0) { if (!ac.len[0]) return false; bw.put(ac.code[0], ac.len[0]); }
            }
        }
    }
    bw.pad(); return true;
}

// ---- coefficient model: three context-selected counters per decision, mixed by a small integer mixer
class Model {
public:
    Model() : t1_((size_t)1 << 20, kInit), t2_((size_t)1 << 20, kInit), t3_((size_t)1 << 20, kInit), mw_((size_t)kSel * 4, 22000) {}
    Enc* e = nullptr; Dec* d = nullptr;
    int bit(int y, u32 c1, u32 c2, u32 c3, u32 sel) {
        const Tables& T = tables();
        u32* s1 = &t1_[hmix(c1, 0x51ED27u) >> 12]; u32* s2 = &t2_[hmix(c2, 0x2545F49u) >> 12]; u32* s3 = &t3_[hmix(c3, 0x7F4A7C1u) >> 12];
        int st[4] = {T.stretch_[clampi((int)(*s1 >> 20), 4, 4091)], T.stretch_[clampi((int)(*s2 >> 20), 4, 4091)], T.stretch_[clampi((int)(*s3 >> 20), 4, 4091)], 128};
        i32* w = &mw_[(size_t)(sel % kSel) * 4];
        i64 dot = 0; for (int i = 0; i < 4; i++) dot += (i64)w[i] * st[i];
        int p = clampi(squash(clampi((int)(dot >> 16), -2047, 2047)), 1, 4095);
        int yy; if (e) { e->bit(y, p); yy = y; } else yy = d->bit(p);
        int err = ((yy << 12) - p) * 6; for (int i = 0; i < 4; i++) w[i] += (st[i] * err + 0x8000) >> 16;
        sm_up(s1, yy, 255); sm_up(s2, yy, 255); sm_up(s3, yy, 255);
        return yy;
    }
private:
    static constexpr int kSel = 4096;
    std::vector<u32> t1_, t2_, t3_; std::vector<i32> mw_;
};
inline u32 H(u32 a, u32 b = 0, u32 c = 0, u32 d = 0, u32 e = 0) { return hmix(hmix(hmix(hmix(a + 1, b), c), d), e); }
inline int qb(int x) { return x < 4 ? x : 2 + bitlen(x); }
// codes one block (both directions); coef values are read when encoding and written when decoding
inline void code_block(Model& M, Info& j, int ci, int bx, int by, int& prevNz, int& prevDcMag) {
    Comp& c = j.c[ci]; const bool enc = M.e != nullptr; const int ct = ci ? 1 : 0, bw = c.bw;
    i16* blk = &c.coef[((size_t)by * (size_t)bw + (size_t)bx) * 64];
    const i16* L = bx > 0 ? blk - 64 : nullptr; const i16* A = by > 0 ? blk - (size_t)bw * 64 : nullptr; const i16* AL = (bx > 0 && by > 0) ? blk - (size_t)bw * 64 - 64 : nullptr;
    // ---- DC: residual against a gradient prediction from the left / above blocks
    int pred, gb = 15;
    if (L && A) { int l = L[0], a = A[0], al = AL[0], g = l + a - al, lo = std::min(l, a), hi = std::max(l, a); pred = g < lo ? lo : (g > hi ? hi : g); gb = bitlen(std::abs(l - a)); }
    else if (L) pred = L[0]; else if (A) pred = A[0]; else pred = 0;
    int e = enc ? blk[0] - pred : 0, ae = e < 0 ? -e : e, s = bitlen(ae);
    int zero = M.bit(ae == 0, H(20, ct, gb), H(21, ct, prevDcMag), H(22, ct, gb, prevDcMag), 20 * 64 + (u32)ct);
    int val = 0;
    if (!zero) {
        int sg = M.bit(e < 0, H(23, ct, gb, L && A ? (L[0] > A[0]) : 2), H(24, ct, prevDcMag), H(25, ct), 21 * 64 + (u32)ct);
        int sz = 1; for (; sz < 12; sz++) { int more = M.bit(s > sz, H(26, ct, gb, sz), H(27, ct, prevDcMag, sz), H(28, ct, sz), 22 * 64 + (u32)sz); if (!more) break; }
        int mag = 1; for (int b = sz - 2; b >= 0; b--) { int y = M.bit((ae >> b) & 1, H(29, sz, b), H(30, ct, sz, b, mag & 3), H(31, b), 23 * 64 + (u32)b); mag = (mag << 1) | y; }
        val = sg ? -mag : mag;
    }
    if (!enc) blk[0] = (i16)(pred + val);
    prevDcMag = bitlen(std::abs(val));
    // ---- number of non-zero AC coefficients (6-bit tree), then the coefficients in zig-zag order
    const size_t bi = (size_t)by * (size_t)bw + (size_t)bx;
    int nl = L ? c.nz[bi - 1] : -1, na = A ? c.nz[bi - (size_t)bw] : -1, cnt = (nl >= 0) + (na >= 0), avg = cnt ? (std::max(nl, 0) + std::max(na, 0) + cnt / 2) / cnt : 0;
    int nzv = 0; if (enc) for (int k = 1; k < 64; k++) nzv += blk[k] != 0;
    int node = 1;
    for (int b = 5; b >= 0; b--) {
        int y = M.bit((nzv >> b) & 1, H(1, ct, node, avg, cnt), H(2, ct, node, nl + 1, na + 1), H(3, ct, node, prevNz), 1 * 64 + (u32)(5 - b));
        node = (node << 1) | y;
    }
    nzv = node & 63; prevNz = nzv; c.nz[bi] = (u8)nzv;
    int remain = nzv;
    for (int k = 1; k < 64 && remain > 0; k++) {
        int v = enc ? blk[k] : 0, p1 = k >= 2 ? std::abs(blk[k - 1]) : 0, p2 = k >= 3 ? std::abs(blk[k - 2]) : 0;
        int ml = L ? std::abs(L[k]) : -1, ma = A ? std::abs(A[k]) : -1, nbm = (ml >= 0 && ma >= 0) ? (ml + ma + 1) / 2 : std::max(std::max(ml, ma), 0);
        int nzf;
        if (remain == 64 - k) nzf = 1;                                           // every position left must be non-zero
        else nzf = M.bit(v != 0, H(40, ct, (u32)k, qb(nbm), (ml >= 0) + (ma >= 0)), H(41, ct, (u32)k, std::min(p1, 7), std::min(p2, 7)), H(42, ct, (u32)k, std::min(remain, 15), std::min((64 - k) / 4, 15)), 2 * 64 + (u32)k);
        if (!nzf) { continue; }
        remain--;
        int av = v < 0 ? -v : v, sv = bitlen(av), mag;
        int sz = 1; for (; sz < 10; sz++) { int more = M.bit(sv > sz, H(50, ct, (u32)k, (u32)sz, qb(nbm)), H(51, ct, (u32)k, (u32)sz, bitlen(p1)), H(52, ct, (u32)sz, bitlen(nbm), std::min(remain, 7)), 3 * 64 + (u32)std::min(k, 63)); if (!more) break; }
        mag = 1; for (int b = sz - 2; b >= 0; b--) { int y = M.bit((av >> b) & 1, H(53, (u32)sz, (u32)b, b == sz - 2 ? (u32)k : 0u), H(54, ct, (u32)sz, (u32)b), H(55, (u32)b, qb(nbm)), 4 * 64 + (u32)b); mag = (mag << 1) | y; }
        int sl = L ? (L[k] > 0 ? 1 : (L[k] < 0 ? 2 : 0)) : 3, sa = A ? (A[k] > 0 ? 1 : (A[k] < 0 ? 2 : 0)) : 3;
        int neg = M.bit(v < 0, H(56, ct, (u32)k, sl, sa), H(57, (u32)k, sl), H(58, (u32)k, sa), 5 * 64 + (u32)k);
        if (!enc) blk[k] = (i16)(neg ? -mag : mag);
    }
}
inline void code_all(Model& M, Info& j) {
    int prevNz[4] = {0, 0, 0, 0}, prevDc[4] = {0, 0, 0, 0};
    size_t total = (size_t)j.mcusX * (size_t)j.mcusY;
    for (size_t m = 0; m < total; m++) {
        int mx = (int)(m % (size_t)j.mcusX), my = (int)(m / (size_t)j.mcusX);
        for (int ci = 0; ci < j.ncomp; ci++) { Comp& c = j.c[ci]; for (int vy = 0; vy < c.v; vy++) for (int hx = 0; hx < c.h; hx++) code_block(M, j, ci, mx * c.h + hx, my * c.v + vy, prevNz[ci], prevDc[ci]); }
    }
}
}  // namespace jpg

// payload = [varint hdrLen][varint tailLen][varint blobLen][blob stream][coefficient stream]; blob = header bytes + tail bytes
inline bool jpeg_try(const u8* d, size_t n, std::vector<u8>& payload) {
    jpg::Info j; if (!jpg::parse_header(d, n, j)) return false;
    size_t scanEnd = 0; if (!jpg::decode_scan(d, n, j, scanEnd)) return false;
    std::vector<u8> re; re.reserve(scanEnd - j.scanStart + 16);
    if (!jpg::encode_scan(j, re) || re.size() != scanEnd - j.scanStart || std::memcmp(re.data(), d + j.scanStart, re.size()) != 0) return false;   // must be bit-exact
    std::vector<u8> blob(d, d + j.scanStart); blob.insert(blob.end(), d + scanEnd, d + n);
    if (blob.size() > kSegment) return false;
    std::vector<u8> blobOut; Counters dummy; encode_segment(blob.data(), blob.size(), 20, Hint(), blobOut, dummy);
    std::vector<u8> coefOut; Enc enc(coefOut); jpg::Model* M = new jpg::Model(); M->e = &enc; jpg::code_all(*M, j); delete M; enc.finish();
    payload.clear(); put_varint(payload, j.scanStart); put_varint(payload, n - scanEnd); put_varint(payload, blobOut.size());
    payload.insert(payload.end(), blobOut.begin(), blobOut.end()); payload.insert(payload.end(), coefOut.begin(), coefOut.end());
    return true;
}
inline bool jpeg_restore(const std::vector<u8>& payload, std::vector<u8>& file) {
    const u8* p = payload.data(); const u8* end = p + payload.size(); u64 v[3];
    for (int i = 0; i < 3; i++) { v[i] = 0; int sh = 0; for (;;) { if (p >= end || sh > 56) return false; u8 c = *p++; v[i] |= (u64)(c & 127) << sh; if (!(c & 128)) break; sh += 7; } }
    u64 hdrLen = v[0], tailLen = v[1], blobLen = v[2];
    if (blobLen > (u64)(end - p) || hdrLen + tailLen > kSegment || hdrLen < 4) return false;
    std::vector<u8> blob((size_t)(hdrLen + tailLen)); jpg::Info j;
    try { decode_segment(p, (size_t)blobLen, blob.data(), blob.size(), 20, Hint(), nullptr); } catch (const Failure&) { return false; }
    if (!jpg::parse_header(blob.data(), (size_t)hdrLen, j) || j.scanStart != hdrLen) return false;
    const u8* cp = p + blobLen; Dec dec(cp, (size_t)(end - cp)); jpg::Model* M = new jpg::Model(); M->d = &dec; jpg::code_all(*M, j); delete M;
    std::vector<u8> scan; if (!jpg::encode_scan(j, scan)) return false;
    file.assign(blob.begin(), blob.begin() + (size_t)hdrLen); file.insert(file.end(), scan.begin(), scan.end()); file.insert(file.end(), blob.begin() + (size_t)hdrLen, blob.end());
    return true;
}

inline Hint make_hint(const Header& h, u64 segStart, size_t L, int audioCode) {
    Hint t; t.stride = h.stride; t.bpp = h.bpp;
    if (h.grid.on()) { t.stride = h.grid.stride; t.bpp = h.grid.width; }
    if (h.audio.on() && audioCode > 0) {
        size_t ts, te; audio_region(h.audio, segStart, L, ts, te);
        if (te > ts) { t.ats = ts; t.ate = te; t.channels = h.audio.channels; t.width = h.audio.width; }
    }
    return t;
}
inline void remove_quiet(const fs::path& p) { std::error_code ec; fs::remove(p, ec); }
inline Result fail(Status s, const std::string& m) { Result r; r.status = s; r.message = m; return r; }

// shared state of one parallel run
struct Shared {
    sys::Mutex m; sys::CondVar cv;
    u64 nextWrite = 0; unsigned alive = 0; bool failed = false; Status st = Status::Ok; std::string msg;
    std::atomic<u64> nextSeg{0}; Counters cn; bool userCancel = false;
    void error(Status s, const std::string& text) { sys::Lock l(m); if (!failed) { failed = true; st = s; msg = text; } cn.cancel = true; cv.notify_all(); }
};
// Memory per worker: ~195 MB with 22-bit tables, ~115 MB with 21-bit tables.
inline u64 per_thread_bytes(int tbMax) { return (u64)(tbMax >= 22 ? 200 : 120) << 20; }
inline unsigned pick_threads(const Options& o, u64 segments, int tbMax) {
    unsigned t = o.threads ? o.threads : std::min(sys::hardware_threads(), 16u);
    u64 budget = o.memoryBudget ? o.memoryBudget : ((u64)2 << 30);
    u64 cap = budget / per_thread_bytes(tbMax); t = (unsigned)std::min<u64>(t, std::max<u64>(1, cap));
    if (t > segments) t = (unsigned)std::max<u64>(1, segments);
    return std::max(1u, t);
}
// compression: largest tables that fit the memory budget with the wanted number of threads
inline void plan(const Options& o, u64 segments, unsigned& threads, int& tbMax) {
    unsigned want = o.threads ? o.threads : std::min(sys::hardware_threads(), 16u);
    if (want > segments) want = (unsigned)std::max<u64>(1, segments);
    u64 budget = o.memoryBudget ? o.memoryBudget : ((u64)2 << 30);
    if ((u64)want * per_thread_bytes(22) <= budget) { threads = want; tbMax = 22; return; }
    tbMax = 21; threads = (unsigned)std::min<u64>(want, std::max<u64>(1, budget / per_thread_bytes(21)));
}
// runs `worker` on n threads and polls progress on the calling thread until all are done
inline void run_workers(Shared& sh, unsigned n, u64 total, const std::function<void()>& worker, const ProgressFn& progress) {
    std::vector<std::unique_ptr<sys::Thread>> th;
    { sys::Lock l(sh.m); sh.alive = n; }
    for (unsigned i = 0; i < n; i++) {
        th.emplace_back(new sys::Thread());
        bool ok = th.back()->start([&sh, &worker] {
            try { worker(); } catch (const Cancelled&) {} catch (const Failure& f) { sh.error(f.status, f.msg); }
            catch (const std::bad_alloc&) { sh.error(Status::OutOfMemory, "not enough memory"); }
            catch (const std::exception& e) { sh.error(Status::IoError, e.what()); }
            sys::Lock l(sh.m); sh.alive--; sh.cv.notify_all();
        });
        if (!ok) { sys::Lock l(sh.m); sh.alive--; if (i == 0) { sh.failed = true; sh.st = Status::OutOfMemory; sh.msg = "cannot start worker threads"; } }
    }
    for (;;) {
        { sys::Lock l(sh.m); if (sh.alive == 0) break; sh.cv.wait_ms(sh.m, 100); }
        double f = total ? (double)sh.cn.done.load() / (double)total : 1.0;
        if (progress && !progress(f > 1.0 ? 1.0 : f, sh.cn.out.load())) { sys::Lock l(sh.m); sh.userCancel = true; sh.cn.cancel = true; sh.cv.notify_all(); }
    }
    for (auto& t : th) t->join();
}

}  // namespace detail

namespace detail {
// Encoder-side choice for one segment: 0 = plain bytes, 1..3 = audio presets 0..2. Trial-codes a sample of the
// PCM region each way (about 128 KB each) and keeps the smallest. Decoding never needs to repeat this.
inline int choose_audio_code(const Header& h, const u8* seg, size_t L, u64 segStart, int tbMax) {
    size_t ts, te; audio_region(h.audio, segStart, L, ts, te);
    if (te <= ts) return 0;
    size_t frame = (size_t)h.audio.frame(), span = te - ts, want = std::min<size_t>(span, (size_t)128 << 10);
    want -= want % frame; if (want < frame * 256) return 0;
    size_t from = ts + ((span - want) / 2 / frame) * frame;           // sample from the middle of the region
    std::vector<u8> work(seg + from, seg + from + want), out; Counters dummy;
    Hint none; encode_segment(work.data(), want, tbMax, none, out, dummy); const size_t plain = out.size();
    size_t best = (size_t)-1; int code = 0;
    for (int p = 0; p < 3; p++) {
        std::memcpy(work.data(), seg + from, want);
        AudioCodec(h.audio.channels, h.audio.width, p).forward(work.data(), want / frame);
        Hint hint; hint.ats = 0; hint.ate = want; hint.channels = h.audio.channels; hint.width = h.audio.width;
        encode_segment(work.data(), want, tbMax, hint, out, dummy);
        if (out.size() < best) { best = out.size(); code = p + 1; }
    }
    if (best + best / 64 >= plain) code = 0;          // plain bytes unless audio mode is clearly (1.5%) better
    return code;
}

// Headerless data: looks for (a) 16-bit PCM audio, (b) a record / image-row size, (c) a grid transform (word differences or
// gradient residuals). Cheap statistics on a 2 MB sample decide which candidates are worth a trial; each is trial-coded on a
// 256 KB slice against plain coding. If several beat plain, the finalists are re-tested head-to-head on a larger slice.
inline void detect_structure(const fs::path& src, u64 n, Header& h, bool execOnly = false) {
    if (n < ((u64)512 << 10)) return;
    const size_t S = (size_t)std::min<u64>(n, (u64)2 << 20);
    std::vector<u8> smp(S); const u64 from = ((n - S) / 2) & ~(u64)7;
    { File f; if (!f.open(src, "rb") || seek64(f.f, from) != 0 || std::fread(smp.data(), 1, S, f.f) != S) return; }
    const int kTb = 22;
    struct Cand { int kind = -1; Audio a; int stride = 0, bpp = 0; Grid g; int ek = 0; size_t sz = (size_t)-1; };   // kind: 0 audio, 1 stride hint, 2 grid, 3 executable filter
    auto build = [&](const Cand& c, size_t off, size_t len, std::vector<u8>& data, Hint& hint) {
        data.assign(smp.begin() + off, smp.begin() + off + len); hint = Hint();
        if (c.kind == 0) {
            size_t frame = (size_t)c.a.frame(), usable = (len / frame) * frame; data.resize(usable);
            AudioCodec(c.a.channels, 2, 1).forward(data.data(), usable / frame);
            hint.ats = 0; hint.ate = usable; hint.channels = c.a.channels; hint.width = 2;
        } else if (c.kind == 1) { hint.stride = c.stride; hint.bpp = c.bpp; }
        else if (c.kind == 3) { exec_filter(data.data(), len, from + off, c.ek, true); }
        else {
            size_t ts, te; if (grid_region(c.g, from + off, len, ts, te)) GridCodec(c.g).forward(data.data() + ts, te - ts);
            hint.stride = c.g.stride; hint.bpp = c.g.width;
        }
    };
    auto trialOf = [&](const Cand& c, size_t off, size_t len) {
        std::vector<u8> d, o; Hint hint; Counters dummy; build(c, off, len, d, hint);
        encode_segment(d.data(), d.size(), kTb, hint, o, dummy); return o.size();
    };
    const size_t T = std::min<size_t>(S, (size_t)256 << 10), tOff = ((S - T) / 2) & ~(size_t)7;
    Cand plainC; plainC.kind = 1;                                  // stride 0 / bpp 0 == plain
    size_t plain = 0; bool havePlain = false;
    auto plainSize = [&]() { if (!havePlain) { plain = trialOf(plainC, tOff, T); havePlain = true; } return plain; };
    Cand bestAudio, bestStr, bestGrid, bestExec;                    // best of each kind (vs plain, 2% margin)

    // (a) headerless 16-bit PCM, mono or stereo: samples must be much smoother than they are large
    for (int ch = 1; ch <= (execOnly ? 0 : 2); ch++) {
        size_t fb = (size_t)(2 * ch), frames = std::min<size_t>(S / fb, 40000) - 1; double d = 0, a = 0;
        for (size_t t = 1; t <= frames; t++) {
            int cur = (int)(i16)(smp[t * fb] | (smp[t * fb + 1] << 8)), prv = (int)(i16)(smp[(t - 1) * fb] | (smp[(t - 1) * fb + 1] << 8));
            d += std::abs(cur - prv); a += std::abs(cur);
        }
        if (a > 0 && d / a < 0.8) {
            Cand c; c.kind = 0; c.a.dataOff = 0; c.a.dataLen = n; c.a.channels = ch; c.a.width = 2;
            c.sz = trialOf(c, tOff, T); size_t ref = plainSize();
            if (c.sz + c.sz / 32 < ref && c.sz < bestAudio.sz) bestAudio = c;
        }
    }
    // (d) executable code: 8 stripes of 256 KB spread over the whole file (code is rarely in the middle); the stripe with the most
    //     branch instructions (x86 E8/E9, ARM64 BL, ARM32 BL) is trial-coded with and without the filter
    {
        const size_t SS = (size_t)256 << 10; const int ns = (int)std::min<u64>(8, n / SS);
        size_t bestHits[4] = {0, 0, 0, 0}; u64 bestOff[4] = {0, 0, 0, 0};
        File f2; std::vector<u8> st(SS);
        if (ns >= 1 && f2.open(src, "rb")) {
            for (int k = 0; k < ns; k++) {
                u64 off = (ns == 1 ? 0 : (u64)k * (n - SS) / (u64)(ns - 1)) & ~(u64)7;
                if (seek64(f2.f, off) != 0 || std::fread(st.data(), 1, SS, f2.f) != SS) break;
                size_t x86 = 0, a64 = 0, a32 = 0;
                for (size_t i = 0; i + 5 <= SS; i++) if ((st[i] == 0xE8 || st[i] == 0xE9) && (st[i + 4] == 0 || st[i + 4] == 0xFF)) x86++;
                for (size_t i = 0; i + 4 <= SS; i += 4) { if ((st[i + 3] >> 2) == 0x25) a64++; if (st[i + 3] == 0xEB) a32++; }
                size_t hs[4] = {0, x86, a64, a32};
                for (int q = 1; q <= 3; q++) if (hs[q] > bestHits[q]) { bestHits[q] = hs[q]; bestOff[q] = off; }
            }
            for (int q = 1; q <= 3; q++) {
                if (bestHits[q] < SS / 2500) continue;                            // ~100 hits per stripe
                if (seek64(f2.f, bestOff[q]) != 0 || std::fread(st.data(), 1, SS, f2.f) != SS) break;
                std::vector<u8> o1, o2, w = st; Counters dummy; Hint none;
                encode_segment(st.data(), SS, kTb, none, o1, dummy);
                exec_filter(w.data(), SS, bestOff[q], q, true);
                encode_segment(w.data(), SS, kTb, none, o2, dummy);
                if (o2.size() + o2.size() / 66 < o1.size() && o2.size() < bestExec.sz) { bestExec = Cand(); bestExec.kind = 3; bestExec.ek = q; bestExec.sz = o2.size(); }
            }
        }
    }
    if (execOnly) { if (bestExec.kind >= 0) h.exec = bestExec.ek; return; }
    // (b) record length / image row length: byte difference at lag L far below that of unrelated bytes, and a local dip
    const size_t maxLag = std::min<size_t>(S / 4, 49152), minLag = 8, np = 3000;
    std::vector<double> sad(maxLag + 1, 0.0);
    size_t first = maxLag, span = S - first, stepP = std::max<size_t>(1, span / np);
    std::vector<size_t> pos; for (size_t i = first; i < S; i += stepP) pos.push_back(i);
    for (size_t L = 1; L <= maxLag; L++) { u64 acc = 0; for (size_t i : pos) { int dv = (int)smp[i] - (int)smp[i - L]; acc += (u64)(dv < 0 ? -dv : dv); } sad[L] = (double)acc / (double)pos.size(); }
    std::vector<double> srt(sad.begin() + 1, sad.end()); std::nth_element(srt.begin(), srt.begin() + srt.size() / 2, srt.end());
    const double median = srt[srt.size() / 2];
    std::vector<size_t> cand;
    {
        std::vector<std::pair<double, size_t>> order;
        for (size_t L = minLag + 3; L + 3 <= maxLag; L++) {
            if (!(sad[L] < 0.8 * median)) continue;
            double nb = (sad[L - 3] + sad[L - 2] + sad[L + 2] + sad[L + 3]) / 4.0, prom = (nb - sad[L]) / (nb > 0 ? nb : 1.0);
            if (prom >= 0.03 && sad[L] <= sad[L - 1] && sad[L] <= sad[L + 1]) order.push_back({prom, L});
        }
        std::sort(order.begin(), order.end(), [](const std::pair<double, size_t>& x, const std::pair<double, size_t>& y) { return x.first > y.first; });
        for (auto& pr : order) { size_t L = pr.second; bool tooClose = false; for (size_t c : cand) if ((L > c ? L - c : c - L) <= 4) tooClose = true; if (!tooClose) cand.push_back(L); if (cand.size() == 2) break; }
    }
    if (!cand.empty()) {
        int bppA = 1; double bs = sad[1]; for (int k = 2; k <= 4; k++) if (sad[k] < bs) { bs = sad[k]; bppA = k; }
        std::vector<int> bpps; bpps.push_back(bppA); if (bppA != 1) bpps.push_back(1);
        for (size_t L : cand) for (int bp : bpps) {
            Cand c; c.kind = 1; c.stride = (int)L; c.bpp = bp; c.sz = trialOf(c, tOff, T); size_t ref = plainSize();
            if (c.sz + c.sz / 50 < ref && c.sz < bestStr.sz) bestStr = c;
        }
    }
    // (c) grid transform, screened by order-0 entropy (top 2 configurations get a trial)
    {
        auto h0 = [](const u8* p, size_t len) { u32 c[256]; std::memset(c, 0, sizeof c); for (size_t i = 0; i < len; i++) c[p[i]]++; double e = 0; for (int i = 0; i < 256; i++) if (c[i]) e -= (double)c[i] * std::log2((double)c[i] / (double)len); return e / (double)len; };
        std::vector<int> strides; for (size_t L : cand) strides.push_back((int)L); strides.push_back(2); strides.push_back(4);
        struct Cfg { Grid g; double ent; }; std::vector<Cfg> cfgs;
        for (int St : strides) for (int w : {1, 2, 4}) {
            if (St % w || St < w) continue;
            for (int mode = 0; mode < 2; mode++) {
                if (mode == 1 && St == w) continue;
                for (int zz = 0; zz < 2; zz++) {
                    Grid g; g.stride = St; g.width = w; g.mode = mode; g.zz = zz;
                    size_t ts, te; if (!grid_region(g, from + tOff, T, ts, te) || te - ts < (size_t)St * 8) continue;
                    std::vector<u8> w2(smp.begin() + tOff, smp.begin() + tOff + T); GridCodec(g).forward(w2.data() + ts, te - ts);
                    cfgs.push_back({g, h0(w2.data() + ts, te - ts)});
                }
            }
        }
        if (!cfgs.empty()) {
            double hPlain = h0(smp.data() + tOff, T);
            std::sort(cfgs.begin(), cfgs.end(), [](const Cfg& a, const Cfg& b) { return a.ent < b.ent; });
            for (size_t i = 0; i < cfgs.size() && i < 2; i++) {
                if (cfgs[i].ent > 0.97 * hPlain) break;
                Cand c; c.kind = 2; c.g = cfgs[i].g; c.sz = trialOf(c, tOff, T); size_t ref = plainSize();
                if (c.sz + c.sz / 50 < ref && c.sz < bestGrid.sz) bestGrid = c;
            }
        }
    }
    std::vector<Cand> fin; if (bestAudio.kind >= 0) fin.push_back(bestAudio); if (bestStr.kind >= 0) fin.push_back(bestStr); if (bestGrid.kind >= 0) fin.push_back(bestGrid);
    if (bestExec.kind >= 0) { h.exec = bestExec.ek; return; }     // code files: the filter is chosen on its own stripes
    if (fin.empty()) return;
    size_t pick = 0;
    if (fin.size() > 1) {                                           // head-to-head on a larger slice
        const size_t big = std::min<size_t>(S, (size_t)768 << 10), bOff = ((S - big) / 2) & ~(size_t)7;
        size_t bestSz = (size_t)-1;
        for (size_t i = 0; i < fin.size(); i++) { size_t z = trialOf(fin[i], bOff, big); if (z < bestSz) { bestSz = z; pick = i; } }
    }
    const Cand& w = fin[pick];
    if (w.kind == 0) h.audio = w.a; else if (w.kind == 1) { h.stride = w.stride; h.bpp = w.bpp; } else if (w.kind == 3) h.exec = w.ek; else h.grid = w.g;
}
}  // namespace detail

// ------------------------------------------------------------------ public functions
// Compress src into <outDir>/<stem>.3p. The original name + extension are stored in the metadata.
inline Result compress_file(const fs::path& src, const Options& opt, const ProgressFn& progress) {
    using namespace detail;
    fs::path tmp;
    try {
        std::error_code ec;
        u64 n = (u64)fs::file_size(src, ec); if (ec) return fail(Status::IoError, "cannot read file size: " + ec.message());
        Header h; h.name = to_utf8(src.filename()); h.total = n;
        if (h.name.empty() || h.name.size() > kMaxName) return fail(Status::IoError, "file name is empty or too long");
        { std::string e = to_utf8(src.extension()); for (char& c : e) c = (char)std::tolower((unsigned char)c); if (e == extension()) return fail(Status::BadFormat, "already a .3p file"); }
        { File f; if (!f.open(src, "rb")) return fail(Status::IoError, "cannot open the source file");
          if (!detect_wav(f.f, n, h.audio)) { h.audio = Audio(); seek64(f.f, 0); u8 head[64]; size_t got = std::fread(head, 1, sizeof head, f.f); detect_geometry(head, got, h.stride, h.bpp); } }

        // long-distance duplicates (not for WAV / BMP: their audio / pixel modelling relies on the exact byte layout)
        u64 streamLen = n; std::vector<LitRun> runs; double base = 0.0;
        if (opt.detect && !h.audio.on() && !h.stride && n >= 2048 && n <= ((u64)256 << 20)) {          // baseline JPEG: coefficient recompression
            File jf; u8 sig[3] = {0, 0, 0};
            if (jf.open(src, "rb") && std::fread(sig, 1, 3, jf.f) == 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF) {
                std::vector<u8> jd((size_t)n); seek64(jf.f, 0);
                if (std::fread(jd.data(), 1, jd.size(), jf.f) == jd.size()) {
                    std::vector<u8> payload;
                    if (jpeg_try(jd.data(), jd.size(), payload) && payload.size() + 64 < (size_t)n) { h.jpeg = true; h.jpegPayload = std::move(payload); h.fileCrc = crc32(jd.data(), jd.size()); streamLen = 0; }
                }
            }
        }
        if (opt.detect && !h.jpeg && !h.audio.on() && !h.stride && detect_mt19937(src, n, h)) streamLen = 0;      // whole file regenerable: no frames at all
        if (!h.gen && !h.jpeg && !h.audio.on() && !h.stride && opt.dedup && n >= ((u64)2 << 20)) {
            CrcJob job; job.path = src; sys::Thread crcThread; crcThread.start([&job] { crc_file(&job); });
            std::vector<Match> found; u64 matched = 0; bool cancelled = false;
            bool ok = find_duplicates(src, n, (u64)256 << 20, [&](double f) { return !progress || progress(0.03 * f, 0); }, found, matched, cancelled);
            crcThread.join();
            if (!ok) return fail(cancelled ? Status::Cancelled : Status::IoError, cancelled ? "cancelled" : "could not read the source file");
            base = 0.03;
            if (job.ok && matched >= std::max<u64>(n / 100, (u64)65536)) {
                h.dedup = true; h.matches = std::move(found); h.fileCrc = job.crc; streamLen = n - matched; runs = make_runs(h.matches, n);
            }
        }
        if (!h.gen && !h.jpeg && !h.dedup && !h.audio.on() && !h.stride && opt.detect) detect_structure(src, n, h);
        else if (h.dedup && opt.detect) detect_structure(src, n, h, true);            // copy records + executable filter
        u64 nseg = (streamLen + kSegment - 1) / kSegment;
        unsigned threads = 1;
        plan(opt, nseg, threads, h.tbMax);

        fs::path dir = opt.outDir.empty() ? src.parent_path() : opt.outDir; if (dir.empty()) dir = ".";
        fs::path finalPath = unique_path(dir, to_utf8(src.stem()) + extension());
        tmp = finalPath; tmp += ".tmp";
        File out; if (!out.open(tmp, "wb")) return fail(Status::IoError, "cannot create the output file (folder not writable?)");
        std::vector<u8> hv = make_header(h);
        if (std::fwrite(hv.data(), 1, hv.size(), out.f) != hv.size()) return fail(Status::IoError, "write failed");

        Shared sh; u64 written = hv.size(); sh.cn.out = hv.size();
        auto worker = [&]() {
            File f; if (!f.open(src, "rb")) throw Failure{Status::IoError, "cannot open the source file"};
            std::vector<u8> seg(kSegment), payload, check;
            for (;;) {
                u64 idx = sh.nextSeg.fetch_add(1);
                if (idx >= nseg || sh.cn.cancel.load()) return;
                size_t L = (size_t)std::min<u64>(kSegment, streamLen - idx * kSegment);
                if (h.dedup ? !read_literals(f.f, runs, idx * kSegment, L, seg.data()) : (seek64(f.f, idx * kSegment) != 0 || std::fread(seg.data(), 1, L, f.f) != L)) throw Failure{Status::IoError, "could not read the source file (changed while reading?)"};
                u32 crc = crc32(seg.data(), L);
                int code = h.audio.on() ? choose_audio_code(h, seg.data(), L, idx * kSegment, h.tbMax) : 0;
                Hint hint = make_hint(h, idx * kSegment, L, code);
                size_t gts = 0, gte = 0; bool gridOn = grid_region(h.grid, idx * kSegment, L, gts, gte);
                if (hint.ate > hint.ats) AudioCodec(h.audio.channels, h.audio.width, code - 1).forward(seg.data() + hint.ats, (size_t)((hint.ate - hint.ats) / (u64)h.audio.frame()));
                if (gridOn) GridCodec(h.grid).forward(seg.data() + gts, gte - gts);
                if (h.exec) exec_filter(seg.data(), L, idx * kSegment, h.exec, true);
                encode_segment(seg.data(), L, h.tbMax, hint, payload, sh.cn);
                bool modeled = payload.size() < L;
                if (modeled && opt.verify) {
                    check.resize(L); decode_segment(payload.data(), payload.size(), check.data(), L, h.tbMax, hint, nullptr);
                    if (hint.ate > hint.ats) AudioCodec(h.audio.channels, h.audio.width, code - 1).inverse(check.data() + hint.ats, (size_t)((hint.ate - hint.ats) / (u64)h.audio.frame()));
                    if (gridOn) GridCodec(h.grid).inverse(check.data() + gts, gte - gts);
                    if (h.exec) exec_filter(check.data(), L, idx * kSegment, h.exec, false);
                    if (crc32(check.data(), L) != crc) throw Failure{Status::Corrupt, "verification failed: decoded data differs from the original"};
                }
                if (!modeled) {
                    if (hint.ate > hint.ats || gridOn || h.exec) { if (h.dedup ? !read_literals(f.f, runs, idx * kSegment, L, seg.data()) : (seek64(f.f, idx * kSegment) != 0 || std::fread(seg.data(), 1, L, f.f) != L)) throw Failure{Status::IoError, "could not read the source file"}; }
                    payload.assign(seg.begin(), seg.begin() + L);
                }
                std::vector<u8> fh; put_varint(fh, ((u64)payload.size() << 3) | (u64)((modeled ? code : 0) << 1) | (modeled ? 1u : 0u));
                for (int i = 0; i < 4; i++) fh.push_back((u8)(crc >> (8 * i)));
                sys::Lock l(sh.m);
                while (sh.nextWrite != idx && !sh.cn.cancel.load()) sh.cv.wait(sh.m);
                if (sh.cn.cancel.load()) return;
                if (std::fwrite(fh.data(), 1, fh.size(), out.f) != fh.size() || std::fwrite(payload.data(), 1, payload.size(), out.f) != payload.size()) {
                    sh.failed = true; sh.st = Status::DiskFull; sh.msg = "write failed (disk full?)"; sh.cn.cancel = true; sh.cv.notify_all(); return; }
                written += fh.size() + payload.size(); sh.nextWrite++; sh.cv.notify_all();
            }
        };
        ProgressFn mapped = progress ? ProgressFn([&](double f, u64 o) { return progress(base + (1.0 - base) * f, o); }) : ProgressFn();
        run_workers(sh, threads, streamLen, worker, mapped);
        if (sh.failed) { out.close(); remove_quiet(tmp); return fail(sh.st, sh.msg); }
        if (sh.userCancel || sh.nextWrite != nseg) { out.close(); remove_quiet(tmp); return fail(Status::Cancelled, "cancelled"); }
        if (std::fflush(out.f) != 0) { out.close(); remove_quiet(tmp); return fail(Status::DiskFull, "write failed (disk full?)"); }
        out.close();
        fs::rename(tmp, finalPath, ec);
        if (ec) { remove_quiet(tmp); return fail(Status::IoError, "cannot finalize the output file: " + ec.message()); }
        Result r; r.inSize = n; r.outSize = written; r.outPath = finalPath; r.storedName = h.name;
        if (progress) progress(1.0, written);
        return r;
    } catch (const std::bad_alloc&) { if (!tmp.empty()) remove_quiet(tmp); return fail(Status::OutOfMemory, "not enough memory");
    } catch (const std::exception& e) { if (!tmp.empty()) remove_quiet(tmp); return fail(Status::IoError, e.what()); }
}

namespace detail {
inline Result restore_jpeg(const fs::path& src, u64 fileSize, const Header& h, const Options& opt, const ProgressFn& progress) {
    fs::path tmp;
    try {
        std::vector<u8> file;
        if (!jpeg_restore(h.jpegPayload, file) || file.size() != h.total || crc32(file.data(), file.size()) != h.fileCrc)
            return fail(Status::Corrupt, "file is damaged: the JPEG could not be rebuilt exactly");
        std::string name = safe_name(h.name);
        fs::path dir = opt.outDir.empty() ? src.parent_path() : opt.outDir; if (dir.empty()) dir = ".";
        { std::error_code e2; auto sp = fs::space(dir, e2); if (!e2 && sp.available < h.total) return fail(Status::DiskFull, "not enough free disk space for the restored file"); }
        fs::path finalPath = unique_path(dir, name); tmp = finalPath; tmp += ".tmp";
        File out; if (!out.open(tmp, "wb")) return fail(Status::IoError, "cannot create the output file (folder not writable?)");
        if (std::fwrite(file.data(), 1, file.size(), out.f) != file.size() || std::fflush(out.f) != 0) { out.close(); remove_quiet(tmp); return fail(Status::DiskFull, "write failed (disk full?)"); }
        out.close();
        std::error_code ec; fs::rename(tmp, finalPath, ec);
        if (ec) { remove_quiet(tmp); return fail(Status::IoError, "cannot finalize the output file: " + ec.message()); }
        if (progress) progress(1.0, h.total);
        Result r; r.inSize = fileSize; r.outSize = h.total; r.outPath = finalPath; r.storedName = h.name; return r;
    } catch (const std::exception& e) { if (!tmp.empty()) remove_quiet(tmp); return fail(Status::IoError, e.what()); }
}
inline Result restore_generated(const fs::path& src, u64 fileSize, const Header& h, const Options& opt, const ProgressFn& progress) {
    fs::path tmp;
    try {
        std::string name = safe_name(h.name);
        fs::path dir = opt.outDir.empty() ? src.parent_path() : opt.outDir; if (dir.empty()) dir = ".";
        { std::error_code e2; auto sp = fs::space(dir, e2); if (!e2 && sp.available < h.total) return fail(Status::DiskFull, "not enough free disk space for the restored file"); }
        fs::path finalPath = unique_path(dir, name); tmp = finalPath; tmp += ".tmp";
        File out; if (!out.open(tmp, "wb")) return fail(Status::IoError, "cannot create the output file (folder not writable?)");
        u32 w[624]; for (int i = 0; i < 624; i++) w[i] = rd32le(&h.genPrefix[(size_t)i * 4]);
        Mt19937Stream mt(w); u32 crc = 0; u64 words = (h.total - h.genTail.size()) / 4, done = 0;
        std::vector<u8> buf((size_t)4 << 20);
        auto emit = [&](const u8* p, size_t n) { if (std::fwrite(p, 1, n, out.f) != n) throw Failure{Status::DiskFull, "write failed (disk full?)"}; crc = crc32_update(crc, p, n); };
        emit(h.genPrefix.data(), h.genPrefix.size()); done = 624;
        while (done < words) {
            size_t cnt = (size_t)std::min<u64>(buf.size() / 4, words - done);
            for (size_t i = 0; i < cnt; i++) { u32 v = mt.next(); buf[i * 4] = (u8)v; buf[i * 4 + 1] = (u8)(v >> 8); buf[i * 4 + 2] = (u8)(v >> 16); buf[i * 4 + 3] = (u8)(v >> 24); }
            emit(buf.data(), cnt * 4); done += cnt;
            if (progress && !progress((double)done / (double)words, done * 4)) { out.close(); remove_quiet(tmp); return fail(Status::Cancelled, "cancelled"); }
        }
        if (!h.genTail.empty()) emit(h.genTail.data(), h.genTail.size());
        if (std::fflush(out.f) != 0) throw Failure{Status::DiskFull, "write failed (disk full?)"};
        out.close();
        if (crc != h.fileCrc) { remove_quiet(tmp); return fail(Status::Corrupt, "file is damaged: checksum does not match"); }
        std::error_code ec; fs::rename(tmp, finalPath, ec);
        if (ec) { remove_quiet(tmp); return fail(Status::IoError, "cannot finalize the output file: " + ec.message()); }
        Result r; r.inSize = fileSize; r.outSize = h.total; r.outPath = finalPath; r.storedName = h.name; return r;
    } catch (const Failure& f) { if (!tmp.empty()) remove_quiet(tmp); return fail(f.status, f.msg);
    } catch (const std::exception& e) { if (!tmp.empty()) remove_quiet(tmp); return fail(Status::IoError, e.what()); }
}
}  // namespace detail

// Restore a .3p file under its original name and extension (read from the metadata).
inline Result restore_file(const fs::path& src, const Options& opt, const ProgressFn& progress) {
    using namespace detail;
    fs::path tmp;
    try {
        std::error_code ec;
        u64 fileSize = (u64)fs::file_size(src, ec); if (ec) return fail(Status::IoError, "cannot read file size: " + ec.message());
        Header h;
        { File f; if (!f.open(src, "rb")) return fail(Status::IoError, "cannot open the file"); if (!read_header(f.f, h)) return fail(Status::BadFormat, "not a valid .3p file"); }
        if (h.gen) return restore_generated(src, fileSize, h, opt, progress);
        if (h.jpeg) return restore_jpeg(src, fileSize, h, opt, progress);
        const u64 streamLen = h.dedup ? h.litTotal : h.total;
        u64 nseg = (streamLen + kSegment - 1) / kSegment;
        if ((!h.dedup && h.total > fileSize * 4096 + kSegment) || nseg * 5 > fileSize) return fail(Status::BadFormat, "not a valid .3p file (damaged header)");

        struct Frame { u64 off = 0, plen = 0; u32 crc = 0; bool modeled = false; int code = 0; };
        std::vector<Frame> frames((size_t)nseg);
        { File f; if (!f.open(src, "rb") || seek64(f.f, h.bytes) != 0) return fail(Status::IoError, "cannot open the file");
          u64 pos = h.bytes;
          for (u64 i = 0; i < nseg; i++) {
              u64 v = 0, used = 0; u8 c4[4];
              if (!get_varint(f.f, v, used) || std::fread(c4, 1, 4, f.f) != 4) return fail(Status::Corrupt, "file is truncated or damaged");
              Frame& fr = frames[(size_t)i]; fr.plen = v >> 3; fr.modeled = (v & 1) != 0; fr.code = (int)((v >> 1) & 3);
              fr.crc = (u32)c4[0] | ((u32)c4[1] << 8) | ((u32)c4[2] << 16) | ((u32)c4[3] << 24);
              fr.off = pos + used + 4;
              u64 L = std::min<u64>(kSegment, streamLen - i * kSegment);
              if (fr.plen > L || (!fr.modeled && fr.plen != L) || fr.off + fr.plen > fileSize) return fail(Status::Corrupt, "file is truncated or damaged");
              pos = fr.off + fr.plen; if (seek64(f.f, pos) != 0) return fail(Status::Corrupt, "file is truncated or damaged");
          } }

        std::string name = safe_name(h.name);
        fs::path dir = opt.outDir.empty() ? src.parent_path() : opt.outDir; if (dir.empty()) dir = ".";
        { std::error_code e2; auto sp = fs::space(dir, e2); if (!e2 && sp.available < h.total) return fail(Status::DiskFull, "not enough free disk space for the restored file"); }
        fs::path finalPath = unique_path(dir, name);
        tmp = finalPath; tmp += ".tmp";
        File out; if (!out.open(tmp, "wb")) return fail(Status::IoError, "cannot create the output file (folder not writable?)");

        unsigned threads = pick_threads(opt, nseg, h.tbMax);
        Shared sh;
        OutWriter writer(out.f, tmp, h.dedup ? &h.matches : nullptr, sh.cn);
        auto worker = [&]() {
            File f; if (!f.open(src, "rb")) throw Failure{Status::IoError, "cannot open the file"};
            std::vector<u8> payload, seg;
            for (;;) {
                u64 idx = sh.nextSeg.fetch_add(1);
                if (idx >= nseg || sh.cn.cancel.load()) return;
                const Frame& fr = frames[(size_t)idx];
                size_t L = (size_t)std::min<u64>(kSegment, streamLen - idx * kSegment);
                payload.resize((size_t)fr.plen); seg.resize(L);
                if (seek64(f.f, fr.off) != 0 || std::fread(payload.data(), 1, payload.size(), f.f) != payload.size()) throw Failure{Status::Corrupt, "file is truncated or damaged"};
                Hint hint = make_hint(h, idx * kSegment, L, fr.modeled ? fr.code : 0);
                if (fr.modeled) {
                    decode_segment(payload.data(), payload.size(), seg.data(), L, h.tbMax, hint, &sh.cn);
                    { size_t gts = 0, gte = 0; if (grid_region(h.grid, idx * kSegment, L, gts, gte)) GridCodec(h.grid).inverse(seg.data() + gts, gte - gts); }
                    if (h.exec) exec_filter(seg.data(), L, idx * kSegment, h.exec, false);
                    if (hint.ate > hint.ats) AudioCodec(h.audio.channels, h.audio.width, fr.code - 1).inverse(seg.data() + hint.ats, (size_t)((hint.ate - hint.ats) / (u64)h.audio.frame()));
                }
                else { std::memcpy(seg.data(), payload.data(), L); sh.cn.done.fetch_add(L); sh.cn.out.fetch_add(L); }
                if (crc32(seg.data(), L) != fr.crc) throw Failure{Status::Corrupt, "file is damaged: checksum does not match"};
                sys::Lock l(sh.m);
                while (sh.nextWrite != idx && !sh.cn.cancel.load()) sh.cv.wait(sh.m);
                if (sh.cn.cancel.load()) return;
                writer.append(seg.data(), L);
                sh.nextWrite++; sh.cv.notify_all();
            }
        };
        run_workers(sh, threads, streamLen, worker, progress);
        if (sh.failed) { out.close(); remove_quiet(tmp); return fail(sh.st, sh.msg); }
        if (sh.userCancel || sh.nextWrite != nseg) { out.close(); remove_quiet(tmp); return fail(Status::Cancelled, "cancelled"); }
        try { writer.finish(); } catch (const Failure& f) { out.close(); remove_quiet(tmp); return fail(f.status, f.msg); }
        if (writer.pos() != h.total || (h.dedup && writer.crc() != h.fileCrc)) { out.close(); remove_quiet(tmp); return fail(Status::Corrupt, "file is damaged: checksum does not match"); }
        if (std::fflush(out.f) != 0) { out.close(); remove_quiet(tmp); return fail(Status::DiskFull, "write failed (disk full?)"); }
        out.close();
        fs::rename(tmp, finalPath, ec);
        if (ec) { remove_quiet(tmp); return fail(Status::IoError, "cannot finalize the output file: " + ec.message()); }
        Result r; r.inSize = fileSize; r.outSize = h.total; r.outPath = finalPath; r.storedName = h.name;
        if (progress) progress(1.0, h.total);
        return r;
    } catch (const std::bad_alloc&) { if (!tmp.empty()) remove_quiet(tmp); return fail(Status::OutOfMemory, "not enough memory");
    } catch (const std::exception& e) { if (!tmp.empty()) remove_quiet(tmp); return fail(Status::IoError, e.what()); }
}

// Reads only the metadata: stored name and original size (instant, any file size).
inline Result inspect_file(const fs::path& src) {
    using namespace detail;
    try {
        std::error_code ec; u64 fileSize = (u64)fs::file_size(src, ec); if (ec) return fail(Status::IoError, ec.message());
        File f; if (!f.open(src, "rb")) return fail(Status::IoError, "cannot open the file");
        Header h; if (!read_header(f.f, h)) return fail(Status::BadFormat, "not a valid .3p file");
        Result r; r.inSize = fileSize; r.outSize = h.total; r.storedName = h.name; return r;
    } catch (const std::exception& e) { return fail(Status::IoError, e.what()); }
}

}  // namespace p3
