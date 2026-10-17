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

    // Tells the window whether JUCE's OpenGL context came up: JUCE calls
    // newOpenGLContextCreated() on its render thread once it has, and
    // nothing when it cannot create it (the window stays black).
    class MainWindow::OpenGLWatch : public juce::OpenGLRenderer
        , private juce::Timer
    {
    public:
        explicit OpenGLWatch( MainWindow& windowToWatch )
            : window( windowToWatch )
        {
        }

        ~OpenGLWatch() override { stopTimer(); }

        void start() { startTimer( checkIntervalInMS ); }

        void newOpenGLContextCreated() override
        {
            const auto* version  = juce::gl::glGetString( juce::gl::GL_VERSION );
            const auto* renderer = juce::gl::glGetString( juce::gl::GL_RENDERER );
            const juce::ScopedLock lock( descriptionLock );
            description = juce::String( version != nullptr ? (const char*)version : "unknown version" ) + ", "
                          + juce::String( renderer != nullptr ? (const char*)renderer : "unknown renderer" );
            created = true;
        }

        void renderOpenGL() override {}
        void openGLContextClosing() override {}

    private:
        void timerCallback() override
        {
            if( created )
            {
                stopTimer();
                const juce::ScopedLock lock( descriptionLock );
                juce::Logger::writeToLog( "Display: OpenGL context created (" + description + ")" );
                return;
            }

            // Only while the window is on screen does JUCE try.
            if( window.isShowing() && ( waitedInMS += checkIntervalInMS ) >= timeoutInMS )
            {
                stopTimer();
                window.fallBackToSoftware( "no OpenGL context within " + juce::String( timeoutInMS / 1000 ) + " s" );
            }
        }

        static constexpr int checkIntervalInMS = 250;
        static constexpr int timeoutInMS       = 10000;

        MainWindow&           window;
        std::atomic<bool>     created{ false };
        juce::CriticalSection descriptionLock;
        juce::String          description;
        int                   waitedInMS{ 0 };
    };

    MainWindow::MainWindow( const juce::String& name, juce::Component& content, Options windowOptions )
        // Not on the desktop yet: the peer is created once the window is
        // set up, with the right style and bounds.
        : juce::DocumentWindow( name, juce::Colours::black, titleBarButtonsFor( windowOptions.windowMode ), false ),
          options( std::move( windowOptions ) ),
          host( content )
    {
        setTitle( name );
        host.setOverlay( options.overlay, options.overlayMargin );
        setContentNonOwned( &host, false );

        if( options.windowMode == WindowMode::windowed )
            openWindowed();
        else
            openKiosk();

        detail::setDefaultCursor( options.showCursor, options.windowMode == WindowMode::kiosk );

        if( options.renderer == Renderer::openGL )
            attachOpenGL();
    }

    MainWindow::~MainWindow()
    {
        // While the components it paints still exist.
        detachOpenGL();

        // Whatever has not been saved yet: the process is quitting (the
        // window was closed, or SIGTERM, or KickMain's exit message).
        if( options.windowMode == WindowMode::windowed )
        {
            boundsSaveTimer.stopTimer();
            if( !placementOffset.has_value() )
                notePlacement();
            noteBounds();
            if( const auto bounds = boundsSaving.flush() )
                saveBounds( *bounds );
        }

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

        juce::Rectangle<int> preferredArea;
        if( auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForPoint( juce::Desktop::getMousePosition() ) )
            preferredArea = display->userArea;

        std::optional<juce::Rectangle<int>> saved;
        if( options.loadBounds )
        {
            const auto text = options.loadBounds();
            boundsSaving.setSaved( text );
            saved = parseWindowBounds( text );
        }

        setBounds( chooseWindowBounds( saved, getDisplayAreas(), preferredArea, { layout.getWidth(), layout.getHeight() }, minimum ) );
        requestedPosition = getPosition();
        juce::Logger::writeToLog( "Window: opening at " + formatWindowBounds( getBounds() ) + " (saved: "
                                  + ( saved.has_value() ? formatWindowBounds( *saved ) : juce::String( "none" ) ) + ")" );

        addToDesktop();
        detail::askForPosition( *this );
        settledBounds = getBounds();
        setVisible( true );
        placementCheck.startTimer( placementDelayInMS );
    }

    juce::Array<juce::Rectangle<int>> MainWindow::getDisplayAreas()
    {
        juce::Array<juce::Rectangle<int>> areas;
        for( const auto& display : juce::Desktop::getInstance().getDisplays().displays )
            areas.add( display.userArea );
        return areas;
    }

    void MainWindow::attachOpenGL()
    {
        if( const auto problem = detail::findOpenGLProblem(); problem.isNotEmpty() )
        {
            juce::Logger::writeToLog( "Display: OpenGL is not available (" + problem + "); using the software renderer" );
            return;
        }

        openGLWatch   = std::make_unique<OpenGLWatch>( *this );
        openGLContext = std::make_unique<juce::OpenGLContext>();
        openGLContext->setRenderer( openGLWatch.get() );
        openGLContext->setComponentPaintingEnabled( true );
        // Repaint when the components do, as the software renderer does.
        // (JUCE swaps at the display's refresh: swap interval 1.)
        openGLContext->setContinuousRepainting( false );
        openGLContext->attachTo( host );
        openGLWatch->start();

        juce::Logger::writeToLog( "Display: rendering with OpenGL" );
    }

    void MainWindow::detachOpenGL()
    {
        if( openGLContext != nullptr )
            openGLContext->detach();
        openGLContext.reset();
        openGLWatch.reset();
    }

    void MainWindow::fallBackToSoftware( const juce::String& reason )
    {
        juce::Logger::writeToLog( "Display: OpenGL failed (" + reason + "); using the software renderer" );

        // Called from the watch's timer: it must not be deleted while its
        // callback runs.
        juce::MessageManager::callAsync( [safeThis = juce::Component::SafePointer<MainWindow>( this )]() {
            if( safeThis != nullptr )
            {
                safeThis->detachOpenGL();
                safeThis->host.repaint();
            }
        } );
    }

    void MainWindow::moved()
    {
        juce::DocumentWindow::moved();
        noteBounds();
    }

    void MainWindow::resized()
    {
        // The scaled host, the content component, follows the window.
        juce::DocumentWindow::resized();

        if( options.windowMode == WindowMode::windowed && isOnDesktop() )
            aspectCheck.startTimer( aspectCheckDelayInMS );
        noteBounds();
    }

    void MainWindow::notePlacement()
    {
        placementCheck.stopTimer();

        auto* peer = getPeer();
        if( peer == nullptr )
            return;

        // The window manager's frame is known once the window is shown.
        const auto frame = peer->getFrameSize();
        placementOffset  = findPlacementOffset( requestedPosition, getPosition(), frame );
        juce::Logger::writeToLog( "Window: placed with its client area at " + formatWindowBounds( getBounds() ) + ", asked for "
                                  + juce::String( requestedPosition.x ) + " " + juce::String( requestedPosition.y )
                                  + "; frame extents (left right top bottom) " + juce::String( frame.getLeft() ) + " "
                                  + juce::String( frame.getRight() ) + " " + juce::String( frame.getTop() ) + " "
                                  + juce::String( frame.getBottom() ) + "; saving with the offset "
                                  + juce::String( placementOffset->x ) + " " + juce::String( placementOffset->y ) );
        noteBounds();
    }

    void MainWindow::noteBounds()
    {
        auto* peer = getPeer();
        if( options.windowMode != WindowMode::windowed || !options.saveBounds || peer == nullptr )
            return;

        // Not until the window manager has placed the window: until then
        // the window is not where it stays, and the offset is not known.
        if( !placementOffset.has_value() )
        {
            if( isVisible() )
                placementCheck.startTimer( placementDelayInMS );
            return;
        }

        // Not bounds the window keeps when it is shown again.
        if( isMinimised() || isFullScreen() || fillsADisplay( getBounds(), getDisplayAreas() ) )
            return;

        boundsSaving.changed( formatWindowBounds( windowBoundsToSave( getBounds(), *placementOffset ) ),
                              juce::Time::getMillisecondCounter() );
        if( boundsSaving.isWaiting() )
            boundsSaveTimer.startTimer( boundsSaveDelayInMS );
        else
            boundsSaveTimer.stopTimer();
    }

    void MainWindow::saveBoundsWhenDue()
    {
        boundsSaveTimer.stopTimer();

        const auto now = juce::Time::getMillisecondCounter();
        if( const auto bounds = boundsSaving.takeDue( now ) )
            saveBounds( *bounds );
        else if( boundsSaving.isWaiting() )
            boundsSaveTimer.startTimer( juce::jmax( 1, boundsSaving.getTimeUntilDue( now ) ) );
    }

    void MainWindow::saveBounds( const juce::String& bounds )
    {
        juce::Logger::writeToLog( "Window: saving its bounds (" + bounds + "; client area " + formatWindowBounds( getBounds() ) + ")" );
        options.saveBounds( bounds );
    }

    void MainWindow::keepAspectRatio()
    {
        aspectCheck.stopTimer();

        const auto bounds = getBounds();
        if( isMinimised() || isFullScreen() || fillsADisplay( bounds, getDisplayAreas() ) )
        {
            settledBounds = bounds;
            return;
        }

        auto*      constrainer = getConstrainer();
        const auto layout      = host.getLayoutBounds();
        const auto wanted      = kick::window::keepAspectRatio( bounds, settledBounds, { layout.getWidth(), layout.getHeight() },
                                                                { constrainer->getMinimumWidth(), constrainer->getMinimumHeight() },
                                                                { constrainer->getMaximumWidth(), constrainer->getMaximumHeight() } );

        // At the aspect ratio, or the window manager keeps this size (a
        // tiled or maximised window): the host letterboxes the layout.
        if( wanted == bounds || ( bounds.getWidth() == correctedFrom.getWidth() && bounds.getHeight() == correctedFrom.getHeight() ) )
        {
            settledBounds = bounds;
            if( wanted == bounds )
                correctedFrom = {};
            return;
        }

        correctedFrom = bounds;
        setBounds( wanted );
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
