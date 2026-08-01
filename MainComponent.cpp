#include "MainComponent.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "Version.h"

namespace
{
constexpr int kFeatureCount = spex::numSpectralFeatures;

const std::array<const char*, kFeatureCount> kFeatureNames
{
    "Windowed Peak",
    "Sliding RMS",
    "Interpolated Spectral Peak",
    "Spectral PAPR",
    "Local Spectral Crest",
    "Spectral Flatness"
};

const std::array<juce::Range<float>, kFeatureCount> kDefaultValueRanges
{
    juce::Range<float>(-90.0f, 0.0f),
    juce::Range<float>(-90.0f, 0.0f),
    juce::Range<float>(-90.0f, 20.0f),
    juce::Range<float>(0.0f, 36.0f),
    juce::Range<float>(0.0f, 1.0f)
};

const std::array<float, kFeatureCount> kSmoothingAlpha
{
    0.86f,
    0.90f,
    0.88f,
    0.85f,
    0.85f,
    0.65f
};

const std::array<float, kFeatureCount> kHysteresis
{
    0.20f,
    0.20f,
    0.20f,
    0.15f,
    0.15f,
    0.0001f
};

const std::array<juce::Colour, kFeatureCount> kFeatureColours
{
    juce::Colour(0xff38bdf8),
    juce::Colour(0xff22c55e),
    juce::Colour(0xfff59e0b),
    juce::Colour(0xffef4444),
    juce::Colour(0xffa78bfa),
    juce::Colour(0xffa78bfa)
};

juce::String csvHeaderName(int featureIndex)
{
    switch (featureIndex)
    {
        case spex::windowedPeakAmplitude:    return "windowed_peak_dbfs";
        case spex::slidingWindowRms:         return "rms_dbfs";
        case spex::interpolatedSpectralPeak: return "interp_spectral_peak_db";
        case spex::papr:                     return "spectral_papr_db";
        case spex::localSpectralCrest:       return "local_spectral_crest_db";
        case spex::spectralFlatness:         return "spectral_flatness";
        default:                                                 return "feature";
    }
}

juce::String formatFeatureValueText(int featureIndex, float value)
{
    if (!std::isfinite(value))
        return "nan";

    switch (featureIndex)
    {
        case spex::spectralFlatness:
            return juce::String(value, 4);
        default:
            return juce::String(value, 2) + " dB";
    }
}

juce::Range<float> makeAutoscaledRange(int featureIndex,
                                       float minValue,
                                       float maxValue,
                                       bool hasBounds)
{
    const auto fallback = kDefaultValueRanges[static_cast<size_t>(featureIndex)];
    if (!hasBounds)
        return fallback;

    const float lo = std::min(minValue, maxValue);
    const float hi = std::max(minValue, maxValue);
    const float span = hi - lo;
    const float pad = std::max(span * 0.1f, featureIndex == spex::spectralFlatness ? 0.005f : 0.1f);

    float start = lo - pad;
    float end = hi + pad;

    if (span < 1.0e-6f)
    {
        const float center = lo;
        const float halfWidth = featureIndex == spex::spectralFlatness ? 0.02f : 0.5f;
        start = center - halfWidth;
        end = center + halfWidth;
    }

    if (featureIndex == spex::spectralFlatness)
    {
        start = std::max(0.0f, start);
        end = std::min(1.0f, end);
        if (end - start < 0.01f)
            end = std::min(1.0f, start + 0.01f);
    }

    return { start, end };
}

void drawTrace(juce::Graphics& g,
               juce::Rectangle<float> area,
               const std::vector<float>& history,
               juce::Range<float> valueRange,
               juce::Colour colour)
{
    g.setColour(juce::Colour(0x141ffffff));
    g.fillRoundedRectangle(area, 4.0f);

    if (history.size() < 2)
        return;

    g.setColour(juce::Colour(0x20ffffff));
    g.drawLine(area.getX(), area.getCentreY(), area.getRight(), area.getCentreY(), 1.0f);

    juce::Path trace;
    const auto clampToRange = [&](float value)
    {
        return std::clamp(value, valueRange.getStart(), valueRange.getEnd());
    };

    const float minV = valueRange.getStart();
    const float maxV = valueRange.getEnd();
    const float xStep = area.getWidth() / static_cast<float>(history.size() - 1);
    bool started = false;

    for (size_t i = 0; i < history.size(); ++i)
    {
        if (!std::isfinite(history[i]))
        {
            started = false;
            continue;
        }

        const float x = area.getX() + xStep * static_cast<float>(i);
        const float clamped = clampToRange(history[i]);
        const float y = juce::jmap(clamped, minV, maxV, area.getBottom(), area.getY());

        if (!started)
        {
            trace.startNewSubPath(x, y);
            started = true;
        }
        else
        {
            trace.lineTo(x, y);
        }
    }

    g.setColour(colour.withAlpha(0.95f));
    g.strokePath(trace, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
}

MainComponent::MainComponent()
{
    audioSettingsButton.onClick = [this] { openAudioSettings(); };
    audioSettingsButton.setWantsKeyboardFocus(false);

    controlsButton.setClickingTogglesState(true);
    controlsButton.setToggleState(true, juce::dontSendNotification);
    controlsButton.onClick = [this] { setControlsVisible(controlsButton.getToggleState()); };

    featuresButton.setClickingTogglesState(true);
    featuresButton.setToggleState(true, juce::dontSendNotification);
    featuresButton.onClick = [this] { setFeaturePanelVisible(featuresButton.getToggleState()); };

    pauseButton.onClick = [this] { toggleScrollingPause(); };
    captureButton.onClick = [this] { toggleCapture(); };
    exportCsvButton.onClick = [this] { exportCaptureCsv(); };

    exportCsvButton.setEnabled(false);

    featureFreqRangeLabel.setText("Feature Range", juce::dontSendNotification);
    featureFreqRangeLabel.setJustificationType(juce::Justification::centredLeft);
    featureFreqRangeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    featureFreqRangeSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    featureFreqRangeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    featureFreqRangeSlider.onValueChange = [this] { applyFeatureFrequencyRangeFromUi(); };

    flatnessPowerFloorLabel.setText("Flatness Floor", juce::dontSendNotification);
    flatnessPowerFloorLabel.setJustificationType(juce::Justification::centredLeft);
    flatnessPowerFloorLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    flatnessPowerFloorSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    flatnessPowerFloorSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    flatnessPowerFloorSlider.setRange(-180.0, -30.0, 0.1);
    flatnessPowerFloorSlider.setValue(-120.0, juce::dontSendNotification);
    flatnessPowerFloorSlider.onValueChange = [this] { applyFlatnessPowerFloorFromUi(); };

    flatnessPowerFloorValueLabel.setJustificationType(juce::Justification::centredRight);
    flatnessPowerFloorValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    slopeRegionLabel.setText("Slope Region", juce::dontSendNotification);
    slopeRegionLabel.setJustificationType(juce::Justification::centredLeft);
    slopeRegionLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    slopeRegionSlider.setSliderStyle(juce::Slider::TwoValueHorizontal);
    slopeRegionSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    slopeRegionSlider.onValueChange = [this] { applySlopeRegionFromUi(); };

    minSlopeFreqValueLabel.setJustificationType(juce::Justification::centredRight);
    minSlopeFreqValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));
    maxSlopeFreqValueLabel.setJustificationType(juce::Justification::centredRight);
    maxSlopeFreqValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    captureReferenceButton.onClick = [this] { captureSlopeReference(); };
    clearReferenceButton.onClick = [this] { clearSlopeReference(); };
    clearReferenceButton.setEnabled(false);

