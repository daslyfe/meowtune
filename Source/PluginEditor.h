#pragma once

#include "PluginProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

// The whole UI is ui/index.html (Meow Terminal stylesheet) served from BinaryData.
// Native functions the page calls:
//   getInit()                 -> { params: [...], scales: [{ id, name, steps }], ranges: [{ name, hz, latency }],
//                                  colours, version, rate, latency }
//   setParam(id, value)       a real (not normalised) value
//   gesture(id, begin)        wraps a drag so host automation records one gesture
//   setColours(obj | null)    the colour settings, shared by every instance; null resets them
// Events sent to the page: "tick" (30 Hz: pitch, held notes, meters, pitch history, changed params), "reload".
class MeowTuneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit MeowTuneEditor (MeowTuneProcessor&);
    ~MeowTuneEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Runs JavaScript in the page (used by tools/proc_test).
    void evaluate (const juce::String& js, std::function<void (juce::var)> done);

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::WebBrowserComponent::Options makeOptions();
    juce::var makeInit();
    juce::RangedAudioParameter* param (const juce::var& id) const;

    MeowTuneProcessor& proc;
    juce::WebBrowserComponent web;

    int lastStateVersion = 0;
    std::map<juce::String, float> lastSent;
    std::vector<float> histBuf;
    juce::String lastHeld;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeowTuneEditor)
};
