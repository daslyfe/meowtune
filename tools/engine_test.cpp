// Runs TuneEngine on synthetic voices (no JUCE) and measures what comes out with its own pitch
// estimator, so a bug in the engine's detector can't hide itself.
#include "TuneEngine.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;
static void check (bool ok, const std::string& what)
{
    std::printf ("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (! ok) ++failures;
}

static std::string fmt (const char* f, double a, double b = 0, double c = 0)
{
    char buf[256];
    std::snprintf (buf, sizeof buf, f, a, b, c);
    return buf;
}

static constexpr double kPi = 3.14159265358979;

static double hzOf (double midi) { return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0); }
static double cents (double a, double b) { return 1200.0 * std::log2 (a / b); }

// A voice-like source: harmonics at 1/k, plus optional formant resonators, at a (possibly changing) pitch.
struct Voice
{
    double sr, phase = 0;
    bool formants = false;
    double z[2][2] = {};
    float next (double hz)
    {
        phase += hz / sr;
        if (phase >= 1) phase -= 1;
        double v = 0;
        for (int k = 1; k * hz < sr * 0.45 && k <= 40; ++k)
            v += std::sin (2 * kPi * k * phase) / k;
        v *= 0.25;
        if (! formants) return (float) v;
        // Two resonators (700 Hz, 1200 Hz), like an "a" vowel.
        const double fs[2] = { 700, 1200 }, bw[2] = { 110, 140 };
        double out = 0;
        for (int i = 0; i < 2; ++i)
        {
            const double r = std::exp (-kPi * bw[i] / sr), c = 2 * r * std::cos (2 * kPi * fs[i] / sr);
            const double y = (1 - r) * v + c * z[i][0] - r * r * z[i][1];
            z[i][1] = z[i][0];
            z[i][0] = y;
            out += y;
        }
        return (float) (out * 2.0);
    }
};

// Normalised autocorrelation pitch, picking the shortest lag near the best peak.
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
            const double s = std::abs (den) > 1e-12 ? 0.5 * (a - c) / den : 0;
            return sr / (lag + s);
        }
    return 0;
}

static double rms (const float* x, int n)
{
    double e = 0;
    for (int i = 0; i < n; ++i) e += (double) x[i] * x[i];
    return std::sqrt (e / std::max (1, n));
}