    slopeReadoutLabel.setJustificationType(juce::Justification::centredLeft);
    slopeReadoutLabel.setColour(juce::Label::textColourId, juce::Colour(0xfffde68a));
    slopeReadoutLabel.setFont(juce::Font(14.0f, juce::Font::bold));

    loadTargetButton.onClick = [this] { loadTargetAudio(); };

    manualTargetToggle.setButtonText("Manual dB/oct");
    manualTargetToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    manualTargetToggle.onClick = [this] { applyManualTargetFromUi(); };

    targetSlopeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    targetSlopeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    targetSlopeSlider.setRange(-24.0, 0.0, 0.01);
    targetSlopeSlider.setValue(-6.97, juce::dontSendNotification);
    targetSlopeSlider.onValueChange = [this] { applyManualTargetFromUi(); };

    targetSlopeValueLabel.setJustificationType(juce::Justification::centredRight);
    targetSlopeValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xfff9a8d4));

    cubicFitToggle.setButtonText("Cubic fit");
    cubicFitToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    cubicFitToggle.onClick = [this] { applyCubicFitFromUi(); };

    peakMarkersToggle.setButtonText("Peak marks");
    peakMarkersToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    peakMarkersToggle.onClick = [this] { applyPeakMarkersFromUi(); };

    fitPeaksToggle.setButtonText("Fit peaks");
    fitPeaksToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    fitPeaksToggle.setToggleState(true, juce::dontSendNotification);
    fitPeaksToggle.onClick = [this] { applyFitPeaksFromUi(); };

    peakFloorLabel.setText("Peak Floor", juce::dontSendNotification);
    peakFloorLabel.setJustificationType(juce::Justification::centredLeft);
    peakFloorLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    peakFloorSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    peakFloorSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    peakFloorSlider.setRange(-80.0, 0.0, 0.5);
    peakFloorSlider.setValue(-60.0, juce::dontSendNotification);
    peakFloorSlider.onValueChange = [this] { applyPeakFloorFromUi(); };

    peakFloorValueLabel.setJustificationType(juce::Justification::centredRight);
    peakFloorValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    averageToggle.setButtonText("Average");
    averageToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    averageToggle.onClick = [this] { applyAveragingFromUi(); };

    waterfallToggle.setButtonText("Waterfall");
    waterfallToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    waterfallToggle.setToggleState(true, juce::dontSendNotification);
    waterfallToggle.onClick = [this] { applyWaterfallVisibilityFromUi(); };

    envelopeToggle.setButtonText("Envelope");
    envelopeToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    envelopeToggle.setToggleState(true, juce::dontSendNotification);
    envelopeToggle.onClick = [this] { applyEnvelopeVisibilityFromUi(); };

    regressionToggle.setButtonText("Regression");
    regressionToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffd1d5db));
    regressionToggle.setToggleState(true, juce::dontSendNotification);
    regressionToggle.onClick = [this] { applyRegressionFromUi(); };

    clearAverageButton.onClick = [this] { spectralDisplay.clearAveraging(); };
    clearAverageButton.setEnabled(false);

    gainLabel.setText("Pre-gain", juce::dontSendNotification);
    gainLabel.setJustificationType(juce::Justification::centredLeft);
    gainLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    gainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    gainSlider.setRange(-12.0, 24.0, 0.1);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.onValueChange = [this]
    {
        preAnalysisGainLinear.store(
            juce::Decibels::decibelsToGain(static_cast<float>(gainSlider.getValue())),
            std::memory_order_relaxed);
        updateGainLabel();
    };

    gainValueLabel.setJustificationType(juce::Justification::centredRight);
    gainValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    freqWarpLabel.setText("Freq Warp", juce::dontSendNotification);
    freqWarpLabel.setJustificationType(juce::Justification::centredLeft);
    freqWarpLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd1d5db));

    freqWarpSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    freqWarpSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    freqWarpSlider.setRange(1.0, 4.0, 0.01);
    freqWarpSlider.setValue(1.0, juce::dontSendNotification);
    freqWarpSlider.onValueChange = [this] { applyFreqWarpFromUi(); };

    freqWarpValueLabel.setJustificationType(juce::Justification::centredRight);
    freqWarpValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    minFeatureFreqValueLabel.setJustificationType(juce::Justification::centredRight);
    minFeatureFreqValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));
    maxFeatureFreqValueLabel.setJustificationType(juce::Justification::centredRight);
    maxFeatureFreqValueLabel.setColour(juce::Label::textColourId, juce::Colour(0xff94a3b8));

    addAndMakeVisible(audioSettingsButton);
    addAndMakeVisible(controlsButton);
    addAndMakeVisible(featuresButton);
    addAndMakeVisible(pauseButton);
    addAndMakeVisible(captureButton);
    addAndMakeVisible(exportCsvButton);
    addAndMakeVisible(featureFreqRangeLabel);
    addAndMakeVisible(featureFreqRangeSlider);
    addAndMakeVisible(minFeatureFreqValueLabel);
    addAndMakeVisible(maxFeatureFreqValueLabel);
    addAndMakeVisible(flatnessPowerFloorLabel);
    addAndMakeVisible(flatnessPowerFloorSlider);
    addAndMakeVisible(flatnessPowerFloorValueLabel);
    addAndMakeVisible(slopeRegionLabel);
    addAndMakeVisible(slopeRegionSlider);
    addAndMakeVisible(minSlopeFreqValueLabel);
    addAndMakeVisible(maxSlopeFreqValueLabel);
    addAndMakeVisible(captureReferenceButton);
    addAndMakeVisible(clearReferenceButton);
    addAndMakeVisible(loadTargetButton);
    addAndMakeVisible(manualTargetToggle);
    addAndMakeVisible(targetSlopeSlider);
    addAndMakeVisible(targetSlopeValueLabel);
    addAndMakeVisible(cubicFitToggle);
    addAndMakeVisible(fitPeaksToggle);
    addAndMakeVisible(peakMarkersToggle);
    addAndMakeVisible(peakFloorLabel);
    addAndMakeVisible(peakFloorSlider);
    addAndMakeVisible(peakFloorValueLabel);
    addAndMakeVisible(averageToggle);
    addAndMakeVisible(clearAverageButton);
    addAndMakeVisible(waterfallToggle);
    addAndMakeVisible(envelopeToggle);
    addAndMakeVisible(regressionToggle);
    addAndMakeVisible(slopeReadoutLabel);
    addAndMakeVisible(gainLabel);
    addAndMakeVisible(gainSlider);
    addAndMakeVisible(gainValueLabel);
    addAndMakeVisible(freqWarpLabel);
    addAndMakeVisible(freqWarpSlider);
    addAndMakeVisible(freqWarpValueLabel);
    addAndMakeVisible(spectralDisplay);

    for (auto& history : featureHistory)
        history.reserve(featureHistoryLength + 16);

    resetAutoscaleBounds();

    featureFreqRangeSlider.setRange(0.0, currentNyquistHz, 1.0);
    featureFreqRangeSlider.setMinValue(0.0, juce::dontSendNotification, false);
    featureFreqRangeSlider.setMaxValue(currentNyquistHz, juce::dontSendNotification, false);
    featureFreqRangeSlider.setSkewFactorFromMidPoint(std::sqrt(currentNyquistHz));
    applyFeatureFrequencyRangeFromUi();
    applyFlatnessPowerFloorFromUi();
    updateGainLabel();

    slopeRegionSlider.setRange(0.0, currentNyquistHz, 1.0);
    slopeRegionSlider.setMinValue(std::min(3000.0, static_cast<double>(currentNyquistHz)), juce::dontSendNotification, false);
    slopeRegionSlider.setMaxValue(std::min(20000.0, static_cast<double>(currentNyquistHz)), juce::dontSendNotification, false);
    slopeRegionSlider.setSkewFactorFromMidPoint(std::sqrt(currentNyquistHz));
    applySlopeRegionFromUi();
    updateSlopeReadout();

    applyManualTargetFromUi();
    applyCubicFitFromUi();
    applyFitPeaksFromUi();
    applyPeakFloorFromUi();
    applyPeakMarkersFromUi();
    applyFreqWarpFromUi();
    applyAveragingFromUi();
    applyRegressionFromUi();

    setSize(1320, 800);

    // 2 input channels (analysis source), 2 output channels (kept silent).
    setAudioChannels(2, 2);
    startTimerHz(30);
}

