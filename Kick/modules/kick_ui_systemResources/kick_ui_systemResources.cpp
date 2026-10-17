#ifdef KICK_UI_H_INCLUDE
    /* When you add this cpp file to your project, you mustn't include it in a file where you've
already included any other headers - just put it inside a file on its own, possibly with your config
flags preceding it, but don't include anything else. That also includes avoiding any automatic prefix
header files that the compiler may be using.
*/
    #error "Incorrect use of JUCE cpp file"
#endif

#include "hardware/source/kick_Resorces.h"
#include "kick_ui_systemResources.h"
#include <project/project.h>

namespace kick_ui
{
    KSystemResources& KSystemResources::Instance()
    {
        static KSystemResources _sysR;
        return _sysR;
    }

    KSystemResources::KSystemResources()
    {
        setTitle( "SYSTEM RESOURCES" );

        mt::utils::addChildComponents( this, cpu, ram, flash, sdcard, usb );
        mt::utils::addAndMakeVisible( this, storage, overlayLabel, overlay, purgeAll, purgeUnused, ok );

        cpu.setEnabled( false );
        cpu.setRange( 0.0, 100.0 );
        cpu.setTitleProportion( .35f );
        cpu.setValueProportion( .35f );
        cpu.showTitle( true, juce::Justification::centredTop );
        cpu.showValue( true, juce::Justification::centredBottom, juce::Slider::TextBoxRight );

        ram.setEnabled( false );
        ram.setRange( 0.0, 100.0 );
        ram.setTitleProportion( .35f );
        ram.setValueProportion( .35f );
        ram.showTitle( true, juce::Justification::centred );
        ram.showValue( true, juce::Justification::centred, juce::Slider::TextBoxBelow );

        flash.setEnabled( false );
        flash.setRange( 0.0, 100.0 );
        flash.setTitleProportion( .35f );
        flash.setValueProportion( .35f );
        flash.showTitle( true, juce::Justification::centred );
        flash.showValue( true, juce::Justification::centred, juce::Slider::TextBoxBelow );

        sdcard.setEnabled( false );
        sdcard.setRange( 0.0, 100.0 );
        sdcard.setTitleProportion( .35f );
        sdcard.setValueProportion( .35f );
        sdcard.showTitle( true, juce::Justification::centred );
        sdcard.showValue( true, juce::Justification::centred, juce::Slider::TextBoxBelow );

        usb.setEnabled( false );
        usb.setRange( 0.0, 100.0 );
        usb.setTitleProportion( .35f );
        usb.setValueProportion( .35f );
        usb.showTitle( true, juce::Justification::centred );
        usb.showValue( true, juce::Justification::centred, juce::Slider::TextBoxBelow );

        // The figures read "--" until they are known, and a card or disk
        // that is not there says so rather than showing 0 %.
        cpu.textFromValueFunction    = [this]( double v ) { return percentText( v, cpuKnown, "--" ); };
        ram.textFromValueFunction    = [this]( double v ) { return percentText( v, ramKnown, "--" ); };
        flash.textFromValueFunction  = [this]( double v ) { return percentText( v, flashKnown, "--" ); };
        sdcard.textFromValueFunction = [this]( double v ) { return percentText( v, sdCardKnown, "Not present" ); };
        usb.textFromValueFunction    = [this]( double v ) { return percentText( v, usbKnown, "Not present" ); };

        storage.setValueBackgroundColour( ui::Theme::backgroundColour( 9 ) );
        storage.setValueColour( ui::Theme::textColour( 2 ) );
        storage.setValueJustification( juce::Justification::centredTop );
        storage.setValueFontHeight( 16 );

        overlayLabel.setInterceptsMouseClicks( false, false );
        overlayLabel.setValueBackgroundColour( ui::Theme::backgroundColour( 9 ) );
        overlayLabel.setValueColour( ui::Theme::textColour( 2 ) );
        overlayLabel.setValueJustification( juce::Justification::centredLeft );
        overlayLabel.setValueFontHeight( 16 );
        overlay.setValueFontHeight( 18 );

        // The button shows the setting and changes it; the overlay follows
        // the setting, wherever it is changed.
        overlay.getToggleStateValue().referTo( mt::getSettings().window.performanceOverlay.getValue() );

        purgeAll.setPlacementFlag( juce::Justification::centred );
        purgeUnused.setPlacementFlag( juce::Justification::centred );
        ok.setPlacementFlag( juce::Justification::centred );

        purgeAll.onClick    = [&]() {};
        purgeUnused.onClick = [&]() {};
        ok.onClick          = [&]() {
            mt::utils::exitDialog( this, UI::APPLY );
        };

        ui::themeChanged.connect( [&]() {
            purgeAll.setImages( "purge" );
            purgeUnused.setImages( "purgeused" );
            ok.setImages( "ok_resource" );
        } );
    }

