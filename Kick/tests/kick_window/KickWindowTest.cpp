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

// Tests of how a Kick process chooses its window mode and renderer
// (kick_StartupOptions.h): command-line options, environment, build default
// and their order, unknown values, and removing the options from the
// command line so that the IPC configuration still parses. And of the
// window's geometry (kick_WindowGeometry.h): the letterboxed scale of the
// layout and how input maps back through it, the saved window bounds, and
// where the overlay goes. And of the frame timing (kick_FrameTiming.h).
// Built with -DKICK_BUILD_TESTS=ON; it needs no arguments and creates no
// window (no juce::Desktop, no display).

#include "kick_window_options/source/kick_StartupOptions.cpp"
#include "kick_window/source/kick_WindowGeometry.cpp"
#include "kick_window/source/kick_FrameTiming.cpp"

#include <cstdio>
#include <map>
#include <string>
#include <thread>

namespace
{
    int failures = 0;

#define CHECK( condition )                                                      \
    do                                                                          \
    {                                                                           \
        if( !( condition ) )                                                    \
        {                                                                       \
            std::printf( "%s:%d: FAILED: %s\n", __FILE__, __LINE__, #condition ); \
            ++failures;                                                         \
        }                                                                       \
    } while( false )

    using kick::window::Letterbox;
    using kick::window::Renderer;
    using kick::window::StartupOptions;
    using kick::window::WindowMode;

    StartupOptions::EnvironmentLookup environment( std::map<std::string, juce::String> variables )
    {
        return [variables]( const char* name ) -> std::optional<juce::String> {
            auto found = variables.find( name );
            if( found == variables.end() )
                return std::nullopt;
            return found->second;
        };
    }

    StartupOptions resolve( const juce::StringArray& arguments, std::map<std::string, juce::String> variables = {},
                            const juce::String& defaultWindowMode = "kiosk", const juce::String& defaultRenderer = "software" )
    {
        return StartupOptions::resolve( arguments, environment( std::move( variables ) ), defaultWindowMode, defaultRenderer );
    }

    void testParsing()
    {
        CHECK( kick::window::parseWindowMode( "kiosk" ) == WindowMode::kiosk );
        CHECK( kick::window::parseWindowMode( " Windowed " ) == WindowMode::windowed );
        CHECK( !kick::window::parseWindowMode( "fullscreen" ).has_value() );
        CHECK( !kick::window::parseWindowMode( "" ).has_value() );
        CHECK( kick::window::parseRenderer( "software" ) == Renderer::software );
        CHECK( kick::window::parseRenderer( "OpenGL" ) == Renderer::openGL );
        CHECK( !kick::window::parseRenderer( "vulkan" ).has_value() );
        CHECK( kick::window::toString( WindowMode::windowed ) == "windowed" );
        CHECK( kick::window::toString( Renderer::openGL ) == "opengl" );
        // What toString() writes, parse*() reads (exportToEnvironment()).
        CHECK( kick::window::parseWindowMode( kick::window::toString( WindowMode::kiosk ) ) == WindowMode::kiosk );
        CHECK( kick::window::parseRenderer( kick::window::toString( Renderer::openGL ) ) == Renderer::openGL );
    }

    void testDefaults()
    {
        auto options = resolve( {} );
        CHECK( options.windowMode == WindowMode::kiosk );
        CHECK( options.renderer == Renderer::software );
        CHECK( options.windowModeSource == "build default" );
        CHECK( options.problems.isEmpty() );
        CHECK( options.getCommandLine().isEmpty() );

        options = resolve( {}, {}, "windowed", "opengl" );
        CHECK( options.windowMode == WindowMode::windowed );
        CHECK( options.renderer == Renderer::openGL );

        // A bad build default: the fallback, and a problem to log.
        options = resolve( {}, {}, "maximised", "software" );
        CHECK( options.windowMode == WindowMode::kiosk );
        CHECK( options.windowModeSource == "fallback" );
        CHECK( options.problems.size() == 1 );
    }

    void testPrecedence()
    {
        // The environment overrides the build default.
        auto options = resolve( {}, { { "KICK_WINDOW_MODE", "windowed" }, { "KICK_RENDERER", "opengl" } } );
        CHECK( options.windowMode == WindowMode::windowed );
        CHECK( options.renderer == Renderer::openGL );
        CHECK( options.windowModeSource == "KICK_WINDOW_MODE" );
        CHECK( options.rendererSource == "KICK_RENDERER" );

        // The command line overrides the environment.
        options = resolve( { "--window-mode=kiosk", "--renderer=software" },
                           { { "KICK_WINDOW_MODE", "windowed" }, { "KICK_RENDERER", "opengl" } } );
        CHECK( options.windowMode == WindowMode::kiosk );
        CHECK( options.renderer == Renderer::software );
        CHECK( options.windowModeSource == "--window-mode" );
        CHECK( options.rendererSource == "--renderer" );

        // The last option given counts.
        options = resolve( { "--window-mode=kiosk", "--window-mode=windowed" } );
        CHECK( options.windowMode == WindowMode::windowed );

        // An empty variable is as good as none.
        options = resolve( {}, { { "KICK_WINDOW_MODE", "" } }, "windowed" );
        CHECK( options.windowMode == WindowMode::windowed );
        CHECK( options.problems.isEmpty() );
    }

    void testUnknownValues()
    {
        // An unknown option value falls back to the environment.
        auto options = resolve( { "--window-mode=huge" }, { { "KICK_WINDOW_MODE", "windowed" } } );
        CHECK( options.windowMode == WindowMode::windowed );
        CHECK( options.windowModeSource == "KICK_WINDOW_MODE" );
        CHECK( options.problems.size() == 1 );

        // An unknown variable falls back to the build default.
        options = resolve( {}, { { "KICK_RENDERER", "directx" } }, "kiosk", "opengl" );
        CHECK( options.renderer == Renderer::openGL );
        CHECK( options.rendererSource == "build default" );
        CHECK( options.problems.size() == 1 );

        // An option without a value is removed and reported.
        options = resolve( { "--renderer", "standalone" } );
        CHECK( options.renderer == Renderer::software );
        CHECK( options.problems.size() == 1 );
        CHECK( options.remainingArguments == juce::StringArray( "standalone" ) );
    }

    void testCommandLine()
    {
        // KickMain's own argument survives.
        auto options = resolve( { "standalone", "--window-mode=windowed" } );
        CHECK( options.getCommandLine() == "standalone" );

        // A child's IPC configuration: one argument, JSON with spaces, which
        // JUCE quotes; the IPC code strips the quotes again.
        const juce::String json = "{\n  \"hostId\": 2,\n  \"name\": \"KickUserInterface\"\n}";
        options = resolve( { "--renderer=opengl", json } );
        CHECK( options.renderer == Renderer::openGL );
        CHECK( options.getCommandLine() == json.quoted( '"' ) );
        auto parsed = juce::JSON::parse( options.getCommandLine().unquoted() );
        CHECK( (int)parsed.getProperty( "hostId", -1 ) == 2 );

        // Arguments without spaces stay as they are; other options too.
        options = resolve( { "--verbose", "--window-modes=x", "a b" } );
        CHECK( options.getCommandLine() == "--verbose --window-modes=x \"a b\"" );
        CHECK( options.problems.isEmpty() );
    }

    bool near( float a, float b )
    {
        return std::abs( a - b ) < 1.0e-3f;
    }

    bool near( juce::Point<float> a, juce::Point<float> b )
    {
        return near( a.x, b.x ) && near( a.y, b.y );
    }

    const juce::Rectangle<int> layout( 0, 0, 1024, 600 );

    void testLetterbox()
    {
        // The layout's own size: unchanged (today's kiosk window).
        auto box = kick::window::computeLetterbox( { 0, 0, 1024, 600 }, layout );
        CHECK( juce::exactlyEqual( box.scale, 1.0f ) );
        CHECK( box.transform.isIdentity() );
        CHECK( box.contentArea == layout.toFloat() );

        // Twice the size: scaled by two, no bars.
        box = kick::window::computeLetterbox( { 0, 0, 2048, 1200 }, layout );
        CHECK( near( box.scale, 2.0f ) );
        CHECK( near( box.offset, { 0.0f, 0.0f } ) );
        CHECK( box.contentArea == juce::Rectangle<float>( 0, 0, 2048, 1200 ) );

        // 16:9 (wider than 1024:600): bars at the sides, centred, whole
        // pixels.
        box = kick::window::computeLetterbox( { 0, 0, 1920, 1080 }, layout );
        CHECK( near( box.scale, 1.8f ) );
        CHECK( near( box.offset, { 38.0f, 0.0f } ) ); // (1920 - 1843.2) / 2 = 38.4
        CHECK( near( box.contentArea.getWidth(), 1843.2f ) );
        CHECK( near( box.contentArea.getHeight(), 1080.0f ) );

        // 4:3 (taller): bars at the top and bottom.
        box = kick::window::computeLetterbox( { 0, 0, 800, 600 }, layout );
        CHECK( near( box.scale, 800.0f / 1024.0f ) );
        CHECK( near( box.offset, { 0.0f, 66.0f } ) ); // (600 - 468.75) / 2 = 65.6
        CHECK( box.contentArea.getBottom() <= 600.0f );

        // An offset area.
        box = kick::window::computeLetterbox( { 100, 50, 1024, 600 }, layout );
        CHECK( juce::exactlyEqual( box.scale, 1.0f ) );
        CHECK( near( box.offset, { 100.0f, 50.0f } ) );

        // Nothing to show in: no transform.
        box = kick::window::computeLetterbox( {}, layout );
        CHECK( box.transform.isIdentity() );
    }

    void testInputThroughLetterbox()
    {
        // JUCE maps a position in the window back into the layout with the
        // inverse transform: the corners and centre of the layout's area
        // must land on the layout's corners and centre.
        for( auto area : { juce::Rectangle<int>( 0, 0, 1920, 1080 ), juce::Rectangle<int>( 0, 0, 800, 600 ),
                           juce::Rectangle<int>( 0, 0, 1280, 800 ), juce::Rectangle<int>( 0, 0, 512, 300 ) } )
        {
            auto box     = kick::window::computeLetterbox( area, layout );
            auto inverse = box.transform.inverted();

            auto topLeft     = box.contentArea.getTopLeft().transformedBy( inverse );
            auto bottomRight = box.contentArea.getBottomRight().transformedBy( inverse );
            auto centre      = box.contentArea.getCentre().transformedBy( inverse );
            CHECK( near( topLeft, { 0.0f, 0.0f } ) );
            CHECK( near( bottomRight, { 1024.0f, 600.0f } ) );
            CHECK( near( centre, { 512.0f, 300.0f } ) );

            // A pad at (100, 200) in the layout is drawn at, and hit at,
            // the same transformed position.
            auto shown = juce::Point<float>( 100.0f, 200.0f ).transformedBy( box.transform );
            CHECK( near( shown.transformedBy( inverse ), { 100.0f, 200.0f } ) );

            // The bars are outside the layout.
            if( box.offset.x > 1.0f )
                CHECK( juce::Point<float>( box.offset.x / 2.0f, 10.0f ).transformedBy( inverse ).x < 0.0f );
            if( box.offset.y > 1.0f )
                CHECK( juce::Point<float>( 10.0f, box.offset.y / 2.0f ).transformedBy( inverse ).y < 0.0f );
        }
    }

    void testWindowBoundsText()
    {
        CHECK( kick::window::formatWindowBounds( { 10, -20, 1024, 600 } ) == "10 -20 1024 600" );
        auto bounds = kick::window::parseWindowBounds( "10 -20 1024 600" );
        CHECK( bounds.has_value() && *bounds == juce::Rectangle<int>( 10, -20, 1024, 600 ) );
        CHECK( kick::window::parseWindowBounds( "  1  2  3  4 " ).has_value() );

        CHECK( !kick::window::parseWindowBounds( "" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "1 2 3" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "1 2 3 4 5" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "a b c d" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "1 2 3.5 4" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "1 2 0 600" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "1 2 -1024 600" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "1 2 99999999999 600" ).has_value() );
        CHECK( !kick::window::parseWindowBounds( "- 2 3 4" ).has_value() );
    }

    // The window with the frame it is placed with (chooseWindowBounds()
    // gives the frame's top-left corner and the client area's size).
    juce::Rectangle<int> withAssumedFrame( juce::Rectangle<int> bounds )
    {
        const auto& frame = kick::window::assumedWindowFrame;
        return bounds.withSize( bounds.getWidth() + frame.getLeftAndRight(), bounds.getHeight() + frame.getTopAndBottom() );
    }

    // Where a window's frame must be on a display: on it, below the panel.
    bool reachable( juce::Rectangle<int> bounds, juce::Rectangle<int> display )
    {
        return display.withTrimmedTop( kick::window::displayTopReserve ).contains( withAssumedFrame( bounds ) );
    }

    void testWindowBounds()
    {
        const juce::Point<int>                  size( 1024, 600 ), minimum( 512, 300 );
        const juce::Rectangle<int>              primary( 0, 0, 1920, 1050 ), secondary( 1920, 0, 1280, 1024 );
        const juce::Array<juce::Rectangle<int>> displays{ primary, secondary };

        // Nothing saved: the layout's size, centred on the display with its
        // frame, below the panel.
        auto bounds = kick::window::chooseWindowBounds( std::nullopt, displays, primary, size, minimum );
        CHECK( bounds.getWidth() == 1024 && bounds.getHeight() == 600 );
        CHECK( reachable( bounds, primary ) );
        CHECK( std::abs( withAssumedFrame( bounds ).getCentreX() - primary.getCentreX() ) <= 1 );

        // On the second display.
        bounds = kick::window::chooseWindowBounds( std::nullopt, displays, secondary, size, minimum );
        CHECK( reachable( bounds, secondary ) );

        // The QEMU display (1280x800, Weston's panel at the top): the layout's
        // size, the title bar below the panel and the frame on the display.
        const juce::Rectangle<int> qemu( 0, 0, 1280, 800 );
        bounds = kick::window::chooseWindowBounds( std::nullopt, { qemu }, qemu, size, minimum );
        CHECK( bounds == juce::Rectangle<int>( 124, 94, 1024, 600 ) );
        CHECK( reachable( bounds, qemu ) );

        // The device's display: smaller than the layout with a frame, so 90%
        // of what fits, at the same aspect ratio.
        const juce::Rectangle<int> device( 0, 0, 1024, 600 );
        bounds = kick::window::chooseWindowBounds( std::nullopt, { device }, device, size, minimum );
        CHECK( reachable( bounds, device ) );
        CHECK( bounds.getWidth() == 804 ); // 90% of (600 - 32 - 44) * 1024 / 600 = 894
        CHECK( bounds.getHeight() == 471 );

        // A display smaller than the layout: 90% of what fits.
        const juce::Rectangle<int> small( 0, 0, 800, 480 );
        bounds = kick::window::chooseWindowBounds( std::nullopt, { small }, small, size, minimum );
        CHECK( reachable( bounds, small ) );
        CHECK( bounds.getWidth() == 620 ); // 90% of (480 - 32 - 44) * 1024 / 600 = 689
        CHECK( bounds.getHeight() == 363 );

        // Never below the minimum, on the display's top-left corner if it
        // does not fit.
        const juce::Rectangle<int> tiny( 0, 0, 320, 240 );
        bounds = kick::window::chooseWindowBounds( std::nullopt, { tiny }, tiny, size, minimum );
        CHECK( bounds == juce::Rectangle<int>( 0, 32, 512, 300 ) );

        // Saved bounds on a display are used, with the aspect ratio kept.
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 2000, 100, 1024, 640 ), displays, primary, size, minimum );
        CHECK( bounds == juce::Rectangle<int>( 2000, 100, 1024, 600 ) );

