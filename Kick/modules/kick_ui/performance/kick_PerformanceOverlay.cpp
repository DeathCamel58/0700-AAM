/*
 * This file is part of Kick.
 *
 * Kick is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Kick is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Kick. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hardware/source/kick_Resorces.h"
#include "hardware/source/kick_SysFile.h"

#include <unistd.h>

namespace kick_ui
{
    namespace
    {
        const auto backgroundColour = juce::Colours::black.withAlpha( 0.6f );
        const auto titleColour      = juce::Colours::white.withAlpha( 0.9f );
        const auto detailColour     = juce::Colours::white.withAlpha( 0.65f );
        const auto gridColour       = juce::Colours::white.withAlpha( 0.18f );

        const auto paintColour  = juce::Colour( 0xff4fc3f7 );
        const auto gapColour    = juce::Colour( 0xffffb74d );
        const auto systemColour = juce::Colour( 0xff81c784 );
        const auto selfColour   = juce::Colour( 0xfffff176 );
        const auto memoryColour = juce::Colour( 0xffce93d8 );
        const auto audioColour  = juce::Colour( 0xff4dd0e1 );
        const auto xrunColour   = juce::Colour( 0xffef5350 );

        constexpr int padding   = 6;
        constexpr int rowGap    = 2;
        constexpr int textWidth = 112;

        constexpr float titleFontHeight = 12.0f;
        constexpr float textFontHeight  = 11.0f;

        // A frame at 60 Hz.
        constexpr float frameGuideMs = 1000.0f / 60.0f;
    }

    PerformanceOverlay::Row::Row( const juce::String& rowTitle, float topValue )
        : title( rowTitle ),
          top( topValue ),
          a( historySize ),
          b( historySize )
    {
        pathA.preallocateSpace( historySize * 3 + 8 );
        pathB.preallocateSpace( historySize * 3 + 8 );
    }

    PerformanceOverlay::PerformanceOverlay()
    {
        setName( "Performance overlay" );
        setSize( width, height );

        // Touches, clicks and keys go to what is under it: with no clicks
        // for itself or its children, JUCE's hit test (Component::hitTest)
        // fails, and getComponentAt() goes on to the components below.
        setInterceptsMouseClicks( false, false );
        setWantsKeyboardFocus( false );
        setMouseClickGrabsKeyboardFocus( false );
        setAccessible( false );
        setOpaque( false );

        frames.colourA     = paintColour;
        frames.colourB     = gapColour;
        frames.guide       = frameGuideMs;
        cpu.colourA        = systemColour;
        cpu.colourB        = selfColour;
        memory.colourA     = memoryColour;
        memory.colourB     = selfColour;
        audio.colourA      = audioColour;
        frames.textColours = { paintColour, paintColour, gapColour };
        cpu.textColours    = { systemColour, selfColour, detailColour };
        memory.textColours = { memoryColour, detailColour, selfColour };
        audio.textColours  = { audioColour, xrunColour, detailColour };

        if( const auto size = sysconf( _SC_PAGESIZE ); size > 0 )
            pageSize = (juce::uint64)size;

        enabledSetting.referTo( mt::getSettings().window.performanceOverlay.getValue() );
        enabledSetting.addListener( this );
        setVisible( (bool)enabledSetting.getValue() );
    }

    PerformanceOverlay::~PerformanceOverlay()
    {
        enabledSetting.removeListener( this );
        stop();
    }

    void PerformanceOverlay::valueChanged( juce::Value& )
    {
        setVisible( (bool)enabledSetting.getValue() );
    }

    void PerformanceOverlay::visibilityChanged()
    {
        if( isVisible() )
            start();
        else
            stop();
    }

    void PerformanceOverlay::parentHierarchyChanged()
    {
        followHost();
    }

    void PerformanceOverlay::followHost()
    {
        auto* newHost = findParentComponentOfClass<kick::window::ScaledHost>();
        if( newHost == host.getComponent() )
            return;

        // The window shows it in another host, or none (the window is
        // being destroyed: its host is still there).
        if( host != nullptr )
            host->getFrameTiming().setEnabled( false );

        host = newHost;
        if( host != nullptr && running )
            host->getFrameTiming().setEnabled( true );
    }

    void PerformanceOverlay::start()
    {
        if( running )
            return;
        running = true;

        for( auto* row : { &frames, &cpu, &memory, &audio } )
        {
            row->a.clear();
            row->b.clear();
            row->pathA.clear();
            row->pathB.clear();
            for( auto& text : row->texts )
                text.clear();
        }
        xruns.clear();
        recentFrames.clear();
        xrunsShown    = 0;
        lastXRunCount = -1;

        // The first readings only start the counts.
        std::string text;
        if( mt::sysfile::read( "/proc/stat", text ) )
            lastCpu = perf::parseProcStat( text ).value_or( perf::CpuTimes{} );
        if( mt::sysfile::read( "/proc/self/stat", text ) )
            lastProcessTicks = perf::parseProcessCpuTicks( text ).value_or( 0 );
        if( auto* device = mt::getAudioInterface().getAudioDeviceManager().getCurrentAudioDevice() )
            lastXRunCount = device->getXRunCount();

        // Starts the host's frame timing.
        followHost();
        if( host != nullptr )
            host->getFrameTiming().setEnabled( true );

        // The display's ticks, on the message thread: when one comes late,
        // so do the frames.
        displayTicks = std::make_unique<juce::VBlankAttachment>( this, [this]() {
            if( host != nullptr )
                host->getFrameTiming().displayTick();
        } );

        startTimerHz( samplesPerSecond );
    }

    void PerformanceOverlay::stop()
    {
        if( !running )
            return;
        running = false;

        stopTimer();
        displayTicks.reset();
        if( host != nullptr )
            host->getFrameTiming().setEnabled( false );
    }

    void PerformanceOverlay::timerCallback()
    {
        sampleFrames();
        sampleCpu();
        sampleMemory();
        sampleAudio();

        for( auto* row : { &frames, &cpu, &memory, &audio } )
        {
            updatePath( *row, row->a, row->pathA );
            updatePath( *row, row->b, row->pathB );
        }

        // Only this panel: the frame is painted under it too, as with any
        // see-through component.
        repaint();
    }

    void PerformanceOverlay::sampleFrames()
    {
        if( host == nullptr )
        {
            frames.texts = { "-", "paint -", "gap -" };
            return;
        }

        const auto period = host->getFrameTiming().takePeriod();
        frames.a.push( (float)period.longestPaintMs );
        frames.b.push( (float)period.longestTickGapMs );
        recentFrames.add( period.lengthMs, period.frames, period.longestPaintMs, period.longestTickGapMs );

        // Over the last second (the graphs show each period): the rate, and
        // the slowest frame and the longest wait, what is felt, where an
        // average hides it.
        frames.texts[0] = perf::formatFramesPerSecond( recentFrames.framesPerSecond() );
        frames.texts[1] = "paint " + perf::formatMilliseconds( recentFrames.longestPaintMs() );
        frames.texts[2] = "gap " + perf::formatMilliseconds( recentFrames.longestTickGapMs() );
    }

    void PerformanceOverlay::sampleCpu()
    {
        std::string text;
        if( !mt::sysfile::read( "/proc/stat", text ) )
            return;
        const auto now = perf::parseProcStat( text );
        if( !now.has_value() )
            return;

        juce::uint64 processTicks = lastProcessTicks;
        if( mt::sysfile::read( "/proc/self/stat", text ) )
            processTicks = perf::parseProcessCpuTicks( text ).value_or( lastProcessTicks );

        const auto system = perf::busyPercent( lastCpu, *now );
        const auto self   = perf::processPercent( lastProcessTicks, processTicks, lastCpu, *now );
        lastCpu           = *now;
        lastProcessTicks  = processTicks;

        cpu.a.push( (float)system );
        cpu.b.push( (float)self );
        cpu.texts[0] = perf::formatPercent( system );
        cpu.texts[1] = "Kick " + perf::formatPercent( self );
    }

    void PerformanceOverlay::sampleMemory()
    {
        mt::sysRamInfo info{};
        mt::SystemResource::getResourceInterface().ramInfo( info );
        if( info.totalRam <= 0.0 )
            return;

        double      residentMiB = 0.0;
        std::string text;
        if( mt::sysfile::read( "/proc/self/statm", text ) )
            residentMiB = (double)( perf::parseResidentPages( text ).value_or( 0 ) * pageSize ) / ( 1024.0 * 1024.0 );

        const auto usedPercent = info.usedRam * 100.0 / info.totalRam;
        memory.a.push( (float)usedPercent );
        memory.b.push( (float)( residentMiB * 100.0 / info.totalRam ) );
        memory.texts[0] = perf::formatPercent( usedPercent );
        memory.texts[1] = perf::formatMegabytes( info.usedRam ) + " of " + perf::formatMegabytes( info.totalRam );
        memory.texts[2] = "Kick " + perf::formatMegabytes( residentMiB );
    }

    void PerformanceOverlay::sampleAudio()
    {
        auto& deviceManager = mt::getAudioInterface().getAudioDeviceManager();
        auto* device        = deviceManager.getCurrentAudioDevice();
        if( device == nullptr || !device->isPlaying() )
        {
            audio.a.push( 0.0f );
            xruns.push( 0.0f );
            audio.texts   = { "off", "", "" };
            lastXRunCount = -1;
            return;
        }

        // The share of the buffer's time the callback took.
        const auto load = deviceManager.getCpuUsage() * 100.0;
        audio.a.push( (float)juce::jmin( load, (double)audio.top ) );

        const auto count = device->getXRunCount();
        const auto added = lastXRunCount < 0 ? 0 : perf::xrunsSince( lastXRunCount, count );
        lastXRunCount    = count;
        xrunsShown += added;
        xruns.push( (float)added );

        audio.texts[0] = perf::formatPercent( load );
        audio.texts[1] = count < 0 ? juce::String( "xruns -" ) : "xruns " + juce::String( xrunsShown );
        audio.texts[2] = juce::String( device->getCurrentBufferSizeSamples() ) + " @ "
                         + perf::formatSampleRate( device->getCurrentSampleRate() );
    }

    void PerformanceOverlay::updatePath( Row& row, const perf::History& history, juce::Path& path ) const
    {
        path.clear();
        const auto area = row.graphArea;
        if( history.size() < 2 || area.isEmpty() )
            return;

        // The newest sample at the right edge; the graph fills from the
        // right as the history grows.
        const auto step = area.getWidth() / (float)( historySize - 1 );
        auto       x    = area.getRight() - step * (float)( history.size() - 1 );
        for( int i = 0; i < history.size(); ++i, x += step )
        {
            const auto value = juce::jlimit( 0.0f, row.top, history[i] );
            const auto y     = area.getBottom() - value / row.top * area.getHeight();
            if( i == 0 )
                path.startNewSubPath( x, y );
            else
                path.lineTo( x, y );
        }
    }

    void PerformanceOverlay::paint( juce::Graphics& g )
    {
        g.setColour( backgroundColour );
        g.fillRoundedRectangle( getLocalBounds().toFloat(), 6.0f );

        for( const auto* row : { &frames, &cpu, &memory, &audio } )
            paintRow( g, *row );

        // An xrun is a mark across the audio graph where it happened.
        const auto area = audio.graphArea;
        const auto step = area.getWidth() / (float)( historySize - 1 );
        g.setColour( xrunColour );
        for( int i = 0; i < xruns.size(); ++i )
            if( xruns[i] > 0.0f )
                g.fillRect( area.getRight() - step * (float)( xruns.size() - 1 - i ) - 1.0f, area.getY(), 2.0f, area.getHeight() );
    }

    void PerformanceOverlay::paintRow( juce::Graphics& g, const Row& row ) const
    {
        // The texts: the title and its figure, two details under them.
        auto text      = row.textArea;
        auto titleLine = text.removeFromTop( text.getHeight() / 3 );
        g.setFont( juce::Font( titleFontHeight, juce::Font::bold ) );
        g.setColour( titleColour );
        g.drawText( row.title, titleLine, juce::Justification::centredLeft, false );
        g.setColour( row.textColours[0] );
        g.drawText( row.texts[0], titleLine, juce::Justification::centredRight, false );

        g.setFont( juce::Font( textFontHeight ) );
        const auto lineHeight = text.getHeight() / 2;
        for( int line = 1; line < 3; ++line )
        {
            g.setColour( row.textColours[(size_t)line] );
            g.drawText( row.texts[(size_t)line], text.removeFromTop( lineHeight ), juce::Justification::centredLeft, true );
        }

        // The graph: its frame, a guide, the lines.
        const auto area = row.graphArea;
        g.setColour( gridColour );
        g.drawRect( area, 1.0f );
        if( row.guide > 0.0f )
        {
            const auto y = area.getBottom() - row.guide / row.top * area.getHeight();
            g.drawHorizontalLine( juce::roundToInt( y ), area.getX(), area.getRight() );
        }

        const juce::PathStrokeType stroke( 1.25f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded );
        if( !row.pathB.isEmpty() )
        {
            g.setColour( row.colourB );
            g.strokePath( row.pathB, stroke );
        }
        if( !row.pathA.isEmpty() )
        {
            g.setColour( row.colourA );
            g.strokePath( row.pathA, stroke );
        }
    }

    void PerformanceOverlay::resized()
    {
        auto       bounds    = getLocalBounds().reduced( padding );
        const auto rowHeight = ( bounds.getHeight() - 3 * rowGap ) / 4;

        for( auto* row : { &frames, &cpu, &memory, &audio } )
        {
            auto area     = bounds.removeFromTop( rowHeight );
            row->textArea = area.removeFromLeft( textWidth );
            area.removeFromLeft( padding );
            row->graphArea = area.toFloat().reduced( 0.5f );
            bounds.removeFromTop( rowGap );
        }
    }
}
