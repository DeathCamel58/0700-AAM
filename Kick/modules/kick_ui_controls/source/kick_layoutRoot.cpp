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
        juce::Component::SafePointer<juce::Component>& layoutRoot()
        {
            static juce::Component::SafePointer<juce::Component> root;
            return root;
        }
    }

    void setLayoutRoot( juce::Component* root )
    {
        layoutRoot() = root;
    }

    juce::Component* getLayoutRoot()
    {
        return layoutRoot().getComponent();
    }

    juce::Point<int> getLayoutPosition( juce::Component& component )
    {
        if( auto* root = getLayoutRoot() )
            return root->getLocalPoint( &component, juce::Point<int>() );
        return component.getScreenPosition();
    }

    juce::Point<int> screenToLayout( juce::Point<int> screenPosition )
    {
        if( auto* root = getLayoutRoot() )
            return root->getLocalPoint( nullptr, screenPosition );
        return screenPosition;
    }
}
