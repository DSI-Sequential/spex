#pragma once

#include <array>
#include <memory>
#include <vector>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "SpectralDisplayComponent.h"

class MainComponent final : public juce::AudioAppComponent,
                             private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    static constexpr int featureCount = SpectralDisplayComponent::numFeatures;
    static constexpr int featureHistoryLength = 420;

    struct CaptureRow
    {
        double timeSeconds { 0.0 };
        std::array<float, featureCount> values {};
    };

    void openAudioSettings();
    void openFeatureStackWindow();
    void toggleCapture();
    void exportCaptureCsv();
    void updateFeatureState(const SpectralDisplayComponent::FeatureSnapshot& snapshot);
    void resetAutoscaleBounds();
    juce::String formatFeatureValue(int featureIndex, float value) const;
    void paintFeaturePanel(juce::Graphics& g, juce::Rectangle<int> bounds) const;
    void timerCallback() override;

    juce::TextButton          audioSettingsButton { "Audio Settings" };
    juce::TextButton          featureStackButton  { "Feature Stack" };
    juce::TextButton          captureButton       { "Start Capture" };
    juce::TextButton          exportCsvButton     { "Export CSV" };

    SpectralDisplayComponent spectralDisplay;
    std::vector<float>       monoScratch;

    juce::Rectangle<int> featurePanelBounds;

    std::array<std::vector<float>, featureCount> featureHistory;
    std::array<float, featureCount> filteredValues {};
    std::array<float, featureCount> displayedValues {};
    std::array<bool, featureCount> hasDisplayedValues {};
    std::array<float, featureCount> runningMinValues {};
    std::array<float, featureCount> runningMaxValues {};
    std::array<bool, featureCount> hasAutoscaleBounds {};

    bool captureEnabled { false };
    double captureStartMs { 0.0 };
    std::vector<CaptureRow> captureRows;

    std::unique_ptr<juce::DialogWindow> audioSettingsWindow;
    std::unique_ptr<juce::DocumentWindow> featureStackWindow;
    juce::Component* featureStackContent { nullptr };
    std::unique_ptr<juce::FileChooser> exportChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
