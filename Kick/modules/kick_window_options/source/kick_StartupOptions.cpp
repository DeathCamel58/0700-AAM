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

#include "kick_StartupOptions.h"

#include <cstdlib>

#ifndef KICK_DEFAULT_WINDOW_MODE
    #define KICK_DEFAULT_WINDOW_MODE "kiosk"
#endif
#ifndef KICK_DEFAULT_RENDERER
    #define KICK_DEFAULT_RENDERER "software"
#endif

namespace kick::window
{
    namespace
    {
        juce::String normalised( const juce::String& text )
        {
            return text.trim().toLowerCase();
        }

        template<typename Value>
        struct Choice
        {
            Value        value;
            juce::String source;
        };

        // The first value of the option, the variable and the default that
        // parses; the fallback when none does.
        template<typename Value, typename Parser>
        Choice<Value> choose( const std::optional<juce::String>& optionValue,
                              const char*                        optionName,
                              const std::optional<juce::String>& variableValue,
                              const char*                        variableName,
                              const juce::String&                defaultValue,
                              Value                              fallback,
                              Parser                             parse,
                              juce::StringArray&                 problems )
        {
            if( optionValue.has_value() )
            {
                if( auto value = parse( *optionValue ) )
                    return { *value, optionName };
                problems.add( "Ignoring " + juce::String( optionName ) + "=" + *optionValue + ": unknown value" );
            }

            if( variableValue.has_value() && variableValue->trim().isNotEmpty() )
            {
                if( auto value = parse( *variableValue ) )
                    return { *value, variableName };
                problems.add( "Ignoring " + juce::String( variableName ) + "=" + *variableValue + ": unknown value" );
            }

            if( auto value = parse( defaultValue ) )
                return { *value, "build default" };
            problems.add( "Ignoring the build default \"" + defaultValue + "\": unknown value" );

            return { fallback, "fallback" };
        }
    }

    std::optional<WindowMode> parseWindowMode( const juce::String& text )
    {
        const auto value = normalised( text );
        if( value == "kiosk" )
            return WindowMode::kiosk;
        if( value == "windowed" )
            return WindowMode::windowed;
        return std::nullopt;
    }

    std::optional<Renderer> parseRenderer( const juce::String& text )
    {
        const auto value = normalised( text );
        if( value == "software" )
            return Renderer::software;
        if( value == "opengl" )
            return Renderer::openGL;
        return std::nullopt;
    }

    juce::String toString( WindowMode mode )
    {
        return mode == WindowMode::windowed ? "windowed" : "kiosk";
    }

    juce::String toString( Renderer renderer )
    {
        return renderer == Renderer::openGL ? "opengl" : "software";
    }

    juce::String StartupOptions::getCommandLine() const
    {
        juce::String commandLine;
        for( const auto& argument : remainingArguments )
        {
            commandLine << ( argument.containsChar( ' ' ) && !argument.isQuotedString() ? argument.quoted( '"' ) : argument )
                        << ' ';
        }
        return commandLine.trim();
    }

    juce::String StartupOptions::describe() const
    {
        return "window mode " + toString( windowMode ) + " (from " + windowModeSource + "), renderer "
               + toString( renderer ) + " (from " + rendererSource + ")";
    }

    void StartupOptions::log() const
    {
        for( const auto& problem : problems )
            juce::Logger::writeToLog( problem );
        juce::Logger::writeToLog( "Display: " + describe() );
    }

    void StartupOptions::exportToEnvironment() const
    {
        ::setenv( windowModeVariable, toString( windowMode ).toRawUTF8(), 1 );
        ::setenv( rendererVariable, toString( renderer ).toRawUTF8(), 1 );
    }

    StartupOptions StartupOptions::resolve( const juce::StringArray& arguments,
                                            const EnvironmentLookup& environment,
                                            const juce::String&      defaultWindowMode,
                                            const juce::String&      defaultRenderer )
    {
        StartupOptions options;

        // Only the "--name=value" form: a separate value could not be told
        // apart from the IPC configuration that follows.
        std::optional<juce::String> windowModeValue, rendererValue;
        for( const auto& argument : arguments )
        {
            const auto name = argument.upToFirstOccurrenceOf( "=", false, false );
            const bool hasValue = argument.containsChar( '=' );

            if( name == windowModeOption || name == rendererOption )
            {
                if( !hasValue )
                    options.problems.add( "Ignoring " + argument + ": it needs a value (" + name + "=...)" );
                else if( name == windowModeOption )
                    windowModeValue = argument.fromFirstOccurrenceOf( "=", false, false );
                else
                    rendererValue = argument.fromFirstOccurrenceOf( "=", false, false );
                continue;
            }

            options.remainingArguments.add( argument );
        }

        const auto windowMode = choose( windowModeValue, windowModeOption,
                                        environment ? environment( windowModeVariable ) : std::nullopt, windowModeVariable,
                                        defaultWindowMode, WindowMode::kiosk, parseWindowMode, options.problems );
        options.windowMode       = windowMode.value;
        options.windowModeSource = windowMode.source;

        const auto renderer = choose( rendererValue, rendererOption,
                                      environment ? environment( rendererVariable ) : std::nullopt, rendererVariable,
                                      defaultRenderer, Renderer::software, parseRenderer, options.problems );
        options.renderer       = renderer.value;
        options.rendererSource = renderer.source;

        return options;
    }

    StartupOptions StartupOptions::fromProcess( const juce::StringArray& arguments )
    {
        return resolve( arguments,
                        []( const char* name ) -> std::optional<juce::String> {
                            if( const char* value = std::getenv( name ) )
                                return juce::String::fromUTF8( value );
                            return std::nullopt;
                        },
                        KICK_DEFAULT_WINDOW_MODE, KICK_DEFAULT_RENDERER );
    }
}
