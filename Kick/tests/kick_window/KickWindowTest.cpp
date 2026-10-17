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
// command line so that the IPC configuration still parses. Built with
// -DKICK_BUILD_TESTS=ON; it needs no arguments and creates no window.

#include "kick_window_options/source/kick_StartupOptions.cpp"

#include <cstdio>
#include <map>
#include <string>

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
}

int main()
{
    testParsing();
    testDefaults();
    testPrecedence();
    testUnknownValues();
    testCommandLine();

    if( failures != 0 )
    {
        std::printf( "%d check(s) failed\n", failures );
        return 1;
    }

    std::printf( "All window option tests passed\n" );
    return 0;
}
