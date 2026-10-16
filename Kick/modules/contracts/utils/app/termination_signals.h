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
     * The signal handler only writes the signal number to a pipe (the
     * self-pipe pattern), whichever thread the signal arrives on; the
     * message loop watches the read end and calls onSignal with the number.
     *
     * Create one instance, on the message thread, after the MessageManager
     * exists (an application object's constructor qualifies). Destroying it
     * restores the default actions.
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
        int                        readFd{ -1 };

        JUCE_DECLARE_NON_COPYABLE( TerminationSignals )
    };
}
