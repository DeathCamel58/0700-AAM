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

#include "kick_FrameTiming.h"

namespace kick::window
{
    void FrameTiming::setEnabled( bool shouldRecord, double nowMs )
    {
        const juce::SpinLock::ScopedLockType scope( lock );
        if( shouldRecord && !enabled.load( std::memory_order_relaxed ) )
            reset( nowMs );
        enabled.store( shouldRecord, std::memory_order_relaxed );
    }

    void FrameTiming::reset( double nowMs )
    {
        current     = {};
        periodStart = nowMs;
        paintStart  = -1.0;
        lastTick    = -1.0;
    }

    void FrameTiming::paintStarted( double nowMs )
    {
        if( !isEnabled() )
            return;

        const juce::SpinLock::ScopedLockType scope( lock );
        paintStart = nowMs;
    }

    void FrameTiming::paintFinished( double nowMs )
    {
        if( !isEnabled() )
            return;

        const juce::SpinLock::ScopedLockType scope( lock );
        // Not when recording started half-way through a frame.
        if( paintStart < 0.0 )
            return;

        const auto paintMs = juce::jmax( 0.0, nowMs - paintStart );
        paintStart         = -1.0;

        ++current.frames;
        current.totalPaintMs += paintMs;
        current.longestPaintMs = juce::jmax( current.longestPaintMs, paintMs );
    }

    void FrameTiming::displayTick( double nowMs )
    {
        if( !isEnabled() )
            return;

        const juce::SpinLock::ScopedLockType scope( lock );
        if( lastTick >= 0.0 )
            current.longestTickGapMs = juce::jmax( current.longestTickGapMs, nowMs - lastTick );
        lastTick = nowMs;
    }

    FrameTiming::Period FrameTiming::takePeriod( double nowMs )
    {
        const juce::SpinLock::ScopedLockType scope( lock );
        auto period     = current;
        period.lengthMs = juce::jmax( 0.0, nowMs - periodStart );

        current     = {};
        periodStart = nowMs;
        return period;
    }
}
