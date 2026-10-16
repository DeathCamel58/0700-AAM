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

#pragma once

#include "juce_core/juce_core.h"

#include <functional>

namespace contracts
{
    /*
     * Delivers SIGTERM, SIGINT and SIGHUP to the message thread, so that the
     * application can quit through its normal path and run shutdown(). The
     * service manager stops Kick with SIGTERM; with the default action the
     * processes died without saving settings or stopping their children.
     *
     * No handler runs: the signals are blocked in every thread and read
     * from a signalfd that the message loop watches, which calls onSignal
     * with the number. A handler interrupts whichever thread the signal
     * lands on, and a blocking send() interrupted after part of a message
     * returns a short count, which the IPC (one send() per message) does
     * not retry: the rest of the message was lost and the stream's framing
     * broken.
     *
     * Create one instance, on the message thread, after the MessageManager
     * exists and before any other thread starts, since threads inherit the
     * signal mask of the thread that creates them (an application object's
     * constructor qualifies). The signals stay blocked when it is destroyed,
     * so that one arriving during the exit does not kill the process.
     * Processes started with fork() get them unblocked again.
     */
    class TerminationSignals
    {
    public:
        explicit TerminationSignals( std::function<void( int signalNumber )> onSignal );
        ~TerminationSignals();

        static juce::String signalName( int signalNumber );

    private:
        void readSignals();

        std::function<void( int )> onSignal;
        int                        signalFd{ -1 };

        JUCE_DECLARE_NON_COPYABLE( TerminationSignals )
    };
}
