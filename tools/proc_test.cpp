// Runs MeowTuneProcessor directly (built from the plugin's sources) with its editor in a real
// WKWebView, feeding a voice through processBlock in real time. Held native clicks and drags (mouse
// down, a pause, mouse up) go to the keyboard, the pickers, a slider, the MIDI choice and the tabs
// while ticks stream in, because WebKit drops clicks that headless Chrome doesn't.
#include "PluginEditor.h"

#include <juce_gui_extra/juce_gui_extra.h>

using namespace juce;

void nativeClick (Component& editor, Point<int> pos, int holdMs, Point<int> dragTo);

static int failures = 0;
static void check (bool ok, const String& what)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (! ok) ++failures;
}

static constexpr double kRate = 48000;
static constexpr int kBlock = 512;

static void pump (int ms) { MessageManager::getInstance()->runDispatchLoopUntil (ms); }

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    ScopedJuceInitialiser_GUI init;

    // The test changes colours; keep the user's own settings file and put it back afterwards.
    const auto colourFile = ColourSettings::file();
    const bool hadColours = colourFile.existsAsFile();
    const auto savedColours = hadColours ? colourFile.loadFileAsString() : String();

    auto p = std::make_unique<MeowTuneProcessor>();
    p->prepareToPlay (kRate, kBlock);
    double phase = 0, hz = 225.0;
    MidiBuffer pendingMidi;

    // Plays the voice in real time for `ms`, pumping the message loop between blocks.
    auto run = [&] (int ms)
    {
        AudioBuffer<float> buf (2, kBlock);
        const auto until = Time::getMillisecondCounterHiRes() + ms;
        while (Time::getMillisecondCounterHiRes() < until)
        {
            for (int i = 0; i < kBlock; ++i)
            {
                phase += hz / kRate;
                if (phase >= 1) phase -= 1;
                float v = 0;
                for (int k = 1; k <= 20; ++k) v += (float) (std::sin (2 * MathConstants<double>::pi * k * phase) / k);
                buf.setSample (0, i, v * 0.25f);
                buf.setSample (1, i, v * 0.25f);
            }
            MidiBuffer midi;
            midi.swapWith (pendingMidi);
            p->processBlock (buf, midi);
            pump ((int) (kBlock / kRate * 1000.0));
        }
    };
    auto value = [&] (const char* id) { auto* x = p->state.getParameter (id); return x->convertFrom0to1 (x->getValue()); };
    // The notes it tunes to: the scale from KEY, or the note parameters in CUSTOM.
    auto maskString = [&]
    {
        const int sc = roundToInt (value (ParamID::scale));
        const uint16_t m = Scales::mask (roundToInt (value (ParamID::key)), sc);
        String s;
        for (int i = 0; i < 12; ++i)
            s << ((sc == Scales::kCustom ? value (ParamID::note (i).toRawUTF8()) >= 0.5f : ((m >> i) & 1) != 0) ? "1" : "0");
        return s;
    };

    std::printf ("editor\n");
    std::unique_ptr<AudioProcessorEditor> ed (p->createEditor());
    auto* me = dynamic_cast<MeowTuneEditor*> (ed.get());
    check (me != nullptr && ed->getWidth() == 1040, "editor opens at 1040 wide");
    ed->setTopLeftPosition (60, 60);
    ed->addToDesktop (ComponentPeer::windowHasTitleBar);
    ed->setVisible (true);
    run (2500);

    auto ask = [&] (const String& js)
    {
        var out;
        bool got = false;
        me->evaluate (js, [&] (var v) { out = v; got = true; });
        for (int i = 0; i < 200 && ! got; ++i) pump (10);
        return out;
    };
    auto text = [&] (const String& sel) { return ask ("document.querySelector('" + sel + "').textContent").toString(); };
    auto rect = [&] (const String& js)
    {
        const auto r = JSON::parse (ask ("JSON.stringify((() => { const b = " + js + ".getBoundingClientRect(); return [b.x, b.y, b.width, b.height]; })())").toString());
        return Rectangle<int> ((int) (double) r[0], (int) (double) r[1], (int) (double) r[2], (int) (double) r[3]);
    };
    auto human = [&] (const String& sel)
    {
        const auto c = rect ("document.querySelector('" + sel + "')").getCentre();
        nativeClick (*ed, c, 140, c);
        run (150);
    };
    auto keyPoint = [&] (int note)
    {
        const auto k = rect ("document.querySelector('#keys')");
        const auto r = JSON::parse (ask ("JSON.stringify((() => { const k = window.__meowtuneKeys().find((r) => r.n === " + String (note) + "); return [k.x + k.w / 2, k.y + (k.black ? k.h * 0.5 : k.h * 0.85)]; })())").toString());
        return Point<int> (k.getX() + (int) (double) r[0], k.getY() + (int) (double) r[1]);
    };

    check ((bool) ask ("document.fonts.check(\"15px '3270'\")"), "3270 font loads in WebKit");
    check ((int) ask ("document.querySelector('#graph').getBoundingClientRect().height") > 120, "the pitch graph is laid out");
    check (text ("#r-in").startsWith ("IN:A3+3"), "the input pitch shows while audio plays (" + text ("#r-in") + ")");
    check (text ("#r-out") == "OUT:A3", "the target note shows (" + text ("#r-out") + ")");
    check (text ("#r-hz").startsWith ("HZ:22"), "and the frequency (" + text ("#r-hz") + ")");
    check (text ("#status").startsWith ("> TUNING"), "status says tuning (" + text ("#status") + ")");

    std::printf ("held clicks while audio runs\n");
    for (int round = 0; round < 3; ++round)
    {
        const auto c = keyPoint (61); // C#4
        nativeClick (*ed, c, 140, c);
        run (150);
        const bool off = value ("note1") < 0.5f;
        nativeClick (*ed, c, 140, c);
        run (150);
        check (off && value ("note1") >= 0.5f, "held clicks on C# turn it off and on again (round " + String (round + 1) + ")");
    }
    check (roundToInt (value (ParamID::scale)) == Scales::kCustom, "editing the keyboard switches SCALE to CUSTOM");
    {
        const auto from = keyPoint (62), to = keyPoint (65);
        nativeClick (*ed, from, 300, Point<int> (to.x, from.y));
        run (200);
        check (maskString() == "110100111111", "dragging across D..F paints them off (" + maskString() + ")");
    }
    human ("#all-notes");
    check (maskString() == "111111111111", ">>ALL (" + maskString() + ")");
    human ("#scale-next");
    check (maskString() == "101011010101", "SCALE [>]: C major (" + maskString() + ")");
    human ("#key-next");
    check (maskString() == "110101101010", "KEY [>]: C# major (" + maskString() + ")");
    check (roundToInt (value (ParamID::key)) == 1 && roundToInt (value (ParamID::scale)) == 1, "KEY and SCALE are the parameters");
    run (600);
    // A3 is not in C# major: 225 Hz goes to G#3 or A#3.
    check (text ("#r-out") == "OUT:G#3" || text ("#r-out") == "OUT:A#3", "the voice retunes to C# major (" + text ("#r-out") + ")");
    human ("#key-prev");

    std::printf ("KEY and SCALE from the host (automation or a mapped MIDI CC)\n");
    {
        auto setChoice = [&] (const char* id, int index) { auto* x = p->state.getParameter (id); x->setValueNotifyingHost (x->convertTo0to1 ((float) index)); };
        setChoice (ParamID::key, 9);
        setChoice (ParamID::scale, 11); // A minor pentatonic: A C D E G
        run (700);
        check (ask ("document.querySelector('#key').value").toString() == "9"
                   && ask ("document.querySelector('#scale').selectedOptions[0].text").toString() == "MINOR PENTATONIC",
               "the pickers follow (" + ask ("document.querySelector('#scale').selectedOptions[0].text").toString() + ")");
        check (text ("#r-notes") == "NOTES:5", "the note count follows (" + text ("#r-notes") + ")");
        // 225 Hz is A3+37c; A is in the scale.
        check (text ("#r-out") == "OUT:A3", "the voice tunes to the new scale (" + text ("#r-out") + ")");
        setChoice (ParamID::key, 1); // C# minor pentatonic: C# E F# G# B; A3+37c -> G#3 or B3
        run (700);
        check (text ("#r-out") == "OUT:G#3" || text ("#r-out") == "OUT:B3", "KEY alone moves it (" + text ("#r-out") + ")");
        setChoice (ParamID::key, 0);
    }
    human ("#all-notes");
    check (roundToInt (value (ParamID::scale)) == 0, ">>ALL is CHROMATIC");

    std::printf ("voice range\n");
    check (p->getLatencySamples() == 1690 && text ("#r-latency") == "35.2 MS", "TENOR: " + String (p->getLatencySamples()) + " samples, page says " + text ("#r-latency"));
    human ("#range-next");
    run (300);
    check (roundToInt (value (ParamID::range)) == 2 && p->getLatencySamples() == 1130, "held click on RANGE [>]: ALTO, latency reported as " + String (p->getLatencySamples()));
    check (text ("#r-latency") == "23.5 MS" && text ("#i-latency") == "1130 SAMPLES (23.5 MS)", "the page shows it (" + text ("#r-latency") + ")");
    check (text ("#r-out") == "OUT:A3", "still tuning after the switch (" + text ("#r-out") + ")");
    human ("#range-prev");
    run (300);
    check (p->getLatencySamples() == 1690, "back to TENOR");

    {
        const auto r = rect ("document.querySelector('[data-param=amount] input[type=range]')");
        const Point<int> from (r.getRight() - 4, r.getCentreY()), to (r.getX() + r.getWidth() * 4 / 10, r.getCentreY());
        nativeClick (*ed, from, 300, to);
        run (200);
        const float amt = value ("amount");
        check (amt > 25 && amt < 60, "dragging AMOUNT moves the parameter (" + String (amt) + ")");
        check (ask ("document.querySelector('[data-param=amount] input[type=number]').value").toString() == String (roundToInt (amt)), "the number box follows");
    }

    human ("#midi-mode button[data-value=\"1\"]");
    check (roundToInt (value ("midiMode")) == 1, "held click on CHORD");
    pendingMidi.addEvent (MidiMessage::noteOn (1, 60, (uint8) 100), 10);
    pendingMidi.addEvent (MidiMessage::noteOn (1, 64, (uint8) 100), 10);
    run (300);
    check (text ("#r-midi") == "MIDI:2 NOTES", "held MIDI notes show (" + text ("#r-midi") + ")");
    check (text ("#status").startsWith ("> CHORD"), "status says chord (" + text ("#status") + ")");
    pendingMidi.addEvent (MidiMessage::allNotesOff (1), 0);
    run (300);
    check (text ("#r-midi") == "MIDI:CHORD", "released (" + text ("#r-midi") + ")");
    human ("#midi-mode button[data-value=\"0\"]");
    check (roundToInt (value ("midiMode")) == 0, "held click on SNAP");

    std::printf ("host automation shows in the page\n");
    p->state.getParameter ("spread")->setValueNotifyingHost (0.42f);
    run (200);
    check (ask ("document.querySelector('[data-param=spread] input[type=number]').value").toString() == "42", "SPREAD set by the host shows as 42");

    std::printf ("settings\n");
    human (".panel-tabs button[data-tab=settings]");
    check (! (bool) ask ("document.querySelector('#tab-settings').hidden"), "held click on SETTINGS");
    ask ("(() => { const i = document.querySelector('[data-colour=accent]'); i.value = '#ff8800'; i.dispatchEvent(new Event('input')); return 1; })()");
    run (600);
    check (ColourSettings::load()["accent"].toString() == "#ff8800", "a colour is saved to the shared settings");
    {
        // A second editor (a new instance, or the window reopened) starts with the saved colour.
        auto q = std::make_unique<MeowTuneProcessor>();
        std::unique_ptr<AudioProcessorEditor> ed2 (q->createEditor());
        ed2->setTopLeftPosition (120, 120);
        ed2->addToDesktop (ComponentPeer::windowHasTitleBar);
        ed2->setVisible (true);
        pump (2000);
        var out;
        bool got = false;
        dynamic_cast<MeowTuneEditor*> (ed2.get())->evaluate ("getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()", [&] (var v) { out = v; got = true; });
        for (int i = 0; i < 200 && ! got; ++i) pump (10);
        check (out.toString() == "#ff8800", "a newly opened editor starts in the saved colours (" + out.toString() + ")");
        ed2.reset();
    }
    human ("#reset-colours");
    run (200);
    check (! colourFile.existsAsFile(), "held click on >>RESET COLOURS clears them");
    check (ask ("getComputedStyle(document.documentElement).getPropertyValue('--accent').trim()").toString() == "#118ac1", "and the accent is blue again");
    human (".panel-tabs button[data-tab=tune]");
    check ((bool) ask ("document.querySelector('#tab-settings').hidden"), "held click on TUNE");

    std::printf ("state\n");
    {
        MemoryBlock block;
        p->getStateInformation (block);
        auto q = std::make_unique<MeowTuneProcessor>();
        q->setStateInformation (block.getData(), (int) block.getSize());
        auto* qa = q->state.getParameter ("amount");
        check (std::abs (qa->convertFrom0to1 (qa->getValue()) - value ("amount")) < 0.5f, "amount survives a session save and load");
        auto qv = [&] (const char* id) { auto* x = q->state.getParameter (id); return roundToInt (x->convertFrom0to1 (x->getValue())); };
        check (qv (ParamID::scale) == roundToInt (value (ParamID::scale)) && qv (ParamID::key) == roundToInt (value (ParamID::key)) && qv (ParamID::range) == 1,
               "KEY, SCALE and RANGE survive too");

        // A session from before KEY and SCALE were parameters: they were properties, the notes the truth.
        auto old = [&] (const char* scaleId, uint16_t notes)
        {
            auto tree = p->state.copyState();
            for (auto* id : { ParamID::key, ParamID::scale, ParamID::range })
                tree.removeChild (tree.getChildWithProperty ("id", id), nullptr);
            for (int i = 0; i < 12; ++i)
                tree.getChildWithProperty ("id", ParamID::note (i)).setProperty ("value", (notes >> i) & 1 ? 1.0f : 0.0f, nullptr);
            tree.setProperty ("key", 2, nullptr);
            tree.setProperty ("scale", scaleId, nullptr);
            MemoryBlock mb;
            AudioProcessor::copyXmlToBinary (*tree.createXml(), mb);
            auto r = std::make_unique<MeowTuneProcessor>();
            r->setStateInformation (mb.getData(), (int) mb.getSize());
            auto rv = [&] (const char* id) { auto* x = r->state.getParameter (id); return roundToInt (x->convertFrom0to1 (x->getValue())); };
            return std::make_pair (rv (ParamID::key), rv (ParamID::scale));
        };
        const auto dMajor = old ("major", Scales::mask (2, 1));
        check (dMajor.first == 2 && dMajor.second == 1, "an old D major session loads as KEY D, SCALE MAJOR");
        const auto edited = old ("major", (uint16_t) (Scales::mask (2, 1) ^ 1));
        check (edited.second == Scales::kCustom, "an old session whose notes were edited loads as CUSTOM, keeping them");

        // Loading a session while the editor is open makes the page reload its values.
        p->state.getParameter ("amount")->setValueNotifyingHost (1.0f);
        run (200);
        p->setStateInformation (block.getData(), (int) block.getSize());
        run (500);
        check (ask ("document.querySelector('[data-param=amount] input[type=number]').value").toString() == String (roundToInt (value ("amount"))), "the page shows a loaded session's values");
    }

    ed.reset();
    p.reset();
    if (hadColours) colourFile.replaceWithText (savedColours);
    else colourFile.deleteFile();
    std::printf (failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
