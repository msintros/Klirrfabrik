#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

namespace
{
    constexpr float kCeiling = 0.96605088f;   // -0.3 dBFS

    String bandId (int band, const char* suffix) { return "b" + String (band + 1) + suffix; }

    constexpr float kDriveDbPerUnit = 4.f;   // Drive 0..10 -> 0..+40 dB of gain into the distortion
    float driveToGain (float drive) { return Decibels::decibelsToGain (drive * kDriveDbPerUnit); }

    // The amp models follow the Drive curve measured from the recordings instead of the generic dB-per-unit law.
    float driveGainFor (int type, float drive)
    {
        return mbd::isAmp (type) ? mbd::ampDriveGain (type, drive) : driveToGain (drive);
    }

    // Band level is in dB; the bottom of the range means "off".
    float levelToGain (float db) { return db <= -59.9f ? 0.f : Decibels::decibelsToGain (db); }

    // Tape was inserted at the top of the type list, which shifted every older index up by one. Anything
    // saved before that has no "oversample" entry, so use that to recognise it and shift the types.
    void migrateLegacyTypes (ValueTree& state)
    {
        if (state.getChildWithProperty ("id", "oversample").isValid()) return;

        for (int b = 1; b <= 4; ++b)
        {
            auto p = state.getChildWithProperty ("id", "b" + String (b) + "Type");
            if (p.isValid())
                p.setProperty ("value", (float) p.getProperty ("value") + 1.f, nullptr);
        }
    }
}

//==============================================================================
StringArray MBDProcessor::getDistortionNames()
{
    // Same order as mbd::DistType.
    return { "Tape", "Tube", "Log Soft", "Sine Fold", "Tri Fold", "Hard Clip", "Slew Limiter",
             "Decimator", "Bitcrusher", "Fractal", "Tanh", "BJT Pair",
             "Diode", "Rectifier", "Crossover", "Chebyshev", "Amp Crunch", "Amp Lead" };
}

StringArray MBDProcessor::getOversamplingNames() { return { "Off", "2x", "4x", "8x" }; }

StringArray MBDProcessor::getSpectrumColourNames() { return { "Red", "Blue", "Yellow", "White", "Green" }; }

Colour MBDProcessor::getSpectrumColour (int index)
{
    static const Colour cols[] = { Colour (0xffff3b30), Colour (0xff3b9cff), Colour (0xffffd60a),
                                   Colour (0xffffffff), Colour (0xff32d74b) };
    return cols[jlimit (0, (int) std::size (cols) - 1, index)];
}