MainComponent::~MainComponent()
{
    stopTimer();
    shutdownAudio();
}

void MainComponent::prepareToPlay(int /*samplesPerBlockExpected*/, double sampleRate)
{
    spectralDisplay.setSampleRate(static_cast<float>(sampleRate));

    const float newNyquist = std::max(1.0f, static_cast<float>(sampleRate) * 0.5f);
    const bool wasFullRange = featureFreqRangeSlider.getMaxValue() >= (currentNyquistHz - 1.0f);
    const double previousMin = featureFreqRangeSlider.getMinValue();
    const double previousMax = featureFreqRangeSlider.getMaxValue();

    currentNyquistHz = newNyquist;
    featureFreqRangeSlider.setRange(0.0, currentNyquistHz, 1.0);
    featureFreqRangeSlider.setMinValue(std::clamp(previousMin, 0.0, static_cast<double>(currentNyquistHz)),
                                       juce::dontSendNotification,
                                       false);
    featureFreqRangeSlider.setMaxValue(wasFullRange
                                           ? static_cast<double>(currentNyquistHz)
                                           : std::clamp(previousMax, 0.0, static_cast<double>(currentNyquistHz)),
                                       juce::dontSendNotification,
                                       false);
    featureFreqRangeSlider.setSkewFactorFromMidPoint(std::sqrt(currentNyquistHz));

    const double previousSlopeMin = slopeRegionSlider.getMinValue();
    const double previousSlopeMax = slopeRegionSlider.getMaxValue();
    slopeRegionSlider.setRange(0.0, currentNyquistHz, 1.0);
    slopeRegionSlider.setMinValue(std::clamp(previousSlopeMin, 0.0, static_cast<double>(currentNyquistHz)),
                                  juce::dontSendNotification, false);
    slopeRegionSlider.setMaxValue(std::clamp(previousSlopeMax, 0.0, static_cast<double>(currentNyquistHz)),
                                  juce::dontSendNotification, false);
    slopeRegionSlider.setSkewFactorFromMidPoint(std::sqrt(currentNyquistHz));

    applyFeatureFrequencyRangeFromUi();
    applySlopeRegionFromUi();
}

