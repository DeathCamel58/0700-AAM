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

#include <string>

namespace kick
{
    namespace main
    {
        /*
         * Reports KickMain's state to the service manager that started it, with
         * the sd_notify(3) protocol: one datagram of newline-separated
         * VARIABLE=value assignments (READY=1, WATCHDOG=1, STATUS=..., ...)
         * sent to the AF_UNIX socket named by $NOTIFY_SOCKET. This is the
         * documented wire protocol, so libsystemd is not needed.
         *
         * Without $NOTIFY_SOCKET (not started by systemd, or not on Linux)
         * notify() does nothing.
         *
         * The socket address is read once, on construction, and notify()
         * keeps no other state, so it may be called from any thread.
         */
        class KickServiceNotifier
        {
        public:
            KickServiceNotifier();

            bool notify( const juce::String& state ) const;

        private:
            std::string socketAddress;
        };
    }
}
