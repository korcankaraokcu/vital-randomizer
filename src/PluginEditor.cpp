#include "PluginEditor.h"

#include "Axes.h"

namespace
{
    constexpr int kStripHeight = 104;
    constexpr int kMinWidth = 900;

    const juce::Colour kBack       { 0xff141619 };
    const juce::Colour kPanel      { 0xff1d2126 };
    const juce::Colour kAccent     { 0xff4fc3f7 };
    const juce::Colour kAccentWarm { 0xffffb74d };
    const juce::Colour kText       { 0xffd7dde3 };
    const juce::Colour kTextDim    { 0xff7d8790 };

    const std::vector<std::pair<schema::Section, const char*>>& lockables()
    {
        static const std::vector<std::pair<schema::Section, const char*>> list = {
            { schema::Section::osc,    "OSC" },
            { schema::Section::filter, "FILT" },
            { schema::Section::env,    "ENV" },
            { schema::Section::lfo,    "LFO" },
            { schema::Section::fx,     "FX" },
            { schema::Section::mod,    "MOD" },
        };
        return list;
    }
}

// ------------------------------------------------------------- filmstrip ---

void Filmstrip::setContents (int newCount, int newCursor)
{
    if (newCount != count || newCursor != cursor)
    {
        count = newCount;
        cursor = newCursor;
        repaint();
    }
}

int Filmstrip::cellAt (int x) const
{
    if (count <= 0)
        return -1;
    // Only a window around the cursor is ever drawn, so a thousand-deep history
    // still shows something legible.
    const auto visible = juce::jmin (count, juce::jmax (1, getWidth() / 34));
    const auto first = juce::jlimit (0, juce::jmax (0, count - visible), cursor - visible / 2);
    const auto cellWidth = getWidth() / juce::jmax (1, visible);
    const auto index = first + x / juce::jmax (1, cellWidth);
    return index < count ? index : -1;
}

void Filmstrip::paint (juce::Graphics& g)
{
    g.fillAll (kPanel);

    if (count <= 0)
    {
        g.setColour (kTextDim);
        g.setFont (12.0f);
        g.drawText ("no candidates yet, hit ROLL", getLocalBounds(),
                    juce::Justification::centred);
        return;
    }

    const auto visible = juce::jmin (count, juce::jmax (1, getWidth() / 34));
    const auto first = juce::jlimit (0, juce::jmax (0, count - visible), cursor - visible / 2);
    const auto cellWidth = getWidth() / juce::jmax (1, visible);

    for (int i = 0; i < visible; ++i)
    {
        const auto index = first + i;
        if (index >= count)
            break;

        juce::Rectangle<int> cell (i * cellWidth, 0, cellWidth - 2, getHeight());
        const auto isCursor = index == cursor;
        g.setColour (isCursor ? kAccent.withAlpha (0.25f) : juce::Colours::black.withAlpha (0.25f));
        g.fillRoundedRectangle (cell.toFloat().reduced (1.0f), 3.0f);

        if (isCursor)
        {
            g.setColour (kAccent);
            g.drawRoundedRectangle (cell.toFloat().reduced (1.0f), 3.0f, 1.4f);
        }

        g.setColour (isCursor ? kText : kTextDim);
        g.setFont (11.0f);
        g.drawText (juce::String (index + 1), cell, juce::Justification::centred);
    }
}

void Filmstrip::mouseDown (const juce::MouseEvent& e)
{
    const auto index = cellAt (e.x);
    if (index >= 0 && onSelect)
        onSelect (index);
}

// ---------------------------------------------------------------- editor ---

