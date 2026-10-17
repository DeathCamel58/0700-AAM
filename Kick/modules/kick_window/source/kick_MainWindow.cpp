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

namespace kick::window
{
    namespace
    {
        int titleBarButtonsFor( WindowMode mode )
        {
            return mode == WindowMode::windowed ? ( juce::DocumentWindow::minimiseButton | juce::DocumentWindow::closeButton ) : 0;
        }
    }

    MainWindow::MainWindow( const juce::String& name, juce::Component& content, Options windowOptions )
        // Not on the desktop yet: the peer is created once the window is
        // set up, with the right style and bounds.
        : juce::DocumentWindow( name, juce::Colours::black, titleBarButtonsFor( windowOptions.windowMode ), false ),
          options( std::move( windowOptions ) ),
          host( content )
    {
        setTitle( name );
        setContentNonOwned( &host, false );

        if( options.windowMode == WindowMode::windowed )
            openWindowed();
        else
            openKiosk();
    }

    MainWindow::~MainWindow()
    {
        if( options.windowMode == WindowMode::windowed && options.saveBounds && !isMinimised() && !isFullScreen() )
            options.saveBounds( formatWindowBounds( getBounds() ) );

        clearContentComponent();
    }

    void MainWindow::openKiosk()
    {
        setUsingNativeTitleBar( false );
        setTitleBarHeight( 0 );
        setResizable( false, false );

        addToDesktop();

        // The display the layout's area is on, at the screen's origin.
        setBounds( host.getLayoutBounds() );
        juce::Desktop::getInstance().setKioskModeComponent( this, false );
        setTopLeftPosition( 0, 0 );

        setVisible( true );
    }

    void MainWindow::openWindowed()
    {
        const auto layout  = host.getLayoutBounds();
        const auto minimum = juce::Point<int>( layout.getWidth() / 2, layout.getHeight() / 2 );

        setUsingNativeTitleBar( true );
        setResizable( true, false );
        setResizeLimits( minimum.x, minimum.y, layout.getWidth() * 8, layout.getHeight() * 8 );
        getConstrainer()->setFixedAspectRatio( (double)layout.getWidth() / (double)layout.getHeight() );

        const auto&                       displays = juce::Desktop::getInstance().getDisplays();
        juce::Array<juce::Rectangle<int>> displayAreas;
        for( const auto& display : displays.displays )
            displayAreas.add( display.userArea );

        juce::Rectangle<int> preferredArea;
        if( auto* display = displays.getDisplayForPoint( juce::Desktop::getMousePosition() ) )
            preferredArea = display->userArea;

        std::optional<juce::Rectangle<int>> saved;
        if( options.loadBounds )
            saved = parseWindowBounds( options.loadBounds() );

        setBounds( chooseWindowBounds( saved, displayAreas, preferredArea, { layout.getWidth(), layout.getHeight() }, minimum ) );

        addToDesktop();
        setVisible( true );
    }

    void MainWindow::closeButtonPressed()
    {
        if( options.onCloseRequested )
            options.onCloseRequested();
        else if( auto* app = juce::JUCEApplication::getInstance() )
            app->systemRequestedQuit();
    }

    void MainWindow::inputAttemptWhenModal()
    {
        auto numModal = getNumCurrentlyModalComponents();
        auto index    = juce::jmax( 0, numModal - 1 );
        if( auto p = getCurrentlyModalComponent( index ) )
            p->toFront( true );
    }
}
