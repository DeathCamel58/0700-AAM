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

namespace juce
{
    class var;
    class String;
}

namespace ipc
{
    class MTIPCChildProcessExecutorImpl;
    class MTIPCChildProcessExecutor
    {
    public:
        MTIPCChildProcessExecutor( std::shared_ptr<juce::var> config );
        ~MTIPCChildProcessExecutor();
        bool start();
        // Sends SIGTERM; killIfOverdue() sends SIGKILL if the process is
        // still running IPC_PROCESS_WAIT_TO_FINISH_TIMEOUT_IN_MILLI_SECONDS
        // later.
        bool stop();
        bool kill();
        // Returns true if it had to kill the process.
        bool killIfOverdue();
        // Waits for the process to exit, killing it after
        // IPC_PROCESS_WAIT_TO_FINISH_TIMEOUT_IN_MILLI_SECONDS.
        bool join();
        // The exit status, or 128 plus the signal number for a process that
        // a signal killed (as a shell reports it).
        int  exitCode();
        // "exited with status 1", "was killed by SIGSEGV (core dumped)", ...
        juce::String exitDescription();
        bool isRunning();

    private:
        std::unique_ptr<ipc::MTIPCChildProcessExecutorImpl> impl;
        
    };

}
