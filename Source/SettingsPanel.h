#pragma once
#include "Common.h"
#include "PluginProcessor.h"

// Small card that opens under the Settings button: oversampling factor and spectrum colour.
class SettingsPanel : public juce::Component
{
public:
    explicit SettingsPanel (MBDProcessor& p)
    {
        auto styleLabel = [] (juce::Label& l, const juce::String& text, float size, bool bold)
        {
            l.setText (text, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain)));
            l.setJustificationType (juce::Justification::centredLeft);
        };

        styleLabel (heading,  "SETTINGS",        13.f, true);
        styleLabel (osLabel,  "Oversampling",    12.f, false);
        styleLabel (colLabel, "Spectrum colour", 12.f, false);
        heading.setColour (juce::Label::textColourId, juce::Colour (0xffe5383b));
        for (auto* l : { &heading, &osLabel, &colLabel }) addAndMakeVisible (*l);

        osBox.addItemList ({ "Off", "2x", "4x", "8x" }, 1);
        addAndMakeVisible (osBox);
        osAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, "oversample", osBox);

        colParam = p.apvts.getParameter ("specColour");
        colRaw   = p.apvts.getRawParameterValue ("specColour");

        const auto names = MBDProcessor::getSpectrumColourNames();
        for (int i = 0; i < names.size(); ++i)
        {
            auto s = std::make_unique<Swatch> (MBDProcessor::getSpectrumColour (i), names[i]);
            s->onClick = [this, i]
            {
                colParam->setValueNotifyingHost (colParam->convertTo0to1 ((float) i));
                refresh();
            };
            addAndMakeVisible (*s);
            swatches.push_back (std::move (s));
        }
        refresh();
    }

    // Keeps the highlighted swatch in step with the parameter (it can change when a session loads).
    void refresh()
    {
        const int sel = (int) colRaw->load();
        for (size_t i = 0; i < swatches.size(); ++i)
            swatches[i]->setSelected ((int) i == sel);
    }

    static constexpr int panelWidth = 236, panelHeight = 138;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (juce::Colour (0xf0222529));
        g.fillRoundedRectangle (r, 8.f);
        g.setColour (juce::Colour (0xff3a3d44));
        g.drawRoundedRectangle (r, 8.f, 1.f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        heading.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);

        osLabel.setBounds (r.removeFromTop (16));
        osBox.setBounds (r.removeFromTop (24));
        r.removeFromTop (10);

        colLabel.setBounds (r.removeFromTop (16));
        r.removeFromTop (4);
        auto row = r.removeFromTop (24);
        const int step = row.getWidth() / (int) swatches.size();
        for (auto& s : swatches)
            s->setBounds (row.removeFromLeft (step).withSizeKeepingCentre (24, 24));
    }

private:
    // A round colour chip; the selected one gets a ring.
    class Swatch : public juce::Component
    {
    public:
        Swatch (juce::Colour c, const juce::String& name) : colour (c)
        {
            setName (name);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }

        void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (2.f);
            g.setColour (colour);
            g.fillEllipse (r.reduced (3.f));
            g.setColour (selected ? juce::Colours::white : juce::Colours::white.withAlpha (0.18f));
            g.drawEllipse (r, selected ? 2.f : 1.f);
        }

        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }

        std::function<void()> onClick;

    private:
        juce::Colour colour;
        bool selected = false;
    };

    juce::Label heading, osLabel, colLabel;
    juce::ComboBox osBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> osAtt;

    juce::RangedAudioParameter* colParam = nullptr;
    std::atomic<float>* colRaw = nullptr;
    std::vector<std::unique_ptr<Swatch>> swatches;
};