        // Larger than the display they are on: as large as fits, on it.
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 2000, 100, 1536, 900 ), displays, primary, size, minimum );
        CHECK( reachable( bounds, secondary ) );
        CHECK( bounds.getWidth() == 1272 ); // 1280 - 8
        CHECK( bounds.getHeight() == 745 );

        // Partly off the display or under the panel: moved onto it.
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 1800, 900, 1024, 600 ), { primary }, primary, size, minimum );
        CHECK( bounds.getWidth() == 1024 && reachable( bounds, primary ) );
        CHECK( bounds.getPosition() == juce::Point<int>( 1920 - 1032, 1050 - 644 ) );
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( -160, 0, 1024, 600 ), { qemu }, qemu, size, minimum );
        CHECK( bounds == juce::Rectangle<int>( 0, 32, 1024, 600 ) );

        // The bounds of a window the shell made full screen (Weston's kiosk
        // shell): not the window's own, so the default instead, as for
        // bounds that cover the display under the panel.
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 0, 0, 1280, 800 ), { qemu }, qemu, size, minimum );
        CHECK( bounds == juce::Rectangle<int>( 124, 94, 1024, 600 ) );
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 0, 0, 1280, 750 ), { qemu }, qemu, size, minimum );
        CHECK( reachable( bounds, qemu ) );
        CHECK( kick::window::fillsADisplay( { 0, 0, 1280, 800 }, { qemu } ) );
        CHECK( kick::window::fillsADisplay( { -4, -40, 1300, 900 }, { qemu } ) );
        CHECK( !kick::window::fillsADisplay( { 0, 0, 1280, 750 }, { qemu } ) );
        CHECK( !kick::window::fillsADisplay( { 0, 0, 1280, 800 }, {} ) );

        // On a display that is gone: centred on the current one instead.
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 3500, 100, 1024, 600 ), { primary }, primary, size, minimum );
        CHECK( std::abs( withAssumedFrame( bounds ).getCentreX() - primary.getCentreX() ) <= 1 );
        CHECK( reachable( bounds, primary ) );

        // Too small to be valid: the default.
        bounds = kick::window::chooseWindowBounds( juce::Rectangle<int>( 10, 10, 100, 60 ), displays, primary, size, minimum );
        CHECK( bounds.getWidth() == 1024 );

        // No display known: the layout's size at the origin.
        bounds = kick::window::chooseWindowBounds( std::nullopt, {}, {}, size, minimum );
        CHECK( bounds == juce::Rectangle<int>( 0, 0, 1024, 600 ) );
    }
}

