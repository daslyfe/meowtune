// Loads the installed VST3 and AU like a DAW would and checks what comes out: tuned pitch at
// several sample rates and random block sizes, parameters set through the host, MIDI in (SNAP and
// CHORD), the reported latency, state through the host wrappers, and the editor opening.
//
// usage: host_test <plugin path>...

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <random>
#include <thread>

using namespace juce;

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

static double hzOf (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }
static double cents (double a, double b) { return 1200.0 * std::log2 (a / b); }

static double measurePitch (const float* x, int n, double sr)
{
    const int lo = (int) (sr / 1200), hi = (int) (sr / 60);
    std::vector<double> r ((size_t) hi + 2, 0.0);
    const int w = n - hi - 2;
    for (int lag = lo - 1; lag <= hi + 1; ++lag)
    {
        double a = 0, e1 = 0, e2 = 0;
        for (int i = 0; i < w; ++i) { a += x[i] * x[i + lag]; e1 += x[i] * x[i]; e2 += x[i + lag] * x[i + lag]; }
        r[(size_t) lag] = a / std::sqrt (e1 * e2 + 1e-20);
    }
    double best = -1;
    for (int lag = lo; lag <= hi; ++lag) best = std::max (best, r[(size_t) lag]);
    if (best < 0.5) return 0;
    for (int lag = lo; lag <= hi; ++lag)
        if (r[(size_t) lag] > 0.92 * best && r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1])
        {
            const double a = r[(size_t) lag - 1], b = r[(size_t) lag], c = r[(size_t) lag + 1];
            const double den = a - 2 * b + c;
            return sr / (lag + (std::abs (den) > 1e-12 ? 0.5 * (a - c) / den : 0));
        }
    return 0;
}

static double goertzel (const float* x, int n, double hz, double sr)
{
    const double w = 2 * MathConstants<double>::pi * hz / sr, c = 2 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos (2 * MathConstants<double>::pi * i / (n - 1));
        const double s0 = x[i] * h + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

static AudioProcessorParameter* findParam (AudioPluginInstance& p, const String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName (100).equalsIgnoreCase (name)) return prm;
    return nullptr;
}

static void setParam (AudioPluginInstance& p, const String& name, float normalised)
{
    if (auto* prm = findParam (p, name)) prm->setValueNotifyingHost (normalised);
    else check (false, "parameter " + name + " exists");
}