void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    auto* buffer = bufferToFill.buffer;
    const int startSample = bufferToFill.startSample;
    const int numSamples  = bufferToFill.numSamples;
    const int numInputs   = buffer->getNumChannels();

    if (numInputs > 0 && numSamples > 0)
    {
        // Mono-sum the available input channels into a scratch buffer, then
        // feed the spectral display.
        monoScratch.resize(static_cast<size_t>(numSamples));

        const float* first = buffer->getReadPointer(0, startSample);
        for (int i = 0; i < numSamples; ++i)
            monoScratch[static_cast<size_t>(i)] = first[i];

        for (int ch = 1; ch < numInputs; ++ch)
        {
            const float* in = buffer->getReadPointer(ch, startSample);
            for (int i = 0; i < numSamples; ++i)
                monoScratch[static_cast<size_t>(i)] += in[i];
        }

        if (numInputs > 1)
        {
            const float scale = 1.0f / static_cast<float>(numInputs);
            for (int i = 0; i < numSamples; ++i)
                monoScratch[static_cast<size_t>(i)] *= scale;
        }

        const float gain = preAnalysisGainLinear.load(std::memory_order_relaxed);
        if (gain != 1.0f)
        {
            for (int i = 0; i < numSamples; ++i)
                monoScratch[static_cast<size_t>(i)] *= gain;
        }

        spectralDisplay.pushSamples(monoScratch.data(), numSamples);
    }

    // Do not echo the input to the output; keep the device silent.
    bufferToFill.clearActiveBufferRegion();
}

void MainComponent::releaseResources()
{
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff0c0c0c));

    g.setColour(juce::Colour(0x88ffffff));
    g.setFont(14.0f);
    g.drawText("Realtime audio spectrum", 16, 12, 280, 24, juce::Justification::centredLeft, false);

    g.setColour(juce::Colour(0x44ffffff));
    g.setFont(11.0f);
    g.drawText("v" SPEX_VERSION_STRING "  " SPEX_GIT_HASH,
               300, 12, 260, 24, juce::Justification::centredLeft, false);

    if (featurePanelVisible && !featurePanelBounds.isEmpty())
    {
        g.setColour(juce::Colour(0x30ffffff));
        g.drawVerticalLine(featurePanelBounds.getX() - 8,
                           static_cast<float>(featurePanelBounds.getY()),
                           static_cast<float>(featurePanelBounds.getBottom()));

        paintFeaturePanel(g, featurePanelBounds);
    }
}

