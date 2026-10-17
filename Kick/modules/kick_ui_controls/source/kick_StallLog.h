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

#include "kick_StallCheck.h"

namespace ui
{
    namespace stallLog
    {
        // Logs when the message thread is kept from handling input, redraws,
        // timers and IPC messages: "UI: message thread stalled N ms",
        // always from 1 s on, and with input debugging on (--debug-input or
        // KICK_DEBUG_INPUT=1) from 250 ms on, with the longest named step
        // (Phase) that ran meanwhile. Create one on the message thread,
        // once the window is up.
        class Detector final : private juce::Timer
        {
        public:
            Detector();
            ~Detector() override;

        private:
            void timerCallback() override;

            StallCheck check;

            JUCE_DECLARE_NON_COPYABLE( Detector )
        };

        // Names a piece of work on the message thread for the stall log,
        // for as long as it exists. What, and Type (a typeid name, or
        // nullptr), must stay valid: string literals and typeid names do.
        // Costs a clock read when no Detector runs.
        class Phase final
        {
        public:
            explicit Phase( const char* What, const char* Type = nullptr );
            ~Phase();

        private:
            const char* what;
            const char* type;
            double      startMs;

            JUCE_DECLARE_NON_COPYABLE( Phase )
        };
    }
}