// Plays a sawtooth voice through the plugin in random block sizes; returns the left output.
// midiAt(sample) can add events. JUCE's AU host runs AUs at real-time pace only when asked, so AUs sleep.
static bool pace = false;
template <typename Midi>
static std::vector<float> play (AudioPluginInstance& p, double rate, double seconds, double hz, Midi&& midiAt)
{
    std::mt19937 rng (11);
    std::uniform_int_distribution<int> d (64, 512);
    const int total = (int) (seconds * rate);
    std::vector<float> out ((size_t) total);
    AudioBuffer<float> buf (2, 512);
    MidiBuffer midi;
    double phase = 0;
    for (int pos = 0; pos < total;)
    {
        const int n = std::min (d (rng), total - pos);
        for (int i = 0; i < n; ++i)
        {
            phase += hz / rate;
            if (phase >= 1) phase -= 1;
            float v = 0;
            for (int k = 1; k * hz < rate * 0.45 && k <= 30; ++k) v += (float) (std::sin (2 * MathConstants<double>::pi * k * phase) / k);
            buf.setSample (0, i, v * 0.25f);
            buf.setSample (1, i, v * 0.25f);
        }
        midi.clear();
        midiAt (pos, n, midi);
        if (pace) std::this_thread::sleep_for (std::chrono::microseconds ((int) (n / rate * 1e6)));
        AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, 0, n);
        p.processBlock (view, midi);
        std::copy (buf.getReadPointer (0), buf.getReadPointer (0) + n, out.begin() + pos);
        pos += n;
    }
    return out;
}
static std::vector<float> play (AudioPluginInstance& p, double rate, double seconds, double hz)
{
    return play (p, rate, seconds, hz, [] (int, int, MidiBuffer&) {});
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    ScopedJuceInitialiser_GUI init;

    AudioPluginFormatManager pluginFormats;
    pluginFormats.addFormat (new VST3PluginFormat());
    pluginFormats.addFormat (new AudioUnitPluginFormat());

    for (int a = 1; a < argc; ++a)
    {
        const String path (argv[a]);
        OwnedArray<PluginDescription> found;
        for (auto* fmt : pluginFormats.getFormats())
            if (fmt->fileMightContainThisPluginType (path))
                fmt->findAllTypesForFile (found, path);

        if (found.isEmpty()) { check (false, "no plugin found in " + path); continue; }
        const auto& desc = *found[0];
        const bool isAU = desc.pluginFormatName == "AudioUnit";
        pace = isAU;
        std::printf ("// %s (%s)\n", desc.name.toRawUTF8(), desc.pluginFormatName.toRawUTF8());

        for (double rate : { 48000.0, 44100.0, 96000.0 })
        {
            std::printf ("%g Hz\n", rate);
            String err;
            auto plugin = pluginFormats.createPluginInstance (desc, rate, 512, err);
            if (plugin == nullptr) { check (false, "instantiate: " + err); continue; }
            check (plugin->acceptsMidi(), "accepts MIDI");
            check (plugin->getTotalNumInputChannels() == 2 && plugin->getTotalNumOutputChannels() == 2, "stereo in and out");
            plugin->prepareToPlay (rate, 512);
            const int latency = plugin->getLatencySamples();
            check (latency > 0 && latency < rate * 0.04, "latency reported: " + String (latency) + " samples (" + String (1000.0 * latency / rate, 1) + " ms)");

            // Defaults: chromatic, amount 100%.
            auto out = play (*plugin, rate, 1.2, hzOf (57.37));
            double got = measurePitch (out.data() + (int) (0.9 * rate), (int) (0.06 * rate), rate);
            check (std::abs (cents (got, hzOf (57))) < 6, "A3+37c tuned to A3 (" + String (got, 2) + " Hz)");

            // Through host parameters: amount 0, pitch +12 (range -24..24 -> 0.75).
            setParam (*plugin, "Amount", 0.0f);
            setParam (*plugin, "Pitch", 0.75f);
            out = play (*plugin, rate, 1.2, 225.0);
            got = measurePitch (out.data() + (int) (0.9 * rate), (int) (0.06 * rate), rate);
            check (std::abs (cents (got, 450.0)) < 6, "host params: AMOUNT 0, PITCH +12 gives 450 Hz (" + String (got, 2) + ")");
            setParam (*plugin, "Amount", 1.0f);
            setParam (*plugin, "Pitch", 0.5f);

            // KEY and SCALE through host parameters (what a mapped MIDI CC moves): D major.
            // F4+40c is between F and F#; D major has F#, so it goes up.
            setParam (*plugin, "Scale", 1.0f / 19.0f);
            setParam (*plugin, "Key", 2.0f / 11.0f);
            out = play (*plugin, rate, 1.2, hzOf (65.4));
            got = measurePitch (out.data() + (int) (0.9 * rate), (int) (0.06 * rate), rate);
            check (std::abs (cents (got, hzOf (66))) < 6, "host params: KEY D, SCALE MAJOR tunes F4+40c to F#4 (" + String (got, 2) + " Hz)");
            setParam (*plugin, "Key", 0.0f);
            out = play (*plugin, rate, 1.2, hzOf (65.4));
            got = measurePitch (out.data() + (int) (0.9 * rate), (int) (0.06 * rate), rate);
            check (std::abs (cents (got, hzOf (65))) < 6, "KEY C: to F4 (" + String (got, 2) + " Hz)");

            // VOICE RANGE: SOPRANO cuts the latency, and the host hears about it.
            {
                setParam (*plugin, "Voice Range", 1.0f);
                play (*plugin, rate, 0.1, 300.0);
                MessageManager::getInstance()->runDispatchLoopUntil (300);
                const int soprano = plugin->getLatencySamples();
                setParam (*plugin, "Voice Range", 1.0f / 3.0f);
                play (*plugin, rate, 0.1, 300.0);
                MessageManager::getInstance()->runDispatchLoopUntil (300);
                const int tenor = plugin->getLatencySamples();
                check (soprano > 0 && soprano < rate * 0.02 && tenor == latency,
                       "VOICE RANGE: SOPRANO reports " + String (1000.0 * soprano / rate, 1) + " ms, TENOR back to " + String (1000.0 * tenor / rate, 1) + " ms");
                const auto* rp = findParam (*plugin, "Voice Range");
                check (rp != nullptr && ! rp->isAutomatable(), "VOICE RANGE is not automatable");
                check (findParam (*plugin, "Key")->isAutomatable() && findParam (*plugin, "Scale")->isAutomatable(), "KEY and SCALE are automatable");
            }

            // Notes as parameters (SCALE CUSTOM): only C, then MIDI D held in SNAP mode overrides it.
            setParam (*plugin, "Scale", 1.0f);
            for (int i = 0; i < 12; ++i)
                setParam (*plugin, String ("Note ") + StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }[i], i == 0 ? 1.0f : 0.0f);
            out = play (*plugin, rate, 1.2, 290.0, [] (int pos, int, MidiBuffer& m) { if (pos == 0) m.addEvent (MidiMessage::noteOn (1, 62, (uint8) 100), 0); });
            got = measurePitch (out.data() + (int) (0.9 * rate), (int) (0.06 * rate), rate);
            check (std::abs (cents (got, hzOf (62))) < 6, "MIDI D in SNAP mode overrides a C-only scale (" + String (got, 2) + " Hz)");
            out = play (*plugin, rate, 1.0, 290.0, [] (int pos, int, MidiBuffer& m) { if (pos == 0) m.addEvent (MidiMessage::noteOff (1, 62), 0); });
            got = measurePitch (out.data() + (int) (0.7 * rate), (int) (0.06 * rate), rate);
            check (std::abs (cents (got, hzOf (60))) < 6, "note off: back to C (" + String (got, 2) + " Hz)");

            // CHORD: three notes from MIDI, the note-ons landing mid-block.
            setParam (*plugin, "MIDI Mode", 1.0f);
            out = play (*plugin, rate, 1.2, 200.0, [] (int pos, int n, MidiBuffer& m)
            {
                if (pos == 0) for (int note : { 57, 61, 64 }) m.addEvent (MidiMessage::noteOn (1, note, (uint8) 110), std::min (n - 1, 37));
            });
            {
                const int at = (int) (0.8 * rate), len = (int) (0.2 * rate);
                const double a3 = goertzel (out.data() + at, len, hzOf (57), rate), cs = goertzel (out.data() + at, len, hzOf (61), rate);
                const double e4 = goertzel (out.data() + at, len, hzOf (64), rate), src = goertzel (out.data() + at, len, 200.0, rate);
                check (a3 > 8 * src && cs > 8 * src && e4 > 8 * src, "CHORD: A3, C#4 and E4 from one voice (" + String (a3, 4) + " " + String (cs, 4) + " " + String (e4, 4) + " vs " + String (src, 5) + ")");
            }
            play (*plugin, rate, 0.2, 200.0, [] (int pos, int, MidiBuffer& m) { if (pos == 0) m.addEvent (MidiMessage::allNotesOff (1), 0); });

            // State through the host wrapper: change things, save, load into a fresh instance.
            setParam (*plugin, "Detune", 0.37f);
            setParam (*plugin, "Unison", 3.0f / 7.0f);
            MemoryBlock saved;
            plugin->getStateInformation (saved);
            auto fresh = pluginFormats.createPluginInstance (desc, rate, 512, err);
            fresh->setStateInformation (saved.getData(), (int) saved.getSize());
            const auto near = [] (AudioProcessorParameter* x, float v) { return x != nullptr && std::abs (x->getValue() - v) < 0.01f; };
            check (near (findParam (*fresh, "Detune"), 0.37f) && near (findParam (*fresh, "Scale"), 1.0f) && near (findParam (*fresh, "Unison"), 3.0f / 7.0f)
                       && near (findParam (*fresh, "MIDI Mode"), 1.0f) && near (findParam (*fresh, "Note C#"), 0.0f) && near (findParam (*fresh, "Note C"), 1.0f),
                   "state round trip keeps detune, unison, MIDI mode, SCALE and the notes");

            if (rate == 48000.0)
            {
                std::unique_ptr<AudioProcessorEditor> ed (fresh->createEditorIfNeeded());
                check (ed != nullptr && ed->getWidth() >= 760, "the editor opens (" + String (ed ? ed->getWidth() : 0) + "x" + String (ed ? ed->getHeight() : 0) + ")");
                MessageManager::getInstance()->runDispatchLoopUntil (800);
                ed.reset();
            }
            fresh->releaseResources();
            plugin->releaseResources();
        }
    }

    std::printf (failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