static double goertzel (const float* x, int n, double hz, double sr)
{
    const double w = 2 * kPi * hz / sr, c = 2 * std::cos (w);
    double s1 = 0, s2 = 0;
    for (int i = 0; i < n; ++i)
    {
        const double h = 0.5 - 0.5 * std::cos (2 * kPi * i / (n - 1));
        const double s0 = x[i] * h + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt (s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

// Magnitude-weighted mean frequency of the spectrum, up to 5 kHz (a crude formant position).
static double centroid (const float* x, int n, double sr)
{
    double num = 0, den = 0;
    for (double f = 50; f < 5000; f += 25)
    {
        const double m = goertzel (x, n, f, sr);
        num += f * m;
        den += m;
    }
    return num / std::max (den, 1e-20);
}

struct Run
{
    double sr;
    int block;
    TuneEngine e;
    std::vector<float> inL, outL, outR;

    Run (double rate, int blockSize = 512, float minHz = 100.0f) : sr (rate), block (blockSize) { e.prepare (sr, block, minHz); }

    // Renders `seconds` of a source; the callback may change settings or send notes at each block.
    template <typename Src, typename Hook>
    void render (double seconds, TuneSettings& s, Src&& src, Hook&& hook)
    {
        const int total = (int) (seconds * sr);
        const size_t base = outL.size();
        outL.resize (base + (size_t) total);
        outR.resize (base + (size_t) total);
        inL.resize (base + (size_t) total);
        std::vector<float> l ((size_t) block), r ((size_t) block);
        for (int done = 0; done < total; done += block)
        {
            const int n = std::min (block, total - done);
            hook ((int) (base + (size_t) done), s, e);
            for (int i = 0; i < n; ++i) l[(size_t) i] = r[(size_t) i] = inL[base + (size_t) (done + i)] = src();
            const float* in[2] = { l.data(), r.data() };
            float* out[2] = { l.data(), r.data() };
            e.process (in, 2, out, 2, n, s);
            std::copy (l.begin(), l.begin() + n, outL.begin() + (long) (base + (size_t) done));
            std::copy (r.begin(), r.begin() + n, outR.begin() + (long) (base + (size_t) done));
        }
    }
    template <typename Src>
    void render (double seconds, TuneSettings& s, Src&& src) { render (seconds, s, src, [] (int, TuneSettings&, TuneEngine&) {}); }

    // Measures the output pitch at time t seconds (window of 60 ms).
    double pitchAt (double t) const { return measurePitch (outL.data() + (size_t) (t * sr), (int) (0.06 * sr), sr); }
};

static void tuneCase (const char* name, double inHz, double expectHz, TuneSettings s, double rate = 48000, float minHz = 100.0f)
{
    Run r (rate, 512, minHz);
    Voice v { rate };
    r.render (1.2, s, [&] { return v.next (inHz); });
    const double got = r.pitchAt (0.9);
    check (got > 0 && std::abs (cents (got, expectHz)) < 6,
           std::string (name) + fmt (": %.1f Hz in, %.2f Hz out, want %.2f", inHz, got, expectHz));
}

int main()
{
    std::printf ("latency\n");
    {
        const char* names[] = { "BASS", "TENOR", "ALTO", "SOPRANO" };
        int prev = 1 << 30;
        for (int i = 0; i < TuneEngine::kRanges; ++i)
        {
            TuneEngine e;
            e.prepare (48000, 512, TuneEngine::kRangeHz[(size_t) i]);
            const int lat = e.getLatency();
            check (lat == TuneEngine::latencyFor (48000, TuneEngine::kRangeHz[(size_t) i]) && lat < prev,
                   std::string (names[i]) + fmt (" (%.0f Hz floor) at 48 kHz: %.0f samples (%.1f ms)", TuneEngine::kRangeHz[(size_t) i], lat, lat / 48.0));
            prev = lat;
        }
        TuneEngine e;
        e.prepare (96000, 512, 70.0f);
        check (e.getLatency() < 0.055 * 96000, fmt ("BASS at 96 kHz: %.0f samples (%.1f ms)", e.getLatency(), e.getLatency() / 96.0));
    }

    std::printf ("detection\n");
    for (double hz : { 73.4, 82.4, 110.0, 196.0, 220.0, 440.0, 880.0 })
    {
        Run r (48000, 512, 70.0f); // BASS
        Voice v { 48000 };
        TuneSettings s;
        r.render (0.5, s, [&] { return v.next (hz); });
        const double got = r.e.display.inHz.load();
        check (got > 0 && std::abs (cents (got, hz)) < 3, fmt ("%.1f Hz detected as %.2f Hz", hz, got));
    }
    {
        Run r (48000);
        TuneSettings s;
        uint32_t seed = 1;
        r.render (0.5, s, [&] { seed = seed * 1664525u + 1013904223u; return ((float) (seed >> 8) / 16777216.0f - 0.5f) * 0.5f; });
        check (r.e.display.inHz.load() == 0.0f, "white noise is unvoiced");
    }

    std::printf ("tuning\n");
    {
        TuneSettings s;
        tuneCase ("chromatic snaps up", hzOf (57 - 0.39), hzOf (57), s);
        tuneCase ("chromatic snaps down", hzOf (57 + 0.4), hzOf (57), s);
        s.mask = 0b101010110101; // C major
        tuneCase ("C major: A#3+50c goes to B3", hzOf (58.5), hzOf (59), s);
        tuneCase ("C major at 44.1 kHz", hzOf (58.5), hzOf (59), s, 44100);
        tuneCase ("C major at 96 kHz", hzOf (58.5), hzOf (59), s, 96000);
        s.mask = 0;
        tuneCase ("no notes selected: untouched", 225.0, 225.0, s);
    }
    {
        TuneSettings s;
        s.amount = 0;
        s.inputPitch = 12;
        tuneCase ("PITCH +12, amount 0: an octave up", 225.0, 450.0, s);
        s.inputPitch = -12;
        tuneCase ("PITCH -12, amount 0: an octave down", 225.0, 112.5, s);
        s.amount = 1;
        s.inputPitch = 7;
        tuneCase ("PITCH +7 then tuned: 200 Hz -> D4", 200.0, hzOf (62), s);
        s.inputPitch = 0;
        s.amount = 0.5f;
        tuneCase ("amount 50%: halfway", hzOf (57.4), hzOf (57.2), s);
    }

    std::printf ("glide\n");
    {
        auto run = [] (float glide)
        {
            Run r (48000);
            Voice v { 48000 };
            TuneSettings s;
            s.glideMs = glide;
            s.mask = 1u << 9; // A
            r.render (2.0, s, [&] { return v.next (230.0); }, [&] (int at, TuneSettings& st, TuneEngine&)
            {
                if (at >= 48000) st.mask = 1u << 11; // B
            });
            const double lat = r.e.getLatency() / 48000.0;
            return std::make_pair (r.pitchAt (0.8), r.pitchAt (1.0 + lat + 0.05));
        };
        const auto hard = run (0), slow = run (300);
        check (std::abs (cents (hard.first, 220)) < 6, fmt ("on A3 before the change (%.2f Hz)", hard.first));
        check (std::abs (cents (hard.second, hzOf (59))) < 15, fmt ("glide 0: on B3 50 ms after (%.2f Hz)", hard.second));
        check (slow.second > 221 && slow.second < 240, fmt ("glide 300 ms: still on its way 50 ms after (%.2f Hz)", slow.second));
    }

    std::printf ("midi\n");
    {
        Run r (48000);
        Voice v { 48000 };
        TuneSettings s;
        s.mask = 1; // only C
        r.render (1.2, s, [&] { return v.next (290.0); }, [&] (int at, TuneSettings&, TuneEngine& e) { if (at == 0) e.noteOn (62, 0.8f); });
        const double got = r.pitchAt (0.9);
        check (std::abs (cents (got, hzOf (62))) < 6, fmt ("SNAP: a held D overrides the C-only scale (%.2f Hz)", got));
        r.render (0.8, s, [&] { return v.next (290.0); }, [&] (int at, TuneSettings&, TuneEngine& e) { if (at == (int) (1.2 * 48000)) e.noteOff (62); });
        const double after = r.pitchAt (1.75);
        check (std::abs (cents (after, hzOf (60))) < 6 || std::abs (cents (after, hzOf (72))) < 6 || std::abs (cents (after, hzOf (60 + 0))) < 6,
               fmt ("released: back to the scale's C (%.2f Hz)", after));
    }
    {
        Run r (48000);
        Voice v { 48000 };
        TuneSettings s;
        s.midiMode = 1;
        r.render (1.2, s, [&] { return v.next (200.0); }, [&] (int at, TuneSettings&, TuneEngine& e)
        {
            if (at == 0) { e.noteOn (57, 1.0f); e.noteOn (64, 1.0f); e.noteOn (69, 1.0f); }
        });
        const int at = (int) (0.8 * 48000), len = (int) (0.2 * 48000);
        const double a = goertzel (r.outL.data() + at, len, hzOf (57), 48000);
        const double e = goertzel (r.outL.data() + at, len, hzOf (64), 48000);
        const double a2 = goertzel (r.outL.data() + at, len, hzOf (69), 48000);
        const double src = goertzel (r.outL.data() + at, len, 200.0, 48000);
        check (a > 8 * src && e > 8 * src && a2 > 4 * src, fmt ("CHORD: A3, E4 and A4 sound, 200 Hz doesn't (%.4f %.4f vs %.5f)", a, e, src));
        uint32_t bits = r.e.display.held[1].load() | r.e.display.held[2].load();
        check (bits != 0, "held notes are reported to the display");
    }

    std::printf ("formants\n");
    {
        auto run = [] (float inPitch, float correct, float shift)
        {
            Run r (48000);
            Voice v { 48000 };
            v.formants = true;
            TuneSettings s;
            s.amount = 0;
            s.inputPitch = inPitch;
            s.formantCorrect = correct;
            s.formantShift = shift;
            r.render (1.0, s, [&] { return v.next (150.0); });
            const int at = (int) (0.7 * 48000), len = 4800;
            return std::make_pair (r.pitchAt (0.7), centroid (r.outL.data() + at, len, 48000));
        };
        const auto base = run (0, 1, 0);
        const auto up = run (0, 1, 7);
        const auto keep = run (12, 1, 0);
        const auto follow = run (12, 0, 0);
        check (std::abs (cents (up.first, 150)) < 6, fmt ("FORMANT +7 keeps the pitch (%.2f Hz)", up.first));
        check (up.second > base.second * 1.2, fmt ("FORMANT +7 raises the spectral centroid (%.0f -> %.0f Hz)", base.second, up.second));
        check (follow.second > keep.second * 1.3, fmt ("PITCH +12: formants follow at CORRECT 0 (%.0f Hz) more than at 100 (%.0f Hz)", follow.second, keep.second));
        check (keep.second < base.second * 1.4, fmt ("PITCH +12 at CORRECT 100 keeps the formants near (%.0f vs %.0f Hz)", keep.second, base.second));
    }

    std::printf ("levels and stereo\n");
    {
        auto level = [] (TuneSettings s, double hz)
        {
            Run r (48000);
            Voice v { 48000 };
            r.render (1.0, s, [&] { return v.next (hz); });
            const int at = (int) (0.6 * 48000), len = (int) (0.3 * 48000);
            return std::make_tuple (20 * std::log10 (rms (r.outL.data() + at, len) / rms (r.inL.data() + at, len)),
                                    r.outL, r.outR);
        };
        TuneSettings s;
        const double tuned = std::get<0> (level (s, 225));
        check (std::abs (tuned) < 3, fmt ("tuned output level %.1f dB", tuned));
        s.amount = 0;
        s.inputPitch = 12;
        const double up = std::get<0> (level (s, 225));
        s.inputPitch = -12;
        const double down = std::get<0> (level (s, 225));
        check (std::abs (up) < 4.5 && std::abs (down) < 4.5, fmt ("octave up %.1f dB, octave down %.1f dB", up, down));

        TuneSettings u;
        u.unison = 5;
        u.spread = 1;
        u.detuneCents = 20;
        auto [db, l, r] = level (u, 225);
        double lr = 0, ll = 0, rr = 0;
        for (size_t i = 30000; i < 45000; ++i) { lr += l[i] * r[i]; ll += l[i] * l[i]; rr += r[i] * r[i]; }
        const double corr = lr / std::sqrt (ll * rr);
        check (std::abs (db) < 4.5, fmt ("unison 5 level %.1f dB", db));
        check (corr < 0.9, fmt ("unison 5 with spread is stereo (L/R correlation %.2f)", corr));
    }

    std::printf ("voice range\n");
    {
        TuneSettings s;
        s.mask = 0b101010110101; // C major
        // 8% above each floor, and A4+40c, in every range.
        const float floors[] = { 70.0f, 100.0f, 150.0f, 220.0f };
        const char* names[] = { "BASS", "TENOR", "ALTO", "SOPRANO" };
        for (int i = 0; i < 4; ++i)
        {
            const double in = floors[i] * 1.08, inMidi = 69 + 12 * std::log2 (in / 440);
            Run r (48000, 512, floors[i]);
            Voice v { 48000 };
            r.render (1.2, s, [&] { return v.next (in); });
            const double got = r.pitchAt (0.9);
            // Nearest C major note to the input.
            int best = 0;
            for (int n = 30; n < 80; ++n)
                if (((s.mask >> (n % 12)) & 1) && std::abs (n - inMidi) < std::abs (best - inMidi)) best = n;
            check (got > 0 && std::abs (cents (got, hzOf (best))) < 6,
                   std::string (names[i]) + fmt (": %.1f Hz just above the floor tuned to %.2f Hz (want %.2f)", in, got, hzOf (best)));
            tuneCase ((std::string (names[i]) + ": A4+40c to A4").c_str(), hzOf (69.4), hzOf (69), s, 48000, floors[i]);
        }

        // Switching range while running: the latency follows, the dry path lines up with it, and it still tunes.
        Run r (48000, 512, 100.0f);
        Voice v { 48000 };
        TuneSettings m;
        m.mix = 0;
        r.render (0.5, m, [&] { return v.next (hzOf (69.4)); });
        const int before = r.e.getLatency();
        r.e.setMinPitch (220.0f);
        const int after = r.e.getLatency();
        const size_t from = r.outL.size();
        r.render (0.6, m, [&] { return v.next (hzOf (69.4)); });
        double err = 0;
        for (size_t i = from + (size_t) after + 100; i < r.outL.size(); ++i) err = std::max (err, (double) std::abs (r.outL[i] - r.inL[i - (size_t) after]));
        check (before == 1690 && after == 775 && err < 1e-6, fmt ("TENOR -> SOPRANO mid-stream: latency %.0f -> %.0f, dry delayed by the new latency (max error %.1e)", before, after, err));
        m.mix = 1;
        r.render (1.0, m, [&] { return v.next (hzOf (69.4)); });
        const double got = r.pitchAt (1.9);
        check (std::abs (cents (got, 440.0)) < 6, fmt ("and it tunes after the switch (%.2f Hz)", got));
        r.e.setMinPitch (70.0f);
        r.render (1.0, m, [&] { return v.next (hzOf (69.4)); });
        check (r.e.getLatency() == 2411 && std::abs (cents (r.pitchAt (2.9), 440.0)) < 6, fmt ("back down to BASS: %.0f samples, still tuned (%.2f Hz)", r.e.getLatency(), r.pitchAt (2.9)));
    }
    {
        // Below the floor: not a pass/fail on sound, just that it's safe and what the detector does.
        for (double hz : { 85.0, 92.0 })
        {
            Run r (48000, 512, 100.0f);
            Voice v { 48000 };
            TuneSettings s;
            r.render (1.0, s, [&] { return v.next (hz); });
            bool finite = true;
            for (float o : r.outL) finite = finite && std::isfinite (o);
            const double det = r.e.display.inHz.load();
            check (finite, fmt ("TENOR, %.0f Hz in (below the floor): finite; detector says %.1f Hz, output measures %.1f Hz", hz, det, r.pitchAt (0.8)));
        }
    }

    std::printf ("mix and safety\n");
    {
        Run r (48000);
        Voice v { 48000 };
        TuneSettings s;
        s.mix = 0;
        r.render (0.6, s, [&] { return v.next (220); });
        const int lat = r.e.getLatency();
        double err = 0;
        for (size_t i = (size_t) lat + 4000; i < r.outL.size(); ++i) err = std::max (err, (double) std::abs (r.outL[i] - r.inL[i - (size_t) lat]));
        check (err < 1e-6, fmt ("MIX 0 is the input delayed by the reported latency (max error %.1e)", err));

        TuneSettings w;
        w.mix = 1;
        Run q (48000);
        Voice v2 { 48000 };
        q.render (1.0, w, [&] { return v2.next (220); });
        // The wet signal lines up with the dry one within half a period.
        double bestC = -1;
        int bestLag = 0;
        for (int lag = lat - 120; lag <= lat + 120; ++lag)
        {
            double c = 0, e1 = 0, e2 = 0;
            for (int i = 30000; i < 40000; ++i) { c += q.outL[(size_t) i] * q.inL[(size_t) (i - lag)]; e1 += q.outL[(size_t) i] * q.outL[(size_t) i]; e2 += q.inL[(size_t) (i - lag)] * q.inL[(size_t) (i - lag)]; }
            c /= std::sqrt (e1 * e2);
            if (c > bestC) { bestC = c; bestLag = lag; }
        }
        check (bestC > 0.9 && std::abs (bestLag - lat) <= 110, fmt ("wet lines up with the dry signal (lag %.0f vs %.0f, corr %.2f)", bestLag, lat, bestC));

        Run z (48000);
        TuneSettings zs;
        zs.unison = 8;
        z.render (0.5, zs, [] { return 0.0f; });
        double peak = 0;
        for (float x : z.outL) peak = std::max (peak, (double) std::abs (x));
        check (peak < 1e-6, "silence in, silence out");

        Run x (48000, 333);
        TuneSettings xs;
        xs.inputPitch = 24;
        xs.formantShift = -12;
        xs.formantCorrect = 0;
        xs.unison = 8;
        xs.detuneCents = 100;
        xs.midiMode = 1;
        uint32_t seed = 3;
        Voice v3 { 48000 };
        bool finite = true;
        double maxOut = 0;
        x.render (2.0, xs, [&] { seed = seed * 1664525u + 1013904223u; return v3.next (100 + (seed >> 24)) * 0.8f + ((float) (seed >> 8) / 16777216.0f - 0.5f) * 0.2f; },
                  [&] (int at, TuneSettings& st, TuneEngine& e)
                  {
                      if (at % 9000 < 333) e.noteOn (40 + (at / 9000) % 40, 1.0f);
                      if (at % 13000 < 333) e.noteOff (40 + (at / 13000) % 40);
                      st.unison = 1 + (at / 20000) % 8;
                      st.midiMode = (at / 30000) % 2;
                  });
        for (float o : x.outL) { finite = finite && std::isfinite (o); maxOut = std::max (maxOut, (double) std::abs (o)); }
        check (finite && maxOut < 8, fmt ("extreme settings, odd block size, notes and mode changes: finite (peak %.2f)", maxOut));
    }

    std::printf ("speed\n");
    {
        Run r (48000);
        Voice v { 48000 };
        TuneSettings s;
        s.midiMode = 1;
        s.unison = 8;
        const auto t0 = std::chrono::steady_clock::now();
        r.render (5.0, s, [&] { return v.next (180); }, [&] (int at, TuneSettings&, TuneEngine& e)
        {
            if (at == 0) for (int n : { 48, 52, 55, 59, 62, 64, 67, 71 }) e.noteOn (n, 1.0f);
        });
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        check (secs < 2.5, fmt ("8 notes x 8 unison: 5 s of audio in %.2f s (%.0fx real time)", secs, 5.0 / secs));
        Run q (48000);
        Voice v2 { 48000 };
        TuneSettings one;
        const auto t1 = std::chrono::steady_clock::now();
        q.render (5.0, one, [&] { return v2.next (180); });
        const double s1 = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
        check (s1 < 0.5, fmt ("default settings: 5 s of audio in %.3f s (%.0fx real time)", s1, 5.0 / s1));
    }

    std::printf (failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
