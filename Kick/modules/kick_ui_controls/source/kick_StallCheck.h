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
    namespace stallLog
    {
        // How often the message thread is checked, and how late a check
        // must come to count as a stall: logged always from errorMs on (an
        // input or a redraw waited that long), and with input debugging on
        // from traceMs on.
        constexpr int intervalMs = 100;
        constexpr int traceMs    = 250;
        constexpr int errorMs    = 1000;

        // Works out from the times of a timer's callbacks how long the
        // message thread was kept from calling it back, and what to log.
        // The clock is passed in, so that it can be tested.
        class StallCheck
        {
        public:
            struct Report
            {
                juce::String line; // empty: nothing to log
                bool         error{ false };
            };

            explicit StallCheck( double NowMs ) : lastMs( NowMs ) {}

            // A named piece of work on the message thread ended after Ms.
            // The longest since the last check is the one a stall names.
            // What and Type (a typeid name, or nullptr) must stay valid.
            void phaseEnded( const char* What, const char* Type, double Ms )
            {
                if( What != nullptr && Ms > slowestMs )
                {
                    slowestWhat = What;
                    slowestType = Type;
                    slowestMs   = Ms;
                }
            }

            // The timer called back at NowMs. Detail adds the stalls
            // between traceMs and errorMs, and what was running.
            Report check( double NowMs, bool Detail, const std::function<juce::String( const char* )>& TypeName = nullptr )
            {
                const auto stalledMs = juce::roundToInt( NowMs - lastMs - intervalMs );
                lastMs               = NowMs;

                Report report;
                if( stalledMs >= errorMs || ( Detail && stalledMs >= traceMs ) )
                {
                    report.error = stalledMs >= errorMs;
                    report.line  = "UI: message thread stalled " + juce::String( stalledMs ) + " ms";

                    if( Detail )
                    {
                        if( slowestWhat != nullptr )
                        {
                            report.line << ", longest step: " << slowestWhat;
                            if( slowestType != nullptr )
                                report.line << " " << ( TypeName ? TypeName( slowestType ) : juce::String( slowestType ) );
                            report.line << " (" << juce::roundToInt( slowestMs ) << " ms)";
                        }
                        else
                        {
                            report.line << ", no named step";
                        }
                    }
                }

                slowestWhat = nullptr;
                slowestType = nullptr;
                slowestMs   = 0.0;
                return report;
            }

        private:
            double      lastMs;
            const char* slowestWhat{ nullptr };
            const char* slowestType{ nullptr };
            double      slowestMs{ 0.0 };
        };
    }
}
