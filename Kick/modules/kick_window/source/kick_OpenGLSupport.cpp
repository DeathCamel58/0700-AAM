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

// After juce_opengl.h, as juce_opengl.cpp includes it.
#include <GL/glx.h>

namespace kick::window::detail
{
    juce::String findOpenGLProblem()
    {
        auto* display = juce::XWindowSystem::getInstance()->getDisplay();
        if( display == nullptr )
            return "there is no X display";

        juce::XWindowSystemUtilities::ScopedXLock xLock;

        int errorBase = 0, eventBase = 0;
        if( !glXQueryExtension( display, &errorBase, &eventBase ) )
            return "the X server has no GLX extension";

        // The attributes JUCE's NativeContext::tryChooseVisual() asks for
        // with OpenGLContext's default pixel format.
        const juce::OpenGLPixelFormat format;
        const int                     attributes[] = {
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_DOUBLEBUFFER, True,
            GLX_RED_SIZE, format.redBits,
            GLX_GREEN_SIZE, format.greenBits,
            GLX_BLUE_SIZE, format.blueBits,
            GLX_ALPHA_SIZE, format.alphaBits,
            GLX_DEPTH_SIZE, format.depthBufferBits,
            GLX_STENCIL_SIZE, format.stencilBufferBits,
            GLX_ACCUM_RED_SIZE, format.accumulationBufferRedBits,
            GLX_ACCUM_GREEN_SIZE, format.accumulationBufferGreenBits,
            GLX_ACCUM_BLUE_SIZE, format.accumulationBufferBlueBits,
            GLX_ACCUM_ALPHA_SIZE, format.accumulationBufferAlphaBits,
            None
        };

        int   count   = 0;
        auto* configs = glXChooseFBConfig( display, juce::X11Symbols::getInstance()->xDefaultScreen( display ), attributes, &count );
        if( configs == nullptr || count == 0 )
        {
            if( configs != nullptr )
                juce::X11Symbols::getInstance()->xFree( configs );
            return "GLX has no double-buffered RGBA frame buffer configuration";
        }

        // As JUCE creates its context (OpenGLContext's default version).
        auto context = glXCreateNewContext( display, configs[0], GLX_RGBA_TYPE, nullptr, True );
        juce::X11Symbols::getInstance()->xFree( configs );
        if( context == nullptr )
            return "GLX cannot create a context";

        glXDestroyContext( display, context );
        return {};
    }
}
