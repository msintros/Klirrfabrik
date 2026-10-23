#include "PluginEditor.h"

using namespace juce;

MBDEditor::MBDEditor (MBDProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      inKnob  (p.apvts, "inGain",  "INPUT",  " dB", 1),
      outKnob (p.apvts, "outGain", "OUTPUT", " dB", 1),
      spectrum (p),
      gateAmount  (p.apvts, "gateAmount",  "Amount",    "",     2),
      gateThresh  (p.apvts, "gateThresh",  "Threshold", " dB",  1),
      gateAttack  (p.apvts, "gateAttack",  "Attack",    " ms",  1),
      gateRelease (p.apvts, "gateRelease", "Release",   " ms",  0),
      pfMix    (p.apvts, "pfMix",    "Mix",       "",    2),
      pfCutoff (p.apvts, "pfCutoff", "Cutoff",    " Hz", 0),
      pfRes    (p.apvts, "pfRes",    "Resonance", "",    2),
      pfQ      (p.apvts, "pfQ",      "Q",         "",    2),
      settingsPanel (p)
{
    setLookAndFeel (&lnf);

    title.setText ("KLIRRFABRIK", dontSendNotification);
    title.setFont (Font (FontOptions (18.f, Font::bold)));
    title.setColour (Label::textColourId, Colour (0xffe5383b));
    addAndMakeVisible (title);

    // presets
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this]
    {
        const int idx = presetBox.getSelectedItemIndex();
        if (isPositiveAndBelow (idx, presetFiles.size()))
            proc.loadPresetFromFile (presetFiles[idx]);
    };
    saveBtn.onClick = [this] { savePreset(); };
    loadBtn.onClick = [this] { loadPreset(); };
    settingsBtn.onClick = [this]
    {
        settingsPanel.setVisible (! settingsPanel.isVisible());
        if (settingsPanel.isVisible()) { settingsPanel.refresh(); settingsPanel.toFront (false); }
    };
    addAndMakeVisible (presetBox);
    addAndMakeVisible (saveBtn);
    addAndMakeVisible (loadBtn);
    addAndMakeVisible (settingsBtn);
    refreshPresetList();

    // display row
    addAndMakeVisible (inKnob);
    addAndMakeVisible (outKnob);
    addAndMakeVisible (spectrum);

    // bands
    const Colour cols[4] = { Colour (0xff4da3ff), Colour (0xff3ddc84), Colour (0xffe8c33a), Colour (0xffb57bff) };
    for (int b = 0; b < 4; ++b)
    {
        columns[(size_t) b] = std::make_unique<BandColumn> (proc, b, cols[b]);
        addAndMakeVisible (*columns[(size_t) b]);
    }
    const char* xl[3] = { "1 | 2", "2 | 3", "3 | 4" };
    for (int i = 0; i < 3; ++i)
    {
        xoverKnobs[(size_t) i] = std::make_unique<ParamKnob> (proc.apvts, "xover" + String (i + 1), xl[i], " Hz", 0);
        addAndMakeVisible (*xoverKnobs[(size_t) i]);
    }

    // gate
    gateTitle.setText ("NOISE GATE", dontSendNotification);
    gateTitle.setJustificationType (Justification::centred);
    addAndMakeVisible (gateTitle);
    gateOn.setClickingTogglesState (true);
    gateOn.setColour (TextButton::buttonOnColourId, Colour (0xff3ddc84));
    gateOn.setColour (TextButton::textColourOnId, Colours::black);
    addAndMakeVisible (gateOn);
    gateOnAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "gateOn", gateOn);
    gateOnRaw = proc.apvts.getRawParameterValue ("gateOn");
    for (auto* k : { &gateAmount, &gateThresh, &gateAttack, &gateRelease }) addAndMakeVisible (*k);

    // post filter
    pfTitle.setText ("POST FILTER", dontSendNotification);
    pfTitle.setJustificationType (Justification::centred);
    addAndMakeVisible (pfTitle);
    pfOn.setClickingTogglesState (true);
    pfOn.setColour (TextButton::buttonOnColourId, Colour (0xff3ddc84));
    pfOn.setColour (TextButton::textColourOnId, Colours::black);
    addAndMakeVisible (pfOn);
    pfOnAtt = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "pfOn", pfOn);
    pfOnRaw = proc.apvts.getRawParameterValue ("pfOn");
    pfType.addItemList ({ "Lowpass", "Highpass", "Bandpass", "Notch" }, 1);
    addAndMakeVisible (pfType);
    pfTypeAtt = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "pfType", pfType);
    for (auto* k : { &pfMix, &pfCutoff, &pfRes, &pfQ }) addAndMakeVisible (*k);

    addChildComponent (settingsPanel);      // hidden until the Settings button is pressed
    addMouseListener (this, true);          // lets a click anywhere else close it (see mouseDown)

    setSize (1020, 640);   // must come last: this calls resized(), which needs every child to exist
    startTimerHz (30);
}

