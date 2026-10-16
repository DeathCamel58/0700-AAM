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

#pragma once

namespace mt
{
    static const juce::NormalisableRange<float>& getMeterNorm()
    {
        static const auto norm = [] {
            juce::NormalisableRange<float> range{ -100.f, 20.f };
            range.setSkewForCentre( 0.8f );
            return range;
        }();
        return norm;
    }

    // reload() is called by the sequencer's tick thread and the message
    // thread, process() and getMeter() by the message thread: the level
    // reload() sees goes to process() through an atomic.
    class MTMidiMeter
    {
    public:
        virtual ~MTMidiMeter() = default;

        explicit MTMidiMeter( int Steps )
        {
            meterData.resize( 2 );
            setSteps( Steps );
        };

        void enable( bool Enable ) noexcept { enabled.store( Enable ); }
        bool isEnabled() const noexcept { return enabled.load(); }

        void setSteps( int Steps ) noexcept { meter.reset( Steps ); }

        void process() noexcept
        {
            auto newLevel = peak.exchange( DB::MIN );
            if( newLevel > meter.getCurrentValue() )
            {
                meter.setCurrentAndTargetValue( newLevel );
                meter.setTargetValue( DB::MIN );
            }

            auto rms = meter.getNextValue();
            for( auto& it : meterData )
                it.rms = rms;
        }

        void reload( float normLevel ) noexcept
        {
            if( !isEnabled() )
                return;

            auto newLevel = getMeterNorm().convertFrom0to1( normLevel );
            auto curLevel = peak.load();
            while( newLevel > curLevel && !peak.compare_exchange_weak( curLevel, newLevel ) ) {}
        }

        const MTMeterData& getMeter() const { return meterData; }

    private:
        MTMeterData                meterData;
        juce::SmoothedValue<float> meter{ DB::MIN };
        std::atomic<float>         peak{ DB::MIN }; // Highest level since process().

        std::atomic<bool> enabled{ true };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( MTMidiMeter )
    };

} // namespace MT
