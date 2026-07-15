#pragma once

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
    void openAudioSettings();
    void timerCallback() override;

    juce::TextButton          audioSettingsButton { "Audio Settings" };
    SpectralDisplayComponent spectralDisplay;
    std::vector<float>       monoScratch;
    std::unique_ptr<juce::DialogWindow> audioSettingsWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