void MainComponent::resized()
{
    auto bounds = getLocalBounds().reduced(12);
    auto header = bounds.removeFromTop(40);

    exportCsvButton.setBounds(header.removeFromRight(124));
    header.removeFromRight(8);
    captureButton.setBounds(header.removeFromRight(124));
    header.removeFromRight(8);
    pauseButton.setBounds(header.removeFromRight(88));
    header.removeFromRight(8);
    featuresButton.setBounds(header.removeFromRight(96));
    header.removeFromRight(8);
    controlsButton.setBounds(header.removeFromRight(96));
    header.removeFromRight(8);
    audioSettingsButton.setBounds(header.removeFromRight(148));

    if (controlsVisible)
    {
        auto rangeRow = bounds.removeFromTop(34);
        featureFreqRangeLabel.setBounds(rangeRow.removeFromLeft(108));
        minFeatureFreqValueLabel.setBounds(rangeRow.removeFromLeft(72));
        rangeRow.removeFromLeft(12);
        featureFreqRangeSlider.setBounds(rangeRow.removeFromLeft(410));
        rangeRow.removeFromLeft(12);
        maxFeatureFreqValueLabel.setBounds(rangeRow.removeFromLeft(72));

        auto floorRow = bounds.removeFromTop(34);
        flatnessPowerFloorLabel.setBounds(floorRow.removeFromLeft(108));
        flatnessPowerFloorValueLabel.setBounds(floorRow.removeFromLeft(72));
        floorRow.removeFromLeft(12);
        flatnessPowerFloorSlider.setBounds(floorRow.removeFromLeft(410));
        floorRow.removeFromLeft(24);
        averageToggle.setBounds(floorRow.removeFromLeft(110));
        floorRow.removeFromLeft(8);
        clearAverageButton.setBounds(floorRow.removeFromLeft(110));
        floorRow.removeFromLeft(16);
        peakMarkersToggle.setBounds(floorRow.removeFromLeft(120));
        floorRow.removeFromLeft(8);
        waterfallToggle.setBounds(floorRow.removeFromLeft(116));
        floorRow.removeFromLeft(8);
        envelopeToggle.setBounds(floorRow.removeFromLeft(104));

        auto gainRow = bounds.removeFromTop(34);
        gainLabel.setBounds(gainRow.removeFromLeft(108));
        gainValueLabel.setBounds(gainRow.removeFromLeft(72));
        gainRow.removeFromLeft(12);
        gainSlider.setBounds(gainRow.removeFromLeft(410));
        gainRow.removeFromLeft(24);
        freqWarpLabel.setBounds(gainRow.removeFromLeft(96));
        freqWarpValueLabel.setBounds(gainRow.removeFromLeft(64));
        gainRow.removeFromLeft(8);
        freqWarpSlider.setBounds(gainRow.removeFromLeft(200));

        auto slopeRow = bounds.removeFromTop(34);
        slopeRegionLabel.setBounds(slopeRow.removeFromLeft(108));
        minSlopeFreqValueLabel.setBounds(slopeRow.removeFromLeft(72));
        slopeRow.removeFromLeft(12);
        slopeRegionSlider.setBounds(slopeRow.removeFromLeft(410));
        slopeRow.removeFromLeft(12);
        maxSlopeFreqValueLabel.setBounds(slopeRow.removeFromLeft(72));
        slopeRow.removeFromLeft(16);
        captureReferenceButton.setBounds(slopeRow.removeFromLeft(112));
        slopeRow.removeFromLeft(8);
        clearReferenceButton.setBounds(slopeRow.removeFromLeft(112));
        slopeRow.removeFromLeft(16);
        regressionToggle.setBounds(slopeRow.removeFromLeft(130));

        auto targetRow = bounds.removeFromTop(34);
        loadTargetButton.setBounds(targetRow.removeFromLeft(150));
        targetRow.removeFromLeft(16);
        manualTargetToggle.setBounds(targetRow.removeFromLeft(130));
        targetRow.removeFromLeft(4);
        targetSlopeSlider.setBounds(targetRow.removeFromLeft(240));
        targetRow.removeFromLeft(8);
        targetSlopeValueLabel.setBounds(targetRow.removeFromLeft(96));
        targetRow.removeFromLeft(16);
        cubicFitToggle.setBounds(targetRow.removeFromLeft(110));
        targetRow.removeFromLeft(8);
        fitPeaksToggle.setBounds(targetRow.removeFromLeft(110));
        targetRow.removeFromLeft(16);
        peakFloorLabel.setBounds(targetRow.removeFromLeft(74));
        peakFloorSlider.setBounds(targetRow.removeFromLeft(150));
        targetRow.removeFromLeft(6);
        peakFloorValueLabel.setBounds(targetRow.removeFromLeft(64));
    }

    auto readoutRow = bounds.removeFromTop(26);
    slopeReadoutLabel.setBounds(readoutRow.reduced(4, 0));

    bounds.removeFromTop(6);

    if (featurePanelVisible)
    {
        featurePanelBounds = bounds.removeFromRight(360);
        spectralDisplay.setBounds(bounds);
    }
    else
    {
        featurePanelBounds = {};
        spectralDisplay.setBounds(bounds);
    }
}

void MainComponent::applyFeatureFrequencyRangeFromUi()
{
    const float appliedMin = static_cast<float>(featureFreqRangeSlider.getMinValue());
    const float appliedMax = static_cast<float>(featureFreqRangeSlider.getMaxValue());
    spectralDisplay.setFeatureFrequencyRange(appliedMin, appliedMax);
    updateFrequencyRangeLabels();
}

void MainComponent::applyFlatnessPowerFloorFromUi()
{
    const float floorDb = static_cast<float>(flatnessPowerFloorSlider.getValue());
    spectralDisplay.setFlatnessPowerFloorDb(floorDb);
    updateFlatnessPowerFloorLabel();
}

void MainComponent::applySlopeRegionFromUi()
{
    const float minHz = static_cast<float>(slopeRegionSlider.getMinValue());
    const float maxHz = static_cast<float>(slopeRegionSlider.getMaxValue());
    spectralDisplay.setSlopeRegion(minHz, maxHz);
    updateSlopeRegionLabels();
    updateSlopeReadout();
}

void MainComponent::captureSlopeReference()
{
    spectralDisplay.captureReference();
    clearReferenceButton.setEnabled(true);
    updateSlopeReadout();
}

void MainComponent::clearSlopeReference()
{
    spectralDisplay.clearReference();
    clearReferenceButton.setEnabled(false);
    updateSlopeReadout();
}

void MainComponent::loadTargetAudio()
{
    importChooser = std::make_unique<juce::FileChooser>("Select target audio file",
                                                        juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");

    const int flags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectFiles;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    importChooser->launchAsync(flags, [safeThis](const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;
        const auto file = chooser.getResult();
        if (file != juce::File())
        {
            if (safeThis->spectralDisplay.analyzeReferenceFile(file))
            {
                safeThis->clearReferenceButton.setEnabled(true);
                safeThis->updateSlopeReadout();
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Load Target Audio",
                                                       "Could not read or analyse the selected audio file.");
            }
        }

        safeThis->importChooser.reset();
    });
}

