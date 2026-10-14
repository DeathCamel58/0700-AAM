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

#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"

namespace kick
{
    namespace main
    {
        class KickHeartBeatData
        {
        public:

            KickHeartBeatData( std::function<void( const int )> onVerificationFailed )
                : onVerificationFailed(onVerificationFailed)
            {}

        public:
            void registerHeartBeat( const int hostId )
            {
                setHeartBeat( hostId, juce::Time::currentTimeMillis() );
            }

            juce::int64 latestHeartBeat( const int hostId ) const
            {
                const juce::ScopedLock sl( lock );
                if( heartBeatEventTimePoints.contains( hostId ) )
                    return heartBeatEventTimePoints[hostId];
                return -1;
            }

            juce::HashMap<int, juce::int64>::Iterator begin()
            {
                return heartBeatEventTimePoints.begin();
            }

            juce::HashMap<int, juce::int64>::Iterator end()
            {
                return heartBeatEventTimePoints.end();
            }

            void setHeartBeat( const int hostId, juce::int64 timeInMs )
            {
                const juce::ScopedLock sl( lock );
                heartBeatEventTimePoints.set( hostId, juce::Time::currentTimeMillis() );
            }

            // The hosts whose heartbeat verify() expects, whether or not they
            // have sent one yet.
            void expectHeartBeatsFrom( const juce::Array<int>& hostIds )
            {
                const juce::ScopedLock sl( lock );
                expectedHostIds = hostIds;
            }

            // Returns the expected hosts without a heartbeat in the window.
            juce::Array<int> verify( const juce::int64 checkTimePointStart,
                                     const juce::int64 checkPointEnd)
            {
                const juce::ScopedLock sl( lock );
                for( auto itr = heartBeatEventTimePoints.begin(); itr != heartBeatEventTimePoints.end(); itr.next() )
                {
                    juce::int64 timepoint = itr.getValue();
                    int         hostId    = itr.getKey();
                    if( timepoint >= checkTimePointStart && timepoint <= checkPointEnd )
                        continue;
                    juce::MessageManager::callAsync( [&, hostId]() { onVerificationFailed( hostId ); } );
                }

                juce::Array<int> missingHostIds;
                for( const int hostId : expectedHostIds )
                {
                    juce::int64 timepoint = heartBeatEventTimePoints.contains( hostId ) ? heartBeatEventTimePoints[hostId] : -1;
                    if( timepoint < checkTimePointStart || timepoint > checkPointEnd )
                        missingHostIds.add( hostId );
                }
                return missingHostIds;
            }

        private:
            // Heartbeats are registered on the IPC message processor thread
            // and verified on the monitor thread.
            juce::CriticalSection            lock;
            juce::HashMap<int, juce::int64>  heartBeatEventTimePoints;
            juce::Array<int>                 expectedHostIds;
            std::function<void( const int )> onVerificationFailed;
        };
    }
}