VitalRandomizerEditor::VitalRandomizerEditor (VitalRandomizerProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    styleLabel.setText ("STYLE", juce::dontSendNotification);
    styleLabel.setColour (juce::Label::textColourId, kTextDim);
    styleLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (styleLabel);

    styleBox.setColour (juce::ComboBox::backgroundColourId, kBack);
    styleBox.setColour (juce::ComboBox::textColourId, kText);
    styleBox.onChange = [this]
    {
        if (styleBox.getSelectedId() > 0)
            proc.setStyle (styleBox.getText());
    };
    addAndMakeVisible (styleBox);

    for (const auto& axis : axes::all())
    {
        AxisControl control;
        control.key = juce::String (axis.key);

        control.label = std::make_unique<juce::Label>();
        control.label->setText (juce::String (axis.label), juce::dontSendNotification);
        control.label->setColour (juce::Label::textColourId, kTextDim);
        control.label->setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (*control.label);

        control.slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                         juce::Slider::NoTextBox);
        control.slider->setRange (0.0, 1.0, 0.01);
        control.slider->setValue (proc.slider (control.key), juce::dontSendNotification);
        control.slider->setColour (juce::Slider::thumbColourId, kAccent);
        control.slider->setColour (juce::Slider::trackColourId, kAccent.withAlpha (0.45f));
        control.slider->setColour (juce::Slider::backgroundColourId, kBack);
        const auto key = control.key;
        auto* raw = control.slider.get();
        control.slider->onValueChange = [this, key, raw]
        {
            proc.setSlider (key, (float) raw->getValue());
        };
        addAndMakeVisible (*control.slider);

        axisControls.push_back (std::move (control));
    }

    for (const auto& entry : lockables())
    {
        LockControl lock;
        lock.section = entry.first;
        lock.button = std::make_unique<juce::TextButton> (entry.second);
        lock.button->setClickingTogglesState (true);
        lock.button->setToggleState (proc.isLocked (entry.first), juce::dontSendNotification);
        lock.button->setColour (juce::TextButton::buttonColourId, kBack);
        lock.button->setColour (juce::TextButton::buttonOnColourId, kAccentWarm.withAlpha (0.75f));
        lock.button->setColour (juce::TextButton::textColourOffId, kTextDim);
        lock.button->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        const auto section = entry.first;
        auto* raw = lock.button.get();
        lock.button->onClick = [this, section, raw]
        {
            proc.setLocked (section, raw->getToggleState());
        };
        addAndMakeVisible (*lock.button);
        lockControls.push_back (std::move (lock));
    }

    auto styleButton = [] (juce::TextButton& b, juce::Colour colour)
    {
        b.setColour (juce::TextButton::buttonColourId, colour);
        b.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    };
    styleButton (rollButton, kAccent);
    styleButton (varyButton, kAccent.withAlpha (0.6f));
    for (auto* b : { &prevButton, &nextButton, &starButton, &exportButton, &settingsButton })
    {
        b->setColour (juce::TextButton::buttonColourId, kPanel);
        b->setColour (juce::TextButton::textColourOffId, kText);
    }

    rollButton.onClick     = [this] { proc.rollNew(); };
    varyButton.onClick     = [this] { proc.vary(); };
    prevButton.onClick     = [this] { proc.stepHistory (-1); };
    nextButton.onClick     = [this] { proc.stepHistory (1); };
    starButton.onClick     = [this] { proc.starCurrent(); };
    exportButton.onClick   = [this] { exportCurrent(); };
    settingsButton.onClick = [this] { showSettingsMenu(); };

    for (auto* b : { &rollButton, &varyButton, &prevButton, &nextButton,
                     &starButton, &exportButton, &settingsButton })
        addAndMakeVisible (*b);

    varyDepth.setSliderStyle (juce::Slider::LinearHorizontal);
    varyDepth.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    varyDepth.setRange (0.02, 1.0, 0.01);
    varyDepth.setValue (proc.varyAmount(), juce::dontSendNotification);
    varyDepth.setColour (juce::Slider::thumbColourId, kAccentWarm);
    varyDepth.setColour (juce::Slider::trackColourId, kAccentWarm.withAlpha (0.4f));
    varyDepth.setColour (juce::Slider::backgroundColourId, kBack);
    varyDepth.setTooltip ("How far VARY drifts from the current patch");
    varyDepth.onValueChange = [this] { proc.setVaryAmount ((float) varyDepth.getValue()); };
    addAndMakeVisible (varyDepth);

    filmstrip.onSelect = [this] (int index)
    {
        proc.candidates().setCursor (index);
        proc.reloadCurrent();
    };
    addAndMakeVisible (filmstrip);

    statusLabel.setColour (juce::Label::textColourId, kTextDim);
    statusLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (statusLabel);

    macroLabel.setColour (juce::Label::textColourId, kAccent.withAlpha (0.85f));
    macroLabel.setFont (juce::FontOptions (11.0f));
    macroLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (macroLabel);

    proc.addChangeListener (this);
    refreshStyles();

    setResizable (true, true);
    setResizeLimits (kMinWidth, kStripHeight + 260, 3000, 2200);
    setSize (vitalWidth, vitalHeight + kStripHeight);

    startTimerHz (8);
}