namespace
{
    // The panel's corners, where the window shows them.
    juce::Rectangle<float> placedPanel( juce::Rectangle<int> area, juce::Point<int> panel, int margin )
    {
        const auto transform = kick::window::placeInBottomRight( area, layout, panel, margin );
        return juce::Rectangle<float>( 0.0f, 0.0f, (float)panel.x, (float)panel.y ).transformedBy( transform );
    }

    void testOverlayPlacement()
    {
        const juce::Point<int> panel( 300, 170 );

        // The device: the layout's size, the panel unscaled, 6 in from the
        // corner.
        auto shown = placedPanel( { 0, 0, 1024, 600 }, panel, 6 );
        CHECK( shown == juce::Rectangle<float>( 718.0f, 424.0f, 300.0f, 170.0f ) );

        // Twice the size: scaled with the layout, margin too.
        shown = placedPanel( { 0, 0, 2048, 1200 }, panel, 6 );
        CHECK( shown == juce::Rectangle<float>( 1436.0f, 848.0f, 600.0f, 340.0f ) );

        // Bars at the sides (16:9 at 1080 lines, scale 1.8): in the
        // window's corner, over the bar, not in the layout's corner.
        shown = placedPanel( { 0, 0, 1920, 1080 }, panel, 6 );
        CHECK( near( shown.getRight(), 1920.0f - 11.0f ) ); // 6 * 1.8 = 10.8
        CHECK( near( shown.getBottom(), 1080.0f - 11.0f ) );
        CHECK( near( shown.getWidth(), 540.0f ) );
        CHECK( near( shown.getHeight(), 306.0f ) );

        // Bars at the top and bottom (4:3, scale 0.78).
        shown = placedPanel( { 0, 0, 800, 600 }, panel, 6 );
        CHECK( near( shown.getWidth(), 300.0f * 800.0f / 1024.0f ) );
        // (6 * 0.78 = 4.7 in, give or take the rounding to whole pixels.)
        CHECK( shown.getRight() <= 800.0f - 3.5f && shown.getRight() >= 800.0f - 6.0f );
        CHECK( shown.getBottom() <= 600.0f - 3.5f && shown.getBottom() >= 600.0f - 6.0f );

        // Whole pixels at any scale.
        for( auto area : { juce::Rectangle<int>( 0, 0, 1280, 800 ), juce::Rectangle<int>( 0, 0, 1366, 768 ),
                           juce::Rectangle<int>( 0, 0, 777, 455 ) } )
        {
            const auto transform = kick::window::placeInBottomRight( area, layout, panel, 6 );
            CHECK( juce::exactlyEqual( transform.getTranslationX(), std::round( transform.getTranslationX() ) ) );
            CHECK( juce::exactlyEqual( transform.getTranslationY(), std::round( transform.getTranslationY() ) ) );
            CHECK( area.toFloat().contains( placedPanel( area, panel, 6 ) ) );
        }

        // An area not at the origin.
        shown = placedPanel( { 100, 50, 1024, 600 }, panel, 6 );
        CHECK( shown.getBottomRight() == juce::Point<float>( 1118.0f, 644.0f ) );

        // Smaller than the panel: kept at the area's top-left, not outside.
        shown = placedPanel( { 0, 0, 200, 100 }, panel, 6 );
        CHECK( shown.getX() >= 0.0f && shown.getY() >= 0.0f );

        // Nothing to show in: no scale, no singular transform.
        CHECK( !kick::window::placeInBottomRight( {}, layout, panel, 6 ).isSingularity() );
    }