AudioProcessorValueTreeState::ParameterLayout MBDProcessor::createLayout()
{
    AudioProcessorValueTreeState::ParameterLayout l;
    auto pid = [] (const String& s) { return ParameterID { s, 1 }; };

    auto addFloat = [&] (const String& id, const String& name, NormalisableRange<float> r, float def)
    {
        l.add (std::make_unique<AudioParameterFloat> (pid (id), name, r, def));
    };

    // global
    addFloat ("inGain",  "Input Gain",  { -24.f, 24.f, 0.01f }, 0.f);
    addFloat ("outGain", "Output Gain", { -24.f, 12.f, 0.01f }, 0.f);

    // crossovers
    const float xdef[3] = { 150.f, 1000.f, 5000.f };
    for (int i = 0; i < 3; ++i)
    {
        NormalisableRange<float> r (20.f, 20000.f, 0.1f);
        r.setSkewForCentre (1000.f);
        addFloat ("xover" + String (i + 1), "Crossover " + String (i + 1), r, xdef[i]);
    }

    // noise gate
    addFloat ("gateAmount", "Gate Amount",    { 0.f, 1.f, 0.001f }, 0.f);
    addFloat ("gateThresh", "Gate Threshold", { -80.f, 0.f, 0.1f }, -50.f);
    { NormalisableRange<float> r (0.1f, 100.f, 0.01f);  r.setSkewForCentre (5.f);
      addFloat ("gateAttack",  "Gate Attack",  r, 2.f); }
    { NormalisableRange<float> r (5.f, 1000.f, 0.1f);   r.setSkewForCentre (100.f);
      addFloat ("gateRelease", "Gate Release", r, 100.f); }
    l.add (std::make_unique<AudioParameterBool> (pid ("gateOn"), "Gate On", true));

    // bands
    const int typeDef[4] = { mbd::Tube, mbd::Tanh, mbd::SineFold, mbd::LogSoft };
    for (int b = 0; b < 4; ++b)
    {
        addFloat (bandId (b, "Drive"), "Band " + String (b + 1) + " Drive", { 0.f, 10.f, 0.01f }, 0.f);
        l.add (std::make_unique<AudioParameterBool> (pid (bandId (b, "Mute")), "Band " + String (b + 1) + " Mute", false));
        l.add (std::make_unique<AudioParameterBool> (pid (bandId (b, "Solo")), "Band " + String (b + 1) + " Solo", false));
        l.add (std::make_unique<AudioParameterChoice> (pid (bandId (b, "Type")), "Band " + String (b + 1) + " Type",
                                                       getDistortionNames(), typeDef[b]));
        addFloat (bandId (b, "Mod"), "Band " + String (b + 1) + " Mod", { 0.f, 1.f, 0.001f }, 0.f);
        addFloat (bandId (b, "Mix"), "Band " + String (b + 1) + " Mix", { 0.f, 1.f, 0.001f }, 1.f);
        addFloat (bandId (b, "Level"), "Band " + String (b + 1) + " Level", { -60.f, 12.f, 0.01f }, 0.f);
    }

    // post filter
    l.add (std::make_unique<AudioParameterChoice> (pid ("pfType"), "Filter Type",
                                                   StringArray { "Lowpass", "Highpass", "Bandpass", "Notch" }, 0));
    addFloat ("pfMix", "Filter Mix", { 0.f, 1.f, 0.001f }, 1.f);
    { NormalisableRange<float> r (20.f, 20000.f, 0.1f);  r.setSkewForCentre (1000.f);
      addFloat ("pfCutoff", "Filter Cutoff", r, 20000.f); }
    addFloat ("pfRes", "Filter Resonance", { 0.f, 1.f, 0.001f }, 0.f);
    { NormalisableRange<float> r (0.3f, 12.f, 0.01f);    r.setSkewForCentre (1.f);
      addFloat ("pfQ", "Filter Q", r, 0.707f); }

    l.add (std::make_unique<AudioParameterBool> (pid ("pfOn"), "Filter On", true));

    // UI state that the audio thread needs
    l.add (std::make_unique<AudioParameterBool> (pid ("specPost"), "Spectrum Post", false));

    // Settings. Saved with the session, but not automatable and not changed by loading a preset.
    const auto fixed = AudioParameterChoiceAttributes().withAutomatable (false);
    l.add (std::make_unique<AudioParameterChoice> (pid ("oversample"), "Oversampling", getOversamplingNames(), 1, fixed));
    l.add (std::make_unique<AudioParameterChoice> (pid ("specColour"), "Spectrum Colour", getSpectrumColourNames(), 0, fixed));

    return l;
}

//==============================================================================
MBDProcessor::MBDProcessor()
    : AudioProcessor (BusesProperties().withInput  ("Input",  AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "MBD_STATE", createLayout())
{
    inGainP  = apvts.getRawParameterValue ("inGain");
    outGainP = apvts.getRawParameterValue ("outGain");
    for (int i = 0; i < 3; ++i) xoverP[i] = apvts.getRawParameterValue ("xover" + String (i + 1));

    gateAmtP = apvts.getRawParameterValue ("gateAmount");
    gateThrP = apvts.getRawParameterValue ("gateThresh");
    gateAtkP = apvts.getRawParameterValue ("gateAttack");
    gateRelP = apvts.getRawParameterValue ("gateRelease");
    gateOnP  = apvts.getRawParameterValue ("gateOn");
    pfOnP    = apvts.getRawParameterValue ("pfOn");

    for (int b = 0; b < numBands; ++b)
    {
        bandP[b].drive = apvts.getRawParameterValue (bandId (b, "Drive"));
        bandP[b].mute  = apvts.getRawParameterValue (bandId (b, "Mute"));
        bandP[b].solo  = apvts.getRawParameterValue (bandId (b, "Solo"));
        bandP[b].type  = apvts.getRawParameterValue (bandId (b, "Type"));
        bandP[b].mod   = apvts.getRawParameterValue (bandId (b, "Mod"));
        bandP[b].mix   = apvts.getRawParameterValue (bandId (b, "Mix"));
        bandP[b].level = apvts.getRawParameterValue (bandId (b, "Level"));
        bandLevel[(size_t) b].store (0.f);
    }

    pfTypeP   = apvts.getRawParameterValue ("pfType");
    pfMixP    = apvts.getRawParameterValue ("pfMix");
    pfCutP    = apvts.getRawParameterValue ("pfCutoff");
    pfResP    = apvts.getRawParameterValue ("pfRes");
    pfQP      = apvts.getRawParameterValue ("pfQ");
    specPostP = apvts.getRawParameterValue ("specPost");
    osP       = apvts.getRawParameterValue ("oversample");
}

bool MBDProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    return (out == AudioChannelSet::mono() || out == AudioChannelSet::stereo()) && out == l.getMainInputChannelSet();
}

