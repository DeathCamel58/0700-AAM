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
#include "../utils/mt_ipc_exception.h"
namespace ipc
{
    // Processes queued items on its own thread, one at a time. It sleeps
    // until push_to_q() or stopThread() wakes it: it used to wake every
    // millisecond to look at the queue, about a thousand times a second per
    // thread even when idle.
    template<typename T>
    class MTIPCMessageQueueProcessingThread : public juce::Thread
                                            , private juce::Thread::Listener
    {
    public:
        MTIPCMessageQueueProcessingThread( juce::String threadName )
            : juce::Thread( threadName )
        {
            addListener( this );
        }

        ~MTIPCMessageQueueProcessingThread() override
        {
            removeListener( this );
        }

        virtual void run()
        {
            while( !threadShouldExit() )
            {
                // Take one item under the lock and process it outside, so
                // producers are not blocked while it is sent or handled.
                std::shared_ptr<T> data;
                {
                    const juce::ScopedLock locker( lock );
                    data = q.pop();
                }

                if( !data )
                {
                    message_added.wait( -1 );
                    continue;
                }

                process( data );
            }
        }

        // The queue is always locked; lockFree is kept for the callers.
        void push_to_q( std::shared_ptr<T> data, [[maybe_unused]] bool lockFree = false )
        {
            {
                const juce::ScopedLock locker( lock );
                q.push( data );
            }
            message_added.signal();
        }

    protected:
        virtual void processMessage( std::shared_ptr<T> message ) = 0;

    private:
        void exitSignalSent() override
        {
            message_added.signal();
        }

        void process( std::shared_ptr<T> data )
        {
            try
            {
                processMessage( data );
            } catch( const ipc::MTIPCException& ex )
            {
                juce::Logger::writeToLog( "Exception Occurred in " + getThreadName() + ". Error Code: " + juce::String( ex.e_code ) + ". Error Message: " + juce::String( ex.e_message ) );
            } catch( const std::exception& ex )
            {
                juce::Logger::writeToLog( "Exception Occurred in " + getThreadName() + ": " + ex.what() );
            } catch( ... )
            {
                juce::Logger::writeToLog( "Exception Occurred in " + getThreadName() + ": Unknown Exception.." );
            }
        }

    private:
        ipc::MTIPCMessageQueue<T> q;
        // Auto-reset: a signal sent while the thread is busy is kept until
        // its next wait, so no item is left waiting in the queue.
        juce::WaitableEvent       message_added;
        juce::CriticalSection     lock;
    };
}
