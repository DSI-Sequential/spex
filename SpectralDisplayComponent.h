#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>

// ---------------------------------------------------------------------------
// SpectralDisplayComponent
// Top 2/3: waterfall (newest row at bottom, oldest at top, log-freq X).
// Bottom 1/3: instantaneous spectral envelope (log-freq X, dB Y).
// ---------------------------------------------------------------------------
class SpectralDisplayComponent final : public juce::Component
{
public:
    enum FeatureIndex
    {
        windowedPeakAmplitude = 0,
        slidingWindowRms,
        interpolatedSpectralPeak,
        papr,
        spectralFlatness,
        numFeatures
    };

    struct FeatureSnapshot
    {
        std::array<float, numFeatures> values {};
    };

    static constexpr int   fftOrder = 14;
    static constexpr int   fftSize  = 1 << fftOrder;
    static constexpr float minFreq  = 30.0f;
    static constexpr float floorDb  = -80.0f;

    SpectralDisplayComponent()
    {
        for (int i = 0; i < fftSize; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(fftSize);
            hannWin[i] = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * t);
        }
        mag.fill(floorDb);
    }

    void setSampleRate(float sr) { sampleRate = sr; }
    void setFeatureFrequencyRange(float minHz, float maxHz)
    {
        featureMinHz = std::max(0.0f, minHz);
        featureMaxHz = std::max(0.0f, maxHz);
    }
    FeatureSnapshot getLatestFeatureSnapshot() const { return latestFeatures; }

    // Audio thread: push samples into the ring buffer.
    void pushSamples(const float* data, int n)
    {
        int wp = wPos.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i)
        {
            ring[wp] = data[i];
            if (++wp >= fftSize) wp = 0;
        }
        wPos.store(wp, std::memory_order_release);
        hasNew.store(true, std::memory_order_relaxed);
    }

    // Message thread: compute FFT, push waterfall row, repaint.
    bool update()
    {
        if (!hasNew.exchange(false, std::memory_order_acquire)) return false;

        // Copy ring buffer (oldest → newest) with Hann window applied.
        const int rp = wPos.load(std::memory_order_acquire);
        float peakAbs = 0.0f;
        double sumSquares = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            const float raw = ring[(rp + i) % fftSize];
            peakAbs = std::max(peakAbs, std::abs(raw));
            sumSquares += static_cast<double>(raw) * static_cast<double>(raw);
            fftBuf[i] = raw * hannWin[i];
        }
        std::fill(fftBuf.begin() + fftSize, fftBuf.end(), 0.0f);

        // FFT → magnitudes in fftBuf[0..fftSize/2].
        fft.performFrequencyOnlyForwardTransform(fftBuf.data());

        computeFeatures(peakAbs, sumSquares);

        // Normalise and smooth.
        const float normDb = juce::Decibels::gainToDecibels(static_cast<float>(fftSize));
        const float alpha  = 0.65f;
        for (int i = 0; i <= fftSize / 2; ++i)
        {
            const float db = fftBuf[i] > 0.0f
                ? juce::Decibels::gainToDecibels(fftBuf[i]) - normDb
                : floorDb;
            mag[i] = alpha * mag[i] + (1.0f - alpha) * std::max(db, floorDb);
        }

        pushWaterfallRow();
        repaint();
        return true;
    }

    void resized() override
    {
        wfImg = juce::Image(juce::Image::RGB,
                            std::max(1, getWidth()),
                            std::max(1, waterfallH()), true);
        wfRow = 0;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff0c0c0c));
        const auto b = getLocalBounds();
        paintWaterfall(g, b.withHeight(waterfallH()));
        paintEnvelope (g, b.withTrimmedTop(waterfallH()));
    }

