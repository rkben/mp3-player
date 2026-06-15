#pragma once

#include <array>
#include <cmath>
#include <complex>

// Self-contained audio spectrum analyzer for the visualizer. Keeps a sliding
// window of the most recent samples, and every kHop samples runs an in-place
// radix-2 FFT over that window and folds the magnitude spectrum into log-spaced
// bands. Output is the *instantaneous* band magnitudes; temporal smoothing (snap
// up, ease down) is done per painted frame in ShaderArt. No Qt dependency.
//
// kHop fixes the emit cadence in *samples*, so it's independent of the decoder's
// block size: MP3 hands over small ~1152-sample blocks while FLAC hands over
// large ~4096-sample ones, but both emit a new spectrum at the same rate here —
// otherwise MP3 would update the bars far more often and look jumpier than FLAC.
//
// The FFT is a textbook iterative Cooley–Tukey (written from scratch); the band
// gain/normalisation constants are empirical and tuned by eye against music.
class SpectrumAnalyzer
{
public:
    static constexpr int kFftSize = 1024;   // power of two; ~21ms @48kHz per FFT
    static constexpr int kHop = 4096;       // emit every ~93ms @44.1k, codec-agnostic
    static constexpr int kNumBands = 64;
    using Bands = std::array<float, kNumBands>;

    // Append n mono samples into the sliding window. Returns true each time kHop
    // samples have accumulated and a fresh band frame was produced (bands() then
    // holds the latest result). sampleRate maps FFT bins to frequencies.
    bool push(const float *mono, int n, int sampleRate)
    {
        bool produced = false;
        for (int i = 0; i < n; ++i) {
            m_ring[m_pos] = mono[i];                 // newest sample overwrites oldest
            m_pos = (m_pos + 1) % kFftSize;
            if (m_filled < kFftSize)
                ++m_filled;
            if (++m_sinceEmit >= kHop && m_filled == kFftSize) {
                analyze(sampleRate > 0 ? sampleRate : 48000);
                m_sinceEmit = 0;
                produced = true;
            }
        }
        return produced;
    }

    const Bands &bands() const { return m_bands; }

    // Drop accumulated samples and decay bands to rest (visualizer turned off).
    void reset()
    {
        m_pos = 0;
        m_filled = 0;
        m_sinceEmit = 0;
        m_ring.fill(0.0f);
        m_bands.fill(0.0f);
    }

private:
    static constexpr float kPi = 3.14159265358979323846f;

    void analyze(int sampleRate)
    {
        // Hann window into the complex FFT input (real signal, zero imaginary),
        // reading the ring oldest-to-newest starting at the write cursor.
        std::array<std::complex<float>, kFftSize> buf;
        for (int i = 0; i < kFftSize; ++i) {
            const float w =
                0.5f * (1.0f - std::cos(2.0f * kPi * float(i) / float(kFftSize - 1)));
            const float s = m_ring[(m_pos + i) % kFftSize];
            buf[i] = std::complex<float>(s * w, 0.0f);
        }
        fft(buf.data(), kFftSize);

        const int half = kFftSize / 2;          // usable bins (0..half), real signal
        const float nyquist = float(sampleRate) * 0.5f;
        const float fMin = 40.0f;               // ignore sub-bass rumble / DC
        const float fMax = nyquist;

        for (int b = 0; b < kNumBands; ++b) {
            // Log-spaced edges: each band spans an equal ratio of the audible range.
            const float f0 = fMin * std::pow(fMax / fMin, float(b) / float(kNumBands));
            const float f1 = fMin * std::pow(fMax / fMin, float(b + 1) / float(kNumBands));
            int bin0 = int(f0 / nyquist * float(half));
            int bin1 = int(f1 / nyquist * float(half));
            bin0 = std::max(1, bin0);            // skip DC
            bin1 = std::min(half, std::max(bin0 + 1, bin1));

            // Average magnitude across the band, normalised so a full-scale tone
            // lands near 1.0 before perceptual compression (2/N undoes FFT gain).
            float sum = 0.0f;
            for (int k = bin0; k < bin1; ++k)
                sum += std::abs(buf[k]);
            const float mag = (2.0f / float(kFftSize)) * sum / float(bin1 - bin0);

            // Perceptual compression: log curve so quiet detail is visible without
            // loud bands pinning. kGain/kNorm are eyeball-tuned, clamped to [0,1].
            // Instantaneous magnitudes; temporal smoothing is done per painted
            // frame in ShaderArt.
            constexpr float kGain = 90.0f;
            constexpr float kNorm = 0.46f;
            float v = std::log1p(mag * kGain) * kNorm;
            m_bands[b] = std::min(1.0f, std::max(0.0f, v));
        }
    }

    // In-place iterative radix-2 Cooley–Tukey FFT (n must be a power of two).
    static void fft(std::complex<float> *a, int n)
    {
        // Bit-reversal permutation.
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap(a[i], a[j]);
        }
        // Butterflies, doubling the transform length each pass.
        for (int len = 2; len <= n; len <<= 1) {
            const float ang = -2.0f * kPi / float(len);
            const std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int k = 0; k < len / 2; ++k) {
                    const std::complex<float> u = a[i + k];
                    const std::complex<float> v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
    }

    std::array<float, kFftSize> m_ring{};   // sliding window of recent samples
    int m_pos = 0;                          // ring write cursor (also oldest sample)
    int m_filled = 0;                       // samples seen, capped at kFftSize
    int m_sinceEmit = 0;                    // samples since the last produced frame
    Bands m_bands{};
};
