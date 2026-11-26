#include "PluginEditor.h"

#include <BinaryData.h>

namespace
{
    const char* mimeFor (const juce::String& file)
    {
        const auto ext = file.fromLastOccurrenceOf (".", false, false).toLowerCase();
        if (ext == "html") return "text/html";
        if (ext == "css") return "text/css";
        if (ext == "js") return "text/javascript";
        if (ext == "woff") return "font/woff";
        if (ext == "png") return "image/png";
        return "application/octet-stream";
    }

    // The JUCE bridge script ships as index.js; it is served under juce/ so it can't clash with app files.
    juce::String originalNameFor (const juce::String& path)
    {
        if (path == "juce/index.js") return "index.js";
        return path.fromLastOccurrenceOf ("/", false, false);
    }

    // The saved colours as a :root rule, put into the page before it first paints.
    juce::String colourRule()
    {
        const auto c = ColourSettings::load();
        juce::String rule;
        if (auto* o = c.getDynamicObject())
            for (const auto& p : o->getProperties())
                rule << "--" << p.name.toString() << ":" << p.value.toString() << ";";
        return rule.isEmpty() ? juce::String() : ":root{" + rule + "}";
    }
}

MeowTuneEditor::MeowTuneEditor (MeowTuneProcessor& p)
    : AudioProcessorEditor (&p), proc (p), web (makeOptions())
{
    lastStateVersion = proc.stateVersion.load();
    histBuf.resize ((size_t) TuneEngine::kHistory * 2);
    addAndMakeVisible (web);
    web.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    setResizable (true, true);
    setResizeLimits (760, 560, 2560, 1600);
    setSize (1040, 720);
    startTimerHz (30);
}

MeowTuneEditor::~MeowTuneEditor()
{
    stopTimer();
}

juce::RangedAudioParameter* MeowTuneEditor::param (const juce::var& id) const
{
    return proc.state.getParameter (id.toString());
}

juce::WebBrowserComponent::Options MeowTuneEditor::makeOptions()
{
    using Args = const juce::Array<juce::var>&;
    using Done = juce::WebBrowserComponent::NativeFunctionCompletion;

    return juce::WebBrowserComponent::Options {}
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withResourceProvider ([this] (const auto& url) { return getResource (url); })
        .withNativeFunction ("getInit", [this] (Args, Done done) { done (makeInit()); })
        .withNativeFunction ("setParam", [this] (Args a, Done done)
        {
            if (a.size() >= 2)
                if (auto* p = param (a[0]))
                {
                    const float v = p->convertTo0to1 ((float) (double) a[1]);
                    p->setValueNotifyingHost (v);
                    lastSent[a[0].toString()] = p->convertFrom0to1 (p->getValue()); // the page already shows it
                }
            done ({});
        })
        .withNativeFunction ("gesture", [this] (Args a, Done done)
        {
            if (a.size() >= 2)
                if (auto* p = param (a[0]))
                {
                    if ((bool) a[1]) p->beginChangeGesture();
                    else p->endChangeGesture();
                }
            done ({});
        })
        .withNativeFunction ("setColours", [] (Args a, Done done)
        {
            ColourSettings::save (a.size() >= 1 ? a[0] : juce::var());
            done (ColourSettings::load());
        });
}

juce::var MeowTuneEditor::makeInit()
{
    // The page (re)loaded: events sent before it was listening are lost, so send everything again.
    lastSent.clear();
    lastHeld = {};

    juce::Array<juce::var> params;
    for (auto* p : proc.getParameters())
    {
        auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (rp == nullptr) continue;
        const auto& range = rp->getNormalisableRange();
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", rp->getParameterID());
        o->setProperty ("min", range.start);
        o->setProperty ("max", range.end);
        o->setProperty ("step", range.interval);
        o->setProperty ("value", rp->convertFrom0to1 (rp->getValue()));
        o->setProperty ("def", rp->convertFrom0to1 (rp->getDefaultValue()));
        params.add (juce::var (o));
        lastSent[rp->getParameterID()] = rp->convertFrom0to1 (rp->getValue());
    }

    auto* o = new juce::DynamicObject();
    o->setProperty ("params", params);
    // The SCALE parameter's choices, in order, with their intervals from KEY (CUSTOM has none).
    juce::Array<juce::var> scales;
    for (int i = 0; i < Scales::kCount; ++i)
    {
        auto* s = new juce::DynamicObject();
        s->setProperty ("id", Scales::list[i].id);
        s->setProperty ("name", Scales::list[i].name);
        juce::Array<juce::var> steps;
        for (int b = 0; b < 12; ++b)
            if (Scales::list[i].intervals & (1u << b)) steps.add (b);
        s->setProperty ("steps", i == Scales::kCustom ? juce::var() : juce::var (steps));
        scales.add (juce::var (s));
    }
    // VOICE RANGE choices and the latency each gives at this sample rate.
    juce::Array<juce::var> ranges;
    for (int i = 0; i < TuneEngine::kRanges; ++i)
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("name", Ranges::names[i]);
        r->setProperty ("hz", TuneEngine::kRangeHz[(size_t) i]);
        r->setProperty ("latency", TuneEngine::latencyFor (proc.engine.getSampleRate(), TuneEngine::kRangeHz[(size_t) i]));
        ranges.add (juce::var (r));
    }
    o->setProperty ("scales", scales);
    o->setProperty ("ranges", ranges);
    o->setProperty ("colours", ColourSettings::load());
    o->setProperty ("version", MEOWTUNE_VERSION);
    o->setProperty ("rate", proc.engine.getSampleRate());
    o->setProperty ("latency", proc.getLatencySamples());
    return juce::var (o);
}