void MainComponent::applyManualTargetFromUi()
{
    const bool enabled = manualTargetToggle.getToggleState();
    const float slope = static_cast<float>(targetSlopeSlider.getValue());
    targetSlopeSlider.setEnabled(enabled);
    spectralDisplay.setManualTarget(enabled, slope);
    updateManualTargetLabel();
    updateSlopeReadout();
}

void MainComponent::applyCubicFitFromUi()
{
    spectralDisplay.setShowPolynomialFit(cubicFitToggle.getToggleState());
    updateSlopeReadout();
}

void MainComponent::applyPeakMarkersFromUi()
{
    spectralDisplay.setShowPeakMarkers(peakMarkersToggle.getToggleState());
}

void MainComponent::applyFitPeaksFromUi()
{
    spectralDisplay.setFitPeaksOnly(fitPeaksToggle.getToggleState());
    updateSlopeReadout();
}

void MainComponent::applyPeakFloorFromUi()
{
    spectralDisplay.setPeakThresholdDb(static_cast<float>(peakFloorSlider.getValue()));
    updatePeakFloorLabel();
    updateSlopeReadout();
}

void MainComponent::updatePeakFloorLabel()
{
    peakFloorValueLabel.setText(juce::String(static_cast<float>(peakFloorSlider.getValue()), 1) + " dB",
                                juce::dontSendNotification);
}

void MainComponent::applyAveragingFromUi()
{
    const bool enabled = averageToggle.getToggleState();
    spectralDisplay.setAveragingEnabled(enabled);
    clearAverageButton.setEnabled(enabled);
    updateSlopeReadout();
}

void MainComponent::applyWaterfallVisibilityFromUi()
{
    spectralDisplay.setShowWaterfall(waterfallToggle.getToggleState());
    resized();
    repaint();
}

void MainComponent::applyEnvelopeVisibilityFromUi()
{
    spectralDisplay.setShowEnvelope(envelopeToggle.getToggleState());
    resized();
    repaint();
}

void MainComponent::applyRegressionFromUi()
{
    spectralDisplay.setShowRegression(regressionToggle.getToggleState());
}

void MainComponent::applyFreqWarpFromUi()
{
    spectralDisplay.setFreqWarp(static_cast<float>(freqWarpSlider.getValue()));
    updateFreqWarpLabel();
}

void MainComponent::updateFreqWarpLabel()
{
    freqWarpValueLabel.setText(juce::String(static_cast<float>(freqWarpSlider.getValue()), 2) + juce::String::charToString('x'),
                               juce::dontSendNotification);
}

void MainComponent::updateManualTargetLabel()
{
    targetSlopeValueLabel.setText(juce::String(static_cast<float>(targetSlopeSlider.getValue()), 2) + " dB/oct",
                                  juce::dontSendNotification);
}

void MainComponent::updateSlopeRegionLabels()
{
    const auto formatHz = [](float hz) -> juce::String
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 2) + " kHz";
        return juce::String(hz, 0) + " Hz";
    };

    minSlopeFreqValueLabel.setText(formatHz(static_cast<float>(slopeRegionSlider.getMinValue())),
                                   juce::dontSendNotification);
    maxSlopeFreqValueLabel.setText(formatHz(static_cast<float>(slopeRegionSlider.getMaxValue())),
                                   juce::dontSendNotification);
}

void MainComponent::updateSlopeReadout()
{
    const auto live = spectralDisplay.getLiveSlopeFit();

    const juce::String superTwo = juce::String::charToString(static_cast<juce::juce_wchar>(0x00b2));
    const juce::String deltaSym = juce::String::charToString(static_cast<juce::juce_wchar>(0x0394));
    const juce::String checkSym = juce::String::charToString(static_cast<juce::juce_wchar>(0x2713));

    juce::String text;
    if (live.valid)
        text << "Live " << juce::String(live.slopeDbPerOct, 2) << " dB/oct"
             << "  (R" << superTwo << " " << juce::String(live.rSquared, 2) << ")";
    else
        text << "Live --";

    if (cubicFitToggle.getToggleState())
    {
        const auto livePoly = spectralDisplay.getLivePolyFit();
        if (livePoly.valid)
        {
            const float sLo = SpectralDisplayComponent::polyLocalSlopeDbPerOct(livePoly, livePoly.minHz);
            const float sHi = SpectralDisplayComponent::polyLocalSlopeDbPerOct(livePoly, livePoly.maxHz);
            const auto kHz = [](float hz) { return juce::String(hz / 1000.0f, 1) + "k"; };
            text << "  [cubic R" << superTwo << " " << juce::String(livePoly.rSquared, 2)
                 << ": " << kHz(livePoly.minHz) << " " << juce::String(sLo, 1)
                 << " -> " << kHz(livePoly.maxHz) << " " << juce::String(sHi, 1) << " dB/oct]";
        }
    }

    // Target precedence: manual numeric target overrides a captured/imported one.
    bool haveTarget = false;
    float targetSlope = 0.0f;
    juce::String targetSource;

    if (spectralDisplay.isManualTargetEnabled())
    {
        haveTarget = true;
        targetSlope = spectralDisplay.getManualTargetSlope();
        targetSource = "manual";
    }
    else if (spectralDisplay.hasReference())
    {
        const auto ref = spectralDisplay.getReferenceSlopeFit();
        if (ref.valid)
        {
            haveTarget = true;
            targetSlope = ref.slopeDbPerOct;
            targetSource = spectralDisplay.getReferenceName();
        }
    }

    if (haveTarget)
    {
        text << "   |   Target " << juce::String(targetSlope, 2) << " dB/oct";
        if (targetSource.isNotEmpty())
            text << " (" << targetSource << ")";
        if (live.valid)
        {
            const float delta = live.slopeDbPerOct - targetSlope;
            text << "   |   " << deltaSym << " " << (delta >= 0.0f ? "+" : "")
                 << juce::String(delta, 2) << " dB/oct";
            if (std::abs(delta) <= 0.25f)
                text << "  " << checkSym << " matched";
        }
    }

    slopeReadoutLabel.setText(text, juce::dontSendNotification);
}

