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
                // not within their startup window: they hold up the
                // service manager's watchdog.
                juce::Array<int> missingHostIds;
                // Hosts within their startup window without a recent
                // heartbeat: started or restarted, and not up yet, or busy
                // starting up after a first heartbeat.
                juce::Array<int> startingHostIds;
                // Hosts to restart: their heartbeats stopped, or their
                // startup window ran out without a recent one.
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
                windowDenied.removeFirstMatchingValue( hostId );
            }

            // The host's process has exited. Its last heartbeat no longer
            // counts; the time until its successor starts counts as part of
            // the successor's startup window.
            void hostExited( const int hostId, const juce::int64 timePoint = now() )
            {
                const juce::ScopedLock sl( lock );
                heartBeatEventTimePoints.remove( hostId );
                if( windowDenied.contains( hostId ) )
                    startTimePoints.remove( hostId );
                else
                    startTimePoints.set( hostId, timePoint );
            }

            // The host's process has started, the first time or again: its
            // startup window starts now. A process that starts up slowly,
            // or keeps its message loop busy for a while after its first
            // heartbeat (building its screens, loading samples), is neither
            // restarted nor holds up the watchdog within the window.
            void hostStarted( const int hostId, const juce::int64 timePoint = now() )
            {
                const juce::ScopedLock sl( lock );
                heartBeatEventTimePoints.remove( hostId );
                if( windowDenied.contains( hostId ) )
                    startTimePoints.remove( hostId );
                else
                    startTimePoints.set( hostId, timePoint );
            }

            // Returns true, and stops counting the host's heartbeat, if
            // verify() would report the host as silent at timePoint: the
            // caller then restarts its process. The successor gets a
            // startup window if this process had come up (sent a heartbeat
            // after its own window); one that did not come up gets none
            // until a process of the host sends a heartbeat, so a process
            // that keeps hanging on startup is left to the service
            // manager's watchdog rather than restarted forever.
            bool beginRestartIfSilent( const int hostId, const juce::int64 timePoint, const juce::int64 maxAge, const juce::int64 startupLimit )
            {
                const juce::ScopedLock sl( lock );
                if( !isSilent( hostId, timePoint, maxAge, startupLimit ) )
                    return false;

                const bool cameUp = heartBeatEventTimePoints.contains( hostId )
                                    && ( !startTimePoints.contains( hostId )
                                         || heartBeatEventTimePoints[hostId] - startTimePoints[hostId] > startupLimit );

                heartBeatEventTimePoints.remove( hostId );
                if( cameUp )
                {
                    startTimePoints.set( hostId, timePoint );
                    windowDenied.removeFirstMatchingValue( hostId );
                }
                else
                {
                    startTimePoints.remove( hostId );
                    windowDenied.addIfNotAlreadyThere( hostId );
                }
                return true;
            }

            // The hosts whose heartbeat verify() expects, whether or not they
            // have sent one yet.
            void expectHeartBeatsFrom( const juce::Array<int>& hostIds )
            {
                const juce::ScopedLock sl( lock );
                expectedHostIds = hostIds;
            }

            // A heartbeat counts if it is at most maxAge old at timePoint.
            // A host is not judged by its heartbeats for startupLimit after
            // it started.
            Verification verify( const juce::int64 timePoint, const juce::int64 maxAge, const juce::int64 startupLimit ) const
            {
                const juce::ScopedLock sl( lock );
                Verification result;

                juce::Array<int> knownHostIds;
                for( auto itr = heartBeatEventTimePoints.begin(); itr != heartBeatEventTimePoints.end(); itr.next() )
                    knownHostIds.addIfNotAlreadyThere( itr.getKey() );
                for( auto itr = startTimePoints.begin(); itr != startTimePoints.end(); itr.next() )
                    knownHostIds.addIfNotAlreadyThere( itr.getKey() );
                for( const int hostId : knownHostIds )
                    if( isSilent( hostId, timePoint, maxAge, startupLimit ) )
                        result.silentHostIds.add( hostId );

                for( const int hostId : expectedHostIds )
                {
                    if( hasRecentHeartBeat( hostId, timePoint, maxAge ) )
                        continue;
                    if( isStarting( hostId, timePoint, startupLimit ) )
                        result.startingHostIds.add( hostId );
                    else
                        result.missingHostIds.add( hostId );
                }
                return result;
            }

        private:
            bool hasRecentHeartBeat( const int hostId, const juce::int64 timePoint, const juce::int64 maxAge ) const
            {
                return heartBeatEventTimePoints.contains( hostId ) && timePoint - heartBeatEventTimePoints[hostId] <= maxAge;
            }

            bool isStarting( const int hostId, const juce::int64 timePoint, const juce::int64 startupLimit ) const
            {
                return startTimePoints.contains( hostId ) && timePoint - startTimePoints[hostId] <= startupLimit;
            }

            // Silent: no recent heartbeat, past the startup window, and
            // started or heard from (a host that is neither, such as one
            // restarted without a window that has not sent a heartbeat yet,
            // is left to the watchdog).
            bool isSilent( const int hostId, const juce::int64 timePoint, const juce::int64 maxAge, const juce::int64 startupLimit ) const
            {
                if( hasRecentHeartBeat( hostId, timePoint, maxAge ) || isStarting( hostId, timePoint, startupLimit ) )
                    return false;
                return heartBeatEventTimePoints.contains( hostId ) || startTimePoints.contains( hostId );
            }

            // Heartbeats are registered on the IPC message processor thread
            // and verified on the message thread.
            juce::CriticalSection            lock;
            juce::HashMap<int, juce::int64>  heartBeatEventTimePoints;
            // When the host's current process started (or its predecessor
            // exited or was restarted), if it has a startup window.
            juce::HashMap<int, juce::int64>  startTimePoints;
            // Hosts whose next process gets no startup window: the last one
            // was restarted without having come up.
            juce::Array<int>                 windowDenied;
            juce::Array<int>                 expectedHostIds;
        };
    }
}
