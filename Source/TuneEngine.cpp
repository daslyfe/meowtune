#include "TuneEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kPi = 3.14159265358979f;

    int nextPow2 (int v)
    {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    float midiFromHz (float hz) { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
}

// ---- Tune engine -----------------------------------------------------------------------------

namespace
{
    // The lookahead for a floor: grains are two periods long and centred on their mark (horizon),
    // and the mark a grain uses may sit up to 1.5 periods behind the newest input.
    struct Geometry { float maxPeriod, unvoicedPeriod; int horizon, latency; };
    Geometry geometryFor (double sr, float minHz)
    {
        Geometry g;
        g.maxPeriod = (float) (sr / minHz);
        g.unvoicedPeriod = std::min ((float) (sr * 0.005), g.maxPeriod);
        g.horizon = (int) std::ceil (2.0f * g.maxPeriod) + 2;
        g.latency = g.horizon + (int) std::ceil (1.5f * g.maxPeriod) + 8;
        return g;
    }
}

int TuneEngine::latencyFor (double sampleRate, float minHz)
{
    return geometryFor (sampleRate, std::max (kLowestF0, minHz)).latency;
}

void TuneEngine::prepare (double sampleRate, int maxBlock, float minHz)
{
    sr = sampleRate;
    maxBlockSize = std::max (1, maxBlock);

    decim = std::max (1, (int) std::lround (sr / 12000.0));
    srD = sr / decim;
    winD = (int) std::lround (0.021 * srD);
    tauMinD = std::max (2, (int) std::floor (srD / kMaxF0));
    hopD = std::max (8, (int) std::lround (srD * 0.0053));

    // Every buffer is sized for the lowest range, so setMinPitch never allocates.
    const auto low = geometryFor (sr, kLowestF0);
    const int lowTauMaxD = (int) std::ceil (srD / kLowestF0) + 1;
    const int inSize = nextPow2 (low.latency + 2 * low.horizon + 4 * (int) low.maxPeriod + 1024);
    inRing.assign ((size_t) inSize, 0.0f);
    inMask = inSize - 1;
    const int accSize = nextPow2 (2 * low.horizon + 64);
    accL.assign ((size_t) accSize, 0.0f);
    accR.assign ((size_t) accSize, 0.0f);
    accMask = accSize - 1;
    detRing.assign ((size_t) nextPow2 (winD + lowTauMaxD + 8), 0.0f);
    yinD.assign ((size_t) lowTauMaxD + 2, 0.0f);
    dryL.assign ((size_t) std::max (1, low.latency), 0.0f);
    dryR.assign ((size_t) std::max (1, low.latency), 0.0f);

    const float fc = (float) std::min (2000.0, 0.4 * srD);
    const float q[2] = { 0.5412f, 1.3066f };
    for (int i = 0; i < 2; ++i)
    {
        const float w0 = 2.0f * kPi * fc / (float) sr, cw = std::cos (w0), alpha = std::sin (w0) / (2.0f * q[i]);
        const float a0 = 1.0f + alpha;
        auto& b = aa[(size_t) i];
        b.b0 = (1.0f - cw) * 0.5f / a0;
        b.b1 = (1.0f - cw) / a0;
        b.b2 = b.b0;
        b.a1 = -2.0f * cw / a0;
        b.a2 = (1.0f - alpha) / a0;
    }

    marks.assign (kMarks, Mark {});
    hann.resize (1025);
    for (int i = 0; i <= 1024; ++i)
        hann[(size_t) i] = 0.5f + 0.5f * std::cos (kPi * (float) i / 1024.0f);

    wetL.assign ((size_t) maxBlockSize, 0.0f);
    wetR.assign ((size_t) maxBlockSize, 0.0f);
    history.assign ((size_t) kHistory * 2, -1.0f);

    configure (minHz);
    reset();
}

void TuneEngine::configure (float minHz)
{
    minF0 = std::clamp (minHz, kLowestF0, kMaxF0 * 0.5f);
    const auto g = geometryFor (sr, minF0);
    maxPeriod = g.maxPeriod;
    unvoicedPeriod = g.unvoicedPeriod;
    horizon = g.horizon;
    psolaLatency = g.latency;
    latency = psolaLatency;
    dryLen = std::max (1, latency);
    tauMaxD = std::min ((int) yinD.size() - 2, (int) std::ceil (srD / minF0) + 1);
}

void TuneEngine::setMinPitch (float hz)
{
    if (std::clamp (hz, kLowestF0, kMaxF0 * 0.5f) == minF0) return;
    configure (hz);
    reset();
}

void TuneEngine::reset()
{
    std::fill (inRing.begin(), inRing.end(), 0.0f);
    std::fill (accL.begin(), accL.end(), 0.0f);
    std::fill (accR.begin(), accR.end(), 0.0f);
    std::fill (detRing.begin(), detRing.end(), 0.0f);
    std::fill (dryL.begin(), dryL.end(), 0.0f);
    std::fill (dryR.begin(), dryR.end(), 0.0f);
    for (auto& b : aa) b.z1 = b.z2 = 0.0f;
    t = 0;
    detCount = 0;
    decimPhase = hopCount = 0;
    f0 = 0;
    recentN = 0;
    voiced = false;
    markCount = 0;
    nextMark = 0;
    dryPos = 0;
    for (auto& s : slots)
    {
        s.env = 0;
        for (auto& v : s.sub) v.started = false;
    }
    mixSmoothed = -1.0f;
    lastOutMidi = -1.0f;
}

void TuneEngine::noteOn (int note, float velocity)
{
    if (note < 0 || note > 127) return;
    if (velocity <= 0.0f) { noteOff (note); return; }
    held[(size_t) note] = true;
    display.held[(size_t) (note >> 5)].fetch_or (1u << (note & 31));

    // A slot for CHORD mode: the same note, else a silent slot, else the quietest released one,
    // else the oldest held one.
    int pick = -1;
    for (int i = 1; i < kSlots && pick < 0; ++i)
        if (slots[(size_t) i].note == note) pick = i;
    for (int i = 1; i < kSlots && pick < 0; ++i)
        if (! slots[(size_t) i].on && slots[(size_t) i].env <= 0.0f) pick = i;
    if (pick < 0)
    {
        float quietest = 2.0f;
        for (int i = 1; i < kSlots; ++i)
            if (! slots[(size_t) i].on && slots[(size_t) i].env < quietest) { quietest = slots[(size_t) i].env; pick = i; }
    }
    if (pick < 0)
    {
        int64_t oldest = INT64_MAX;
        for (int i = 1; i < kSlots; ++i)
            if (slots[(size_t) i].age < oldest) { oldest = slots[(size_t) i].age; pick = i; }
    }

    auto& s = slots[(size_t) pick];
    if (s.env <= 0.0f)
        for (auto& v : s.sub) v.started = false; // a fresh voice starts on its note; a sounding one glides
    s.note = note;
    s.velocity = velocity;
    s.on = true;
    s.age = ++ageCounter;
}

void TuneEngine::noteOff (int note)
{
    if (note < 0 || note > 127) return;
    held[(size_t) note] = false;
    display.held[(size_t) (note >> 5)].fetch_and (~(1u << (note & 31)));
    for (int i = 1; i < kSlots; ++i)
        if (slots[(size_t) i].note == note) slots[(size_t) i].on = false;
}

void TuneEngine::allNotesOff()
{
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n]) noteOff (n);
}

