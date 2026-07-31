#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_dsp/juce_dsp.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>
#include <vector>

#include "SpectralFeatureAnalyzer.h"

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
        windowedPeakAmplitude = spex::windowedPeakAmplitude,
        slidingWindowRms = spex::slidingWindowRms,
        interpolatedSpectralPeak = spex::interpolatedSpectralPeak,
        papr = spex::papr,
        localSpectralCrest = spex::localSpectralCrest,
        spectralFlatness = spex::spectralFlatness,
        numFeatures = spex::numSpectralFeatures
    };

    using FeatureSnapshot = spex::SpectralFeatureSnapshot;

    // Steady-state analysis: frequency resolution matters, latency does not,
    // so use a very large window (64k) for fine spectral detail.
    static constexpr int   fftOrder = 16;
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
        setMouseCursor(juce::MouseCursor::CrosshairCursor);
    }

    void setSampleRate(float sr) { sampleRate = sr; }
    void setScrollingPaused(bool paused) { scrollingPaused = paused; }
    void setShowWaterfall(bool shouldShow) { showWaterfall = shouldShow; repaint(); }
    void setShowEnvelope(bool shouldShow) { showEnvelope = shouldShow; repaint(); }
    void setFeatureFrequencyRange(float minHz, float maxHz)
    {
        featureAnalyzer.setFeatureFrequencyRange(minHz, maxHz);
    }
    void setFlatnessPowerFloorDb(float floorDb)
    {
        featureAnalyzer.setFlatnessPowerFloorDb(floorDb);
    }
    FeatureSnapshot getLatestFeatureSnapshot() const { return featureAnalyzer.getLatestSnapshot(); }

    // -----------------------------------------------------------------------
    // Roll-off slope analysis (dB/octave over a selectable region) plus a
    // frozen/imported reference envelope, used to dial one response in to
    // match another. Both a linear fit and a cubic-polynomial fit (matching
    // the sem4-resonance-analysis workflow) are provided.
    // -----------------------------------------------------------------------
    struct SlopeFit
    {
        bool  valid { false };
        float slopeDbPerOct { 0.0f };
        float interceptDb   { 0.0f }; // predicted dB at the 1 kHz reference
        float rSquared      { 0.0f };
        float minHz { 0.0f };
        float maxHz { 0.0f };
        int   pointCount { 0 };
    };

    struct PolyFit
    {
        bool  valid { false };
        std::array<double, 4> coeffs {}; // c0 + c1*x + c2*x^2 + c3*x^3, x = log2(f/1kHz)
        float rSquared { 0.0f };
        float minHz { 0.0f };
        float maxHz { 0.0f };
    };

    void setSlopeRegion(float minHz, float maxHz)
    {
        slopeRegionMinHz = std::max(0.0f, std::min(minHz, maxHz));
        slopeRegionMaxHz = std::max(0.0f, std::max(minHz, maxHz));
        recomputeReferenceFits();
        recomputeLiveFits();
    }

    void setShowPolynomialFit(bool shouldShow) { showPoly = shouldShow; repaint(); }
    void setShowRegression(bool shouldShow) { showRegression = shouldShow; repaint(); }
    bool isShowingRegression() const { return showRegression; }

    // Overlay precise peak markers (parabolic-interpolated in x/y) on the
    // envelope so the reader isn't misled by line/pixel interpolation when
    // examining peak amplitude progression.
    void setShowPeakMarkers(bool shouldShow) { showPeakMarkers = shouldShow; repaint(); }

    // Fit the roll-off regression only to the spectral peaks (harmonic tops)
    // rather than the whole magnitude envelope. This is what we want when the
    // source is a harmonic waveform (e.g. a filtered sawtooth) and we only
    // care about the trend of the harmonic amplitudes.
    void setFitPeaksOnly(bool shouldFitPeaks)
    {
        fitPeaksOnly = shouldFitPeaks;
        recomputeReferenceFits();
        recomputeLiveFits();
        repaint();
    }

    // Minimum height (dB) a spectral peak must reach to be tracked by the
    // peak-only regression. Prevents the fit from locking onto tiny peaks
    // (e.g. noise or sub-fundamental ripple below a saw's fundamental).
    void setPeakThresholdDb(float thresholdDb)
    {
        peakThresholdDb = thresholdDb;
        recomputeReferenceFits();
        recomputeLiveFits();
        repaint();
    }

    // Warp the log-frequency axis. gamma == 1 is a plain log scale; gamma > 1
    // progressively expands the upper octaves (a "hyper-logarithmic" scale).
    void setFreqWarp(float gamma)
    {
        freqWarpGamma = std::max(1.0f, gamma);
        repaint();
    }

    // Cumulative-average magnitude envelope. When enabled, the envelope (and
    // therefore the regression fits) represents the running power average of
    // every frame accumulated since the last clear, rather than the default
    // exponential moving average.
    void setAveragingEnabled(bool enabled)
    {
        averagingEnabled = enabled;
        if (enabled)
            clearAveraging();
        repaint();
    }
    bool isAveragingEnabled() const { return averagingEnabled; }

    void clearAveraging()
    {
        avgPower.fill(0.0);
        avgFrameCount = 0;
    }

    // Manual numeric target: an ideal straight roll-off line at a chosen slope.
    void setManualTarget(bool enabled, float slopeDbPerOct)
    {
        manualTargetEnabled = enabled;
        manualTargetSlope = slopeDbPerOct;
        repaint();
    }
    bool  isManualTargetEnabled() const { return manualTargetEnabled; }
    float getManualTargetSlope() const  { return manualTargetSlope; }

    SlopeFit getLiveSlopeFit() const      { return liveFit; }
    PolyFit  getLivePolyFit() const       { return livePoly; }
    SlopeFit getReferenceSlopeFit() const { return refFit; }
    PolyFit  getReferencePolyFit() const  { return refPoly; }
    bool     hasReference() const         { return hasRef; }
    juce::String getReferenceName() const { return referenceName; }

    // Local (tangent) slope of a cubic fit at a given frequency, in dB/octave.
    // d/dx (c0 + c1 x + c2 x^2 + c3 x^3) = c1 + 2 c2 x + 3 c3 x^2, with x in octaves.
    static float polyLocalSlopeDbPerOct(const PolyFit& p, float hz)
    {
        if (!p.valid || hz <= 0.0f)
            return 0.0f;
        const double x = std::log2(hz / 1000.0f);
        return static_cast<float>(p.coeffs[1] + 2.0 * p.coeffs[2] * x + 3.0 * p.coeffs[3] * x * x);
    }

    // Freeze the current live envelope as the match target.
    void captureReference()
    {
        refMagsDb.assign(mag.begin(), mag.end());
        refBinHz = (sampleRate > 0.0f) ? sampleRate / static_cast<float>(fftSize) : 0.0f;
        hasRef = (refBinHz > 0.0f);
        referenceName = "captured (live)";
        recomputeReferenceFits();
        repaint();
    }

    void clearReference()
    {
        hasRef = false;
        refMagsDb.clear();
        refBinHz = 0.0f;
        refFit = SlopeFit {};
        refPoly = PolyFit {};
        referenceName = {};
        repaint();
    }

    // Import an audio file, compute a stable (Welch-averaged) magnitude
    // envelope, and use it as the reference target. Returns false on failure.
    bool analyzeReferenceFile(const juce::File& file)
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
            return false;

        const int    fileSampleRate = static_cast<int>(reader->sampleRate);
        const int    numChannels    = static_cast<int>(reader->numChannels);
        const juce::int64 maxSamples = std::min<juce::int64>(reader->lengthInSamples,
                                                             static_cast<juce::int64>(fileSampleRate) * 20);
        const int    length = static_cast<int>(maxSamples);
        if (length < fftSize)
            return false;

        juce::AudioBuffer<float> buffer(std::max(1, numChannels), length);
        if (!reader->read(&buffer, 0, length, 0, true, true))
            return false;

        // Mono-sum.
        std::vector<float> mono(static_cast<size_t>(length), 0.0f);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = buffer.getReadPointer(ch);
            for (int i = 0; i < length; ++i)
                mono[static_cast<size_t>(i)] += src[i];
        }
        if (numChannels > 1)
        {
            const float scale = 1.0f / static_cast<float>(numChannels);
            for (auto& s : mono) s *= scale;
        }

        // Welch-averaged power spectrum.
        const int hop = fftSize / 2;
        std::vector<double> powerAccum(static_cast<size_t>(fftSize / 2 + 1), 0.0);
        std::vector<float>  scratch(static_cast<size_t>(fftSize) * 2, 0.0f);
        int frames = 0;
        for (int start = 0; start + fftSize <= length; start += hop)
        {
            for (int i = 0; i < fftSize; ++i)
                scratch[static_cast<size_t>(i)] = mono[static_cast<size_t>(start + i)] * hannWin[static_cast<size_t>(i)];
            std::fill(scratch.begin() + fftSize, scratch.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform(scratch.data());
            for (int i = 0; i <= fftSize / 2; ++i)
            {
                const double m = scratch[static_cast<size_t>(i)];
                powerAccum[static_cast<size_t>(i)] += m * m;
            }
            ++frames;
        }
        if (frames == 0)
            return false;

        const float normDb = juce::Decibels::gainToDecibels(static_cast<float>(fftSize));
        refMagsDb.assign(static_cast<size_t>(fftSize / 2 + 1), floorDb);
        for (int i = 0; i <= fftSize / 2; ++i)
        {
            const float rmsMag = std::sqrt(static_cast<float>(powerAccum[static_cast<size_t>(i)] / frames));
            const float db = rmsMag > 0.0f
                ? juce::Decibels::gainToDecibels(rmsMag) - normDb
                : floorDb;
            refMagsDb[static_cast<size_t>(i)] = std::max(db, floorDb);
        }

        refBinHz = static_cast<float>(fileSampleRate) / static_cast<float>(fftSize);
        hasRef = true;
        referenceName = file.getFileName();
        recomputeReferenceFits();
        repaint();
        return true;
    }

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
        // A paused display freezes the spectrogram, the magnitude envelope
        // (including cumulative averaging) and the regression fits alike.
        if (scrollingPaused) return false;
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

        featureAnalyzer.analyze(fftBuf.data(), fftSize / 2 + 1, sampleRate, fftSize, peakAbs, sumSquares);

        // Update the magnitude envelope: either a cumulative power average
        // since the last clear, or an exponential moving average (default).
        const float normDb = juce::Decibels::gainToDecibels(static_cast<float>(fftSize));
        if (averagingEnabled)
        {
            ++avgFrameCount;
            const double invFrames = 1.0 / static_cast<double>(avgFrameCount);
            for (int i = 0; i <= fftSize / 2; ++i)
            {
                const double power = static_cast<double>(fftBuf[i]) * static_cast<double>(fftBuf[i]);
                avgPower[static_cast<size_t>(i)] += power;
                const double meanMag = std::sqrt(avgPower[static_cast<size_t>(i)] * invFrames);
                const float db = meanMag > 0.0
                    ? juce::Decibels::gainToDecibels(static_cast<float>(meanMag)) - normDb
                    : floorDb;
                mag[i] = std::max(db, floorDb);
            }
        }
        else
        {
            const float alpha = 0.65f;
            for (int i = 0; i <= fftSize / 2; ++i)
            {
                const float db = fftBuf[i] > 0.0f
                    ? juce::Decibels::gainToDecibels(fftBuf[i]) - normDb
                    : floorDb;
                mag[i] = alpha * mag[i] + (1.0f - alpha) * std::max(db, floorDb);
            }
        }

        recomputeLiveFits();

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
        if (showWaterfall && showEnvelope)
        {
            paintWaterfall(g, b.withHeight(waterfallH()));
            paintEnvelope (g, b.withTrimmedTop(waterfallH()));
        }
        else if (showWaterfall)
        {
            paintWaterfall(g, b);
        }
        else if (showEnvelope)
        {
            paintEnvelope(g, b);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        cursorX = e.x;
        cursorY = e.y;
        cursorVisible = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent& /*e*/) override
    {
        cursorVisible = false;
        repaint();
    }

