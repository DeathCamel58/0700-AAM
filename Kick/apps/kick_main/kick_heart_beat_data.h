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
                // Expected hosts without a recent enough heartbeat that are
                // not within a startup grace period: they hold up the
                // service manager's watchdog.
                juce::Array<int> missingHostIds;
                // Restarted hosts within their startup grace period, which
                // have not sent a heartbeat yet.
                juce::Array<int> startingHostIds;
                // Hosts to restart: their heartbeats stopped, or their
                // startup grace period ran out before the first one.
                juce::Array<int> silentHostIds;
            };

            // Heartbeat times are on the monotonic millisecond counter, so
            // setting the clock neither causes nor hides a missed heartbeat.
            static juce::int64 now()
            {
                return static_cast<juce::int64>( juce::Time::getMillisecondCounterHiRes() );
            }

            void registerHeartBeat( const int hostId, const juce::int64 timePoint = now() )
            {
                const juce::ScopedLock sl( lock );
                heartBeatEventTimePoints.set( hostId, timePoint );
                startTimePoints.remove( hostId );
            }

            // The host's process has exited. Its last heartbeat no longer
            // counts: the next process gets a startup grace period instead,
            // if this one had come up (sent a heartbeat), or keeps the
            // one it was starting in.
            void hostExited( const int hostId, const juce::int64 timePoint = now() )
            {
                const juce::ScopedLock sl( lock );
                if( heartBeatEventTimePoints.contains( hostId ) )
                {
                    heartBeatEventTimePoints.remove( hostId );
                    startTimePoints.set( hostId, timePoint );
                }
            }

            // The host's process has started: a grace period starts now.
            // Not for the first start of each process, which the service
            // manager's start timeout covers.
            void hostStarted( const int hostId, const juce::int64 timePoint = now() )
            {
                const juce::ScopedLock sl( lock );
                if( startTimePoints.contains( hostId ) )
                    startTimePoints.set( hostId, timePoint );
            }

            // Returns true, and stops counting the host's heartbeat, if
            // verify() would report the host as silent at timePoint: the
            // caller then restarts its process. The process gets a startup
            // grace period if it had come up; one that did not come up
            // within its grace period gets none, so a process that keeps
            // hanging on startup is left to the service manager's watchdog
            // rather than restarted forever.
            bool beginRestartIfSilent( const int hostId, const juce::int64 timePoint, const juce::int64 maxAge, const juce::int64 startupGrace )
            {
                const juce::ScopedLock sl( lock );
                if( heartBeatEventTimePoints.contains( hostId ) )
                {
                    if( timePoint - heartBeatEventTimePoints[hostId] <= maxAge )
                        return false;
                    heartBeatEventTimePoints.remove( hostId );
                    startTimePoints.set( hostId, timePoint );
                    return true;
                }
                if( startTimePoints.contains( hostId ) && timePoint - startTimePoints[hostId] > startupGrace )
                {
                    startTimePoints.remove( hostId );
                    return true;
                }
                return false;
            }

            // The hosts whose heartbeat verify() expects, whether or not they
            // have sent one yet.
            void expectHeartBeatsFrom( const juce::Array<int>& hostIds )
            {
                const juce::ScopedLock sl( lock );
                expectedHostIds = hostIds;
            }

            // A heartbeat counts if it is at most maxAge old at timePoint,
            // and a restarted host is given startupGrace for its first one.
            Verification verify( const juce::int64 timePoint, const juce::int64 maxAge, const juce::int64 startupGrace ) const
            {
                const juce::ScopedLock sl( lock );
                Verification result;
                for( auto itr = heartBeatEventTimePoints.begin(); itr != heartBeatEventTimePoints.end(); itr.next() )
                {
                    if( timePoint - itr.getValue() > maxAge )
                        result.silentHostIds.add( itr.getKey() );
                }
                for( auto itr = startTimePoints.begin(); itr != startTimePoints.end(); itr.next() )
                {
                    if( timePoint - itr.getValue() > startupGrace )
                        result.silentHostIds.add( itr.getKey() );
                }

                for( const int hostId : expectedHostIds )
                {
                    if( heartBeatEventTimePoints.contains( hostId ) && timePoint - heartBeatEventTimePoints[hostId] <= maxAge )
                        continue;
                    if( startTimePoints.contains( hostId ) && timePoint - startTimePoints[hostId] <= startupGrace )
                        result.startingHostIds.add( hostId );
                    else
                        result.missingHostIds.add( hostId );
                }
                return result;
            }

        private:
            // Heartbeats are registered on the IPC message processor thread
            // and verified on the message thread.
            juce::CriticalSection            lock;
            juce::HashMap<int, juce::int64>  heartBeatEventTimePoints;
            // Restarted hosts that have not sent a heartbeat yet, with the
            // start of their grace period.
            juce::HashMap<int, juce::int64>  startTimePoints;
            juce::Array<int>                 expectedHostIds;
        };
    }
}