    juce::String KSystemResources::percentText( double value, bool known, const juce::String& unknown )
    {
        return known ? juce::String( juce::roundToInt( value ) ) + " %" : unknown;
    }

    void KSystemResources::start()
    {
        // Show real figures as soon as the window opens, not a second
        // later: memory and storage need one reading, and CPU use, which is
        // the change between two, gets its first one now.
        mt::SystemResource::getResourceInterface().startCpuMeasurement();
        update( false );
        startTimerHz( 1 );
    }

    void KSystemResources::stop()
    {
        stopTimer();
    }

    void KSystemResources::timerCallback()
    {
        update( true );
    }

    void KSystemResources::update( bool readCpu )
    {
        auto& resources = mt::SystemResource::getResourceInterface();

        double cpuUsage = 0.0;
        cpuKnown        = readCpu && resources.cpuInfo( cpuUsage );
        showValue( cpu, cpuUsage );

        mt::sysRamInfo rInfo;
        resources.ramInfo( rInfo );
        ramKnown = rInfo.totalRam > 0;
        showValue( ram, rInfo.perRam );

        mt::sysSizeInfo sInfo;
        resources.sizeInfo( internalPath, sInfo );
        flashKnown = sInfo.status;
        showValue( flash, sInfo.perSize );

        sdCardPath = mt::getRemovableMediaPath( mt::RemovableMedia::SdCard );
        resources.sizeInfo( sdCardPath, sInfo );
        sdCardKnown = sInfo.status;
        showValue( sdcard, sInfo.perSize );

        usbDiskPath = mt::getRemovableMediaPath( mt::RemovableMedia::UsbDisk );
        resources.sizeInfo( usbDiskPath, sInfo );
        usbKnown = sInfo.status;
        showValue( usb, sInfo.perSize );
    }

    void KSystemResources::showValue( ui::KickSlider& slider, double value )
    {
        // The text depends on more than the value: repaint even when the
        // value is the same.
        slider.setValue( value, juce::dontSendNotification );
        slider.repaint();
    }

    void KSystemResources::paint( juce::Graphics& g )
    {
        g.setColour( ui::Theme::backgroundColour( 14 ) );
        g.drawLine( 20.f, 182.f, 237.f, 182.f );
        g.drawLine( 367.f, 182.f, 584.f, 182.f );
    }

    void KSystemResources::resized()
    {
        cpu.setBounds( 90, 10, 425, 76 );

        ram.setBounds( 90, 96, 425, 76 );
        // The gap between the two lines (paint()): 71 px cut "STORAGE".
        storage.setBounds( 237, 176, 130, 22 );

        flash.setBounds( 90, 199, 425, 76 );
        sdcard.setBounds( 90, 275, 425, 76 );

        usb.setBounds( 90, 357, 425, 76 );

        overlayLabel.setBounds( 90, 441, 290, 36 );
        overlay.setBounds( 395, 441, 120, 36 );

        purgeAll.setBounds( 20, 483, 140, 40 );
        purgeUnused.setBounds( 240, 483, 140, 40 );
        ok.setBounds( 430, 483, 140, 40 );
    }

    void KSystemResources::visibilityChanged()
    {
        auto visible = isVisible();
        cpu.setVisible( visible );
        ram.setVisible( visible );
        storage.setVisible( visible );
        flash.setVisible( visible );
        sdcard.setVisible( visible );
        usb.setVisible( visible );
    }
}
