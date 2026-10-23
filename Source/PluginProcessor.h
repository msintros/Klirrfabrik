#pragma once
#include "Common.h"
#include "Distortions.h"
#include <array>
#include <atomic>

class MBDProcessor : public juce::AudioProcessor,
                     private juce::AsyncUpdater
{
public:
    static constexpr int numBands = 4;
    static constexpr int ringSize = 16384;   // power of two
    static constexpr int kChunk   = 512;     // the audio callback is processed in pieces of at most this many samples

    MBDProcessor();
    ~MBDProcessor() override { cancelPendingUpdate(); }

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Klirrfabrik"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- shared with the editor ----
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    static juce::StringArray getDistortionNames();
    static juce::StringArray getOversamplingNames();
    static juce::StringArray getSpectrumColourNames();
    static juce::Colour getSpectrumColour (int index);

    std::array<std::atomic<float>, numBands> bandLevel {};          // linear peak per band, for the meters
    void getCrossovers (float& f1, float& f2, float& f3) const;     // effective (ordered/clamped) values
    void copyLatestSamples (float* dest, int n) const;              // for the spectrum view

    static juce::File getPresetDirectory();
    bool savePresetToFile (const juce::File&);
    bool loadPresetFromFile (const juce::File&);

private:
    void handleAsyncUpdate() override { setLatencySamples (pendingLatency.load()); }

    // ---- parameter pointers ----
    std::atomic<float> *inGainP, *outGainP, *xoverP[3];
    std::atomic<float> *gateAmtP, *gateThrP, *gateAtkP, *gateRelP, *gateOnP, *pfOnP;
    std::atomic<float> *pfTypeP, *pfMixP, *pfCutP, *pfResP, *pfQP, *specPostP, *osP;
    struct BandP { std::atomic<float> *drive, *mute, *solo, *type, *mod, *mix, *level; } bandP[numBands];

    double sr = 44100.0;

    // ---- oversampling ----
    // The whole split / distort / recombine section runs at sr * osFac. Index 0 = off, 1..3 = 2x/4x/8x.
    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers;
    int osIdx = 0, osFac = 1;
    std::atomic<int> pendingLatency { 0 };
    void applyOversampling (int newIdx, bool fromAudioThread);

    // ---- crossover (Linkwitz-Riley 4th order, cascaded split with allpass phase alignment) ----
    juce::dsp::LinkwitzRileyFilter<float> split1, split2, split3, ap12, ap13, ap23;

    // ---- smoothers (run at the base sample rate) ----
    juce::SmoothedValue<float> inSm, outSm, pfMixSm, pfResSm;
    juce::SmoothedValue<float> modSm[numBands], mixSm[numBands], levelSm[numBands], muteSm[numBands];
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> driveSm[numBands], compSm[numBands];
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> pfCutSm, pfQSm;

    // Per-base-sample band parameters for one chunk, filled before oversampling and read after it.
    enum { pDrive = 0, pComp, pMod, pMix, pLevel, pMute, numPrm };
    float prm[numBands][numPrm][kChunk] {};
    juce::AudioBuffer<float> scratch { 2, kChunk };

    // ---- per band state ----
    mbd::DistState dist[numBands][2];
    float dcX[numBands][2] {}, dcY[numBands][2] {};
    float dcR = 0.999f;

    // ---- noise gate ----
    float gEnv = 0.f, gGain = 1.f, gEnvRel = 0.999f;
    bool  gOpen = false;

    // ---- post filter (TPT state-variable) ----
    float pfIc1[2] {}, pfIc2[2] {};

    // ---- output limiter ----
    float limGain = 1.f, limRelStep = 0.001f;

    // ---- spectrum ring buffer ----
    std::array<float, ringSize> ring {};
    int ringW = 0;
    std::atomic<int> ringPos { 0 };
    void pushSpectrum (float v) noexcept { ring[(size_t) (ringW++ & (ringSize - 1))] = v; }

    void initSmoothers();

    // ---- static drive compensation ----
    // For every (type, mod, drive) the table holds the gain that makes a reference sine come out of the
    // distortion at the same RMS it went in at. Built once per sample rate, never touched by the audio thread
    // except to read it.
    static constexpr int kDriveSteps = 21;   // Drive 0..10 in steps of 0.5
    static constexpr int kModSteps   = 9;    // Mod 0..1 in steps of 1/8
    float compTable[mbd::NumTypes][kModSteps][kDriveSteps] {};
    double compSr = 0.0;
    void  buildCompTable (double fs);
    float lookupComp (int type, float mod, float drive) const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MBDProcessor)
};
