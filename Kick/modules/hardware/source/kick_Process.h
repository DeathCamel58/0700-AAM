/*
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

// Running other programs without a shell.
//
// Each argument reaches the program as it is: nothing is split on spaces,
// expanded or interpreted, so file names, SSIDs and other strings that come
// from outside the application cannot run commands of their own.

#include <juce_core/juce_core.h>
#include <string>

#if( JUCE_LINUX || __linux__ )
    #include <cerrno>
    #include <climits>
    #include <fcntl.h>
    #include <signal.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>

extern char** environ;
#endif

namespace mt
{
    namespace process
    {
        /** Runs args[0] (looked up in PATH) with the other elements as its
            arguments, waits for it and returns its exit status, or -1 if it
            could not be started. Its standard output and error are returned
            in output, if given. */
        inline int run( const juce::StringArray& args, juce::String* output = nullptr )
        {
            juce::ChildProcess child;

            if( !child.start( args ) )
                return -1;

            const auto text = child.readAllProcessOutput();
            if( output != nullptr )
                *output = text;

            child.waitForProcessToFinish( -1 );
            return (int)child.getExitCode();
        }

        /** Like run(), but writes input to the program's standard input,
            which is how secrets are passed: unlike arguments, they are not
            visible to other users in /proc/<pid>/cmdline. Only small inputs
            are supported (up to PIPE_BUF bytes). */
        inline int runWithInput( const juce::StringArray& args, const std::string& input )
        {
#if( JUCE_LINUX || __linux__ )
            if( args.isEmpty() || input.size() > PIPE_BUF )
                return -1;

            int fds[2];
            if( pipe2( fds, O_CLOEXEC ) != 0 )
                return -1;

            // The input fits in the pipe's buffer, so it is written before
            // the program starts: the write cannot block, and cannot fail
            // with SIGPIPE if the program exits without reading it.
            const bool written = write( fds[1], input.data(), input.size() ) == (ssize_t)input.size();
            close( fds[1] );
            if( !written )
            {
                close( fds[0] );
                return -1;
            }

            std::vector<std::string> strings;
            for( const auto& arg : args )
                strings.push_back( arg.toStdString() );

            std::vector<char*> argv;
            for( auto& s : strings )
                argv.push_back( s.data() );
            argv.push_back( nullptr );

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init( &actions );
            posix_spawn_file_actions_adddup2( &actions, fds[0], STDIN_FILENO );

            // Kick blocks its termination signals in every thread (they are
            // read from a signalfd) and ignores SIGPIPE, and posix_spawn()
            // runs no pthread_atfork() handlers: give the program an empty
            // signal mask and those signals' default actions, as a program
            // expects to start with (an ignored signal stays ignored across
            // exec).
            posix_spawnattr_t attributes;
            posix_spawnattr_init( &attributes );
            sigset_t signals;
            sigemptyset( &signals );
            posix_spawnattr_setsigmask( &attributes, &signals );
            for( const int signal : { SIGTERM, SIGINT, SIGHUP, SIGPIPE } )
                sigaddset( &signals, signal );
            posix_spawnattr_setsigdefault( &attributes, &signals );
            posix_spawnattr_setflags( &attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF );

            pid_t      pid;
            const auto error = posix_spawnp( &pid, argv[0], &actions, &attributes, argv.data(), environ );

            posix_spawnattr_destroy( &attributes );
            posix_spawn_file_actions_destroy( &actions );
            close( fds[0] );

            if( error != 0 )
                return -1;

            int status = 0;
            while( waitpid( pid, &status, 0 ) < 0 )
            {
                if( errno != EINTR )
                    return -1;
            }

            return WIFEXITED( status ) ? WEXITSTATUS( status ) : -1;
#else
            juce::ignoreUnused( args, input );
            return -1;
#endif
        }
    }
}
