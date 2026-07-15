#include "MainComponent.h"

MainComponent::MainComponent()
{
    addAndMakeVisible(spectralDisplay);

    setSize(1000, 640);

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
}

void MainComponent::resized()
{
    spectralDisplay.setBounds(getLocalBounds());
}

void MainComponent::timerCallback()
{
    spectralDisplay.update();
}
