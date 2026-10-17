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

namespace ui
{
    // Writes a line to the log ("Tap: ...") for every press and release of
    // the mouse or a touch, and for the decisions the controls take on
    // them, to find out where a tap that did nothing went: what was under
    // it, what was modal and whether that blocked it, where the focus was,
    // and how long the press lasted by the events' times and by when they
    // were handled.
    namespace tapLog
    {
        // A component as the log names it: its class, and its name or
        // title if it has one.
        juce::String describe( const juce::Component* c );

        // The component and its parents, up to the window ("a < b < c").
        juce::String chain( const juce::Component* c );

        // The modal components, front first, and whether each can be seen.
        juce::String modalState();

        // How long the press that ended with Event lasted, in milliseconds,
        // by the events' own times.
        inline juce::int64 pressDuration( const juce::MouseEvent& Event )
        {
            return ( Event.eventTime - Event.mouseDownTime ).inMilliseconds();
        }

        // The "Tap:" lines and the pop-ups' and lists' traces are for
        // finding input problems, so they are only written when input
        // debugging is on (--debug-input or KICK_DEBUG_INPUT=1); errors
        // always are.
        void setEnabled( bool Enabled );
        bool isEnabled();

        // A trace line: written only when input debugging is on.
        void write( const juce::String& Line );

        // An error or an unexpected state: always written.
        void writeError( const juce::String& Line );

        // Sees every press and release in the program, including the ones
        // a modal component keeps from the component they were meant for
        // (JUCE still tells the global mouse listeners of a press it
        // blocks, but not of its release). Logs one line for each.
        class Listener final : public juce::MouseListener
        {
        public:
            Listener();
            ~Listener() override;

            void mouseDown( const juce::MouseEvent& Event ) override;
            void mouseUp( const juce::MouseEvent& Event ) override;

        private:
            // When the last press was handled, by the message thread's clock.
            double downHandledMs{ 0.0 };

            // The smallest difference seen between when an event was
            // handled and its time, taken as no wait (see lagOf()).
            juce::int64 promptDifferenceMs{ 0 };
            bool        havePromptDifference{ false };

            juce::int64  lagOf( const juce::MouseEvent& Event );
            juce::String describeEvent( const juce::MouseEvent& Event );

            JUCE_DECLARE_NON_COPYABLE( Listener )
        };
    }
}
