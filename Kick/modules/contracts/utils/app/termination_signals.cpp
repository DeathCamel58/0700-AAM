/*
 * Copyright (c) 2026 Dylan Corrales
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

#include "termination_signals.h"
#include "juce_events/juce_events.h"

#if JUCE_LINUX
 #include <cerrno>
 #include <csignal>
 #include <cstring>
 #include <fcntl.h>
 #include <unistd.h>
#endif

namespace contracts
{
#if JUCE_LINUX
    namespace
    {
        const int terminationSignals[] = { SIGTERM, SIGINT, SIGHUP };

        // Written by the signal handler, so a plain int: only one
        // TerminationSignals exists at a time.
        int signalWriteFd = -1;

        void writeSignalToPipe( int signalNumber )
        {
            const int           savedErrno = errno;
            const unsigned char byte       = static_cast<unsigned char>( signalNumber );
            // Nothing useful can be done if the pipe is full: the message
            // thread already has a signal to handle.
            [[maybe_unused]] const ssize_t written = ::write( signalWriteFd, &byte, 1 );
            errno = savedErrno;
        }
    }
#endif

    TerminationSignals::TerminationSignals( std::function<void( int )> callback )
        : onSignal( std::move( callback ) )
    {
#if JUCE_LINUX
        jassert( signalWriteFd < 0 );

        int fds[2];
        if( ::pipe2( fds, O_CLOEXEC | O_NONBLOCK ) != 0 )
        {
            juce::Logger::writeToLog( "Cannot create the signal pipe: " + juce::String( std::strerror( errno ) ) );
            return;
        }
        readFd        = fds[0];
        signalWriteFd = fds[1];

        juce::LinuxEventLoop::registerFdCallback( readFd, [this]( int ) { readSignals(); } );

        // This replaces JUCE's own SIGINT handler, which only quits once
        // the next message arrives.
        struct sigaction action{};
        action.sa_handler = writeSignalToPipe;
        sigemptyset( &action.sa_mask );
        action.sa_flags = SA_RESTART;
        for( const int signalNumber : terminationSignals )
            ::sigaction( signalNumber, &action, nullptr );
#endif
    }

    TerminationSignals::~TerminationSignals()
    {
#if JUCE_LINUX
        if( readFd < 0 )
            return;

        for( const int signalNumber : terminationSignals )
            std::signal( signalNumber, SIG_DFL );

        juce::LinuxEventLoop::unregisterFdCallback( readFd );
        ::close( signalWriteFd );
        ::close( readFd );
        signalWriteFd = -1;
#endif
    }

    void TerminationSignals::readSignals()
    {
#if JUCE_LINUX
        unsigned char signalNumbers[16];
        ssize_t       count;
        while( ( count = ::read( readFd, signalNumbers, sizeof( signalNumbers ) ) ) > 0 )
        {
            for( ssize_t i = 0; i < count; ++i )
                if( onSignal )
                    onSignal( signalNumbers[i] );
        }
#endif
    }

    juce::String TerminationSignals::signalName( int signalNumber )
    {
#if defined( __GLIBC__ ) && ( __GLIBC__ > 2 || __GLIBC_MINOR__ >= 32 )
        if( const char* abbreviation = sigabbrev_np( signalNumber ) )
            return "SIG" + juce::String( abbreviation );
#endif
        return "signal " + juce::String( signalNumber );
    }
}