void MeowTuneEditor::evaluate (const juce::String& js, std::function<void (juce::var)> done)
{
    web.evaluateJavascript (js, [done] (juce::WebBrowserComponent::EvaluationResult r)
    {
        if (const auto* v = r.getResult()) done (*v);
        else done ({});
    });
}

void MeowTuneEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black); // --bg, shown only until the page draws
}

void MeowTuneEditor::resized()
{
    web.setBounds (getLocalBounds());
}

void MeowTuneEditor::timerCallback()
{
    if (proc.stateVersion.load() != lastStateVersion)
    {
        // A session was loaded: the page asks for everything again.
        lastStateVersion = proc.stateVersion.load();
        web.emitEventIfBrowserIsVisible ("reload", juce::var());
    }

    auto& d = proc.engine.display;
    auto* o = new juce::DynamicObject();
    o->setProperty ("inHz", d.inHz.load());
    o->setProperty ("inMidi", d.inMidi.load());
    o->setProperty ("outMidi", d.outMidi.load());
    o->setProperty ("target", d.target.load());
    o->setProperty ("inPeak", d.inPeak.exchange (0.0f));
    o->setProperty ("outPeak", d.outPeak.exchange (0.0f));

    juce::String held;
    juce::Array<juce::var> heldArr;
    for (int w = 0; w < 4; ++w)
    {
        const auto bits = d.held[(size_t) w].load();
        for (int b = 0; b < 32; ++b)
            if (bits & (1u << b)) { heldArr.add (w * 32 + b); held << (w * 32 + b) << ","; }
    }
    if (held != lastHeld)
    {
        lastHeld = held;
        o->setProperty ("held", heldArr);
    }

    // Pitch history since the last tick, as hundredths of a semitone (-1 when unvoiced).
    const int pairs = proc.engine.readHistory (histBuf.data(), TuneEngine::kHistory);
    if (pairs > 0)
    {
        juce::Array<juce::var> h;
        h.ensureStorageAllocated (pairs * 2);
        for (int i = 0; i < pairs * 2; ++i)
            h.add (histBuf[(size_t) i] < 0 ? -1 : juce::roundToInt (histBuf[(size_t) i] * 100.0f));
        o->setProperty ("hist", h);
    }

    // Parameters that changed since they were last sent (host automation, other editors).
    auto* changed = new juce::DynamicObject();
    for (auto* p : proc.getParameters())
        if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const float v = rp->convertFrom0to1 (rp->getValue());
            auto it = lastSent.find (rp->getParameterID());
            if (it == lastSent.end() || it->second != v)
            {
                lastSent[rp->getParameterID()] = v;
                changed->setProperty (rp->getParameterID(), v);
            }
        }
    if (! changed->getProperties().isEmpty())
        o->setProperty ("params", juce::var (changed));
    else
        delete changed;

    web.emitEventIfBrowserIsVisible ("tick", juce::var (o));
}

std::optional<juce::WebBrowserComponent::Resource> MeowTuneEditor::getResource (const juce::String& url)
{
    auto path = url.fromFirstOccurrenceOf ("/", false, false).upToFirstOccurrenceOf ("?", false, false);
    if (path.isEmpty())
        path = "index.html";

    const auto wanted = originalNameFor (path);

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
    {
        if (wanted != BinaryData::originalFilenames[i])
            continue;

        int size = 0;
        if (const auto* data = BinaryData::getNamedResource (BinaryData::namedResourceList[i], size))
        {
            if (wanted == "index.html")
            {
                // Saved colours go in before the first paint, so the page never flashes the defaults.
                auto html = juce::String::fromUTF8 (data, size)
                                .replace ("<style id=\"user-colours\"></style>", "<style id=\"user-colours\">" + colourRule() + "</style>");
                const auto utf8 = html.toRawUTF8();
                const auto* bytes = reinterpret_cast<const std::byte*> (utf8);
                return juce::WebBrowserComponent::Resource { std::vector<std::byte> (bytes, bytes + std::strlen (utf8)), "text/html" };
            }
            const auto* bytes = reinterpret_cast<const std::byte*> (data);
            return juce::WebBrowserComponent::Resource { std::vector<std::byte> (bytes, bytes + size),
                                                         juce::String (mimeFor (wanted)) };
        }
    }

    return std::nullopt;
}
