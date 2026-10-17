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

#include <atomic>
#include <typeinfo>

#if defined( __GNUC__ )
    #include <cxxabi.h>
#endif

namespace ui
{
    namespace tapLog
    {
        namespace
        {
            juce::String typeName( const juce::Component& c )
            {
                const char* name = typeid( c ).name();
#if defined( __GNUC__ )
                int status = 0;
                if( auto* readable = abi::__cxa_demangle( name, nullptr, nullptr, &status ) )
                {
                    juce::String result( readable );
                    std::free( readable );
                    if( status == 0 )
                        return result;
                }
#endif
                return name;
            }

            juce::String flag( bool value )
            {
                return value ? "1" : "0";
            }
        }

        juce::String describe( const juce::Component* c )
        {
            if( c == nullptr )
                return "none";

            auto s = typeName( *c );
            if( c->getName().isNotEmpty() )
                s << " \"" << c->getName() << "\"";
            else if( c->getTitle().isNotEmpty() )
                s << " \"" << c->getTitle() << "\"";
            return s;
        }

        juce::String chain( const juce::Component* c )
        {
            if( c == nullptr )
                return "none";

            // The few nearest are what tells a control; the window is
            // always the same.
            constexpr int     maxParts = 6;
            juce::StringArray parts;
            for( auto* p = c; p != nullptr; p = p->getParentComponent() )
            {
                if( parts.size() == maxParts )
                {
                    parts.add( "..." );
                    break;
                }
                parts.add( describe( p ) );
            }
            return parts.joinIntoString( " < " );
        }

        juce::String modalState()
        {
            auto*      manager = juce::ModalComponentManager::getInstance();
            const auto count   = manager->getNumModalComponents();

            juce::String s;
            s << count << " modal";
            for( int i = 0; i < count; ++i )
            {
                auto* m = manager->getModalComponent( i );
                if( m == nullptr )
                    continue;
                s << ( i == 0 ? ": " : "; " ) << describe( m )
                  << " (visible " << flag( m->isVisible() ) << ", showing " << flag( m->isShowing() )
                  << ", bounds " << m->getBounds().toString()
                  << ", in " << ( m->isOnDesktop() ? juce::String( "a window of its own" ) : describe( m->getParentComponent() ) ) << ")";
            }
            return s;
        }

        namespace
        {
            std::atomic<bool> enabled{ false };
        }

        void setEnabled( bool Enabled ) { enabled = Enabled; }
        bool isEnabled() { return enabled.load(); }

        void write( const juce::String& Line )
        {
            if( isEnabled() )
                juce::Logger::writeToLog( Line );
        }

        void writeError( const juce::String& Line )
        {
            juce::Logger::writeToLog( Line );
        }

        Listener::Listener()
        {
            juce::Desktop::getInstance().addGlobalMouseListener( this );
        }

        Listener::~Listener()
        {
            juce::Desktop::getInstance().removeGlobalMouseListener( this );
        }

        juce::String Listener::describeEvent( const juce::MouseEvent& Event ) const
        {
            auto* target = Event.eventComponent;

            juce::String s;
            s << "at " << screenToLayout( Event.getScreenPosition() ).toString()
              << " (" << ( Event.source.isTouch() ? "touch " : "mouse " ) << Event.source.getIndex() << ")"
              << ", on " << chain( target );

            if( auto* under = Event.source.getComponentUnderMouse(); under != target )
                s << ", under the mouse " << describe( under );

            // What a hit test finds there now: something that covers the
            // target since the press would take the release.
            if( target != nullptr )
            {
                auto* top = target->getTopLevelComponent();
                auto* hit = top->getComponentAt( top->getLocalPoint( nullptr, Event.getScreenPosition() ) );
                if( hit != target )
                    s << ", hit " << describe( hit );
            }

            const auto blocked = target != nullptr && target->isCurrentlyBlockedByAnotherModalComponent();
            s << ", clicks " << Event.getNumberOfClicks()
              << ", blocked " << flag( blocked )
              << ", " << modalState()
              << ", focus " << describe( juce::Component::getCurrentlyFocusedComponent() )
              // How long the event waited to be handled, by its own time: a
              // large or a negative figure says the times are off.
              << ", lag " << ( juce::Time::getCurrentTime() - Event.eventTime ).inMilliseconds() << " ms";
            return s;
        }

        void Listener::mouseDown( const juce::MouseEvent& Event )
        {
            downHandledMs = juce::Time::getMillisecondCounterHiRes();
            write( "Tap: down " + describeEvent( Event ) );
        }

        void Listener::mouseUp( const juce::MouseEvent& Event )
        {
            const auto handledAfter = juce::roundToInt( juce::Time::getMillisecondCounterHiRes() - downHandledMs );
            write( "Tap: up after " + juce::String( pressDuration( Event ) ) + " ms (handled " + juce::String( handledAfter )
                   + " ms after the press) " + describeEvent( Event ) );
        }
    }
}