MBDEditor::~MBDEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void MBDEditor::timerCallback()
{
    spectrum.update();
    for (auto& c : columns) c->refresh();
    if (settingsPanel.isVisible()) settingsPanel.refresh();

    // dim a section's controls while it is switched off
    const float ga = gateOnRaw->load() > 0.5f ? 1.f : 0.4f;
    const float pa = pfOnRaw->load()   > 0.5f ? 1.f : 0.4f;
    for (auto* k : { &gateAmount, &gateThresh, &gateAttack, &gateRelease }) k->setAlpha (ga);
    for (auto* k : { &pfMix, &pfCutoff, &pfRes, &pfQ }) k->setAlpha (pa);
    pfType.setAlpha (pa);
}

//==============================================================================
void MBDEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff15171b));
    g.setColour (Colour (0xff1d2025));
    g.fillRoundedRectangle (gateRect.toFloat(), 6.f);
    g.fillRoundedRectangle (pfRect.toFloat(), 6.f);
}

void MBDEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    // top bar
    auto top = r.removeFromTop (32);
    auto pr = top.removeFromRight (180 + 6 + 60 + 6 + 60 + 6 + 80);
    presetBox.setBounds (pr.removeFromLeft (180).reduced (0, 3));
    pr.removeFromLeft (6);
    saveBtn.setBounds (pr.removeFromLeft (60).reduced (0, 3));
    pr.removeFromLeft (6);
    loadBtn.setBounds (pr.removeFromLeft (60).reduced (0, 3));
    pr.removeFromLeft (6);
    settingsBtn.setBounds (pr.reduced (0, 3));
    title.setBounds (top);

    settingsPanel.setBounds (getWidth() - 12 - SettingsPanel::panelWidth, 12 + 32 + 4,
                             SettingsPanel::panelWidth, SettingsPanel::panelHeight);

    r.removeFromTop (8);

    // display row: input | spectrum | output
    auto disp = r.removeFromTop (200);
    inKnob.setBounds (disp.removeFromLeft (96).withSizeKeepingCentre (96, 110));
    disp.removeFromLeft (8);
    outKnob.setBounds (disp.removeFromRight (96).withSizeKeepingCentre (96, 110));
    disp.removeFromRight (8);
    spectrum.setBounds (disp);

    r.removeFromTop (10);

    // lower area: gate | 4 bands (+3 crossover dials) | post filter
    gateRect = r.removeFromLeft (160);
    r.removeFromLeft (8);
    pfRect = r.removeFromRight (160);
    r.removeFromRight (8);

    constexpr int colW = 117, gapW = 64;
    for (int b = 0; b < 4; ++b)
    {
        const int x = r.getX() + b * (colW + gapW);
        columns[(size_t) b]->setBounds (x, r.getY(), colW, 366);
        if (b < 3)
            xoverKnobs[(size_t) b]->setBounds (x + colW, r.getY(), gapW, 88);
    }

    // gate 2x2
    {
        auto a = gateRect.reduced (4);
        auto gRow = a.removeFromTop (20);
        gateOn.setBounds (gRow.removeFromLeft (34).reduced (0, 1));
        gateTitle.setBounds (gRow);
        a.removeFromTop (4);
        auto row1 = a.removeFromTop (84);
        auto row2 = a.removeFromTop (84);
        gateAmount.setBounds  (row1.removeFromLeft (row1.getWidth() / 2));
        gateThresh.setBounds  (row1);
        gateAttack.setBounds  (row2.removeFromLeft (row2.getWidth() / 2));
        gateRelease.setBounds (row2);
    }

    // post filter 2x2
    {
        auto a = pfRect.reduced (4);
        auto pRow = a.removeFromTop (20);
        pfOn.setBounds (pRow.removeFromLeft (34).reduced (0, 1));
        pfTitle.setBounds (pRow);
        a.removeFromTop (4);
        pfType.setBounds (a.removeFromTop (24).reduced (4, 0));
        a.removeFromTop (4);
        auto row1 = a.removeFromTop (84);
        auto row2 = a.removeFromTop (84);
        pfMix.setBounds    (row1.removeFromLeft (row1.getWidth() / 2));
        pfCutoff.setBounds (row1);
        pfRes.setBounds    (row2.removeFromLeft (row2.getWidth() / 2));
        pfQ.setBounds      (row2);
    }
}