void MainComponent::updateFrequencyRangeLabels()
{
    const auto formatHz = [](float hz) -> juce::String
    {
        if (hz >= 1000.0f)
            return juce::String(hz / 1000.0f, 2) + " kHz";
        return juce::String(hz, 0) + " Hz";
    };

    minFeatureFreqValueLabel.setText(formatHz(static_cast<float>(featureFreqRangeSlider.getMinValue())),
                                     juce::dontSendNotification);
    maxFeatureFreqValueLabel.setText(formatHz(static_cast<float>(featureFreqRangeSlider.getMaxValue())),
                                     juce::dontSendNotification);
}

void MainComponent::updateFlatnessPowerFloorLabel()
{
    flatnessPowerFloorValueLabel.setText(juce::String(static_cast<float>(flatnessPowerFloorSlider.getValue()), 1) + " dB",
                                         juce::dontSendNotification);
}

void MainComponent::openAudioSettings()
{
    if (audioSettingsWindow != nullptr)
    {
        if (audioSettingsWindow->isShowing())
        {
            audioSettingsWindow->toFront(true);
            audioSettingsButton.setState(juce::Button::buttonNormal);
            return;
        }

        audioSettingsWindow = nullptr;
    }

    auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager,
                                                            0,
                                                            256,
                                                            0,
                                                            256,
                                                            true,
                                                            true,
                                                            true,
                                                            false);
    selector->setSize(520, 460);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector);
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = juce::Colour(0xff1a1a1a);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    if (auto* topLevel = getTopLevelComponent())
        options.componentToCentreAround = topLevel;
    else
        options.componentToCentreAround = this;

    if (auto* window = options.launchAsync())
    {
        audioSettingsWindow = window;
        window->setAlwaysOnTop(true);
    }

    audioSettingsButton.setState(juce::Button::buttonNormal);
}

void MainComponent::setFeaturePanelVisible(bool visible)
{
    featurePanelVisible = visible;
    if (featuresButton.getToggleState() != visible)
        featuresButton.setToggleState(visible, juce::dontSendNotification);
    resized();
    repaint();
}

void MainComponent::setControlsVisible(bool visible)
{
    controlsVisible = visible;
    if (controlsButton.getToggleState() != visible)
        controlsButton.setToggleState(visible, juce::dontSendNotification);

    const std::initializer_list<juce::Component*> controls
    {
        &featureFreqRangeLabel, &featureFreqRangeSlider, &minFeatureFreqValueLabel, &maxFeatureFreqValueLabel,
        &flatnessPowerFloorLabel, &flatnessPowerFloorSlider, &flatnessPowerFloorValueLabel,
        &gainLabel, &gainSlider, &gainValueLabel,
        &freqWarpLabel, &freqWarpSlider, &freqWarpValueLabel,
        &slopeRegionLabel, &slopeRegionSlider, &minSlopeFreqValueLabel, &maxSlopeFreqValueLabel,
        &captureReferenceButton, &clearReferenceButton, &regressionToggle,
        &loadTargetButton, &manualTargetToggle, &targetSlopeSlider, &targetSlopeValueLabel,
        &cubicFitToggle, &fitPeaksToggle, &averageToggle, &clearAverageButton,
        &peakFloorLabel, &peakFloorSlider, &peakFloorValueLabel, &peakMarkersToggle,
        &waterfallToggle, &envelopeToggle
    };

    for (auto* c : controls)
        c->setVisible(visible);

    resized();
    repaint();
}

void MainComponent::toggleCapture()
{
    captureEnabled = !captureEnabled;
    captureButton.setButtonText(captureEnabled ? "Stop Capture" : "Start Capture");

    if (captureEnabled)
    {
        captureRows.clear();
        resetAutoscaleBounds();
        captureStartMs = juce::Time::getMillisecondCounterHiRes();
        exportCsvButton.setEnabled(false);
    }
    else
    {
        exportCsvButton.setEnabled(!captureRows.empty());
    }
}

void MainComponent::exportCaptureCsv()
{
    if (captureRows.empty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "Export CSV",
                                               "No captured data is available yet. Start capture first.");
        return;
    }

    exportChooser = std::make_unique<juce::FileChooser>("Export feature capture CSV",
                                                         juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                             .getChildFile("spex_feature_capture.csv"),
                                                         "*.csv");

    const int flags = juce::FileBrowserComponent::saveMode
                    | juce::FileBrowserComponent::canSelectFiles
                    | juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<MainComponent> safeThis(this);
    exportChooser->launchAsync(flags, [safeThis](const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;
        const auto target = chooser.getResult();
        if (target == juce::File())
        {
            safeThis->exportChooser.reset();
            return;
        }

        juce::StringArray lines;
        juce::String header = "time_seconds";
        for (int i = 0; i < featureCount; ++i)
            header << "," << csvHeaderName(i);
        lines.add(header);

        for (const auto& row : safeThis->captureRows)
        {
            juce::String line;
            line << juce::String(row.timeSeconds, 6);

            for (int i = 0; i < featureCount; ++i)
            {
                const float value = row.values[static_cast<size_t>(i)];
                if (std::isfinite(value))
                    line << "," << juce::String(value, 6);
                else
                    line << ",nan";
            }

            lines.add(line);
        }

        if (!target.replaceWithText(lines.joinIntoString("\n")))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "Export CSV",
                                                   "Failed to write CSV file.");
        }

        safeThis->exportChooser.reset();
    });
}

