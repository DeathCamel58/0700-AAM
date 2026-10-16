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
#include <memory>
#include <functional>
#include "../process/mt_ipc_process_manager.h"
#include "../utils/mt_ipc_timer.h"
#include "../utils/mt_ipc_constants.h"

namespace ipc
{
    class MTIPCProcessManager;
    class MTIPCTimer;

    /*
     * Restarts one child process each time it exits, and gives up (calls
     * onRestartCompleted with false) when it has been started
     * IPC_RESTART_BURST_LIMIT times within IPC_RESTART_BURST_INTERVAL: a
     * process that keeps crashing, however long it stays up each time, is
     * not restarted forever. A start that fails is retried after
     * IPC_DELAYED_RESTART_DELAY_IN_MILLI_SECONDS and counts as well.
     *
     * Used on the message thread only.
     */
    class MTIPCProcessRestarter
    {
    public:
        MTIPCProcessRestarter( const int                                 processId,
                               std::shared_ptr<ipc::MTIPCProcessManager> processManager,
                               std::function<void( int, bool )>          onRestartCompleted )
            : processId( processId )
            , processManager( processManager )
            , retryTimer( new ipc::MTIPCTimer( ipc::IPC_DELAYED_RESTART_DELAY_IN_MILLI_SECONDS, std::bind( &MTIPCProcessRestarter::restart, this ) ) )
            , onRestartCompleted( onRestartCompleted )
        {}

        void restart()
        {
            if( !processManager )
                return;

            stopRestartAttempt();

            // Monotonic, so a change of the wall clock cannot hide a burst.
            const double now = juce::Time::getMillisecondCounterHiRes();
            while( !restartTimes.isEmpty() && now - restartTimes.getFirst() > ipc::IPC_RESTART_BURST_INTERVAL_IN_MILLI_SECONDS )
                restartTimes.remove( 0 );

            if( restartTimes.size() >= ipc::IPC_RESTART_BURST_LIMIT )
            {
                restartTimes.clear();
                if( onRestartCompleted )
                    onRestartCompleted( processId, false );
                return;
            }

            restartTimes.add( now );
            if( !processManager->startProcess( processId ) )
                retryTimer->start();
        }

        void stopRestartAttempt()
        {
            if( retryTimer->isRunning() )
                retryTimer->stop();
        }

        const int processIdentifier()
        {
            return processId;
        }

    private:
        const int                                 processId;
        juce::Array<double>                       restartTimes;
        std::shared_ptr<ipc::MTIPCProcessManager> processManager;
        std::unique_ptr<ipc::MTIPCTimer>          retryTimer;
        std::function<void( int, bool )>          onRestartCompleted;
    };
}