private:
    int waterfallH() const { return getHeight() * 2 / 3; }

    // Log-frequency <-> pixel mapping.
    float hzToX(float hz, int W) const
    {
        const float maxHz = sampleRate * 0.5f;
        const float t = std::log(hz / minFreq) / std::log(maxHz / minFreq);
        return std::clamp(t, 0.0f, 1.0f) * static_cast<float>(W);
    }

    float xToHz(int x, int W) const
    {
        const float maxHz = sampleRate * 0.5f;
        const float t = static_cast<float>(x) / static_cast<float>(W);
        return minFreq * std::pow(maxHz / minFreq, t);
    }

    // Hz -> smoothed dB magnitude.
    float hzToDb(float hz) const
    {
        const int bin = std::clamp(
            static_cast<int>(hz * static_cast<float>(fftSize) / sampleRate),
            0, fftSize / 2);
        return mag[bin];
    }

    // dB -> waterfall colour.
    static juce::Colour dbToColour(float db)
    {
        const float t = std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
        if (t < 0.25f) return juce::Colour::fromFloatRGBA(0.0f,           0.0f,                   t * 4.0f,           1.0f);
        if (t < 0.5f)  return juce::Colour::fromFloatRGBA(0.0f,           (t - 0.25f) * 4.0f,     1.0f,               1.0f);
        if (t < 0.75f) return juce::Colour::fromFloatRGBA((t - 0.5f)*4.f, 1.0f, 1.0f-(t-0.5f)*4.f, 1.0f);
        return                 juce::Colour::fromFloatRGBA(1.0f,           1.0f,                   (t-0.75f)*4.0f,     1.0f);
    }

    void pushWaterfallRow()
    {
        if (!wfImg.isValid() || sampleRate <= 0.0f) return;
        const int W = wfImg.getWidth();
        const int H = wfImg.getHeight();
        juce::Image::BitmapData bmp(wfImg, juce::Image::BitmapData::writeOnly);
        for (int x = 0; x < W; ++x)
            bmp.setPixelColour(x, wfRow, dbToColour(hzToDb(xToHz(x, W))));
        wfRow = (wfRow + 1) % H;
    }

    void paintWaterfall(juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        if (!wfImg.isValid()) return;
        const int W  = bounds.getWidth();
        const int H  = bounds.getHeight();
        const int bx = bounds.getX();
        const int by = bounds.getY();
        const int IW = wfImg.getWidth();
        const int IH = wfImg.getHeight();

        const int partAH = IH - wfRow;
        if (partAH > 0)
            g.drawImage(wfImg, bx, by,           W, partAH, 0, wfRow, IW, partAH);
        if (wfRow > 0)
            g.drawImage(wfImg, bx, by + partAH,  W, wfRow,  0, 0,     IW, wfRow);

        // Frequency grid overlay.
        if (sampleRate > 0.0f)
        {
            const float maxHz = sampleRate * 0.5f;
            g.setColour(juce::Colour(0x33ffffff));
            for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f,
                              1000.0f, 2000.0f, 5000.0f, 10000.0f,
                              20000.0f, 40000.0f, 80000.0f })
            {
                if (hz >= maxHz) break;
                const int px = bx + static_cast<int>(hzToX(hz, W));
                g.drawVerticalLine(px, static_cast<float>(by), static_cast<float>(by + H));
            }
        }
    }

    void paintEnvelope(juce::Graphics& g, juce::Rectangle<int> bounds)
    {
        if (sampleRate <= 0.0f || bounds.isEmpty()) return;
        const int W  = bounds.getWidth();
        const int H  = bounds.getHeight();
        const int bx = bounds.getX();
        const int by = bounds.getY();

        const float labelH  = 14.0f;
        const float usableH = static_cast<float>(H) - labelH;

        auto dbToY = [&](float db) -> float
        {
            const float t = std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
            return static_cast<float>(by) + usableH * (1.0f - t);
        };

        // dB grid.
        g.setColour(juce::Colour(0x22ffffff));
        for (float db : { -60.0f, -40.0f, -20.0f, -10.0f, -3.0f })
            g.drawHorizontalLine(static_cast<int>(dbToY(db)),
                                 static_cast<float>(bx), static_cast<float>(bx + W));

        // Frequency grid + labels.
        const float maxHz = sampleRate * 0.5f;
        for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f,
                          1000.0f, 2000.0f, 5000.0f, 10000.0f,
                          20000.0f, 40000.0f, 80000.0f })
        {
            if (hz >= maxHz) break;
            const float px = static_cast<float>(bx) + hzToX(hz, W);
            g.setColour(juce::Colour(0x22ffffff));
            g.drawVerticalLine(static_cast<int>(px),
                               static_cast<float>(by), static_cast<float>(by) + usableH);
            const juce::String lbl = hz >= 1000.0f
                ? juce::String(static_cast<int>(hz / 1000)) + "k"
                : juce::String(static_cast<int>(hz));
            g.setColour(juce::Colour(0x88ffffff));
            g.setFont(9.0f);
            g.drawText(lbl, static_cast<int>(px) - 12,
                       by + H - static_cast<int>(labelH), 24, static_cast<int>(labelH),
                       juce::Justification::centred, false);
        }

        // Filled envelope path.
        juce::Path fill, line;
        bool started = false;
        for (int x = 0; x < W; ++x)
        {
            const float px = static_cast<float>(bx + x);
            const float py = dbToY(hzToDb(xToHz(x, W)));
            if (!started)
            {
                fill.startNewSubPath(px, static_cast<float>(by) + usableH);
                fill.lineTo(px, py);
                line.startNewSubPath(px, py);
                started = true;
            }
            else
            {
                fill.lineTo(px, py);
                line.lineTo(px, py);
            }
        }
        if (started)
        {
            fill.lineTo(static_cast<float>(bx + W), static_cast<float>(by) + usableH);
            fill.closeSubPath();
            g.setColour(juce::Colour(0x4400aaff));
            g.fillPath(fill);
            g.setColour(juce::Colour(0xff00aaff));
            g.strokePath(line, juce::PathStrokeType(1.5f));
        }
    }

    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize>         ring    {};
    std::array<float, fftSize * 2>     fftBuf  {};
    std::array<float, fftSize>         hannWin {};
    std::array<float, fftSize / 2 + 1> mag     {};

    std::atomic<int>  wPos   { 0 };
    std::atomic<bool> hasNew { false };

    float sampleRate { 44100.0f };
    FeatureSnapshot latestFeatures {};
    float featureMinHz { 0.0f };
    float featureMaxHz { 0.0f };

    juce::Image wfImg;
    int wfRow { 0 };

    void computeFeatures(float peakAbs, double sumSquares)
    {
        constexpr float epsilon = 1.0e-12f;
        const float meanPower = static_cast<float>(sumSquares / static_cast<double>(fftSize));
        const float rms = std::sqrt(std::max(meanPower, epsilon));
        const float safePeak = std::max(peakAbs, epsilon);

        latestFeatures.values[windowedPeakAmplitude] =
            juce::Decibels::gainToDecibels(safePeak, floorDb);
        latestFeatures.values[slidingWindowRms] =
            juce::Decibels::gainToDecibels(rms, floorDb);

        const int maxBin = fftSize / 2;
        const float nyquist = sampleRate * 0.5f;
        const float minHz = std::clamp(featureMinHz, 0.0f, nyquist);
        const float maxHz = (featureMaxHz <= 0.0f)
            ? nyquist
            : std::clamp(featureMaxHz, minHz, nyquist);

        int analysisStartBin = static_cast<int>(std::ceil(minHz * static_cast<float>(fftSize) / sampleRate));
        int analysisEndBin = static_cast<int>(std::floor(maxHz * static_cast<float>(fftSize) / sampleRate));
        analysisStartBin = std::clamp(analysisStartBin, 0, maxBin);
        analysisEndBin = std::clamp(analysisEndBin, 0, maxBin);
        if (analysisEndBin < analysisStartBin)
            std::swap(analysisStartBin, analysisEndBin);

        int searchStartBin = std::max(1, analysisStartBin);
        int searchEndBin = std::min(maxBin - 1, analysisEndBin);
        if (searchEndBin <= searchStartBin)
        {
            searchStartBin = 1;
            searchEndBin = maxBin - 1;
        }

        int peakBin = searchStartBin;
        for (int i = searchStartBin + 1; i <= searchEndBin; ++i)
        {
            if (fftBuf[i] > fftBuf[peakBin])
                peakBin = i;
        }

        const float l = std::log(std::max(fftBuf[peakBin - 1], epsilon));
        const float c = std::log(std::max(fftBuf[peakBin], epsilon));
        const float r = std::log(std::max(fftBuf[peakBin + 1], epsilon));
        const float denom = (l - 2.0f * c + r);
        const float delta = std::abs(denom) > epsilon ? 0.5f * (l - r) / denom : 0.0f;
        const float interpLogMag = c - 0.25f * (l - r) * delta;
        const float interpMag = std::exp(interpLogMag);
        latestFeatures.values[interpolatedSpectralPeak] =
            juce::Decibels::gainToDecibels(interpMag, floorDb);

        double logPowerSum = 0.0;
        double powerSum = 0.0;
        double maxPower = 0.0;
        int nonZeroCount = 0;
        int count = 0;
        for (int i = analysisStartBin; i <= analysisEndBin; ++i)
        {
            const double p = static_cast<double>(fftBuf[i]) * static_cast<double>(fftBuf[i]);
            if (p > 0.0)
            {
                logPowerSum += std::log(p);
                ++nonZeroCount;
            }
            powerSum += p;
            maxPower = std::max(maxPower, p);
            ++count;
        }

        if (count <= 0)
        {
            latestFeatures.values[papr] = 0.0f;
            latestFeatures.values[spectralFlatness] = 0.8f;
            return;
        }

        // Spectral PAPR is peak spectral power divided by average spectral power.
        const double avgPower = powerSum / static_cast<double>(count);
        const float spectralPapr = static_cast<float>(maxPower / std::max(avgPower, 1.0e-20));
        latestFeatures.values[papr] = 10.0f * std::log10(std::max(spectralPapr, epsilon));

        // Spectral flatness = GM(power spectrum) / AM(power spectrum), in log domain for stability.
        if (avgPower <= 1.0e-20 || nonZeroCount == 0)
        {
            latestFeatures.values[spectralFlatness] = 0.8f;
        }
        else
        {
            const double gm = std::exp(logPowerSum / static_cast<double>(count));
            const float flatness = static_cast<float>(gm / avgPower);
            latestFeatures.values[spectralFlatness] = std::clamp(flatness, 0.0f, 1.0f);
        }
    }
};