void MBDProcessor::getCrossovers (float& f1, float& f2, float& f3) const
{
    f1 = jlimit (20.f, 12800.f, xoverP[0]->load());
    f2 = jlimit (f1 * 1.25f, 16000.f, xoverP[1]->load());
    f3 = jlimit (f2 * 1.25f, 20000.f, xoverP[2]->load());
}

void MBDProcessor::copyLatestSamples (float* dest, int n) const
{
    const int pos = ringPos.load();
    for (int i = 0; i < n; ++i)
        dest[i] = ring[(size_t) ((pos - n + i) & (ringSize - 1))];
}

//==============================================================================
void MBDProcessor::initSmoothers()
{
    const double ramp = 0.02;
    auto init = [&] (SmoothedValue<float>& s, float v) { s.reset (sr, ramp); s.setCurrentAndTargetValue (v); };

    init (inSm,  Decibels::decibelsToGain (inGainP->load()));
    init (outSm, Decibels::decibelsToGain (outGainP->load()));
    init (pfMixSm, pfOnP->load() > 0.5f ? pfMixP->load() : 0.f);
    init (pfResSm, pfResP->load());
    pfCutSm.reset (sr, ramp); pfCutSm.setCurrentAndTargetValue (jmax (20.f, pfCutP->load()));
    pfQSm.reset (sr, ramp);   pfQSm.setCurrentAndTargetValue (jmax (0.3f, pfQP->load()));

    for (int b = 0; b < numBands; ++b)
    {
        driveSm[b].reset (sr, ramp);
        driveSm[b].setCurrentAndTargetValue (driveGainFor (jlimit (0, (int) mbd::NumTypes - 1, (int) bandP[b].type->load()),
                                                           bandP[b].drive->load()));
        compSm[b].reset (sr, ramp);
        compSm[b].setCurrentAndTargetValue (lookupComp ((int) bandP[b].type->load(), bandP[b].mod->load(),
                                                        bandP[b].drive->load()));
        init (mixSm[b],  bandP[b].mix->load());
        init (modSm[b],  bandP[b].mod->load());
        init (levelSm[b], levelToGain (bandP[b].level->load()));
        muteSm[b].reset (sr, 0.005);
        muteSm[b].setCurrentAndTargetValue (1.f);
    }
}

// Switches the distortion section to a new oversampling factor: re-tunes everything that runs at the
// oversampled rate and clears its state. Called from prepareToPlay and, on a settings change, from the
// audio thread (everything it touches is pre-allocated; only the host's latency update is deferred).
void MBDProcessor::applyOversampling (int newIdx, bool fromAudioThread)
{
    osIdx = jlimit (0, 3, newIdx);
    osFac = 1 << osIdx;
    const double osSr = sr * (double) osFac;

    dsp::ProcessSpec spec { osSr, (uint32) (kChunk * osFac), 2 };
    for (auto* f : { &split1, &split2, &split3, &ap12, &ap13, &ap23 })
    {
        f->prepare (spec);
        f->reset();
    }
    for (auto* f : { &ap12, &ap13, &ap23 })
        f->setType (dsp::LinkwitzRileyFilterType::allpass);

    for (int b = 0; b < numBands; ++b)
        for (int c = 0; c < 2; ++c)
        {
            dist[b][c] = {};
            dist[b][c].osRatio = (float) osFac;
            dcX[b][c] = dcY[b][c] = 0.f;
        }

    dcR = 1.f - (MathConstants<float>::twoPi * 10.f / (float) osSr);   // ~10 Hz DC blocker

    int latency = 0;
    if (osIdx > 0)
    {
        auto& os = *oversamplers[(size_t) osIdx - 1];
        os.reset();
        latency = (int) std::ceil (os.getLatencyInSamples());
    }

    pendingLatency.store (latency);
    if (fromAudioThread) triggerAsyncUpdate();
    else                 setLatencySamples (latency);
}

void MBDProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    sr = sampleRate;

    if (compSr != sampleRate)
    {
        buildCompTable (sampleRate);
        compSr = sampleRate;
    }

    // Linear-phase FIR half-band stages with integer latency, so host delay compensation stays exact.
    for (int order = 1; order <= 3; ++order)
    {
        auto os = std::make_unique<dsp::Oversampling<float>> (2, (size_t) order,
                                                               dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                                               true, true);
        os->initProcessing ((size_t) kChunk);
        oversamplers[(size_t) order - 1] = std::move (os);
    }

    initSmoothers();
    applyOversampling ((int) osP->load(), false);

    gEnv       = 0.f; gGain = 1.f; gOpen = false;
    gEnvRel    = std::exp (-1.f / (0.015f * (float) sr));
    pfIc1[0] = pfIc1[1] = pfIc2[0] = pfIc2[1] = 0.f;
    limGain    = 1.f;
    limRelStep = 1.f - std::exp (-1.f / (0.08f * (float) sr));

    ring.fill (0.f);
    ringW = 0;
    ringPos.store (0);
}

//==============================================================================
// Drive compensation. A reference 1 kHz sine at -10.5 dBFS peak is run through every distortion at every
// Drive/Mod grid point; the stored gain brings the AC RMS of the result back to the RMS of the input.
// Static (it only changes when a knob does), so there is no pumping, unlike a live auto-gain.
void MBDProcessor::buildCompTable (double fs)
{
    const float f      = (float) fs;
    const int   warm   = (int) (0.010f * f);
    const int   meas   = (int) (0.020f * f);                    // 20 periods of the reference tone
    constexpr float amp = 0.3f, freq = 1000.f;
    const float w      = MathConstants<float>::twoPi * freq / f;
    const float inRms  = amp * 0.70710678f;

    for (int t = 0; t < (int) mbd::NumTypes; ++t)
        for (int m = 0; m < kModSteps; ++m)
            for (int d = 0; d < kDriveSteps; ++d)
            {
                mbd::DistState st {};
                const float mod = (float) m / (float) (kModSteps - 1);
                const float g   = driveGainFor (t, 10.f * (float) d / (float) (kDriveSteps - 1));

                double sum = 0.0, sum2 = 0.0;
                for (int i = 0; i < warm + meas; ++i)
                {
                    const float y = mbd::process (t, amp * std::sin (w * (float) i) * g, mod, st, f);
                    if (i >= warm) { sum += y; sum2 += (double) y * y; }
                }
                const double mean = sum / meas;
                const double rms  = std::sqrt (jmax (1.0e-12, sum2 / meas - mean * mean));
                compTable[t][m][d] = jlimit (0.005f, 8.f, (float) (inRms / rms));
            }
}

float MBDProcessor::lookupComp (int type, float mod, float drive) const noexcept
{
    type = jlimit (0, (int) mbd::NumTypes - 1, type);
    const float mp = jlimit (0.f, 1.f, mod) * (float) (kModSteps - 1);
    const float dp = jlimit (0.f, 10.f, drive) * (float) (kDriveSteps - 1) / 10.f;
    const int   m0 = jmin ((int) mp, kModSteps - 2),   d0 = jmin ((int) dp, kDriveSteps - 2);
    const float mf = mp - (float) m0,                  df = dp - (float) d0;

    const auto& T = compTable[type];
    const float a = T[m0][d0]     + (T[m0][d0 + 1]     - T[m0][d0])     * df;
    const float b = T[m0 + 1][d0] + (T[m0 + 1][d0 + 1] - T[m0 + 1][d0]) * df;
    const float c = a + (b - a) * mf;
    return c > 0.f ? c : 1.f;    // an empty table (never prepared) means "no compensation"
}

