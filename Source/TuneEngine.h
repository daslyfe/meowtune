#pragma once

// MeowTune's DSP, with no JUCE so tools/engine_test can run it on its own.
//
// Signal path (the input is summed to mono first):
//   pitch detection (YIN on a decimated copy)
//   -> pitch marks one period apart
//   -> PSOLA voices: each voice re-spaces grains to its target pitch and resamples each grain
//      to place the formants (FORMANT CORRECT keeps them, FORMANT SHIFT moves them)
//   -> stereo sum of the voices (UNISON copies detuned and panned by DETUNE and SPREAD)
//   -> dry/wet mix with the input delayed by the same latency.
//
// Voice slots: slot 0 follows the scale (or the pitch classes of held MIDI notes in SNAP mode).
// Slots 1..8 each sing one held MIDI note in CHORD mode.

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

struct TuneSettings
{
    float amount = 1.0f;          // 0..1 how far the pitch is pulled to the target
    float glideMs = 20.0f;        // retune time constant
    float inputPitch = 0.0f;      // semitones added to the input before it is tuned
    float formantCorrect = 1.0f;  // 0 = formants follow the pitch, 1 = formants stay put
    float formantShift = 0.0f;    // semitones
    int unison = 1;               // 1..8 copies per voice
    float detuneCents = 15.0f;    // spread of the unison copies, cents either side
    float spread = 0.5f;          // 0..1 stereo width of the unison copies
    float mix = 1.0f;             // 0..1
    float outputGain = 1.0f;      // linear
    int midiMode = 0;             // 0 = SNAP (held notes replace the scale), 1 = CHORD (a voice per held note)
    uint16_t mask = 0x0fff;       // enabled pitch classes, bit 0 = C
};

class TuneEngine
{
public:
    static constexpr int kSlots = 9;
    static constexpr int kMaxUnison = 8;
    static constexpr float kMaxF0 = 1100.0f;

    // VOICE RANGE: the lowest pitch detected. The lookahead is about 3.5 periods of it, so a higher
    // floor means less latency; a voice below it is not tracked.
    static constexpr int kRanges = 4;
    static constexpr std::array<float, kRanges> kRangeHz { 70.0f, 100.0f, 150.0f, 220.0f };
    static constexpr float kLowestF0 = 70.0f; // buffers are sized for this, so any range fits

    void prepare (double sampleRate, int maxBlock, float minHz = 100.0f);
    void reset();

    // Changes the floor without allocating (safe on the audio thread). Resets the engine when it changes.
    void setMinPitch (float hz);
    float getMinPitch() const { return minF0; }
    static int latencyFor (double sampleRate, float minHz);

    // Call between process() calls, at the sample the event happened.
    void noteOn (int note, float velocity);
    void noteOff (int note);
    void allNotesOff();

    // in: numIn channels (summed to mono). out: numOut channels (1 or 2). in and out may alias.
    void process (const float* const* in, int numIn, float* const* out, int numOut, int n, const TuneSettings&);

    int getLatency() const { return latency; }
    double getSampleRate() const { return sr; }

    // Readouts for the editor, written by the audio thread.
    struct Display
    {
        std::atomic<float> inHz { 0.0f };      // detected input pitch (0 when unvoiced)
        std::atomic<float> inMidi { -1.0f };   // detected pitch + PITCH, as a MIDI number (-1 unvoiced)
        std::atomic<float> outMidi { -1.0f };  // what slot 0 sings now
        std::atomic<int> target { -1 };        // slot 0's target note
        std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
        std::array<std::atomic<uint32_t>, 4> held {}; // held MIDI notes, 128 bits
    } display;

    // Pitch history for the editor's graph: pairs (input midi, output midi), -1 when unvoiced.
    // Single producer (audio thread), single consumer (editor timer).
    static constexpr int kHistory = 2048;
    int readHistory (float* dest, int maxPairs);

private:
    struct Mark
    {
        double pos = 0;
        float period = 0;
        float midi = -1;   // detected midi (without PITCH), -1 unvoiced
    };

    struct SubVoice
    {
        double next = 0;        // next synthesis mark, absolute output sample
        int64_t mark = 0;       // index of the analysis mark last used
        float corr = 0;         // smoothed correction, semitones
        int targetNote = -1;    // current snap target (hysteresis)
        float lastShift = 0;    // last total shift in semitones (used for unvoiced grains)
        double lastS = 0;       // position of the previous grain
        bool started = false;
        bool fresh = true;      // no grain yet: start on the target instead of gliding to it
    };

    struct Slot
    {
        int note = -1;          // CHORD: the MIDI note; -1 for slot 0
        float velocity = 1;
        bool on = false;
        float env = 0;
        int64_t age = 0;
        std::array<SubVoice, kMaxUnison> sub;
    };

    void detect();
    void placeMarks();
    void placeGrain (Slot&, int slotIndex, int subIndex, const TuneSettings&, int unison);
    float snapTarget (SubVoice&, float midi, uint16_t mask);
    uint16_t heldPitchClasses() const;
    float readInput (double pos) const;
    void pushHistory (float in, float out);
    void startSub (SubVoice&, int subIndex);

    double sr = 48000;
    int maxBlockSize = 512;
    float curInputPitch = 0;
    uint16_t heldPCs = 0;
    int primarySlot = 0;
    float mixSmoothed = -1, gainSmoothed = 1;
    float lastOutMidi = -1;
    int latency = 0, psolaLatency = 0, horizon = 0;
    float minF0 = 100.0f, maxPeriod = 480, unvoicedPeriod = 240;
    void configure (float minHz);

    // Input ring (mono), indexed by absolute sample.
    std::vector<float> inRing;
    int64_t inMask = 0;
    int64_t t = 0;              // absolute index of the sample being processed

    // Output accumulation rings (stereo).
    std::vector<float> accL, accR;
    int64_t accMask = 0;

    // Decimated detection.
    int decim = 4;
    double srD = 12000;
    int winD = 256, tauMinD = 10, tauMaxD = 150, hopD = 64;
    std::vector<float> detRing;
    int64_t detCount = 0;
    int decimPhase = 0, hopCount = 0;
    struct Biquad { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; float run (float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    std::array<Biquad, 2> aa;
    std::vector<float> yinD;
    float rmsGate = 0.003f;
    float f0 = 0;               // smoothed detection, Hz, 0 = unvoiced
    std::array<float, 3> recent {};
    int recentN = 0;
    bool voiced = false;

    // Marks.
    static constexpr int kMarks = 4096;
    std::vector<Mark> marks;
    int64_t markCount = 0;
    double nextMark = 0;

    std::array<Slot, kSlots> slots;
    int64_t ageCounter = 0;
    std::array<bool, 128> held {};

    std::vector<float> hann;    // 1025 points over [0, pi]

    // Dry delay (dryLen = latency, within buffers sized for the lowest range).
    std::vector<float> dryL, dryR;
    int dryPos = 0, dryLen = 0;
    std::vector<float> wetL, wetR;

    // Display.
    std::vector<float> history;
    std::atomic<int> histWrite { 0 }, histRead { 0 };
    uint32_t rng = 12345;
    float rand01() { rng = rng * 1664525u + 1013904223u; return (float) (rng >> 8) * (1.0f / 16777216.0f); }
};
