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

namespace kick_ui
{
    // A small see-through panel of live graphs over the last ten seconds:
    // the window's frames (rate, paint time, display tick gaps), the CPU
    // (all of it, and this process), the memory (in use, and this process)
    // and the audio device (callback load, xruns, buffer and rate).
    //
    // The window shows it in its bottom-right corner, above everything else
    // (MainWindow::Options::overlay). It takes no clicks, touches or
    // keyboard focus: they go to whatever is under it.
    //
    // It is shown while the setting Window/windowperfoverlay is on (the
    // system resources window turns it on and off), and works only while
    // shown: it reads /proc five times a second, and the window times its
    // frames only then.
    class PerformanceOverlay
        : public juce::Component
        , private juce::Timer
        , private juce::Value::Listener
    {
    public:
        PerformanceOverlay();
        ~PerformanceOverlay() override;

        // In layout units; the window scales it with the layout.
        static constexpr int width  = 260;
        static constexpr int height = 170;

        // Where it shows, in layout units in from the window's right edge
        // and up from its bottom edge: above the button row (Apply,
        // Cancel) of the standard dialogs and the keyboard pop-up, and so
        // above the status bar and its BAR : BEAT : TICK counter too. At
        // 1024x600 it covers 758-1018 x 307-477.
        static juce::Point<int> getMargin();

    private:
        // One graph and its figures. Up to two lines (a and b), and up to
        // three texts: the first on the title's line, the others under it.
        struct Row
        {
            explicit Row( const juce::String& title, float top );

            juce::String title;
            float        top; // the value at the top of the graph
            float        guide{ -1.0f };

            perf::History a, b;
            juce::Path    pathA, pathB;
            juce::Colour  colourA, colourB;

            std::array<juce::String, 3> texts;
            std::array<juce::Colour, 3> textColours;

            juce::Rectangle<int>   textArea;
            juce::Rectangle<float> graphArea;
        };

        void valueChanged( juce::Value& ) override;
        void visibilityChanged() override;
        void parentHierarchyChanged() override;
        void timerCallback() override;
        void paint( juce::Graphics& g ) override;
        void resized() override;

        void start();
        void stop();
        void followHost();

        void sampleFrames();
        void sampleCpu();
        void sampleMemory();
        void sampleAudio();

        void updatePath( Row& row, const perf::History& history, juce::Path& path ) const;
        void paintRow( juce::Graphics& g, const Row& row ) const;

        static constexpr int samplesPerSecond = 5;
        static constexpr int historySeconds   = 10;
        static constexpr int historySize      = samplesPerSecond * historySeconds;

        juce::Value enabledSetting;
        bool        running{ false };

        juce::Component::SafePointer<kick::window::ScaledHost> host;
        std::unique_ptr<juce::VBlankAttachment>                displayTicks;

        Row frames{ "FRAMES", 50.0f };
        Row cpu{ "CPU", 100.0f };
        Row memory{ "RAM", 100.0f };
        Row audio{ "AUDIO", 100.0f };

        perf::History      xruns{ historySize };
        perf::RecentFrames recentFrames{ samplesPerSecond };

        perf::CpuTimes lastCpu;
        juce::uint64   lastProcessTicks{ 0 };
        int            lastXRunCount{ -1 };
        int            xrunsShown{ 0 };
        juce::uint64   pageSize{ 4096 };

        JUCE_DECLARE_NON_COPYABLE( PerformanceOverlay )
    };
}