uint16_t TuneEngine::heldPitchClasses() const
{
    uint16_t m = 0;
    for (int n = 0; n < 128; ++n)
        if (held[(size_t) n]) m |= (uint16_t) (1u << (n % 12));
    return m;
}

float TuneEngine::readInput (double pos) const
{
    const double fl = std::floor (pos);
    const auto i = (int64_t) fl;
    const float f = (float) (pos - fl);
    const float y0 = inRing[(size_t) ((i - 1) & inMask)], y1 = inRing[(size_t) (i & inMask)];
    const float y2 = inRing[(size_t) ((i + 1) & inMask)], y3 = inRing[(size_t) ((i + 2) & inMask)];
    // Catmull-Rom.
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void TuneEngine::detect()
{
    const int frame = winD + tauMaxD;
    if (detCount < frame) return;

    const int64_t start = detCount - frame;
    const int64_t m = (int64_t) detRing.size() - 1;
    auto x = [&] (int j) { return detRing[(size_t) ((start + j) & m)]; };

    float energy = 0.0f;
    for (int j = tauMaxD; j < frame; ++j) energy += x (j) * x (j);
    const float rms = std::sqrt (energy / (float) winD);

    float hz = 0.0f;
    if (rms >= rmsGate)
    {
        // YIN: difference function, cumulative mean normalised.
        yinD[0] = 1.0f;
        float running = 0.0f;
        for (int tau = 1; tau <= tauMaxD; ++tau)
        {
            float d = 0.0f;
            for (int j = 0; j < winD; ++j)
            {
                const float diff = x (j) - x (j + tau);
                d += diff * diff;
            }
            running += d;
            yinD[(size_t) tau] = running > 0.0f ? d * (float) tau / running : 1.0f;
        }

        int best = -1;
        for (int tau = tauMinD; tau < tauMaxD; ++tau)
            if (yinD[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < tauMaxD && yinD[(size_t) tau + 1] < yinD[(size_t) tau]) ++tau;
                best = tau;
                break;
            }
        if (best < 0)
        {
            best = tauMinD;
            for (int tau = tauMinD; tau < tauMaxD; ++tau)
                if (yinD[(size_t) tau] < yinD[(size_t) best]) best = tau;
            if (yinD[(size_t) best] > 0.3f) best = -1;
        }

        if (best > 0)
        {
            const float a = yinD[(size_t) best - 1], b = yinD[(size_t) best], c = yinD[(size_t) best + 1];
            const float den = a - 2.0f * b + c;
            const float shift = std::abs (den) > 1.0e-9f ? std::clamp (0.5f * (a - c) / den, -0.5f, 0.5f) : 0.0f;
            const float candidate = (float) srD / ((float) best + shift);
            if (candidate >= minF0 && candidate <= kMaxF0) hz = candidate;
        }
    }

    if (hz > 0.0f)
    {
        // Median of the last three estimates removes single-frame octave slips.
        recent[0] = recent[1];
        recent[1] = recent[2];
        recent[2] = hz;
        recentN = std::min (3, recentN + 1);
        if (recentN == 3)
        {
            float a = recent[0], b = recent[1], c = recent[2];
            f0 = std::max (std::min (a, b), std::min (std::max (a, b), c));
        }
        else
            f0 = hz;
        voiced = true;
    }
    else
    {
        recentN = 0;
        voiced = false;
        f0 = 0.0f;
    }

    const float inMidi = voiced ? midiFromHz (f0) + curInputPitch : -1.0f;
    display.inHz.store (voiced ? f0 : 0.0f);
    display.inMidi.store (inMidi);
    pushHistory (inMidi, voiced ? lastOutMidi : -1.0f);
}

void TuneEngine::placeMarks()
{
    while (nextMark <= (double) t)
    {
        Mark mk;
        mk.pos = nextMark;
        if (voiced && f0 > 0.0f)
        {
            mk.period = (float) sr / f0;
            mk.midi = midiFromHz (f0);
        }
        else
        {
            mk.period = unvoicedPeriod;
            mk.midi = -1.0f;
        }
        marks[(size_t) (markCount % kMarks)] = mk;
        ++markCount;
        nextMark += mk.period;
    }
}

float TuneEngine::snapTarget (SubVoice& v, float midi, uint16_t mask)
{
    const int centre = (int) std::lround (midi);
    int best = -1;
    float bestD = 1.0e9f;
    for (int k = -7; k <= 7; ++k)
    {
        const int n = centre + k;
        if (mask & (1u << (((n % 12) + 12) % 12)))
        {
            const float d = std::abs (midi - (float) n);
            if (d < bestD) { bestD = d; best = n; }
        }
    }
    // Hysteresis: stay on the current note until another is clearly nearer.
    const bool currentOk = v.targetNote >= 0 && (mask & (1u << (v.targetNote % 12)))
                           && std::abs (midi - (float) v.targetNote) < 6.0f;
    if (! (currentOk && std::abs (midi - (float) v.targetNote) < bestD + 0.15f))
        v.targetNote = best;
    return (float) v.targetNote;
}

void TuneEngine::startSub (SubVoice& v, int subIndex)
{
    v.next = (double) (t + horizon) + rand01() * unvoicedPeriod + subIndex * 0.37 * unvoicedPeriod;
    v.mark = std::max<int64_t> (0, markCount - 1);
    v.lastS = v.next;
    v.started = true;
    v.fresh = true;
}

void TuneEngine::placeGrain (Slot& slot, int slotIndex, int subIndex, const TuneSettings& s, int unison)
{
    auto& v = slot.sub[(size_t) subIndex];
    const double sPos = v.next;
    const double target = sPos - psolaLatency;

    // The analysis mark nearest the input time this grain stands for.
    const int64_t lowest = std::max<int64_t> (0, markCount - kMarks + 2);
    if (markCount == 0) { v.next += unvoicedPeriod; return; }
    v.mark = std::clamp<int64_t> (v.mark, lowest, markCount - 1);
    while (v.mark > lowest && marks[(size_t) (v.mark % kMarks)].pos > target) --v.mark;
    while (v.mark + 1 < markCount
           && std::abs (marks[(size_t) ((v.mark + 1) % kMarks)].pos - target) <= std::abs (marks[(size_t) (v.mark % kMarks)].pos - target))
        ++v.mark;
    const Mark& m = marks[(size_t) (v.mark % kMarks)];

    const float pos = unison > 1 ? -1.0f + 2.0f * (float) subIndex / (float) (unison - 1) : 0.0f;
    const float detune = s.detuneCents * 0.01f * pos;

    float shift, outPeriod;
    if (m.midi >= 0.0f)
    {
        const float p = m.midi + s.inputPitch;
        float corrTarget = 0.0f;
        if (slotIndex == 0)
        {
            const uint16_t held = heldPCs;
            const uint16_t mask = (s.midiMode == 0 && held != 0) ? held : (uint16_t) (s.mask & 0x0fff);
            if (mask != 0) corrTarget = snapTarget (v, p, mask) - p;
            else v.targetNote = -1;
        }
        else
        {
            corrTarget = (float) slot.note - p;
            v.targetNote = slot.note;
        }

        if (v.fresh)
            v.corr = corrTarget;
        else
        {
            const double dt = std::max (0.0, sPos - v.lastS);
            const float tau = s.glideMs * 0.001f * (float) sr;
            const float alpha = tau < 1.0f ? 1.0f : 1.0f - std::exp (-(float) dt / tau);
            v.corr += (corrTarget - v.corr) * alpha;
        }
        v.fresh = false;
        shift = std::clamp (s.inputPitch + s.amount * v.corr + detune, -36.0f, 36.0f);
        v.lastShift = shift;
        outPeriod = m.period / std::exp2 (shift / 12.0f);

        if (subIndex == 0 && slotIndex == primarySlot)
            lastOutMidi = m.midi + shift - detune;
    }
    else
    {
        // Noise and silence: keep the spacing near the analysis spacing, jittered so copies don't line up.
        shift = v.lastShift;
        outPeriod = m.period * (0.8f + 0.4f * rand01());
    }
    v.lastS = sPos;

    const float ratio = std::exp2 (shift / 12.0f);
    const float fr = std::clamp (std::exp2 (s.formantShift / 12.0f) * std::pow (ratio, 1.0f - s.formantCorrect), 0.5f, 2.0f);
    const float half = std::min (m.period / fr, (float) horizon - 1.0f);

    // Keep the level steady: more grains per second overlap more.
    const float overlap = (m.period / fr) / std::max (1.0f, outPeriod);
    const float norm = std::clamp (1.0f / std::sqrt (std::max (overlap, 1.0e-3f)), 0.25f, 2.0f);
    const float voiceGain = slotIndex == 0 ? 1.0f : 0.7f * (0.25f + 0.75f * slot.velocity);
    const float g = slot.env * voiceGain * norm / std::sqrt ((float) unison);

    const float pan = std::clamp (s.spread * pos, -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * kPi * 0.25f;
    const float gl = g * std::cos (angle) * 1.41421356f, gr = g * std::sin (angle) * 1.41421356f;

    if (g > 0.0f)
    {
        const auto first = (int64_t) std::ceil (sPos - half), last = (int64_t) std::floor (sPos + half);
        const float invHalf = 1024.0f / half;
        for (int64_t n = first; n <= last; ++n)
        {
            const float j = (float) ((double) n - sPos);
            const float wi = std::abs (j) * invHalf;
            const int k = std::min (1023, (int) wi);
            const float w = hann[(size_t) k] + (hann[(size_t) k + 1] - hann[(size_t) k]) * (wi - (float) k);
            const float x = readInput (m.pos + (double) (j * fr)) * w;
            accL[(size_t) (n & accMask)] += x * gl;
            accR[(size_t) (n & accMask)] += x * gr;
        }
    }

    v.next = sPos + std::max (1.0f, outPeriod);
}

void TuneEngine::pushHistory (float in, float out)
{
    const int w = histWrite.load (std::memory_order_relaxed);
    history[(size_t) w * 2] = in;
    history[(size_t) w * 2 + 1] = out;
    const int next = (w + 1) % kHistory;
    if (next != histRead.load (std::memory_order_acquire)) // full: drop the newest
        histWrite.store (next, std::memory_order_release);
}

int TuneEngine::readHistory (float* dest, int maxPairs)
{
    int r = histRead.load (std::memory_order_relaxed);
    const int w = histWrite.load (std::memory_order_acquire);
    int n = 0;
    while (r != w && n < maxPairs)
    {
        dest[n * 2] = history[(size_t) r * 2];
        dest[n * 2 + 1] = history[(size_t) r * 2 + 1];
        r = (r + 1) % kHistory;
        ++n;
    }
    histRead.store (r, std::memory_order_release);
    return n;
}

void TuneEngine::process (const float* const* in, int numIn, float* const* out, int numOut, int n, const TuneSettings& s)
{
    // Larger blocks than prepared: run in pieces.
    if (n > maxBlockSize)
    {
        const float* inP[2];
        float* outP[2];
        for (int done = 0; done < n; done += maxBlockSize)
        {
            const int len = std::min (maxBlockSize, n - done);
            for (int c = 0; c < std::min (numIn, 2); ++c) inP[c] = in[c] + done;
            for (int c = 0; c < std::min (numOut, 2); ++c) outP[c] = out[c] + done;
            process (inP, std::min (numIn, 2), outP, std::min (numOut, 2), len, s);
        }
        return;
    }

    numIn = std::min (numIn, 2);
    numOut = std::min (numOut, 2);
    curInputPitch = s.inputPitch;
    heldPCs = heldPitchClasses();
    const int unison = std::clamp (s.unison, 1, kMaxUnison);
    const bool anyHeld = heldPCs != 0;

    // Which slots sing: slot 0 unless CHORD mode has notes; slots 1.. for held notes in CHORD mode.
    std::array<bool, kSlots> want {};
    want[0] = ! (s.midiMode == 1 && anyHeld);
    for (int i = 1; i < kSlots; ++i)
        want[(size_t) i] = s.midiMode == 1 && slots[(size_t) i].on;
    primarySlot = 0;
    if (! want[0])
    {
        int lowest = 128;
        for (int i = 1; i < kSlots; ++i)
            if (want[(size_t) i] && slots[(size_t) i].note < lowest) { lowest = slots[(size_t) i].note; primarySlot = i; }
    }
    const float envStep = 1.0f / (float) (0.01 * sr);

    float inPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float x = 0.0f;
        for (int c = 0; c < numIn; ++c) x += in[c][i];
        if (numIn > 1) x *= 1.0f / (float) numIn;
        inPeak = std::max (inPeak, std::abs (x));
        inRing[(size_t) (t & inMask)] = x;

        const float y = aa[1].run (aa[0].run (x));
        if (++decimPhase >= decim)
        {
            decimPhase = 0;
            detRing[(size_t) (detCount & (int64_t) (detRing.size() - 1))] = y;
            ++detCount;
            if (++hopCount >= hopD)
            {
                hopCount = 0;
                detect();
            }
        }

        placeMarks();

        for (int si = 0; si < kSlots; ++si)
        {
            auto& slot = slots[(size_t) si];
            const bool on = want[(size_t) si];
            slot.env = on ? std::min (1.0f, slot.env + envStep) : std::max (0.0f, slot.env - envStep);
            if (slot.env <= 0.0f && ! on)
            {
                for (auto& v : slot.sub) v.started = false;
                continue;
            }
            for (int u = 0; u < kMaxUnison; ++u)
            {
                auto& v = slot.sub[(size_t) u];
                if (u >= unison) { v.started = false; continue; }
                if (! v.started) startSub (v, u);
                while (v.next <= (double) (t + horizon))
                    placeGrain (slot, si, u, s, unison);
            }
        }

        const auto a = (size_t) (t & accMask);
        wetL[(size_t) i] = accL[a];
        wetR[(size_t) i] = accR[a];
        accL[a] = accR[a] = 0.0f;
        ++t;
    }

    if (mixSmoothed < 0.0f) { mixSmoothed = s.mix; gainSmoothed = s.outputGain; }
    const float k = 1.0f - std::exp (-1.0f / (float) (0.02 * sr));
    float outPeak = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        mixSmoothed += (s.mix - mixSmoothed) * k;
        gainSmoothed += (s.outputGain - gainSmoothed) * k;
        const float inL = in[0][i], inR = numIn > 1 ? in[1][i] : in[0][i];
        const float dl = dryL[(size_t) dryPos], dr = dryR[(size_t) dryPos];
        dryL[(size_t) dryPos] = inL;
        dryR[(size_t) dryPos] = inR;
        if (++dryPos >= dryLen) dryPos = 0;

        const float wet = mixSmoothed, dry = 1.0f - mixSmoothed;
        if (numOut >= 2)
        {
            out[0][i] = (dl * dry + wetL[(size_t) i] * wet) * gainSmoothed;
            out[1][i] = (dr * dry + wetR[(size_t) i] * wet) * gainSmoothed;
            outPeak = std::max (outPeak, std::max (std::abs (out[0][i]), std::abs (out[1][i])));
        }
        else if (numOut == 1)
        {
            out[0][i] = ((dl + dr) * 0.5f * dry + (wetL[(size_t) i] + wetR[(size_t) i]) * 0.5f * wet) * gainSmoothed;
            outPeak = std::max (outPeak, std::abs (out[0][i]));
        }
    }

    // Peaks are collected until the editor takes them.
    float prev = display.inPeak.load();
    while (inPeak > prev && ! display.inPeak.compare_exchange_weak (prev, inPeak)) {}
    prev = display.outPeak.load();
    while (outPeak > prev && ! display.outPeak.compare_exchange_weak (prev, outPeak)) {}

    const auto& primary = slots[(size_t) primarySlot].sub[0];
    display.target.store (voiced && primary.started ? primary.targetNote : -1);
    display.outMidi.store (voiced ? lastOutMidi : -1.0f);
}
