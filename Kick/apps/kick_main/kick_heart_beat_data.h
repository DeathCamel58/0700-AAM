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
            struct Verification
            {
                // Expected hosts without a recent enough heartbeat.
                juce::Array<int> missingHostIds;
                // Hosts that have sent a heartbeat before, but none recent
                // enough.
                juce::Array<int> silentHostIds;
            };

            // Heartbeat times are on the monotonic millisecond counter, so
            // setting the clock neither causes nor hides a missed heartbeat.
            static juce::int64 now()
            {
                return static_cast<juce::int64>( juce::Time::getMillisecondCounterHiRes() );
            }

            void registerHeartBeat( const int hostId )
            {
                const juce::ScopedLock sl( lock );
                heartBeatEventTimePoints.set( hostId, now() );
            }

            juce::int64 latestHeartBeat( const int hostId ) const
            {
                const juce::ScopedLock sl( lock );
                if( heartBeatEventTimePoints.contains( hostId ) )
                    return heartBeatEventTimePoints[hostId];
                return -1;
            }

            // Drops the host's heartbeat, so that verify() asks for no
            // further restart until the host has sent a new one.
            void forgetHeartBeat( const int hostId )
            {
                const juce::ScopedLock sl( lock );
                heartBeatEventTimePoints.remove( hostId );
            }

            // The hosts whose heartbeat verify() expects, whether or not they
            // have sent one yet.
            void expectHeartBeatsFrom( const juce::Array<int>& hostIds )
            {
                const juce::ScopedLock sl( lock );
                expectedHostIds = hostIds;
            }

            // A heartbeat counts if it is at most maxAge old at timePoint.
            Verification verify( const juce::int64 timePoint, const juce::int64 maxAge ) const
            {
                const juce::ScopedLock sl( lock );
                Verification result;
                for( auto itr = heartBeatEventTimePoints.begin(); itr != heartBeatEventTimePoints.end(); itr.next() )
                {
                    if( timePoint - itr.getValue() > maxAge )
                        result.silentHostIds.add( itr.getKey() );
                }

                for( const int hostId : expectedHostIds )
                {
                    if( !heartBeatEventTimePoints.contains( hostId ) || timePoint - heartBeatEventTimePoints[hostId] > maxAge )
                        result.missingHostIds.add( hostId );
                }
                return result;
            }

        private:
            // Heartbeats are registered on the IPC message processor thread
            // and verified on the message thread.
            juce::CriticalSection            lock;
            juce::HashMap<int, juce::int64>  heartBeatEventTimePoints;
            juce::Array<int>                 expectedHostIds;
        };
    }
}