    bool nearMs( double a, double b )
    {
        return std::abs( a - b ) < 1.0e-6;
    }

    void testFrameTiming()
    {
        kick::window::FrameTiming timing;

        // Off: nothing is recorded.
        timing.paintStarted( 0.0 );
        timing.paintFinished( 5.0 );
        timing.displayTick( 1.0 );
        CHECK( !timing.isEnabled() );
        CHECK( timing.takePeriod( 10.0 ).frames == 0 );

        timing.setEnabled( true, 100.0 );
        CHECK( timing.isEnabled() );

        // Three frames: 100-104, 116.7-118.7, 150-160.
        timing.paintStarted( 100.0 );
        timing.paintFinished( 104.0 );
        timing.paintStarted( 116.7 );
        timing.paintFinished( 118.7 );
        timing.paintStarted( 150.0 );
        timing.paintFinished( 160.0 );

        // Display ticks 16.7 ms apart, then one 50 ms late.
        timing.displayTick( 100.0 );
        timing.displayTick( 116.7 );
        timing.displayTick( 133.4 );
        timing.displayTick( 183.4 );

        auto period = timing.takePeriod( 200.0 );
        CHECK( juce::exactlyEqual( period.lengthMs, 100.0 ) );
        CHECK( period.frames == 3 );
        CHECK( nearMs( period.framesPerSecond(), 30.0 ) );
        CHECK( nearMs( period.longestPaintMs, 10.0 ) );
        CHECK( nearMs( period.averagePaintMs(), 16.0 / 3.0 ) );
        CHECK( nearMs( period.longestTickGapMs, 50.0 ) );

        // The next period starts empty, but the gaps carry on from the last
        // tick.
        timing.displayTick( 200.0 );
        timing.paintStarted( 210.0 );
        timing.paintFinished( 211.0 );
        period = timing.takePeriod( 300.0 );
        CHECK( juce::exactlyEqual( period.lengthMs, 100.0 ) );
        CHECK( period.frames == 1 );
        CHECK( nearMs( period.longestTickGapMs, 16.6 ) );
        CHECK( nearMs( period.longestPaintMs, 1.0 ) );

        // Nothing painted: no frames, no rates, no division by zero.
        period = timing.takePeriod( 400.0 );
        CHECK( period.frames == 0 );
        CHECK( juce::exactlyEqual( period.framesPerSecond(), 0.0 ) );
        CHECK( juce::exactlyEqual( period.averagePaintMs(), 0.0 ) );
        CHECK( juce::exactlyEqual( timing.takePeriod( 400.0 ).framesPerSecond(), 0.0 ) );

        // Turned on half-way through a frame: that frame is not counted.
        timing.setEnabled( false, 500.0 );
        timing.paintStarted( 500.0 );
        timing.setEnabled( true, 501.0 );
        timing.paintFinished( 502.0 );
        period = timing.takePeriod( 600.0 );
        CHECK( period.frames == 0 );

        // Turning it on again starts afresh: no gap from before.
        timing.setEnabled( false, 700.0 );
        timing.setEnabled( true, 1000.0 );
        timing.paintStarted( 1000.0 );
        timing.paintFinished( 1001.0 );
        timing.displayTick( 1000.0 );
        period = timing.takePeriod( 1100.0 );
        CHECK( period.frames == 1 );
        CHECK( juce::exactlyEqual( period.longestTickGapMs, 0.0 ) );
    }