VitalRandomizerEditor::~VitalRandomizerEditor()
{
    proc.removeChangeListener (this);
    // The hosted editor has to go before this component does. Its destructor
    // tells Vital it is gone, and Vital asserts on shutdown if an editor is
    // still registered.
    vitalEditor.reset();
}

void VitalRandomizerEditor::attachVitalEditor()
{
    if (vitalEditor != nullptr || triedAttaching)
        return;
    if (! proc.status().vitalReady)
        return;

    triedAttaching = true;
    if (auto* editor = proc.vital().createEditor())
    {
        vitalEditor.reset (editor);
        addAndMakeVisible (*vitalEditor);
        vitalWidth = juce::jmax (kMinWidth, editor->getWidth());
        vitalHeight = juce::jmax (260, editor->getHeight());
        setSize (vitalWidth, vitalHeight + kStripHeight);
        resized();
    }
}

void VitalRandomizerEditor::refreshStyles()
{
    const auto styles = proc.availableStyles();
    if (styles.empty())
        return;

    const auto wanted = proc.style();
    styleBox.clear (juce::dontSendNotification);
    int id = 1, selected = 1;
    for (const auto& s : styles)
    {
        styleBox.addItem (juce::String (s), id);
        if (juce::String (s) == wanted)
            selected = id;
        ++id;
    }
    styleBox.setSelectedId (selected, juce::dontSendNotification);
}

void VitalRandomizerEditor::refreshFromProcessor()
{
    const auto s = proc.status();

    juce::String text = s.message;
    if (s.progress >= 0.0f && s.working)
        text << "  " << juce::String (juce::roundToInt (s.progress * 100.0f)) << "%";
    // What the offline screen measured, so the level matching is visible rather
    // than something the plugin does silently.
    const auto audition = proc.lastAuditionSummary();
    if (audition.isNotEmpty() && ! s.working)
        text << "   |   " << audition;
    const auto error = proc.lastError();
    if (error.isNotEmpty())
        text = error;
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, error.isNotEmpty()
                                                          ? juce::Colours::orangered
                                                          : kTextDim);

    const auto macros = proc.lastMacroSummary();
    macroLabel.setText (macros.isNotEmpty() ? "macros: " + macros : juce::String(),
                        juce::dontSendNotification);

    const auto ready = s.vitalReady && s.modelReady && ! s.working;
    rollButton.setEnabled (ready);
    varyButton.setEnabled (ready && ! proc.candidates().empty());
    starButton.setEnabled (ready && ! proc.candidates().empty());
    exportButton.setEnabled (s.vitalReady);

    const auto& candidates = proc.candidates();
    prevButton.setEnabled (ready && candidates.cursor() > 0);
    nextButton.setEnabled (ready && candidates.cursor() + 1 < (int) candidates.size());
    filmstrip.setContents ((int) candidates.size(), candidates.cursor());

    if (styleBox.getNumItems() == 0)
        refreshStyles();

    for (auto& control : axisControls)
    {
        const auto value = proc.slider (control.key);
        if (std::abs (control.slider->getValue() - value) > 1.0e-4)
            control.slider->setValue (value, juce::dontSendNotification);
    }
    for (auto& lock : lockControls)
    {
        const auto locked = proc.isLocked (lock.section);
        if (lock.button->getToggleState() != locked)
            lock.button->setToggleState (locked, juce::dontSendNotification);
    }
}

void VitalRandomizerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromProcessor();
    attachVitalEditor();
}

void VitalRandomizerEditor::timerCallback()
{
    refreshFromProcessor();
    attachVitalEditor();
}

