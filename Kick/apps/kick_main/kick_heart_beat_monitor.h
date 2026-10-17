/*
 * Copyright (c) 2024 MUSIC Tribe
 *
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

#include "memory"
#include "kick_heart_beat_data.h"
#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"

const int HEART_BEAT_MONITOR_TIMEOUT_IN_MS( 2000 );
// How long a child may go without a heartbeat before it is restarted. The
// heartbeat comes from the child's message loop, so this is also how long
// that loop may be busy (with the UI, say). It must leave time for the
// restart within the service's WatchdogSec (30 s).
const int HEART_BEAT_RESTART_TIMEOUT_IN_MS( 10000 );
// How long after it starts (the first time or again) a child is not
// judged by its heartbeats: it may take that long to send its first one,
// or keep its message loop busy after it (the user interface builds its
// screens, the engine loads its samples from eMMC). Meanwhile it neither
// holds up the watchdog nor counts as silent. KICK_STARTUP_LIMIT (in
// seconds) overrides it.
const int HEART_BEAT_STARTUP_LIMIT_IN_MS( 60000 );

namespace kick
{
    namespace main
    {
        class KickHeartBeatData;

        // Checks the children's heartbeats every HEART_BEAT_MONITOR_TIMEOUT_IN_MS
        // on the message thread, so the result also shows that KickMain's own
        // message loop is running. Calls onHeartBeatNotReceived for each host
        // that has been quiet for HEART_BEAT_RESTART_TIMEOUT_IN_MS and started
        // more than startupLimit ago, then onHeartBeatsVerified with the state
        // after those restarts.
        class KickHearBeatMonitor : private juce::Timer
        {
        public:
            KickHearBeatMonitor( std::shared_ptr<kick::main::KickHeartBeatData> heartBeatData,
                                 const juce::int64                              startupLimit,
                                 std::function<void( const int )>               onHeartBeatNotReceived,
                                 std::function<void( const KickHeartBeatData::Verification& )> onHeartBeatsVerified = nullptr )
                : heartBeatData( heartBeatData )
                , startupLimit( startupLimit )
                , onHeartBeatNotReceived( onHeartBeatNotReceived )
                , onHeartBeatsVerified( onHeartBeatsVerified )
            {

            }

            ~KickHearBeatMonitor() override
            {
                stopTimer();
            }

            void startMonitoring()
            {
                startTimer( HEART_BEAT_MONITOR_TIMEOUT_IN_MS );
            }

            void stopMonintoring()
            {
                stopTimer();
            }

        private:
            void timerCallback() override
            {
                const auto result = heartBeatData->verify( KickHeartBeatData::now(), HEART_BEAT_RESTART_TIMEOUT_IN_MS, startupLimit );

                if( onHeartBeatNotReceived )
                    for( const int hostId : result.silentHostIds )
                        onHeartBeatNotReceived( hostId );

                // Again, so that a host restarted just now counts as starting.
                if( onHeartBeatsVerified )
                    onHeartBeatsVerified( result.silentHostIds.isEmpty() ? result
                                                                         : heartBeatData->verify( KickHeartBeatData::now(), HEART_BEAT_RESTART_TIMEOUT_IN_MS, startupLimit ) );
            }

            std::shared_ptr<kick::main::KickHeartBeatData> heartBeatData;
            const juce::int64                              startupLimit;
            std::function<void( const int )>               onHeartBeatNotReceived;
            std::function<void( const KickHeartBeatData::Verification& )> onHeartBeatsVerified;
        };
    }
}