    void testFrameTimingThreads()
    {
        // The OpenGL renderer paints on its own thread while the overlay
        // takes the figures on the message thread: no frame may be lost or
        // counted twice.
        kick::window::FrameTiming timing;
        timing.setEnabled( true, 0.0 );

        constexpr int     framesToPaint = 200000;
        std::atomic<bool> done{ false };
        int               taken = 0;

        std::thread painter( [&]() {
            for( int i = 0; i < framesToPaint; ++i )
            {
                timing.paintStarted( i * 2.0 );
                timing.paintFinished( i * 2.0 + 1.0 );
            }
            done = true;
        } );

        double now = 0.0;
        while( !done )
            taken += timing.takePeriod( now += 1.0 ).frames;
        painter.join();
        taken += timing.takePeriod( now + 1.0 ).frames;

        CHECK( taken == framesToPaint );
    }
}

int main()
{
    testParsing();
    testDefaults();
    testPrecedence();
    testUnknownValues();
    testCommandLine();
    testLetterbox();
    testInputThroughLetterbox();
    testWindowBoundsText();
    testWindowBounds();
    testOverlayPlacement();
    testFrameTiming();
    testFrameTimingThreads();

    if( failures != 0 )
    {
        std::printf( "%d check(s) failed\n", failures );
        return 1;
    }

    std::printf( "All window tests passed\n" );
    return 0;
}
