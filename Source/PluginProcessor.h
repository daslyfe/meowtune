#pragma once

#include "TuneEngine.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

// Parameter IDs, in the order the page lists them.
namespace ParamID
{
    inline constexpr const char* amount = "amount";
    inline constexpr const char* glide = "glide";
    inline constexpr const char* pitch = "pitch";
    inline constexpr const char* formantCorrect = "formantCorrect";
    inline constexpr const char* formantShift = "formantShift";
    inline constexpr const char* unison = "unison";
    inline constexpr const char* detune = "detune";
    inline constexpr const char* spread = "spread";
    inline constexpr const char* mix = "mix";
    inline constexpr const char* output = "output";
    inline constexpr const char* midiMode = "midiMode";
    inline constexpr const char* key = "key";
    inline constexpr const char* scale = "scale";
    inline constexpr const char* range = "range";
    juce::String note (int pitchClass); // "note0" (C) .. "note11" (B)
}

// The SCALE parameter's choices, in order. CUSTOM (the last) is the keyboard's own notes, the
// noteN parameters; every other scale is its intervals from KEY.
namespace Scales
{
    struct Scale { const char* id; const char* name; uint16_t intervals; }; // bit i = i semitones above the key
    inline constexpr int kCount = 20;
    inline constexpr int kCustom = kCount - 1;
    extern const Scale list[kCount];
    uint16_t mask (int key, int scale); // pitch classes, bit 0 = C; 0 for CUSTOM
    int indexOf (const juce::String& id); // -1 when unknown
}

// VOICE RANGE choices (TuneEngine::kRangeHz).
namespace Ranges
{
    inline constexpr const char* names[TuneEngine::kRanges] = { "BASS", "TENOR", "ALTO", "SOPRANO" };
    inline constexpr int kDefault = 1;
}

// The colour settings are shared by every MeowTune instance, so they live in a file, not the session.
struct ColourSettings
{
    static juce::StringArray names() { return { "bg", "text", "accent", "muted", "border" }; }
    static juce::var load();                 // { bg: "#000000", ... } or an empty object
    static void save (const juce::var& colours); // a non-object clears them
    static juce::File file();
};

class MeowTuneProcessor : public juce::AudioProcessor
{
public:
    MeowTuneProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 1.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState state;
    TuneEngine engine;
    std::atomic<int> stateVersion { 0 }; // bumped when a session is loaded

private:
    TuneSettings readSettings() const;
    float rangeHz() const;
    void migrateOldScaleChoice (int oldKey, const juce::String& oldScale);

    struct Raw { std::atomic<float>* p = nullptr; float get() const { return p->load(); } };
    Raw amount, glide, pitch, formantCorrect, formantShift, unison, detune, spread, mix, output, midiMode, key, scale, range;
    std::array<Raw, 12> notes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeowTuneProcessor)
};
