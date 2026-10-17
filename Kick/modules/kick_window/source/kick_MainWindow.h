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
    //
    // With the OpenGL renderer, JUCE paints the components with OpenGL
    // (an OpenGLContext attached to the scaled host, swapping at the
    // display's refresh, repainting only what changed, as the software
    // renderer does). When OpenGL is not available, or no context comes up,
    // the window falls back to the software renderer.
    class MainWindow : public juce::DocumentWindow
    {
    public:
        struct Options
        {
            WindowMode windowMode{ WindowMode::kiosk };
            Renderer   renderer{ Renderer::software };

            // The saved bounds (formatWindowBounds()), read when the
            // windowed window opens and written when it is destroyed.
            std::function<juce::String()>             loadBounds;
            std::function<void( const juce::String& )> saveBounds;

            // The close button (or the window manager closing the window).
            // Without one: JUCEApplication::systemRequestedQuit().
            std::function<void()> onCloseRequested;

            // Shown above the content, in the window's bottom-right corner
            // (ScaledHost::setOverlay()). Not owned; it is taken out of the
            // window when the window is destroyed.
            juce::Component* overlay{ nullptr };
        };

        // The content is not owned. The window is shown when this returns.
        MainWindow( const juce::String& name, juce::Component& content, Options options );
        ~MainWindow() override;

        void resized() override;
        void closeButtonPressed() override;
        void inputAttemptWhenModal() override;

        // The renderer in use (software after a fallback).
        Renderer getRenderer() const { return openGLContext != nullptr ? Renderer::openGL : Renderer::software; }

    private:
        class OpenGLWatch;

        // The displays' user areas (on Linux, all of each display).
        static juce::Array<juce::Rectangle<int>> getDisplayAreas();

        // Gives a desktop window the layout's aspect ratio again after the
        // window manager resized it (JUCE only passes the constrainer's
        // minimum and maximum size on to it on Linux).
        void keepAspectRatio();

        void openKiosk();
        void openWindowed();
        void attachOpenGL();
        void detachOpenGL();
        void fallBackToSoftware( const juce::String& reason );

        Options    options;
        ScaledHost host;

        // Once a resize has settled.
        static constexpr int aspectCheckDelayInMS = 150;
        juce::TimedCallback  aspectCheck{ [this]() { keepAspectRatio(); } };
        // The bounds after the last resize that settled, and the size the
        // window had when it was last given the aspect ratio (the window
        // manager may give it back: the window is then left letterboxed).
        juce::Rectangle<int> settledBounds;
        juce::Rectangle<int> correctedFrom;

        std::unique_ptr<OpenGLWatch>         openGLWatch;
        std::unique_ptr<juce::OpenGLContext> openGLContext;

        JUCE_DECLARE_NON_COPYABLE( MainWindow )
    };
}
