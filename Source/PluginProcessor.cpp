#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::String ParamID::note (int pitchClass) { return "note" + juce::String (pitchClass); }

namespace
{
    constexpr uint16_t iv (std::initializer_list<int> steps)
    {
        uint16_t m = 0;
        for (int s : steps) m = (uint16_t) (m | (1u << s));
        return m;
    }
}

// The ids are what older sessions saved; keep them.
const Scales::Scale Scales::list[Scales::kCount] = {
    { "chromatic", "CHROMATIC", iv ({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }) },
    { "major", "MAJOR", iv ({ 0, 2, 4, 5, 7, 9, 11 }) },
    { "minor", "MINOR", iv ({ 0, 2, 3, 5, 7, 8, 10 }) },
    { "harmonic", "HARMONIC MINOR", iv ({ 0, 2, 3, 5, 7, 8, 11 }) },
    { "melodic", "MELODIC MINOR", iv ({ 0, 2, 3, 5, 7, 9, 11 }) },
    { "dorian", "DORIAN", iv ({ 0, 2, 3, 5, 7, 9, 10 }) },
    { "phrygian", "PHRYGIAN", iv ({ 0, 1, 3, 5, 7, 8, 10 }) },
    { "lydian", "LYDIAN", iv ({ 0, 2, 4, 6, 7, 9, 11 }) },
    { "mixolydian", "MIXOLYDIAN", iv ({ 0, 2, 4, 5, 7, 9, 10 }) },
    { "locrian", "LOCRIAN", iv ({ 0, 1, 3, 5, 6, 8, 10 }) },
    { "majpent", "MAJOR PENTATONIC", iv ({ 0, 2, 4, 7, 9 }) },
    { "minpent", "MINOR PENTATONIC", iv ({ 0, 3, 5, 7, 10 }) },
    { "blues", "BLUES", iv ({ 0, 3, 5, 6, 7, 10 }) },
    { "phrygdom", "PHRYGIAN DOMINANT", iv ({ 0, 1, 4, 5, 7, 8, 10 }) },
    { "hungarian", "HUNGARIAN MINOR", iv ({ 0, 2, 3, 6, 7, 8, 11 }) },
    { "wholetone", "WHOLE TONE", iv ({ 0, 2, 4, 6, 8, 10 }) },
    { "diminished", "DIMINISHED", iv ({ 0, 2, 3, 5, 6, 8, 9, 11 }) },
    { "hirajoshi", "HIRAJOSHI", iv ({ 0, 2, 3, 7, 8 }) },
    { "insen", "IN SEN", iv ({ 0, 1, 5, 7, 10 }) },
    { "custom", "CUSTOM", 0 },
};

uint16_t Scales::mask (int key, int scale)
{
    if (scale < 0 || scale >= kCustom) return 0;
    key = ((key % 12) + 12) % 12;
    const uint32_t m = (uint32_t) list[scale].intervals << key;
    return (uint16_t) ((m | (m >> 12)) & 0x0fff);
}

int Scales::indexOf (const juce::String& id)
{
    for (int i = 0; i < kCount; ++i)
        if (id == list[i].id) return i;
    return -1;
}

namespace
{
    const char* const kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        auto label = [] (const char* l, int decimals)
        {
            return AudioParameterFloatAttributes().withLabel (l).withStringFromValueFunction (
                [decimals] (float v, int) { return String (v, decimals); });
        };
        auto pid = [] (const char* id) { return ParameterID { id, 1 }; };

        AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::amount), "Amount", NormalisableRange<float> (0, 100, 1), 100.0f, label ("%", 0)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::glide), "Glide", NormalisableRange<float> (0, 500, 1, 0.5f), 20.0f, label ("ms", 0)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::pitch), "Pitch", NormalisableRange<float> (-24, 24, 0.01f), 0.0f, label ("st", 2)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::formantCorrect), "Formant Correct", NormalisableRange<float> (0, 100, 1), 100.0f, label ("%", 0)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::formantShift), "Formant Shift", NormalisableRange<float> (-12, 12, 0.01f), 0.0f, label ("st", 2)));
        layout.add (std::make_unique<AudioParameterInt> (pid (ParamID::unison), "Unison", 1, TuneEngine::kMaxUnison, 1));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::detune), "Detune", NormalisableRange<float> (0, 100, 0.1f), 15.0f, label ("ct", 1)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::spread), "Spread", NormalisableRange<float> (0, 100, 1), 50.0f, label ("%", 0)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::mix), "Mix", NormalisableRange<float> (0, 100, 1), 100.0f, label ("%", 0)));
        layout.add (std::make_unique<AudioParameterFloat> (pid (ParamID::output), "Output", NormalisableRange<float> (-24, 24, 0.1f), 0.0f, label ("dB", 1)));
        layout.add (std::make_unique<AudioParameterChoice> (pid (ParamID::midiMode), "MIDI Mode", StringArray { "Snap", "Chord" }, 0));

        // KEY and SCALE are host parameters so they can be automated and MIDI-mapped.
        // Host-facing names in title case ("HARMONIC MINOR" -> "Harmonic Minor").
        auto title = [] (const char* upper)
        {
            StringArray words;
            words.addTokens (String (upper).toLowerCase(), " ", "");
            for (auto& w : words) w = w.substring (0, 1).toUpperCase() + w.substring (1);
            return words.joinIntoString (" ");
        };
        StringArray keys, scales, ranges;
        for (auto* n : kNoteNames) keys.add (n);
        for (const auto& s : Scales::list) scales.add (title (s.name));
        for (int i = 0; i < TuneEngine::kRanges; ++i)
            ranges.add (title (Ranges::names[i]) + " (" + String (roundToInt (TuneEngine::kRangeHz[(size_t) i])) + " Hz+)");
        layout.add (std::make_unique<AudioParameterChoice> (pid (ParamID::key), "Key", keys, 0));
        layout.add (std::make_unique<AudioParameterChoice> (pid (ParamID::scale), "Scale", scales, 0));
        // Not automatable: changing it changes the latency and restarts the engine.
        layout.add (std::make_unique<AudioParameterChoice> (pid (ParamID::range), "Voice Range", ranges, Ranges::kDefault,
                                                            AudioParameterChoiceAttributes().withAutomatable (false)));
        for (int i = 0; i < 12; ++i)
            layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamID::note (i), 1 }, String ("Note ") + kNoteNames[i], true));
        return layout;
    }
}

// ---- colour settings ---------------------------------------------------------------------------

juce::File ColourSettings::file()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support/MeowTune/colours.json");
}

juce::var ColourSettings::load()
{
    auto* out = new juce::DynamicObject();
    const auto parsed = juce::JSON::parse (file());
    for (const auto& n : names())
    {
        const auto v = parsed[juce::Identifier (n)].toString();
        if (v.length() == 7 && v.startsWithChar ('#') && v.substring (1).containsOnly ("0123456789abcdefABCDEF"))
            out->setProperty (n, v.toLowerCase());
    }
    return juce::var (out);
}

void ColourSettings::save (const juce::var& colours)
{
    if (! colours.isObject())
    {
        file().deleteFile();
        return;
    }
    auto* out = new juce::DynamicObject();
    for (const auto& n : names())
    {
        const auto v = colours[juce::Identifier (n)].toString();
        if (v.length() == 7 && v.startsWithChar ('#') && v.substring (1).containsOnly ("0123456789abcdefABCDEF"))
            out->setProperty (n, v.toLowerCase());
    }
    file().getParentDirectory().createDirectory();
    file().replaceWithText (juce::JSON::toString (juce::var (out)));
}

// ---- processor ---------------------------------------------------------------------------------

MeowTuneProcessor::MeowTuneProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "MeowTune", createLayout())
{
    auto raw = [this] (const juce::String& id) { return Raw { state.getRawParameterValue (id) }; };
    amount = raw (ParamID::amount);
    glide = raw (ParamID::glide);
    pitch = raw (ParamID::pitch);
    formantCorrect = raw (ParamID::formantCorrect);
    formantShift = raw (ParamID::formantShift);
    unison = raw (ParamID::unison);
    detune = raw (ParamID::detune);
    spread = raw (ParamID::spread);
    mix = raw (ParamID::mix);
    output = raw (ParamID::output);
    midiMode = raw (ParamID::midiMode);
    key = raw (ParamID::key);
    scale = raw (ParamID::scale);
    range = raw (ParamID::range);
    for (int i = 0; i < 12; ++i)
        notes[(size_t) i] = raw (ParamID::note (i));

    // Report a latency before the first prepare, so hosts that read it early get a sane value.
    engine.prepare (48000.0, 512, rangeHz());
    setLatencySamples (engine.getLatency());
}

bool MeowTuneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    // Mono in, stereo out is fine too: the unison spread makes stereo from a mono voice.
    return in == out || (in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo());
}

void MeowTuneProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, juce::jmax (1, samplesPerBlock), rangeHz());
    setLatencySamples (engine.getLatency());
}

