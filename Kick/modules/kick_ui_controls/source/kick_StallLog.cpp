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

#include <cstdlib>

#if defined( __GNUC__ )
    #include <cxxabi.h>
#endif

namespace ui
{
    namespace stallLog
    {
        namespace
        {
            // The running detector's check; used on the message thread only.
            StallCheck* activeCheck = nullptr;

            juce::String readableTypeName( const char* Name )
            {
#if defined( __GNUC__ )
                int status = 0;
                if( auto* readable = abi::__cxa_demangle( Name, nullptr, nullptr, &status ) )
                {
                    juce::String result( readable );
                    std::free( readable );
                    if( status == 0 )
                        return result;
                }
#endif
                return Name;
            }
        }

        Detector::Detector()
            : check( juce::Time::getMillisecondCounterHiRes() )
        {
            JUCE_ASSERT_MESSAGE_THREAD
            activeCheck = &check;
            startTimer( intervalMs );
        }

        Detector::~Detector()
        {
            stopTimer();
            if( activeCheck == &check )
                activeCheck = nullptr;
        }

        void Detector::timerCallback()
        {
            const auto report = check.check( juce::Time::getMillisecondCounterHiRes(), tapLog::isEnabled(), readableTypeName );
            if( report.line.isEmpty() )
                return;

            if( report.error )
                tapLog::writeError( report.line );
            else
                tapLog::write( report.line );
        }

        Phase::Phase( const char* What, const char* Type )
            : what( What ), type( Type ), startMs( juce::Time::getMillisecondCounterHiRes() )
        {
        }

        Phase::~Phase()
        {
            if( activeCheck != nullptr && juce::MessageManager::existsAndIsCurrentThread() )
                activeCheck->phaseEnded( what, type, juce::Time::getMillisecondCounterHiRes() - startMs );
        }
    }
}
