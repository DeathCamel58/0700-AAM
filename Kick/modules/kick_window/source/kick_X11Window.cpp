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

namespace kick::window::detail
{
    namespace
    {
        // XChangeWindowAttributes(), which JUCE's X11 symbols leave out,
        // from the libX11 JUCE has loaded.
        using ChangeWindowAttributes = int ( * )( ::Display*, ::Window, unsigned long, XSetWindowAttributes* );

        ChangeWindowAttributes getChangeWindowAttributes()
        {
            static juce::DynamicLibrary x11( "libX11.so.6" );
            return (ChangeWindowAttributes)x11.getFunction( "XChangeWindowAttributes" );
        }

        // Moves the window directly, not through the window manager.
        void moveUnmanaged( ::Display* display, ::Window windowH, juce::Rectangle<int> bounds )
        {
            auto* changeAttributes = getChangeWindowAttributes();
            if( changeAttributes == nullptr )
                return;

            XSetWindowAttributes attributes{};
            attributes.override_redirect = True;
            changeAttributes( display, windowH, CWOverrideRedirect, &attributes );

            juce::X11Symbols::getInstance()->xMoveResizeWindow( display, windowH, bounds.getX(), bounds.getY(),
                                                                (unsigned int)bounds.getWidth(), (unsigned int)bounds.getHeight() );

            attributes.override_redirect = False;
            changeAttributes( display, windowH, CWOverrideRedirect, &attributes );
        }
    }

    void askForPosition( juce::ResizableWindow& window )
    {
        auto* peer    = window.getPeer();
        auto* display = juce::XWindowSystem::getInstance()->getDisplay();
        if( peer == nullptr || display == nullptr )
            return;

        const auto bounds = juce::Desktop::getInstance().getDisplays().logicalToPhysical( window.getBounds() );
        const auto scale  = peer->getPlatformScaleFactor();

        juce::XWindowSystemUtilities::ScopedXLock xLock;
        auto*                                     x11   = juce::X11Symbols::getInstance();
        auto*                                     hints = x11->xAllocSizeHints();
        if( hints == nullptr )
            return;

        hints->flags  = USPosition | USSize;
        hints->x      = bounds.getX();
        hints->y      = bounds.getY();
        hints->width  = bounds.getWidth();
        hints->height = bounds.getHeight();

        // The limits JUCE sets (without a frame: it is not known yet).
        if( auto* constrainer = window.getConstrainer() )
        {
            hints->flags |= PMinSize | PMaxSize;
            hints->min_width  = juce::jmax( 1, (int)( scale * constrainer->getMinimumWidth() ) );
            hints->min_height = juce::jmax( 1, (int)( scale * constrainer->getMinimumHeight() ) );
            hints->max_width  = juce::jmax( 1, (int)( scale * constrainer->getMaximumWidth() ) );
            hints->max_height = juce::jmax( 1, (int)( scale * constrainer->getMaximumHeight() ) );
        }

        const auto windowH = (::Window)(juce::pointer_sized_uint)peer->getNativeHandle();
        x11->xSetWMNormalHints( display, windowH, hints );
        x11->xFree( hints );

        if( !window.isOnDesktop() || window.isVisible() )
            return;

        moveUnmanaged( display, windowH, bounds );
        x11->xFlush( display );
    }
}