void MeowTuneProcessor::reset()
{
    engine.reset();
}

float MeowTuneProcessor::rangeHz() const
{
    return TuneEngine::kRangeHz[(size_t) juce::jlimit (0, TuneEngine::kRanges - 1, juce::roundToInt (range.get()))];
}

TuneSettings MeowTuneProcessor::readSettings() const
{
    TuneSettings s;
    s.amount = amount.get() * 0.01f;
    s.glideMs = glide.get();
    s.inputPitch = pitch.get();
    s.formantCorrect = formantCorrect.get() * 0.01f;
    s.formantShift = formantShift.get();
    s.unison = juce::roundToInt (unison.get());
    s.detuneCents = detune.get();
    s.spread = spread.get() * 0.01f;
    s.mix = mix.get() * 0.01f;
    s.outputGain = juce::Decibels::decibelsToGain (output.get(), -100.0f);
    s.midiMode = juce::roundToInt (midiMode.get());
    const int sc = juce::jlimit (0, Scales::kCustom, juce::roundToInt (scale.get()));
    if (sc != Scales::kCustom)
        s.mask = Scales::mask (juce::roundToInt (key.get()), sc);
    else
    {
        s.mask = 0;
        for (int i = 0; i < 12; ++i)
            if (notes[(size_t) i].get() >= 0.5f) s.mask |= (uint16_t) (1u << i);
    }
    return s;
}

void MeowTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // VOICE RANGE changed: new floor, new latency (the wrappers tell the host asynchronously).
    if (! juce::exactlyEqual (rangeHz(), engine.getMinPitch()))
    {
        engine.setMinPitch (rangeHz());
        setLatencySamples (engine.getLatency());
    }

    const auto s = readSettings();
    const int numIn = juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels());
    const int numOut = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());
    const int n = buffer.getNumSamples();

    // Mono in, stereo out: the engine reads channel 0 only.
    const int engineIn = juce::jmax (1, numIn);

    // Run the audio between MIDI events so notes land on their sample.
    int pos = 0;
    auto runTo = [&] (int end)
    {
        if (end <= pos) return;
        const float* in[2];
        float* out[2];
        for (int c = 0; c < juce::jmin (2, engineIn); ++c) in[c] = buffer.getReadPointer (c, pos);
        for (int c = 0; c < juce::jmin (2, numOut); ++c) out[c] = buffer.getWritePointer (c, pos);
        engine.process (in, juce::jmin (2, engineIn), out, juce::jmin (2, numOut), end - pos, s);
        pos = end;
    };

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        runTo (juce::jlimit (0, n, meta.samplePosition));
        if (m.isNoteOn())
            engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())
            engine.noteOff (m.getNoteNumber());
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            engine.allNotesOff();
    }
    runTo (n);
}

void MeowTuneProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void MeowTuneProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (state.state.getType()))
        {
            const auto loaded = juce::ValueTree::fromXml (*xml);
            // Decide before loading: replaceState shares the tree and adds the missing parameters to it.
            const bool old = ! loaded.getChildWithProperty ("id", ParamID::scale).isValid() && loaded.hasProperty ("scale");
            const int oldKey = (int) loaded.getProperty ("key", 0);
            const auto oldScale = loaded.getProperty ("scale").toString();
            state.replaceState (loaded);
            if (old) migrateOldScaleChoice (oldKey, oldScale);
            ++stateVersion;
        }
}

// Sessions saved before KEY and SCALE were parameters kept them as properties ("key", "scale" id),
// with the notes as the truth. Keep the notes: use the named scale only if it still matches them.
void MeowTuneProcessor::migrateOldScaleChoice (int oldKey, const juce::String& oldScale)
{
    uint16_t notesMask = 0;
    for (int i = 0; i < 12; ++i)
        if (notes[(size_t) i].get() >= 0.5f) notesMask |= (uint16_t) (1u << i);
    const int k = juce::jlimit (0, 11, oldKey);
    const int named = Scales::indexOf (oldScale);
    const int sc = named >= 0 && named != Scales::kCustom && Scales::mask (k, named) == notesMask ? named : Scales::kCustom;
    auto set = [this] (const char* id, int index)
    {
        auto* p = state.getParameter (id);
        p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
    };
    set (ParamID::key, k);
    set (ParamID::scale, sc);
    state.state.removeProperty ("key", nullptr);
    state.state.removeProperty ("scale", nullptr);
}

juce::AudioProcessorEditor* MeowTuneProcessor::createEditor()
{
    return new MeowTuneEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MeowTuneProcessor();
}
