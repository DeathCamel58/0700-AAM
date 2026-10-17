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

#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>

namespace kick::window
{
    // Kiosk: a borderless window that covers the display (the device).
    // Windowed: a desktop window with a title bar, which can be resized.
    enum class WindowMode
    {
        kiosk,
        windowed
    };

    // Software: JUCE's software renderer. OpenGL: JUCE paints the window's
    // components with OpenGL (GLX).
    enum class Renderer
    {
        software,
        openGL
    };

    std::optional<WindowMode> parseWindowMode( const juce::String& text );
    std::optional<Renderer>   parseRenderer( const juce::String& text );
    juce::String              toString( WindowMode mode );
    juce::String              toString( Renderer renderer );

    // The window mode and renderer a process starts with. Each comes from,
    // in this order: its command-line option (--window-mode=, --renderer=),
    // its environment variable (KICK_WINDOW_MODE, KICK_RENDERER), which
    // KickMain's children inherit, and the build's default
    // (KICK_DEFAULT_WINDOW_MODE, KICK_DEFAULT_RENDERER). A value that is not
    // understood is reported and the next source is used.
    //
    // The options are removed from the arguments, because the rest of the
    // command line is the process's IPC configuration.
    struct StartupOptions
    {
        static constexpr const char* windowModeOption   = "--window-mode";
        static constexpr const char* rendererOption     = "--renderer";
        static constexpr const char* windowModeVariable = "KICK_WINDOW_MODE";
        static constexpr const char* rendererVariable   = "KICK_RENDERER";
        static constexpr const char* showCursorVariable = "KICK_SHOW_CURSOR";
        static constexpr const char* debugInputOption   = "--debug-input";
        static constexpr const char* debugInputVariable = "KICK_DEBUG_INPUT";

        WindowMode windowMode{ WindowMode::kiosk };
        Renderer   renderer{ Renderer::software };

        // Whether the mouse pointer shows over the window: always in
        // windowed mode; in kiosk mode (a touch screen) only when
        // KICK_SHOW_CURSOR is 1 (or true, yes, on).
        bool showCursor{ false };

        // Whether every tap and the pop-ups' and lists' decisions go to the
        // log, for finding input problems: --debug-input (or
        // --debug-input=1), or KICK_DEBUG_INPUT=1. Off by default.
        bool debugInput{ false };

        // Where each choice came from, for the log.
        juce::String windowModeSource;
        juce::String rendererSource;

        // The arguments without the options above.
        juce::StringArray remainingArguments;

        // Values (or options) that were not understood.
        juce::StringArray problems;

        // The remaining arguments as one string, quoted as
        // JUCEApplicationBase::getCommandLineParameters() quotes them.
        juce::String getCommandLine() const;

        // "window mode windowed (from --window-mode), renderer software
        // (...), mouse pointer shown"
        juce::String describe() const;

        // Writes every problem and the choice to the current logger.
        void log() const;

        // Sets the environment variables to the choices, so that the
        // processes this one starts use them too.
        void exportToEnvironment() const;

        using EnvironmentLookup = std::function<std::optional<juce::String>( const char* name )>;

        static StartupOptions resolve( const juce::StringArray& arguments,
                                       const EnvironmentLookup& environment,
                                       const juce::String&      defaultWindowMode,
                                       const juce::String&      defaultRenderer );

        // resolve() with this process's environment and the build defaults.
        static StartupOptions fromProcess( const juce::StringArray& arguments );
    };
}