//==============================================================================
void MBDEditor::mouseDown (const MouseEvent& e)
{
    // addMouseListener (this, true) means we hear presses on every child; close the card unless the
    // press was on the card itself or on the button that toggles it.
    if (settingsPanel.isVisible()
        && e.originalComponent != &settingsPanel
        && ! settingsPanel.isParentOf (e.originalComponent)
        && e.originalComponent != &settingsBtn)
        settingsPanel.setVisible (false);
}

void MBDEditor::refreshPresetList()
{
    presetFiles = MBDProcessor::getPresetDirectory().findChildFiles (File::findFiles, false, "*.mbdpreset");
    std::sort (presetFiles.begin(), presetFiles.end(),
               [] (const File& a, const File& b) { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });

    presetBox.clear (dontSendNotification);
    for (int i = 0; i < presetFiles.size(); ++i)
        presetBox.addItem (presetFiles[i].getFileNameWithoutExtension(), i + 1);
}

void MBDEditor::savePreset()
{
    chooser = std::make_unique<FileChooser> ("Save preset",
                                             MBDProcessor::getPresetDirectory().getChildFile ("New Preset.mbdpreset"),
                                             "*.mbdpreset");
    const auto flags = FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                     | FileBrowserComponent::warnAboutOverwriting;

    Component::SafePointer<MBDEditor> safe (this);
    chooser->launchAsync (flags, [safe] (const FileChooser& fc)
    {
        if (safe == nullptr) return;
        auto f = fc.getResult();
        if (f == File()) return;

        f = f.withFileExtension ("mbdpreset");
        safe->proc.savePresetToFile (f);
        safe->refreshPresetList();
        for (int i = 0; i < safe->presetFiles.size(); ++i)
            if (safe->presetFiles[i] == f)
                safe->presetBox.setSelectedItemIndex (i, dontSendNotification);
    });
}

void MBDEditor::loadPreset()
{
    chooser = std::make_unique<FileChooser> ("Load preset", MBDProcessor::getPresetDirectory(), "*.mbdpreset");
    const auto flags = FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles;

    Component::SafePointer<MBDEditor> safe (this);
    chooser->launchAsync (flags, [safe] (const FileChooser& fc)
    {
        if (safe == nullptr) return;
        const auto f = fc.getResult();
        if (f.existsAsFile())
            safe->proc.loadPresetFromFile (f);
    });
}