void VitalRandomizerEditor::showSettingsMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Locate Vital.vst3");
    menu.addItem (2, "Rescan preset library");
    menu.addSeparator();
    menu.addItem (3, "Vital: " + (proc.status().vitalReady ? juce::String ("loaded")
                                                           : juce::String ("not loaded")), false);
    menu.addItem (4, "Library: " + proc.libraryRoot().getFileName(), false);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (settingsButton),
                        [this] (int result)
    {
        if (result == 1)
        {
            chooser = std::make_unique<juce::FileChooser> ("Where is Vital.vst3?",
                                                           juce::File(), "*.vst3");
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file != juce::File())
                    proc.locateVital (file);
            });
        }
        else if (result == 2)
        {
            chooser = std::make_unique<juce::FileChooser> ("Which folder holds your presets?",
                                                           proc.libraryRoot());
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
            {
                const auto dir = fc.getResult();
                if (dir.isDirectory())
                    proc.rescanLibrary (dir);
            });
        }
    });
}

void VitalRandomizerEditor::exportCurrent()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Save this patch",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ("randomized.vital"),
        "*.vital");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();
        if (file != juce::File())
            proc.exportCurrent (file);
    });
}

void VitalRandomizerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBack);
    g.setColour (kPanel);
    g.fillRect (0, 0, getWidth(), kStripHeight);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.drawHorizontalLine (kStripHeight - 1, 0.0f, (float) getWidth());

    if (vitalEditor == nullptr)
    {
        g.setColour (kTextDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (proc.status().vitalReady ? "Opening Vital..." : proc.status().message,
                    getLocalBounds().withTrimmedTop (kStripHeight),
                    juce::Justification::centred);
    }
}

void VitalRandomizerEditor::resized()
{
    auto area = getLocalBounds();
    auto strip = area.removeFromTop (kStripHeight).reduced (8, 6);

    auto topRow = strip.removeFromTop (24);
    styleLabel.setBounds (topRow.removeFromLeft (40));
    styleBox.setBounds (topRow.removeFromLeft (120));
    topRow.removeFromLeft (12);

    // Axis sliders share the middle of the top row, one label plus track each.
    const auto axisCount = juce::jmax (1, (int) axisControls.size());
    auto axisArea = topRow.removeFromLeft (juce::jmax (240, topRow.getWidth() - 300));
    const auto axisWidth = axisArea.getWidth() / axisCount;
    for (auto& control : axisControls)
    {
        auto cell = axisArea.removeFromLeft (axisWidth).reduced (4, 0);
        control.label->setBounds (cell.removeFromLeft (46));
        control.slider->setBounds (cell);
    }

    topRow.removeFromLeft (8);
    settingsButton.setBounds (topRow.removeFromRight (30));
    topRow.removeFromRight (4);
    exportButton.setBounds (topRow.removeFromRight (66));

    strip.removeFromTop (6);
    auto midRow = strip.removeFromTop (26);

    for (auto& lock : lockControls)
    {
        lock.button->setBounds (midRow.removeFromLeft (46).reduced (2, 0));
    }
    midRow.removeFromLeft (10);

    rollButton.setBounds (midRow.removeFromLeft (74).reduced (2, 0));
    varyButton.setBounds (midRow.removeFromLeft (66).reduced (2, 0));
    varyDepth.setBounds (midRow.removeFromLeft (86).reduced (4, 4));
    midRow.removeFromLeft (10);
    prevButton.setBounds (midRow.removeFromLeft (30).reduced (2, 0));
    nextButton.setBounds (midRow.removeFromLeft (30).reduced (2, 0));
    starButton.setBounds (midRow.removeFromLeft (60).reduced (2, 0));
    midRow.removeFromLeft (10);
    filmstrip.setBounds (midRow.reduced (2, 1));

    strip.removeFromTop (4);
    auto bottomRow = strip;
    macroLabel.setBounds (bottomRow.removeFromRight (juce::jmin (320, bottomRow.getWidth() / 2)));
    statusLabel.setBounds (bottomRow);

    if (vitalEditor != nullptr)
        vitalEditor->setBounds (area);
}
