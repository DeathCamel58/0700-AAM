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
 #include <pthread.h>
 #include <sys/signalfd.h>
 #include <unistd.h>
#endif

namespace contracts
{
#if JUCE_LINUX
    namespace
    {
        const int terminationSignals[] = { SIGTERM, SIGINT, SIGHUP };

        sigset_t terminationSignalSet()
        {
            sigset_t set;
            sigemptyset( &set );
            for( const int signalNumber : terminationSignals )
                sigaddset( &set, signalNumber );
            return set;
        }

        // Runs in the child of every fork(), so that the programs this
        // process starts do not inherit the blocked signals (exec keeps the
        // mask). Their action is the default, so no handler of this
        // process's runs in the child. Async-signal-safe. (posix_spawn()
        // and system() do not run it: what they start keeps them blocked.)
        void unblockInChild()
        {
            const sigset_t set = terminationSignalSet();
            ::sigprocmask( SIG_UNBLOCK, &set, nullptr );
        }

        // Threads started before the signals were blocked would still get
        // them, with the default action: say so.
        int countThreads()
        {
            return juce::File( "/proc/self/task" ).getNumberOfChildFiles( juce::File::findDirectories );
        }
    }
#endif

    TerminationSignals::TerminationSignals( std::function<void( int )> callback )
        : onSignal( std::move( callback ) )
    {
#if JUCE_LINUX
        jassert( signalFd < 0 );

        const int threads = countThreads();
        if( threads > 1 )
            juce::Logger::writeToLog( juce::String( threads - 1 ) + " other threads are already running, which may still receive SIGTERM, SIGINT and SIGHUP" );

        // No handler of this process's (such as JUCE's for SIGINT) runs in a
        // forked child before it execs, once that child unblocks them.
        for( const int signalNumber : terminationSignals )
            std::signal( signalNumber, SIG_DFL );

        const sigset_t set = terminationSignalSet();
        ::pthread_sigmask( SIG_BLOCK, &set, nullptr );

        static const bool registered = ::pthread_atfork( nullptr, nullptr, unblockInChild ) == 0;
        juce::ignoreUnused( registered );

        signalFd = ::signalfd( -1, &set, SFD_CLOEXEC | SFD_NONBLOCK );
        if( signalFd < 0 )
        {
            juce::Logger::writeToLog( "Cannot create the signal descriptor: " + juce::String( std::strerror( errno ) ) );
            return;
        }

        juce::LinuxEventLoop::registerFdCallback( signalFd, [this]( int ) { readSignals(); } );
#endif
    }

    TerminationSignals::~TerminationSignals()
    {
#if JUCE_LINUX
        if( signalFd < 0 )
            return;

        juce::LinuxEventLoop::unregisterFdCallback( signalFd );
        ::close( signalFd );
#endif
    }

    void TerminationSignals::readSignals()
    {
#if JUCE_LINUX
        signalfd_siginfo info;
        while( ::read( signalFd, &info, sizeof( info ) ) == static_cast<ssize_t>( sizeof( info ) ) )
        {
            if( onSignal )
                onSignal( static_cast<int>( info.ssi_signo ) );
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