//==============================================================================
// Signal flow, per chunk of at most kChunk samples:
//   A (base rate)        input gain -> spectrum tap -> noise gate; band parameters are smoothed here
//   B (sr * osFac)       upsample -> 4-band split -> Drive/distortion/Mix/Level/mute -> sum -> downsample
//   C (base rate)        post filter -> output gain -> -0.3 dB limiter -> spectrum tap
void MBDProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    const int nc = jmin (2, buffer.getNumChannels());
    const int n  = buffer.getNumSamples();
    if (nc == 0 || sr <= 0.0 || oversamplers[0] == nullptr) return;   // not prepared yet

    for (int c = nc; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);

    // oversampling setting changed?
    const int wantOs = jlimit (0, 3, (int) osP->load());
    if (wantOs != osIdx) applyOversampling (wantOs, true);

    float* host[2] = { buffer.getWritePointer (0), nc > 1 ? buffer.getWritePointer (1) : nullptr };
    const float fs   = (float) sr;
    const float osFs = fs * (float) osFac;

    // ---------- block-rate parameter handling ----------
    inSm.setTargetValue  (Decibels::decibelsToGain (inGainP->load()));
    outSm.setTargetValue (Decibels::decibelsToGain (outGainP->load()));

    float f1, f2, f3;
    getCrossovers (f1, f2, f3);
    const float fMax = 0.49f * osFs;
    split1.setCutoffFrequency (jmin (f1, fMax));
    split2.setCutoffFrequency (jmin (f2, fMax));
    split3.setCutoffFrequency (jmin (f3, fMax));
    ap12.setCutoffFrequency (jmin (f2, fMax));
    ap13.setCutoffFrequency (jmin (f3, fMax));
    ap23.setCutoffFrequency (jmin (f3, fMax));

    bool anySolo = false;
    for (int b = 0; b < numBands; ++b) anySolo = anySolo || bandP[b].solo->load() > 0.5f;

    int   type[numBands];
    float bandPk[numBands] {};
    for (int b = 0; b < numBands; ++b)
    {
        type[b] = jlimit (0, (int) mbd::NumTypes - 1, (int) bandP[b].type->load());
        const float drive = bandP[b].drive->load();
        driveSm[b].setTargetValue (driveGainFor (type[b], drive));
        compSm[b].setTargetValue  (lookupComp (type[b], bandP[b].mod->load(), drive));
        mixSm[b].setTargetValue   (bandP[b].mix->load());
        modSm[b].setTargetValue   (bandP[b].mod->load());
        levelSm[b].setTargetValue (levelToGain (bandP[b].level->load()));
        const bool audible = bandP[b].mute->load() < 0.5f && (! anySolo || bandP[b].solo->load() > 0.5f);
        muteSm[b].setTargetValue (audible ? 1.f : 0.f);
    }

    const bool  gateOn = gateOnP->load() > 0.5f;
    const float gThr   = Decibels::decibelsToGain (gateThrP->load());
    const float gFloor = Decibels::decibelsToGain (-80.f * gateAmtP->load());
    const float gAtk   = std::exp (-1.f / (jmax (0.0001f, gateAtkP->load() * 0.001f) * fs));
    const float gRel   = std::exp (-1.f / (jmax (0.0001f, gateRelP->load() * 0.001f) * fs));

    const int   pfType = jlimit (0, 3, (int) pfTypeP->load());
    pfMixSm.setTargetValue (pfOnP->load() > 0.5f ? pfMixP->load() : 0.f);
    pfResSm.setTargetValue (pfResP->load());
    pfCutSm.setTargetValue (jlimit (20.f, 0.45f * fs, pfCutP->load()));
    pfQSm.setTargetValue   (jmax (0.3f, pfQP->load()));

    const bool specPost = specPostP->load() > 0.5f;

    // ---------- chunk loop ----------
    for (int off = 0; off < n; off += kChunk)
    {
        const int cnt = jmin (kChunk, n - off);
        float* sc[2] = { scratch.getWritePointer (0), scratch.getWritePointer (1) };

        // ---- A: front end at the base rate ----
        for (int i = 0; i < cnt; ++i)
        {
            const float inG = inSm.getNextValue();
            float x[2] = { 0.f, 0.f };
            float pk = 0.f;
            for (int ch = 0; ch < nc; ++ch)
            {
                x[ch] = host[ch][off + i] * inG;
                pk = jmax (pk, std::abs (x[ch]));
            }

            if (! specPost)
                pushSpectrum (nc == 2 ? 0.5f * (x[0] + x[1]) : x[0]);

            // noise gate (linked, with 3 dB hysteresis)
            gEnv = jmax (pk, gEnv * gEnvRel);
            if (! gOpen && gEnv > gThr)             gOpen = true;
            else if (gOpen && gEnv < gThr * 0.708f) gOpen = false;
            const float gTarget = (! gateOn || gOpen) ? 1.f : gFloor;
            const float gC      = gTarget > gGain ? gAtk : gRel;
            gGain = gTarget + (gGain - gTarget) * gC;

            for (int ch = 0; ch < nc; ++ch) sc[ch][i] = x[ch] * gGain;

            for (int b = 0; b < numBands; ++b)
            {
                prm[b][pDrive][i] = driveSm[b].getNextValue();
                prm[b][pComp][i]  = compSm[b].getNextValue();
                prm[b][pMod][i]   = modSm[b].getNextValue();
                prm[b][pMix][i]   = mixSm[b].getNextValue();
                prm[b][pLevel][i] = levelSm[b].getNextValue();
                prm[b][pMute][i]  = muteSm[b].getNextValue();
            }
        }
        if (nc == 1) FloatVectorOperations::clear (sc[1], cnt);

        // ---- B: split, distort and recombine at sr * osFac ----
        dsp::AudioBlock<float> base = dsp::AudioBlock<float> (scratch).getSubBlock (0, (size_t) cnt);
        dsp::AudioBlock<float> up;
        float* d[2] = { sc[0], sc[1] };

        if (osIdx > 0)
        {
            up = oversamplers[(size_t) osIdx - 1]->processSamplesUp (dsp::AudioBlock<const float> (base));
            d[0] = up.getChannelPointer (0);
            d[1] = up.getChannelPointer (1);
        }

        for (int i = 0; i < cnt; ++i)
        {
            for (int k = 0; k < osFac; ++k)
            {
                const int j = i * osFac + k;

                for (int ch = 0; ch < nc; ++ch)
                {
                    float lo1, hi1, lo2, hi2, lo3, hi3;
                    split1.processSample (ch, d[ch][j], lo1, hi1);
                    split2.processSample (ch, hi1, lo2, hi2);
                    split3.processSample (ch, hi2, lo3, hi3);

                    const float bnd[numBands] = { ap13.processSample (ch, ap12.processSample (ch, lo1)),
                                                  ap23.processSample (ch, lo2),
                                                  lo3,
                                                  hi3 };

                    float sum = 0.f;
                    for (int b = 0; b < numBands; ++b)
                    {
                        const float dr = bnd[b] * prm[b][pDrive][i];
                        const float y  = mbd::process (type[b], dr, prm[b][pMod][i], dist[b][ch], osFs)
                                           * prm[b][pComp][i];                  // level-compensated

                        const float dc = y - dcX[b][ch] + dcR * dcY[b][ch];     // remove DC from asymmetric shapers
                        dcX[b][ch] = y;
                        dcY[b][ch] = dc;

                        const float wet = bnd[b] + (dc - bnd[b]) * prm[b][pMix][i];   // Mix: clean band <-> distorted band
                        const float o   = wet * prm[b][pLevel][i];                    // Level: final output of the band
                        bandPk[b] = jmax (bandPk[b], std::abs (o));
                        sum += o * prm[b][pMute][i];
                    }
                    d[ch][j] = sum;
                }
            }
        }

        if (osIdx > 0)
            oversamplers[(size_t) osIdx - 1]->processSamplesDown (base);

        // ---- C: post filter, output gain and limiter at the base rate ----
        for (int i = 0; i < cnt; ++i)
        {
            float sum[2] = { sc[0][i], nc > 1 ? sc[1][i] : 0.f };

            // post filter (TPT state-variable)
            {
                const float cut = pfCutSm.getNextValue();
                const float q   = pfQSm.getNextValue();
                const float res = pfResSm.getNextValue();
                const float mix = pfMixSm.getNextValue();

                const float gg = std::tan (MathConstants<float>::pi * cut / fs);
                const float k  = 1.f / q;
                const float a1 = 1.f / (1.f + gg * (gg + k));
                const float a2 = gg * a1;
                const float a3 = gg * a2;

                for (int ch = 0; ch < nc; ++ch)
                {
                    const float v3 = sum[ch] - pfIc2[ch];
                    const float v1 = a1 * pfIc1[ch] + a2 * v3;
                    const float v2 = pfIc2[ch] + a2 * pfIc1[ch] + a3 * v3;
                    pfIc1[ch] = 2.f * v1 - pfIc1[ch];
                    pfIc2[ch] = 2.f * v2 - pfIc2[ch];

                    const float low  = v2;
                    const float bp   = k * v1;                    // unity-gain band-pass
                    const float high = sum[ch] - k * v1 - v2;

                    float f = pfType == 0 ? low : pfType == 1 ? high : pfType == 2 ? bp : (low + high);
                    f += res * 2.f * bp;                          // resonance = extra band-pass emphasis at cutoff
                    sum[ch] += (f - sum[ch]) * mix;
                }
            }

            // output gain + invisible -0.3 dB brickwall limiter (instant attack, 80 ms release)
            const float og = outSm.getNextValue();
            float y[2] = { 0.f, 0.f };
            float pk2 = 0.f;
            for (int ch = 0; ch < nc; ++ch)
            {
                y[ch] = sum[ch] * og;
                pk2 = jmax (pk2, std::abs (y[ch]));
            }

            const float needed = pk2 > kCeiling ? kCeiling / pk2 : 1.f;
            limGain += (1.f - limGain) * limRelStep;
            limGain = jmin (limGain, needed);

            for (int ch = 0; ch < nc; ++ch)
            {
                float v = y[ch] * limGain;
                if (! std::isfinite (v)) v = 0.f;
                host[ch][off + i] = jlimit (-kCeiling, kCeiling, v);
            }

            if (specPost)
                pushSpectrum (nc == 2 ? 0.5f * (host[0][off + i] + host[1][off + i]) : host[0][off + i]);
        }
    }

    ringPos.store (ringW);
    for (int b = 0; b < numBands; ++b)
        bandLevel[(size_t) b].store (bandPk[b]);
}

