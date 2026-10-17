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

namespace kick_ui
{
    std::unique_ptr<kick::window::MainWindow> createMainWindow( const juce::String&     name,
                                                                kick::window::WindowMode windowMode,
                                                                std::function<void()>   onCloseRequested )
    {
        kick::window::MainWindow::Options options;
        options.windowMode       = windowMode;
        options.onCloseRequested = std::move( onCloseRequested );
        options.loadBounds       = []() { return mt::getSettings().window.bounds.get(); };
        options.saveBounds       = []( const juce::String& bounds ) {
            auto& settings         = mt::getSettings();
            settings.window.bounds = bounds;
            // The window is destroyed when the application quits, before
            // the settings' delayed save would run.
            settings.save();
        };

        return std::make_unique<kick::window::MainWindow>( name, KickHome::Instance(), std::move( options ) );
    }
}