private:
    int waterfallH() const { return getHeight() * 2 / 3; }

    // Log-frequency <-> pixel mapping, with an optional axis warp that expands
    // the upper octaves (freqWarpGamma > 1).
    float hzToX(float hz, int W) const
    {
        const float maxHz = sampleRate * 0.5f;
        float t = std::log(hz / minFreq) / std::log(maxHz / minFreq);
        t = std::pow(std::clamp(t, 0.0f, 1.0f), freqWarpGamma);
        return t * static_cast<float>(W);
    }

    float xToHz(int x, int W) const
    {
        const float maxHz = sampleRate * 0.5f;
        float t = static_cast<float>(x) / static_cast<float>(W);
        t = std::pow(std::clamp(t, 0.0f, 1.0f), 1.0f / freqWarpGamma);
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

    // Detect harmonic peak bins within [startBin, endBin]. A coarse
    // local-maximum pass estimates the harmonic spacing from the strongest
    // candidates, then a spacing-aware local-maximum pass keeps only the
    // harmonic tops (not the noise between them). Returns false if the region
    // is too small or shows no clear harmonic structure.
    template <typename DbAt>
    bool collectHarmonicPeaks(DbAt dbAt, int startBin, int endBin, std::vector<int>& out) const
    {
        out.clear();
        if (endBin - startBin < 8)
            return false;

        // Coarse strict local maxima over +/-2 bins, above the height threshold.
        std::vector<std::pair<float, int>> candidates;
        for (int b = startBin + 2; b <= endBin - 2; ++b)
        {
            const float v = dbAt(b);
            if (v >= peakThresholdDb
                && v > dbAt(b - 1) && v >= dbAt(b + 1) && v > dbAt(b - 2) && v >= dbAt(b + 2))
                candidates.emplace_back(v, b);
        }
        if (candidates.size() < 4)
            return false;

        // Estimate harmonic spacing (in bins) from the strongest candidates,
        // which are almost certainly true harmonics rather than noise ripple.
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        const int strongCount = std::min<int>(12, static_cast<int>(candidates.size()));
        std::vector<int> strongBins;
        strongBins.reserve(static_cast<size_t>(strongCount));
        for (int i = 0; i < strongCount; ++i)
            strongBins.push_back(candidates[static_cast<size_t>(i)].second);
        std::sort(strongBins.begin(), strongBins.end());

        std::vector<int> gaps;
        gaps.reserve(strongBins.size());
        for (size_t i = 1; i < strongBins.size(); ++i)
            gaps.push_back(strongBins[i] - strongBins[i - 1]);
        if (gaps.empty())
            return false;
        std::sort(gaps.begin(), gaps.end());
        const int medianGap = gaps[gaps.size() / 2];
        const int radius = std::max(2, static_cast<int>(std::lround(medianGap * 0.4)));

        // Spacing-aware local-maximum pass: keep a bin only if it is the
        // maximum within +/-radius bins and clears the height threshold.
        for (int b = startBin; b <= endBin; ++b)
        {
            const float v = dbAt(b);
            if (v < peakThresholdDb)
                continue;
            bool isMax = true;
            for (int k = 1; k <= radius && isMax; ++k)
            {
                const int lo = std::max(startBin, b - k);
                const int hi = std::min(endBin, b + k);
                if (dbAt(lo) > v || dbAt(hi) > v)
                    isMax = false;
            }
            if (isMax)
                out.push_back(b);
        }
        return out.size() >= 4;
    }

    // Refine harmonic peak locations to sub-bin precision (x = frequency,
    // y = dB) via parabolic interpolation across each peak's three bins, over
    // the whole displayed spectrum. Fills out with (hz, dB) pairs.
    void collectRefinedPeaks(std::vector<std::pair<float, float>>& out) const
    {
        out.clear();
        const float binHz = (sampleRate > 0.0f) ? sampleRate / static_cast<float>(fftSize) : 0.0f;
        if (binHz <= 0.0f)
            return;

        const int startBin = std::max(1, static_cast<int>(std::ceil(minFreq / binHz)));
        const int endBin = fftSize / 2;
        if (endBin <= startBin)
            return;

        std::vector<int> peakBins;
        if (!collectHarmonicPeaks([this](int i) { return mag[static_cast<size_t>(i)]; },
                                  startBin, endBin, peakBins))
            return;

        out.reserve(peakBins.size());
        for (int b : peakBins)
        {
            float hz;
            float db;
            if (b > 0 && b < fftSize / 2)
            {
                const float ym1 = mag[static_cast<size_t>(b - 1)];
                const float y0  = mag[static_cast<size_t>(b)];
                const float yp1 = mag[static_cast<size_t>(b + 1)];
                const float denom = ym1 - 2.0f * y0 + yp1;
                float delta = std::abs(denom) > 1.0e-9f ? 0.5f * (ym1 - yp1) / denom : 0.0f;
                delta = std::clamp(delta, -0.5f, 0.5f);
                hz = (static_cast<float>(b) + delta) * binHz;
                db = y0 - 0.25f * (ym1 - yp1) * delta;
            }
            else
            {
                hz = static_cast<float>(b) * binHz;
                db = mag[static_cast<size_t>(b)];
            }
            out.emplace_back(hz, db);
        }
    }

    // Least-squares fit of dB against log2(freq) over the selected region for
    // an arbitrary bin source. Produces both a linear (dB/octave) fit and a
    // cubic-polynomial fit in a single accumulation pass. binHz is the
    // frequency spacing of the source; dbAt(i) returns the dB at bin i.
    template <typename DbAt>
    void computeFits(float binHz, int count, DbAt dbAt, SlopeFit& lin, PolyFit& poly) const
    {
        lin = SlopeFit {};
        poly = PolyFit {};
        lin.minHz = poly.minHz = slopeRegionMinHz;
        lin.maxHz = poly.maxHz = slopeRegionMaxHz;

        if (binHz <= 0.0f || count <= 0)
            return;

        const float upperFreq = static_cast<float>(count - 1) * binHz;
        const float lo = std::clamp(std::min(slopeRegionMinHz, slopeRegionMaxHz), minFreq, upperFreq);
        const float hi = std::clamp(std::max(slopeRegionMinHz, slopeRegionMaxHz), minFreq, upperFreq);
        if (hi <= lo)
            return;

        const int startBin = std::max(1, static_cast<int>(std::ceil (lo / binHz)));
        const int endBin   = std::min(count - 1, static_cast<int>(std::floor(hi / binHz)));
        if (endBin <= startBin)
            return;

        // Choose the bins to fit: either every bin (full magnitude envelope)
        // or only the spectral peaks (harmonic tops). The latter is what makes
        // the roll-off estimate meaningful for a harmonic source such as a
        // filtered sawtooth, where the trend of the harmonic amplitudes - not
        // the noise floor between them - is what we care about.
        std::vector<int> peakBins;
        const bool usePeaks = fitPeaksOnly
                            && collectHarmonicPeaks(dbAt, startBin, endBin, peakBins)
                            && peakBins.size() >= 4;

        // Moments: M[k] = sum x^k (k=0..6); Y[j] = sum y*x^j (j=0..3); Syy = sum y^2.
        std::array<double, 7> M {};
        std::array<double, 4> Y {};
        double Syy = 0.0;
        double n = 0.0;

        auto accumulate = [&](int b)
        {
            const float freq = static_cast<float>(b) * binHz;
            if (freq <= 0.0f)
                return;
            const double x = std::log2(freq / 1000.0f);
            const double y = static_cast<double>(dbAt(b));
            double xp = 1.0;
            for (int k = 0; k < 7; ++k) { M[static_cast<size_t>(k)] += xp; xp *= x; }
            double xj = 1.0;
            for (int j = 0; j < 4; ++j) { Y[static_cast<size_t>(j)] += y * xj; xj *= x; }
            Syy += y * y;
            n += 1.0;
        };

        if (usePeaks)
            for (int b : peakBins) accumulate(b);
        else
            for (int b = startBin; b <= endBin; ++b) accumulate(b);

        if (n < 4.0)
            return;

        const double ssTot = Syy - Y[0] * Y[0] / n;

        // Linear fit.
        const double denom = n * M[2] - M[1] * M[1];
        if (std::abs(denom) > 1.0e-12)
        {
            const double slope     = (n * Y[1] - M[1] * Y[0]) / denom;
            const double intercept = (Y[0] - slope * M[1]) / n;
            const double ssRes     = Syy - intercept * Y[0] - slope * Y[1];
            lin.valid          = true;
            lin.slopeDbPerOct  = static_cast<float>(slope);
            lin.interceptDb    = static_cast<float>(intercept);
            lin.rSquared       = ssTot > 1.0e-12 ? static_cast<float>(1.0 - ssRes / ssTot) : 0.0f;
            lin.pointCount     = static_cast<int>(n);
        }

        // Cubic fit via 4x4 normal equations A c = Y, A[i][j] = M[i+j].
        double A[4][4];
        double rhs[4];
        for (int i = 0; i < 4; ++i)
        {
            rhs[i] = Y[static_cast<size_t>(i)];
            for (int j = 0; j < 4; ++j)
                A[i][j] = M[static_cast<size_t>(i + j)];
        }
        std::array<double, 4> coeffs {};
        if (solveLinearSystem4(A, rhs, coeffs))
        {
            double ssRes = Syy;
            for (int j = 0; j < 4; ++j)
                ssRes -= coeffs[static_cast<size_t>(j)] * Y[static_cast<size_t>(j)];
            poly.valid    = true;
            poly.coeffs   = coeffs;
            poly.rSquared = ssTot > 1.0e-12 ? static_cast<float>(1.0 - ssRes / ssTot) : 0.0f;
        }
    }

    // Solve a 4x4 linear system with partial pivoting. Returns false if singular.
    static bool solveLinearSystem4(double A[4][4], double b[4], std::array<double, 4>& out)
    {
        for (int col = 0; col < 4; ++col)
        {
            int pivot = col;
            for (int r = col + 1; r < 4; ++r)
                if (std::abs(A[r][col]) > std::abs(A[pivot][col]))
                    pivot = r;
            if (std::abs(A[pivot][col]) < 1.0e-12)
                return false;
            if (pivot != col)
            {
                for (int c = 0; c < 4; ++c) std::swap(A[pivot][c], A[col][c]);
                std::swap(b[pivot], b[col]);
            }
            for (int r = 0; r < 4; ++r)
            {
                if (r == col) continue;
                const double factor = A[r][col] / A[col][col];
                for (int c = col; c < 4; ++c) A[r][c] -= factor * A[col][c];
                b[r] -= factor * b[col];
            }
        }
        for (int i = 0; i < 4; ++i)
            out[static_cast<size_t>(i)] = b[i] / A[i][i];
        return true;
    }

    // Reference envelope dB at an arbitrary frequency (linear interpolation).
    float sampleReferenceDb(float hz) const
    {
        if (!hasRef || refBinHz <= 0.0f || refMagsDb.empty())
            return floorDb;
        const float pos = hz / refBinHz;
        if (pos <= 0.0f)
            return refMagsDb.front();
        const int i0 = static_cast<int>(pos);
        if (i0 >= static_cast<int>(refMagsDb.size()) - 1)
            return refMagsDb.back();
        const float frac = pos - static_cast<float>(i0);
        return refMagsDb[static_cast<size_t>(i0)] * (1.0f - frac)
             + refMagsDb[static_cast<size_t>(i0 + 1)] * frac;
    }

    void recomputeReferenceFits()
    {
        if (!hasRef || refBinHz <= 0.0f || refMagsDb.empty())
        {
            refFit = SlopeFit {};
            refPoly = PolyFit {};
            return;
        }
        computeFits(refBinHz,
                    static_cast<int>(refMagsDb.size()),
                    [this](int i) { return refMagsDb[static_cast<size_t>(i)]; },
                    refFit, refPoly);
    }

    // Recompute the live linear/cubic fits from the current (possibly frozen
    // or averaged) magnitude envelope. Called every frame and whenever the
    // fit parameters change so the fit stays correct even while paused.
    void recomputeLiveFits()
    {
        const float liveBinHz = (sampleRate > 0.0f) ? sampleRate / static_cast<float>(fftSize) : 0.0f;
        computeFits(liveBinHz,
                    fftSize / 2 + 1,
                    [this](int i) { return mag[static_cast<size_t>(i)]; },
                    liveFit, livePoly);
    }

    // dB -> waterfall colour and its inverse (used for waterfall cursor readout).
    static juce::Colour dbToColour(float db)
    {
        const float t = std::clamp((db - floorDb) / -floorDb, 0.0f, 1.0f);
        if (t < 0.25f) return juce::Colour::fromFloatRGBA(0.0f,           0.0f,                   t * 4.0f,           1.0f);
        if (t < 0.5f)  return juce::Colour::fromFloatRGBA(0.0f,           (t - 0.25f) * 4.0f,     1.0f,               1.0f);
        if (t < 0.75f) return juce::Colour::fromFloatRGBA((t - 0.5f)*4.f, 1.0f, 1.0f-(t-0.5f)*4.f, 1.0f);
        return                 juce::Colour::fromFloatRGBA(1.0f,           1.0f,                   (t-0.75f)*4.0f,     1.0f);
    }

    // Reverse the 4-segment heat-map encoding to recover approximate dB.
    static float colourToDb(juce::Colour c)
    {
        const float r  = c.getFloatRed();
        const float gr = c.getFloatGreen();
        const float b  = c.getFloatBlue();
        float t;
        if      (r < 0.02f && gr < 0.02f) t = b  * 0.25f;          // black→blue
        else if (r < 0.02f)               t = gr * 0.25f + 0.25f;  // blue→cyan
        else if (r < 0.98f)               t = r  * 0.25f + 0.5f;   // cyan→yellow
        else                              t = b  * 0.25f + 0.75f;  // yellow→white
        return std::clamp(t, 0.0f, 1.0f) * (-floorDb) + floorDb;
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

        // Cursor: vertical line + Hz/dB readout in a fixed corner box.
        if (cursorVisible && cursorX >= bx && cursorX < bx + W)
        {
            g.setColour(juce::Colour(0x88ffffff));
            g.drawVerticalLine(cursorX, static_cast<float>(by), static_cast<float>(by + H));

            const float hz = xToHz(cursorX - bx, W);
            const juce::String hzStr = hz >= 1000.0f
                ? juce::String(hz / 1000.0f, 2) + " kHz"
                : juce::String(static_cast<int>(std::round(hz))) + " Hz";

            // Sample the waterfall image pixel at (cursorX, cursorY) and reverse
            // the colour encoding to recover the approximate dB magnitude.
            bool haveDb = false;
            float wfDb  = floorDb;
            if (wfImg.isValid() && cursorY >= by && cursorY < by + H)
            {
                const int localY = cursorY - by;
                const int imgH   = wfImg.getHeight();
                const int partAH = imgH - wfRow;
                const int imgY   = std::clamp(localY < partAH ? wfRow + localY
                                                               : localY - partAH,
                                              0, imgH - 1);
                const int imgX   = std::clamp(cursorX - bx, 0, wfImg.getWidth() - 1);
                wfDb   = colourToDb(wfImg.getPixelAt(imgX, imgY));
                haveDb = true;
            }

            constexpr int lineH = 14, rW = 82, rX0 = 6;
            const int rLines = haveDb ? 2 : 1;
            const int rH = rLines * lineH + 6;
            const int rX = bx + rX0, rY = by + 6;
            g.setColour(juce::Colour(0xbb000000));
            g.fillRoundedRectangle(static_cast<float>(rX), static_cast<float>(rY),
                                   static_cast<float>(rW), static_cast<float>(rH), 3.0f);
            g.setColour(juce::Colour(0xeeffffff));
            g.setFont(11.0f);
            g.drawText(hzStr, rX + 4, rY + 3, rW - 8, lineH, juce::Justification::centredLeft, false);
            if (haveDb)
                g.drawText(juce::String(wfDb, 1) + " dB",
                           rX + 4, rY + 3 + lineH, rW - 8, lineH, juce::Justification::centredLeft, false);
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

        // Slope-analysis region band (only when regression overlay is on).
        const float regionLo = std::clamp(std::min(slopeRegionMinHz, slopeRegionMaxHz), minFreq, sampleRate * 0.5f);
        const float regionHi = std::clamp(std::max(slopeRegionMinHz, slopeRegionMaxHz), minFreq, sampleRate * 0.5f);
        if (showRegression && regionHi > regionLo)
        {
            const float rxLo = static_cast<float>(bx) + hzToX(regionLo, W);
            const float rxHi = static_cast<float>(bx) + hzToX(regionHi, W);
            g.setColour(juce::Colour(0x14ffd54f));
            g.fillRect(juce::Rectangle<float>(rxLo, static_cast<float>(by), rxHi - rxLo, usableH));
            g.setColour(juce::Colour(0x66ffd54f));
            g.drawVerticalLine(static_cast<int>(rxLo), static_cast<float>(by), static_cast<float>(by) + usableH);
            g.drawVerticalLine(static_cast<int>(rxHi), static_cast<float>(by), static_cast<float>(by) + usableH);
        }

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

        // Frozen/imported reference envelope (target to match), ghost line.
        if (showRegression && hasRef)
        {
            juce::Path refLine;
            bool refStarted = false;
            for (int x = 0; x < W; ++x)
            {
                const float hz  = xToHz(x, W);
                const float px  = static_cast<float>(bx + x);
                const float py  = dbToY(sampleReferenceDb(hz));
                if (!refStarted) { refLine.startNewSubPath(px, py); refStarted = true; }
                else             { refLine.lineTo(px, py); }
            }
            if (refStarted)
            {
                g.setColour(juce::Colour(0x99ffffff));
                juce::Path dashed;
                const float dashes[] { 5.0f, 4.0f };
                juce::PathStrokeType(1.2f).createDashedStroke(dashed, refLine, dashes, 2);
                g.strokePath(dashed, juce::PathStrokeType(1.2f));
            }
        }

        // Regression lines over the analysis region.
        auto drawFitLine = [&](const SlopeFit& fit, juce::Colour colour, bool dashed, const juce::String& tag)
        {
            if (!fit.valid)
                return;
            const float f0 = std::clamp(std::min(fit.minHz, fit.maxHz), minFreq, sampleRate * 0.5f);
            const float f1 = std::clamp(std::max(fit.minHz, fit.maxHz), minFreq, sampleRate * 0.5f);
            if (f1 <= f0)
                return;

            const float x0 = static_cast<float>(bx) + hzToX(f0, W);
            const float x1 = static_cast<float>(bx) + hzToX(f1, W);
            const float y1 = dbToY(fit.slopeDbPerOct * std::log2(f1 / 1000.0f) + fit.interceptDb);

            // Sample the line in log-frequency so it stays straight-in-log2
            // even when the display axis is warped.
            juce::Path fitPath;
            constexpr int steps = 96;
            for (int i = 0; i <= steps; ++i)
            {
                const float t  = static_cast<float>(i) / static_cast<float>(steps);
                const float hz = f0 * std::pow(f1 / f0, t);
                const float db = fit.slopeDbPerOct * std::log2(hz / 1000.0f) + fit.interceptDb;
                const float px = static_cast<float>(bx) + hzToX(hz, W);
                const float py = dbToY(db);
                if (i == 0) fitPath.startNewSubPath(px, py);
                else        fitPath.lineTo(px, py);
            }

            g.setColour(colour);
            if (dashed)
            {
                juce::Path d;
                const float dp[] { 7.0f, 5.0f };
                juce::PathStrokeType(2.0f).createDashedStroke(d, fitPath, dp, 2);
                g.strokePath(d, juce::PathStrokeType(2.0f));
            }
            else
            {
                g.strokePath(fitPath, juce::PathStrokeType(2.0f));
            }

            g.setFont(11.0f);
            const juce::String label = tag + juce::String(fit.slopeDbPerOct, 1) + " dB/oct";
            g.drawText(label,
                       static_cast<int>(x1) - 116, static_cast<int>(y1) - 16, 112, 14,
                       juce::Justification::centredRight, false);
        };

        if (showRegression)
        {
            if (hasRef)
                drawFitLine(refFit, juce::Colour(0xbbffffff), true, "target ");
            drawFitLine(liveFit, juce::Colour(0xffffd54f), false, "");
        }

        // Cubic-polynomial fits (optional), matching the offline analysis.
        if (showRegression && showPoly)
        {
            auto drawPolyCurve = [&](const PolyFit& p, juce::Colour colour, bool dashed)
            {
                if (!p.valid)
                    return;
                const float f0 = std::clamp(std::min(p.minHz, p.maxHz), minFreq, sampleRate * 0.5f);
                const float f1 = std::clamp(std::max(p.minHz, p.maxHz), minFreq, sampleRate * 0.5f);
                if (f1 <= f0)
                    return;

                juce::Path curve;
                constexpr int steps = 96;
                for (int i = 0; i <= steps; ++i)
                {
                    const float t  = static_cast<float>(i) / static_cast<float>(steps);
                    const float hz = f0 * std::pow(f1 / f0, t);
                    const double x = std::log2(hz / 1000.0f);
                    const double y = p.coeffs[0] + p.coeffs[1] * x
                                   + p.coeffs[2] * x * x + p.coeffs[3] * x * x * x;
                    const float px = static_cast<float>(bx) + hzToX(hz, W);
                    const float py = dbToY(static_cast<float>(y));
                    if (i == 0) curve.startNewSubPath(px, py);
                    else        curve.lineTo(px, py);
                }
                g.setColour(colour);
                if (dashed)
                {
                    juce::Path d;
                    const float dp[] { 4.0f, 4.0f };
                    juce::PathStrokeType(1.6f).createDashedStroke(d, curve, dp, 2);
                    g.strokePath(d, juce::PathStrokeType(1.6f));
                }
                else
                {
                    g.strokePath(curve, juce::PathStrokeType(1.6f));
                }

                // Endpoint tangent-slope labels (dB/oct at each region edge).
                auto edgeDb = [&](float hz)
                {
                    const double x = std::log2(hz / 1000.0f);
                    return static_cast<float>(p.coeffs[0] + p.coeffs[1] * x
                                            + p.coeffs[2] * x * x + p.coeffs[3] * x * x * x);
                };
                g.setFont(10.0f);
                const float sLo = polyLocalSlopeDbPerOct(p, f0);
                const float sHi = polyLocalSlopeDbPerOct(p, f1);
                g.drawText(juce::String(sLo, 1),
                           static_cast<int>(static_cast<float>(bx) + hzToX(f0, W)) + 2,
                           static_cast<int>(dbToY(edgeDb(f0))) - 14, 44, 12,
                           juce::Justification::centredLeft, false);
                g.drawText(juce::String(sHi, 1),
                           static_cast<int>(static_cast<float>(bx) + hzToX(f1, W)) - 46,
                           static_cast<int>(dbToY(edgeDb(f1))) + 2, 44, 12,
                           juce::Justification::centredRight, false);
            };

            if (hasRef)
                drawPolyCurve(refPoly, juce::Colour(0xcc93c5fd), true);
            drawPolyCurve(livePoly, juce::Colour(0xff34d399), false);
        }

        // Manual numeric target: ideal straight roll-off line at a chosen
        // slope, placed to minimise the vertical distance to the live
        // regression line. Both lines cross at the log-frequency midpoint of
        // the region, so slope differences are immediately visible.
        if (showRegression && manualTargetEnabled && regionHi > regionLo)
        {
            float intercept;
            if (liveFit.valid)
            {
                // Optimal vertical offset: the two lines intersect at the
                // centre of the analysis region (in log2(hz/1kHz) space).
                const float xMid = 0.5f * (std::log2(regionLo / 1000.0f)
                                          + std::log2(regionHi / 1000.0f));
                intercept = liveFit.interceptDb
                           - (manualTargetSlope - liveFit.slopeDbPerOct) * xMid;
            }
            else
            {
                intercept = -20.0f; // nominal when no live fit is available yet
            }
            const float x1 = static_cast<float>(bx) + hzToX(regionHi, W);
            const float y1 = dbToY(manualTargetSlope * std::log2(regionHi / 1000.0f) + intercept);

            juce::Path targetPath;
            constexpr int steps = 96;
            for (int i = 0; i <= steps; ++i)
            {
                const float t  = static_cast<float>(i) / static_cast<float>(steps);
                const float hz = regionLo * std::pow(regionHi / regionLo, t);
                const float db = manualTargetSlope * std::log2(hz / 1000.0f) + intercept;
                const float px = static_cast<float>(bx) + hzToX(hz, W);
                const float py = dbToY(db);
                if (i == 0) targetPath.startNewSubPath(px, py);
                else        targetPath.lineTo(px, py);
            }
            juce::Path d;
            const float dp[] { 6.0f, 5.0f };
            juce::PathStrokeType(2.0f).createDashedStroke(d, targetPath, dp, 2);
            g.setColour(juce::Colour(0xfff472b6));
            g.strokePath(d, juce::PathStrokeType(2.0f));

            g.setFont(11.0f);
            g.drawText("manual " + juce::String(manualTargetSlope, 1) + " dB/oct",
                       static_cast<int>(x1) - 130, static_cast<int>(y1) + 2, 126, 14,
                       juce::Justification::centredRight, false);
        }

        // Precise peak markers: parabolic-interpolated peak positions so the
        // reader is not misled by line/pixel interpolation when examining the
        // progression of peak amplitudes.
        if (showPeakMarkers)
        {
            std::vector<std::pair<float, float>> peaks; // (hz, dB)
            collectRefinedPeaks(peaks);

            if (!peaks.empty())
            {
                // Faint line connecting successive peak tops (the progression).
                juce::Path progression;
                bool progStarted = false;
                for (const auto& pk : peaks)
                {
                    const float px = static_cast<float>(bx) + hzToX(pk.first, W);
                    const float py = dbToY(pk.second);
                    if (!progStarted) { progression.startNewSubPath(px, py); progStarted = true; }
                    else              { progression.lineTo(px, py); }
                }
                g.setColour(juce::Colour(0x5500e5ff));
                g.strokePath(progression, juce::PathStrokeType(1.0f));

                // Discrete markers at the exact interpolated peak positions.
                g.setColour(juce::Colour(0xff00e5ff));
                for (const auto& pk : peaks)
                {
                    const float px = static_cast<float>(bx) + hzToX(pk.first, W);
                    const float py = dbToY(pk.second);
                    g.fillEllipse(px - 2.5f, py - 2.5f, 5.0f, 5.0f);
                }
            }
        }

        // Cursor: vertical + horizontal lines, plus a fixed-position readout box.
        if (cursorVisible && cursorX >= bx && cursorX < bx + W)
        {
            const bool inPanel = (cursorY >= by && cursorY < by + static_cast<int>(usableH));

            g.setColour(juce::Colour(0x88ffffff));
            g.drawVerticalLine(cursorX, static_cast<float>(by), static_cast<float>(by) + usableH);
            if (inPanel)
                g.drawHorizontalLine(cursorY, static_cast<float>(bx), static_cast<float>(bx + W));

            const float hz = xToHz(cursorX - bx, W);
            const juce::String hzStr = hz >= 1000.0f
                ? juce::String(hz / 1000.0f, 2) + " kHz"
                : juce::String(static_cast<int>(std::round(hz))) + " Hz";

            constexpr int lineH = 14, rW = 82, rX0 = 6;
            const int rLines = inPanel ? 2 : 1;
            const int rH = rLines * lineH + 6;
            const int rX = bx + rX0, rY = by + 6;
            g.setColour(juce::Colour(0xbb000000));
            g.fillRoundedRectangle(static_cast<float>(rX), static_cast<float>(rY),
                                   static_cast<float>(rW), static_cast<float>(rH), 3.0f);
            g.setColour(juce::Colour(0xeeffffff));
            g.setFont(11.0f);
            g.drawText(hzStr, rX + 4, rY + 3, rW - 8, lineH, juce::Justification::centredLeft, false);
            if (inPanel)
            {
                const float cursorDb = juce::jmap(static_cast<float>(cursorY),
                                                   static_cast<float>(by),
                                                   static_cast<float>(by) + usableH,
                                                   0.0f, floorDb);
                g.drawText(juce::String(cursorDb, 1) + " dB",
                           rX + 4, rY + 3 + lineH, rW - 8, lineH,
                           juce::Justification::centredLeft, false);
            }
        }
    }

    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize>         ring    {};
    std::array<float, fftSize * 2>     fftBuf  {};
    std::array<float, fftSize>         hannWin {};
    std::array<float, fftSize / 2 + 1> mag     {};
    spex::SpectralFeatureAnalyzer<fftSize / 2 + 1> featureAnalyzer;

    std::atomic<int>  wPos   { 0 };
    std::atomic<bool> hasNew { false };

    float sampleRate { 44100.0f };
    bool  scrollingPaused { false };

    // Slope analysis / reference matching state.
    float slopeRegionMinHz { 3000.0f };
    float slopeRegionMaxHz { 20000.0f };
    SlopeFit liveFit {};
    PolyFit  livePoly {};
    SlopeFit refFit  {};
    PolyFit  refPoly {};
    bool  hasRef { false };
    std::vector<float> refMagsDb;      // reference magnitude per bin (dB)
    float refBinHz { 0.0f };           // frequency spacing of reference bins
    juce::String referenceName;
    bool  showPoly { false };
    bool  showPeakMarkers { false };
    bool  showRegression { true };
    bool  manualTargetEnabled { false };
    float manualTargetSlope { -6.97f };
    bool  fitPeaksOnly { true };   // fit regression to spectral peaks only
    float peakThresholdDb { -60.0f }; // minimum peak height for peak-only fit
    float freqWarpGamma { 1.0f };  // >1 expands the upper octaves of the axis
    bool  averagingEnabled { false };
    long long avgFrameCount { 0 };
    std::array<double, fftSize / 2 + 1> avgPower {}; // cumulative power per bin

    juce::Image wfImg;
    int wfRow { 0 };
    bool cursorVisible { false };
    int  cursorX { 0 };
    int  cursorY { 0 };

    bool showWaterfall { true };
    bool showEnvelope { true };
};