//==============================================================================
AudioProcessorEditor* MBDProcessor::createEditor() { return new MBDEditor (*this); }

void MBDProcessor::getStateInformation (MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void MBDProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = ValueTree::fromXml (*xml);
            migrateLegacyTypes (vt);
            apvts.replaceState (vt);
        }
}

File MBDProcessor::getPresetDirectory()
{
    const auto base = File::getSpecialLocation (File::userApplicationDataDirectory);
    auto d = base.getChildFile ("Klirrfabrik").getChildFile ("Presets");

    if (! d.exists())   // first run under the new name: bring presets over from the old folder
    {
        const auto old = base.getChildFile ("MultiBandDistortion").getChildFile ("Presets");
        if (old.isDirectory()) old.copyDirectoryTo (d);
    }
    d.createDirectory();
    return d;
}

bool MBDProcessor::savePresetToFile (const File& f)
{
    if (auto xml = apvts.copyState().createXml())
        return xml->writeTo (f);
    return false;
}

bool MBDProcessor::loadPresetFromFile (const File& f)
{
    if (auto xml = XmlDocument::parse (f))
        if (xml->hasTagName (apvts.state.getType()))
        {
            // Oversampling and spectrum colour are settings of this installation, not part of a sound.
            auto* osParam  = apvts.getParameter ("oversample");
            auto* colParam = apvts.getParameter ("specColour");
            const float keepOs = osParam->getValue(), keepCol = colParam->getValue();

            auto vt = ValueTree::fromXml (*xml);
            migrateLegacyTypes (vt);
            apvts.replaceState (vt);

            osParam->setValueNotifyingHost (keepOs);
            colParam->setValueNotifyingHost (keepCol);
            return true;
        }
    return false;
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new MBDProcessor(); }
