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

#include "mt_ipc_child_process_executor.h"
#include "juce_core/juce_core.h"
#include "../utils/mt_ipc_constants.h"
#include "../utils/mt_ipc_keys.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ipc
{
    namespace
    {
        // Runs in the forked child, before exec: async-signal-safe calls
        // only. Closes every descriptor from 'first' on except 'keep'.
        void closeDescriptorsFrom( int first, int keep, int maxFd )
        {
#ifdef SYS_close_range
            bool closed = true;
            if( keep > first )
                closed = ::syscall( SYS_close_range, first, keep - 1, 0 ) == 0;
            if( closed )
                closed = ::syscall( SYS_close_range, keep >= first ? keep + 1 : first, ~0U, 0 ) == 0;
            if( closed )
                return;
#endif
            for( int fd = first; fd < maxFd; ++fd )
                if( fd != keep )
                    ::close( fd );
        }

        juce::String signalName( int signalNumber )
        {
#if defined( __GLIBC__ ) && ( __GLIBC__ > 2 || __GLIBC_MINOR__ >= 32 )
            if( const char* abbreviation = sigabbrev_np( signalNumber ) )
                return "SIG" + juce::String( abbreviation );
#endif
            return "signal " + juce::String( signalNumber );
        }
    }

    /*
     * Runs one child process with fork() and execv() rather than
     * juce::ChildProcess, which cannot do what a supervised child needs:
     * - the child inherits this process's stdin, stdout and stderr, so its
     *   output reaches the journal (juce::ChildProcess captured it in a pipe
     *   nobody read, and a child blocked once that pipe was full);
     * - every other descriptor is closed before exec, so children do not
     *   hold this process's listening socket or each other's connections;
     * - the child gets SIGTERM if this process dies (PR_SET_PDEATHSIG);
     * - the exit status is kept as waitpid() reports it, so a crash is
     *   reported as the signal that ended the process (juce::ChildProcess
     *   reports 0 for a process killed by a signal).
     */
    class MTIPCChildProcessExecutorImpl
    {
    public:
        MTIPCChildProcessExecutorImpl( std::shared_ptr<juce::var> config )
            : config( config ) {}

        ~MTIPCChildProcessExecutorImpl()
        {
            if( isRunning() )
            {
                kill( SIGKILL );
                reap( 0 );
            }
        }

        bool startChildProcess()
        {
            juce::String props = juce::JSON::toString( config->getProperty( ipc::IPC_KEY_ARGUMENTS, "" ) );
            juce::String path  = config->getProperty( ipc::IPC_KEY_PROCESS_PATH, "" );
            if( path.isEmpty() )
                return false;

            // Everything the child needs is prepared before fork(): after it,
            // in the child, only async-signal-safe calls are allowed.
            std::string pathArgument  = path.toStdString();
            std::string propsArgument = props.toStdString();
            char*       argv[]        = { pathArgument.data(), propsArgument.data(), nullptr };
            const pid_t parentPid     = ::getpid();
            const long  openMax       = ::sysconf( _SC_OPEN_MAX );
            const int   maxFd         = openMax > 0 && openMax < 65536 ? static_cast<int>( openMax ) : 65536;

            // Reports a failed exec: the write end closes on a successful one.
            int errorPipe[2];
            if( ::pipe2( errorPipe, O_CLOEXEC ) != 0 )
            {
                juce::Logger::writeToLog( "Cannot start " + path + ": " + juce::String( std::strerror( errno ) ) );
                return false;
            }

            const std::lock_guard<std::mutex> lock( mutex );
            const pid_t                       child = ::fork();
            if( child == 0 )
            {
                // The death signal is sent when the thread that forked exits:
                // processes are started on the message thread, which lives as
                // long as this process.
                ::prctl( PR_SET_PDEATHSIG, SIGTERM );
                if( ::getppid() != parentPid )
                    ::_exit( 127 );

                sigset_t noSignals;
                sigemptyset( &noSignals );
                ::sigprocmask( SIG_SETMASK, &noSignals, nullptr );
                ::signal( SIGPIPE, SIG_DFL );

                closeDescriptorsFrom( STDERR_FILENO + 1, errorPipe[1], maxFd );
                ::execv( argv[0], argv );

                const int error = errno;
                [[maybe_unused]] const ssize_t written = ::write( errorPipe[1], &error, sizeof( error ) );
                ::_exit( 127 );
            }

            ::close( errorPipe[1] );
            if( child < 0 )
            {
                juce::Logger::writeToLog( "Cannot start " + path + ": " + juce::String( std::strerror( errno ) ) );
                ::close( errorPipe[0] );
                return false;
            }

            int     execError = 0;
            ssize_t count;
            do
                count = ::read( errorPipe[0], &execError, sizeof( execError ) );
            while( count < 0 && errno == EINTR );
            ::close( errorPipe[0] );

            pid    = child;
            exited = false;
            if( count == static_cast<ssize_t>( sizeof( execError ) ) )
            {
                juce::Logger::writeToLog( "Cannot run " + path + ": " + juce::String( std::strerror( execError ) ) );
                reapLocked( 0 );
                return false;
            }
            return true;
        }

        bool isRunning()
        {
            const std::lock_guard<std::mutex> lock( mutex );
            return reapLocked( WNOHANG ) == false;
        }

        bool kill( int signalNumber )
        {
            const std::lock_guard<std::mutex> lock( mutex );
            if( pid <= 0 || exited )
                return true;
            return ::kill( pid, signalNumber ) == 0;
        }

        // Waits up to timeoutMs (-1: no limit) for the process to exit.
        bool waitForExit( int timeoutMs )
        {
            const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while( isRunning() )
            {
                if( timeoutMs >= 0 && juce::Time::getMillisecondCounterHiRes() >= deadline )
                    return false;
                juce::Thread::sleep( 10 );
            }
            return true;
        }

        void reap( int options )
        {
            const std::lock_guard<std::mutex> lock( mutex );
            reapLocked( options );
        }

        int exitCode()
        {
            const std::lock_guard<std::mutex> lock( mutex );
            if( !exited )
                return 0;
            if( WIFSIGNALED( status ) )
                return 128 + WTERMSIG( status );
            return WIFEXITED( status ) ? WEXITSTATUS( status ) : 0;
        }

        juce::String exitDescription()
        {
            const std::lock_guard<std::mutex> lock( mutex );
            if( !exited )
                return "is running";
            if( WIFSIGNALED( status ) )
                return "was killed by " + signalName( WTERMSIG( status ) ) + ( WCOREDUMP( status ) ? " (core dumped)" : "" );
            if( WIFEXITED( status ) )
                return "exited with status " + juce::String( WEXITSTATUS( status ) );
            return "exited";
        }

    private:
        // Returns true once the process has exited and been reaped.
        bool reapLocked( int options )
        {
            if( pid <= 0 || exited )
                return true;

            int   childStatus = 0;
            pid_t result;
            do
                result = ::waitpid( pid, &childStatus, options );
            while( result < 0 && errno == EINTR );

            if( result == 0 )
                return false;

            // ECHILD: someone else reaped it; the status is unknown.
            status = result == pid ? childStatus : 0;
            exited = true;
            return true;
        }

        std::shared_ptr<juce::var> config;
        std::mutex                 mutex;
        pid_t                      pid{ -1 };
        int                        status{ 0 };
        bool                       exited{ false };
    };

    MTIPCChildProcessExecutor::MTIPCChildProcessExecutor( std::shared_ptr<juce::var> config )
        : impl( new MTIPCChildProcessExecutorImpl(config) )
    {
    }

    MTIPCChildProcessExecutor::~MTIPCChildProcessExecutor()
    {
        impl = nullptr;
    }

    bool MTIPCChildProcessExecutor::start( )
    {
        if( isRunning() ) return true;

        return impl->startChildProcess();
    }

    bool MTIPCChildProcessExecutor::stop()
    {
        if( isRunning() )
            return impl->kill( SIGKILL );
        return true;
    }

    bool MTIPCChildProcessExecutor::join()
    {
        return impl->waitForExit( IPC_PROCESS_WAIT_TO_FINISH_TIMEOUT_IN_MILLI_SECONDS );
    }

    int MTIPCChildProcessExecutor::exitCode()
    {
        return impl->exitCode();
    }

    juce::String MTIPCChildProcessExecutor::exitDescription()
    {
        return impl->exitDescription();
    }

    bool MTIPCChildProcessExecutor::isRunning()
    {
        return impl->isRunning();
    }

}
