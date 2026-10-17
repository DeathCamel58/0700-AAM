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

namespace kick::window
{
    // The window Kick's user interface is shown in, shared by
    // KickUserInterface and the standalone Kick.
    //
    // It shows the content (the home screen, laid out at its current size)
    // through a ScaledHost, so the layout keeps its size whatever the
    // window's size is.
    //
    // Kiosk: no title bar or border, covering the display the layout's
    // area is on, at the screen's origin (the device: Weston's kiosk shell
    // shows it full screen).
    // Windowed: a desktop window with the window manager's title bar
    // (minimise and close), resizable at the layout's aspect ratio between
    // half and eight times its size, opened where it was closed, or centred
    // on the display under the mouse.
    class MainWindow : public juce::DocumentWindow
    {
    public:
        struct Options
        {
            WindowMode windowMode{ WindowMode::kiosk };

            // The saved bounds (formatWindowBounds()), read when the
            // windowed window opens and written when it is destroyed.
            std::function<juce::String()>             loadBounds;
            std::function<void( const juce::String& )> saveBounds;

            // The close button (or the window manager closing the window).
            // Without one: JUCEApplication::systemRequestedQuit().
            std::function<void()> onCloseRequested;
        };

        // The content is not owned. The window is shown when this returns.
        MainWindow( const juce::String& name, juce::Component& content, Options options );
        ~MainWindow() override;

        void closeButtonPressed() override;
        void inputAttemptWhenModal() override;

    private:
        void openKiosk();
        void openWindowed();

        Options    options;
        ScaledHost host;

        JUCE_DECLARE_NON_COPYABLE( MainWindow )
    };
}