void MainComponent::updateFeatureState(const spex::SpectralFeatureSnapshot& snapshot)
{
    for (int i = 0; i < featureCount; ++i)
    {
        const size_t index = static_cast<size_t>(i);
        const float raw = snapshot.values[index];

        if (!std::isfinite(raw))
        {
            displayedValues[index] = raw;
            auto& history = featureHistory[index];
            history.push_back(raw);

            if (history.size() > static_cast<size_t>(featureHistoryLength))
                history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - featureHistoryLength));

            continue;
        }

        if (!hasDisplayedValues[index])
        {
            hasDisplayedValues[index] = true;
            filteredValues[index] = raw;
            displayedValues[index] = raw;
        }
        else
        {
            filteredValues[index] = kSmoothingAlpha[index] * filteredValues[index]
                                  + (1.0f - kSmoothingAlpha[index]) * raw;

            if (std::abs(filteredValues[index] - displayedValues[index]) >= kHysteresis[index])
                displayedValues[index] = filteredValues[index];
        }

        auto& history = featureHistory[index];
        history.push_back(displayedValues[index]);

        if (!hasAutoscaleBounds[index])
        {
            hasAutoscaleBounds[index] = true;
            runningMinValues[index] = displayedValues[index];
            runningMaxValues[index] = displayedValues[index];
        }
        else
        {
            runningMinValues[index] = std::min(runningMinValues[index], displayedValues[index]);
            runningMaxValues[index] = std::max(runningMaxValues[index], displayedValues[index]);
        }

        if (history.size() > static_cast<size_t>(featureHistoryLength))
            history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - featureHistoryLength));
    }
}

void MainComponent::resetAutoscaleBounds()
{
    hasAutoscaleBounds.fill(false);
    runningMinValues.fill(0.0f);
    runningMaxValues.fill(0.0f);
}

juce::String MainComponent::formatFeatureValue(int featureIndex, float value) const
{
    return formatFeatureValueText(featureIndex, value);
}

void MainComponent::paintFeaturePanel(juce::Graphics& g, juce::Rectangle<int> bounds) const
{
    auto panel = bounds.reduced(8);
    g.setColour(juce::Colour(0x14111827));
    g.fillRoundedRectangle(panel.toFloat(), 8.0f);

    g.setColour(juce::Colour(0x40ffffff));
    g.drawRoundedRectangle(panel.toFloat(), 8.0f, 1.0f);

    panel.reduce(10, 10);
    const int rowGap = 8;
    const int rowHeight = std::max(62, (panel.getHeight() - rowGap * (featureCount - 1)) / featureCount);

    for (int i = 0; i < featureCount; ++i)
    {
        auto row = panel.removeFromTop(rowHeight);
        if (i < featureCount - 1)
            panel.removeFromTop(rowGap);

        g.setColour(juce::Colour(0x1cffffff));
        g.fillRoundedRectangle(row.toFloat(), 6.0f);

        auto title = row.removeFromTop(20).reduced(8, 0);
        g.setColour(juce::Colour(0xffe5e7eb));
        g.setFont(13.0f);
        g.drawText(kFeatureNames[static_cast<size_t>(i)], title.removeFromLeft(220), juce::Justification::centredLeft, false);

        g.setColour(kFeatureColours[static_cast<size_t>(i)]);
        g.setFont(13.0f);
        g.drawText(formatFeatureValue(i, displayedValues[static_cast<size_t>(i)]),
                   title,
                   juce::Justification::centredRight,
                   false);

        drawTrace(g,
                  row.reduced(8, 5).toFloat(),
                  featureHistory[static_cast<size_t>(i)],
                  makeAutoscaledRange(i,
                                      runningMinValues[static_cast<size_t>(i)],
                                      runningMaxValues[static_cast<size_t>(i)],
                                      hasAutoscaleBounds[static_cast<size_t>(i)]),
                  kFeatureColours[static_cast<size_t>(i)]);
    }
}

void MainComponent::toggleScrollingPause()
{
    scrollingPaused = !scrollingPaused;
    spectralDisplay.setScrollingPaused(scrollingPaused);
    pauseButton.setButtonText(scrollingPaused ? "Resume" : "Pause");
}

void MainComponent::updateGainLabel()
{
    const float db = static_cast<float>(gainSlider.getValue());
    const juce::String text = (db >= 0.0f ? "+" : "") + juce::String(db, 1) + " dB";
    gainValueLabel.setText(text, juce::dontSendNotification);
}

void MainComponent::timerCallback()
{
    if (!spectralDisplay.update())
        return;

    if (!scrollingPaused)
    {
        const auto snapshot = spectralDisplay.getLatestFeatureSnapshot();
        updateFeatureState(snapshot);

        updateSlopeReadout();

        if (captureEnabled)
        {
            CaptureRow row;
            row.timeSeconds = (juce::Time::getMillisecondCounterHiRes() - captureStartMs) * 0.001;
            row.values = displayedValues;
            captureRows.push_back(row);
        }

        if (!captureEnabled)
            exportCsvButton.setEnabled(!captureRows.empty());

        repaint();
    }
}
