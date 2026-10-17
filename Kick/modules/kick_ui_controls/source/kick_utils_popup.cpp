/*
 * Copyright (c) 2021 MUSIC Tribe
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

#include "contracts/utils/keyboard/keyboard_detector.h"

namespace mt
{

    class fsPopup : public juce::Component
    {
    public:
        fsPopup()
        {
            setOpaque( true );

            // Covers the layout; getFullScreenModalPopUP() makes it a child
            // of the home screen.
            setBounds( ui::layout::screen );
        }

        virtual ~fsPopup() = default;

        void setContent( juce::Component* Content, juce::Rectangle<int> Bounds )
        {
            // Remove previous content.
            removeChildComponent( pCont );
            // Update new content.
            pCont = Content;
            if( pCont ) addAndMakeVisible( pCont );
            // Update content bounds.
            setContentBounds( Bounds );
        }

        void setContentBounds( juce::Rectangle<int> bounds )
        {
            dialogArea = bounds;
            resized();
        }

        virtual void show( std::function<void( int R )> Callback )
        {
            image.clear( ui::layout::screen );

            // The blurred background: what the layout shows now.
            juce::Component* background = ui::getLayoutRoot();
            if( background == nullptr )
                background = juce::TopLevelWindow::getActiveTopLevelWindow();
            if( background != nullptr )
            {
                image = background->createComponentSnapshot( ui::layout::screen, false );
                stack_blur<10>( image );
            }

            isInUse = true;
            setAlwaysOnTop( true );
            setVisible( true );
            toFront( true );
            const auto shown = ++timesShown;
            enterModalState( true, juce::ModalCallbackFunction::create( [Callback, this, shown]( int R ) {
                                 if( Callback ) Callback( R );
                                 // Not if it has been reset and shown again in
                                 // the meantime: that would hide the new one
                                 // and leave it modal.
                                 if( shown == timesShown )
                                     reset();
                             } ),
                             false );
        }

        void close( int exitCode = UI::CLOSE )
        {
            const auto shown = timesShown;
            juce::MessageManager::callAsync( [this, exitCode, shown]() {
                // Already closed, and maybe showing something else now.
                if( shown != timesShown || !isInUse )
                    return;

                // Ending the modal state resets the pop-up (show()). Hiding it
                // first, as this did, cancelled the modal state with 0 and
                // freed the pop-up before that callback came, whose reset()
                // could then hide the pop-up's next use.
                if( isCurrentlyModal( false ) )
                    exitModalState( exitCode );
                else
                    reset();
            } );
        }

    protected:
        virtual void reset()
        {
            setAlwaysOnTop( false );
            setVisible( false );
            setContent( nullptr, {} );
            isInUse = false;
        }

        virtual void resized()
        {
            auto b = dialogArea.constrainedWithin( getLocalBounds() );
            b.setCentre( ui::layout::screen.getCentre() );
            if( pCont ) { pCont->setBounds( b ); }
        }

        virtual void paint( juce::Graphics& g )
        {
            // The pop-up is opaque, so JUCE paints nothing under it; but the
            // snapshot is transparent wherever the layout paints nothing
            // (around the project name and the header buttons, say), and
            // there the old, sharp pixels showed through the blur.
            g.fillAll( juce::Colours::black );
            g.drawImageAt( image, 0, 0 );
        }

    public:
        std::atomic<bool> isInUse{ false };

    protected:
        juce::Rectangle<int> dialogArea{ ui::layout::dialog };
        juce::Component*     pCont{ nullptr };

    private:
        juce::Image image;
        // Counts the times shown, to tell a use's callbacks from the next's.
        int timesShown{ 0 };
    };

    class dialogDisplayer : public ui::KickControlBase<fsPopup>
    {
    public:
        dialogDisplayer( bool ShowYesNoButtons = false, const juce::String& Title = "", juce::Component* Content = nullptr, juce::Rectangle<int> bounds = {}, juce::Colour backgroundColour = juce::Colours::transparentBlack )
        {
            setLookAndFeel( ui::KickLooknFeel::getLnf() );

            title.setInterceptsMouseClicks( false, false );
            title.setValueJustification( juce::Justification::centredLeft );
            title.setColours( ui::Theme::textColour( 2 ), ui::Theme::backgroundColour( 6 ) );
            title.setPadding( 10 );
            title.setCornerRadius( 6.0 );
            title.setBorderCurves( true, true, false, false );
            title.setFontHeight( 18.0f );

            apply.setValueColour( ui::Theme::textColour( 1 ) );
            apply.setValueFontHeight( 18 );

            cancel.setValueColour( ui::Theme::textColour( 1 ) );
            cancel.setValueFontHeight( 18 );

            cancel.setPlacementFlag( juce::Justification::centred );
            apply.setPlacementFlag( juce::Justification::centred );
            closeButton.setPlacementFlag( juce::RectanglePlacement::centred );

            utils::addAndMakeVisible( this, title, closeButton );
            utils::addChildComponents( this, apply, cancel );

            apply.onClick       = [&]() { utils::exitDialog( this, UI::APPLY ); };
            cancel.onClick      = [&]() { utils::exitDialog( this, UI::CANCEL ); };
            closeButton.onClick = [&]() { utils::exitDialog( this, UI::CANCEL ); };

            ui::themeChanged.connect( [&]() {
                closeButton.setImages( "Icon Close" );
            } );

            setWantsKeyboardFocus( true );
            // It takes the focus for the front panel's controls, but an
            // outline around the whole screen says nothing.
            setHasFocusOutline( false );
            setContentBackgroundColour( backgroundColour );
            setShowYesNoButtons( ShowYesNoButtons );
            setTitle( Title );
            setContent( Content, bounds );
        }

        ~dialogDisplayer()
        {
            DBG( "Dialog Destroyed..." );
        }

        void setTitle( const juce::String& Title )
        {
            title.setText( Title.length() ? Title : ( pCont ? pCont->getName() : "" ), juce::dontSendNotification );
        }

        void setTitleColours( juce::Colour textColour, juce::Colour backgroundColour )
        {
            title.setColours( textColour, backgroundColour );
        }

        void setShowYesNoButtons( bool ShowYesNoButtons )
        {
            showYesNoButtons = ShowYesNoButtons;
            resized();
        }

        void setContentBackgroundColour( juce::Colour backgroundColour )
        {
            contentBackgroundColour = backgroundColour;
            repaint();
        }

        void setButtonBackgroundColour( juce::Colour backgroundColour )
        {
            buttonBackgroundColour = backgroundColour;
            repaint();
        }

        void setBackgroundAlpha( float Alpha )
        {
            backgroundAlpha = Alpha;
            repaint();
        }

        void setButtonOffsets( int x, int y )
        {
            buttonOffsetX = x;
            buttonOffsetY = y;
            resized();
        };

        void reset() override
        {
            removeKeyListener( &contracts::getKeypressDetector() );
            setFocus( false );
            fsPopup::reset();
        }

        void show( std::function<void( int R )> Callback ) override
        {
            addKeyListener( &contracts::getKeypressDetector() );
            setFocus( true );

            fsPopup::show( std::forward<std::function<void( int R )>>( Callback ) );
        }

        void handleCommand( CPktMgr::HWMsg_t& f_hwMsg ) override
        {
            if( auto p = dynamic_cast<ui::KickControlsProcessor*>( pCont ) )
            {
                p->handleCommand( f_hwMsg );
            }
        }

    private:
        bool                 showYesNoButtons{ false };
        juce::Rectangle<int> contentArea;
        juce::Rectangle<int> buttonArea;
        juce::Rectangle<int> closeBounds;
        juce::Colour         contentBackgroundColour{ ui::Theme::backgroundColour( 1 ) };
        juce::Colour         buttonBackgroundColour{ ui::Theme::backgroundColour( 1 ) };

        ui::KickImageButton closeButton{ "Close", "Icon Close" };
        ui::KickImageButton apply{ "Apply", "Icon_Apply" };
        ui::KickImageButton cancel{ "Cancel", "Icon_Cancel" };

        ui::KickLabel title{ "" };

        int       buttonOffsetX{ 175 };
        int       buttonOffsetY{ 20 };
        const int closeButtonSize{ 24 };
        float     backgroundAlpha{ 0.20f };

        void paint( juce::Graphics& g )
        {
            fsPopup::paint( g );

            // Yes/No or Apply/Cancel buttons
            if( showYesNoButtons )
            {
                g.setColour( buttonBackgroundColour );
                g.fillRect( buttonArea );
            }

            // Content Background
            utils::fillRoundedRect( g, contentArea, contentBackgroundColour, 0.f );

            // Outline
            utils::renderRoundedRect( g, dialogArea, ui::Theme::outlineColour( 6 ), 1, 6.f );
        }

        void resized() override
        {
            auto b = dialogArea.constrainedWithin( getLocalBounds() );
            b.setCentre( ui::layout::screen.getCentre() );
            dialogArea = b;

            b = b.reduced( 4 );

            // Setup Title and close button
            // The title bar spans the dialog, under the close button: next
            // to a shorter one, the blurred background showed around the
            // button (a colored glow behind the X).
            auto bTitle = b.removeFromTop( ( ui::layout::screen == dialogArea ) ? 76 : 40 );
            title.setBounds( bTitle );
            auto cBounds = bTitle.removeFromRight( 16 + closeButtonSize );
            closeBounds  = cBounds.withSizeKeepingCentre( closeButtonSize, closeButtonSize );
            closeButton.setBounds( closeBounds );

            // If options, Setup Apply/Cancel buttons
            if( showYesNoButtons )
            {
                buttonArea = b.removeFromBottom( 73 );
                auto row   = buttonArea.reduced( 20, buttonOffsetY );
                apply.setBounds( row.removeFromRight( buttonOffsetX ) );
                cancel.setBounds( row.removeFromLeft( buttonOffsetX ) );
                // Show
                utils::makeVisible( apply, cancel );
            }
            else // Hide
                utils::makeHidden( apply, cancel );

            // Setup content
            contentArea = b.reduced( 1, 0 );
            if( pCont ) pCont->setBounds( contentArea );
        }

        void mouseDown( const juce::MouseEvent& e )
        {
            if( !dialogArea.contains( e.getPosition() ) )
                utils::exitDialog( this, UI::CANCEL );
        }

        double getIncrement() { return 1.0; }
        void   sliderMoved( float ) override{};
        void   knobRotated( double ) override{};
        void   knobClicked() override{};
    };

    template<typename Type>
    Type* getFullScreenModalPopUP()
    {
        static std::vector<std::unique_ptr<Type>> _popups;
        for( auto& it : _popups )
        {
            if( !it->isInUse.load() )
            {
                it->isInUse = true;
                return it.get();
            }
        }

        _popups.emplace_back( new Type() );
        auto& p = _popups.back();

        // A child of the home screen, so that it shows in Kick's window,
        // scaled with the layout. (As a window of its own it was placed at
        // the screen's origin, and the active window could be none.)
        if( auto root = ui::getLayoutRoot() )
            root->addChildComponent( p.get() );
        else if( auto c = juce::TopLevelWindow::getActiveTopLevelWindow() )
            c->addChildComponent( p.get() );
        else
            p->addToDesktop( compPeer::windowIsTemporary | compPeer::windowHasDropShadow );
        return p.get();
    }

} // namespace mt
