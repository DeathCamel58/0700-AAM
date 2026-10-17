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

namespace kick::window
{
    // How the window paints its frames, gathered while something watches
    // (the performance overlay): the frames painted, how long painting them
    // took, and the longest gap between two display ticks. (On Linux,
    // JUCE's display tick is a timer on the message thread at the display's
    // refresh rate, which also flushes the pending repaints: a late tick is
    // a late frame, whatever was painted.)
    //
    // The software renderer paints on the message thread; the OpenGL
    // renderer on its own thread, holding the message manager's lock. A
    // spin lock keeps the figures whole either way. Times are in
    // milliseconds, from now().
    class FrameTiming
    {
    public:
        // What happened between two takePeriod() calls.
        struct Period
        {
            double lengthMs{ 0.0 };
            int    frames{ 0 };
            double totalPaintMs{ 0.0 };
            double longestPaintMs{ 0.0 };
            double longestTickGapMs{ 0.0 };

            double framesPerSecond() const { return lengthMs > 0.0 ? frames * 1000.0 / lengthMs : 0.0; }
            double averagePaintMs() const { return frames > 0 ? totalPaintMs / frames : 0.0; }
        };

        static double now() { return juce::Time::getMillisecondCounterHiRes(); }

        // Off, nothing is recorded (the paint calls cost an atomic load).
        // Turning it on starts a new period.
        void setEnabled( bool shouldRecord, double nowMs = now() );
        bool isEnabled() const { return enabled.load( std::memory_order_relaxed ); }

        // Around painting a frame.
        void paintStarted( double nowMs = now() );
        void paintFinished( double nowMs = now() );

        void displayTick( double nowMs = now() );

        // The figures since the previous call (or since recording started),
        // and starts the next period.
        Period takePeriod( double nowMs = now() );

    private:
        void reset( double nowMs );

        std::atomic<bool> enabled{ false };
        juce::SpinLock    lock;

        Period current;
        double periodStart{ 0.0 };
        double paintStart{ -1.0 }; // < 0: not painting
        double lastTick{ -1.0 };
    };
}
