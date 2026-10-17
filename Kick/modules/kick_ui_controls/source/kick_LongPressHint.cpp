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

namespace ui
{
    namespace
    {
        // The bubble: the tooltip's text, wrapped to at most maxWidth, in a
        // rounded box like the theme's tooltips.
        class HintBubble final : public juce::Component
        {
        public:
            static constexpr float fontHeight = 16.f;
            static constexpr int   padX       = 12;
            static constexpr int   padY       = 6;
            static constexpr int   maxWidth   = 360;

            explicit HintBubble( const juce::String& Text )
            {
                setInterceptsMouseClicks( false, false );
                setAlwaysOnTop( true );

                juce::AttributedString string;
                string.setJustification( juce::Justification::centred );
                string.append( Text, juce::Font( fontHeight ), ui::Theme::textColour( 1 ) );
                layout.createLayout( string, (float)( maxWidth - 2 * padX ) );

                setSize( (int)std::ceil( layout.getWidth() ) + 2 * padX, (int)std::ceil( layout.getHeight() ) + 2 * padY );
            }

            void paint( juce::Graphics& g ) override
            {
                auto b = getLocalBounds().toFloat().reduced( 0.5f );
                g.setColour( ui::Theme::backgroundColour( 2 ) );
                g.fillRoundedRectangle( b, 6.f );
                g.setColour( ui::Theme::outlineColour( 1 ) );
                g.drawRoundedRectangle( b, 6.f, 1.f );
                layout.draw( g, getLocalBounds().reduced( padX, padY ).toFloat() );
            }

        private:
            juce::TextLayout layout;
        };
    }

    LongPressHint::~LongPressHint()
    {
        hide();
    }

    void LongPressHint::mouseDown( juce::Component& Target, const juce::MouseEvent& )
    {
        hide();
        stopTimer();

        auto* client = dynamic_cast<juce::TooltipClient*>( &Target );
        text         = client != nullptr ? client->getTooltip() : juce::String();
        target       = &Target;

        // Only where there is something to say.
        if( text.isNotEmpty() )
            startTimer( holdTimeMs );
    }

    void LongPressHint::mouseDrag( const juce::MouseEvent& Event )
    {
        // A drag (scrolling a list, sliding off a button) is not a hold.
        if( isTimerRunning() && Event.getDistanceFromDragStart() > moveTolerance )
            stopTimer();
    }

    bool LongPressHint::mouseUp()
    {
        stopTimer();
        auto wasShown = isShowing();
        hide();
        return wasShown;
    }

    void LongPressHint::timerCallback()
    {
        stopTimer();

        auto* root = getLayoutRoot();
        if( target == nullptr || root == nullptr || !target->isShowing() )
            return;

        // Below the component, or above it when there is no room below; in
        // the layout, as pop-ups are (ui::setLayoutRoot()).
        auto newBubble = std::make_unique<HintBubble>( text );
        auto area      = target->getLocalBounds().withPosition( getLayoutPosition( *target ) );
        auto bounds    = newBubble->getBounds().withCentre( { area.getCentreX(), 0 } ).withY( area.getBottom() + 6 );
        if( bounds.getBottom() > root->getHeight() )
            bounds.setY( area.getY() - 6 - bounds.getHeight() );
        bounds = bounds.constrainedWithin( root->getLocalBounds().reduced( 4 ) );

        newBubble->setBounds( bounds );
        root->addAndMakeVisible( newBubble.get() );
        bubble = std::move( newBubble );
    }

    void LongPressHint::hide()
    {
        if( bubble != nullptr )
        {
            if( auto* parent = bubble->getParentComponent() )
                parent->removeChildComponent( bubble.get() );
            bubble.reset();
        }
    }
}
