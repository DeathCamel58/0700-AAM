/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of AudIO.
 *
 * AudIO is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * AudIO is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with AudIO. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */


#pragma once
#ifndef AUDIO_FILTER_H_INCLUDE
    #define AUDIO_FILTER_H_INCLUDE

    #include <array>
    #include <cmath>
    #include <cstdint>
    #include <type_traits>

    #include "../Includes/idsp.h"
    #include "../Includes/Utils.h"

namespace AudIO
{
    //! Filter responses.
    //!
    //! The order follows the filter type list of Kick's UI ("Lowpass 1 Pole" ...
    //! "Kick3000 LPF"), so that the type index a program stores converts to the
    //! matching response with a plain cast. The number is the order of the
    //! filter (6 dB/octave per pole).
    enum FilterType
    {
        LOWPASS_1_POLE = 0,
        LOWPASS_2_POLE,
        LOWPASS_4_POLE,
        LOWPASS_6_POLE,
        LOWPASS_8_POLE,
        HIGHPASS_1_POLE,
        HIGHPASS_2_POLE,
        HIGHPASS_4_POLE,
        HIGHPASS_6_POLE,
        HIGHPASS_8_POLE,
        BANDPASS_2_POLE,
        BANDPASS_4_POLE,
        BANDPASS_6_POLE,
        BANDPASS_8_POLE,
        BANDSTOP_2_POLE,
        BANDSTOP_4_POLE,
        BANDSTOP_6_POLE,
        BANDSTOP_8_POLE,
        BANDBOOST_2_POLE,
        BANDBOOST_4_POLE,
        BANDBOOST_6_POLE,
        BANDBOOST_8_POLE,
        MODEL_1, // Transistor ladder style 4 pole lowpass.
        MODEL_2, // Transistor ladder style 4 pole bandpass.
        MODEL_3, // Transistor ladder style 4 pole highpass.
        MODEL_4, // Driven 2 pole lowpass.
        LOWPASS_CLASSIC, // Ladder style 4 pole lowpass, uncompensated and driven harder ("Kick3000 LPF").
        NUM_FILTER_TYPES
    };

    //! Logarithmic mapping between a frequency in Hz and a 0 - 1 control range.
    //! The member names follow juce::NormalisableRange.
    struct FrequencyRange
    {
        double start; // Lowest frequency in Hz.
        double end; // Highest frequency in Hz.

        double snapToLegalValue( double Freq ) const noexcept { return utils::clamp( Freq, start, end ); }

        double convertTo0to1( double Freq ) const noexcept
        {
            return std::log( snapToLegalValue( Freq ) / start ) / std::log( end / start );
        }

        double convertFrom0to1( double Proportion ) const noexcept
        {
            return start * std::pow( end / start, utils::clamp( Proportion, 0.0, 1.0 ) );
        }
    };

    //! Resonant multi mode filter.
    //!
    //! The 2 pole sections are trapezoidal (zero delay feedback) state variable
    //! filters, the 1 pole and ladder stages trapezoidal one pole filters, so
    //! the cutoff can be modulated at audio rate without the response going
    //! unstable. Higher orders are cascades of sections; the lowpass and
    //! highpass cascades use Butterworth damping, with the resonance raising
    //! the Q of the last section. Each channel has its own filter state.
    template<typename T>
    class Filter : public IDSP<T>
    {
    public:
        enum class Type
        { //! Filter types...
            LOW_PASS,
            HIGH_PASS,
            BAND_PASS,
        };

        enum class Slope
        { //! Filter slope...
            _12DB,
            _24DB,
        };

        struct Params
        { //! Filter parameters...
            double cutoff; // Cutoff position in freqRange ( 0.0 - 1.0 ).
            double resonance; // ( 0.0 - 1.0 )
            double depth; // Amount of the cutoff modulation ( 0.0 - 1.0 ).
            Type   type;
        };

        static constexpr int            maxChannels = 8;
        static constexpr FrequencyRange freqRange{ 20.0, 20000.0 }; //! Cutoff range.

        Filter( Type Typ = Type::LOW_PASS ) noexcept { setType( Typ ); }
        Filter( FilterType Typ ) noexcept { setFilterType( Typ ); }
        Filter( const Params& P ) noexcept { setParameters( P ); }

        //! The closest of the three basic responses to the current filter type.
        Type getType() const noexcept
        {
            if( HIGHPASS_1_POLE <= filterType_ && filterType_ <= HIGHPASS_8_POLE )
                return Type::HIGH_PASS;
            if( BANDPASS_2_POLE <= filterType_ && filterType_ <= BANDPASS_8_POLE )
                return Type::BAND_PASS;
            return Type::LOW_PASS;
        }

        //! Selects a 2 pole (12 dB) or 4 pole (24 dB) response, depending on the slope.
        void setType( Type Typ ) noexcept
        {
            type_ = Typ;
            setFilterType( toFilterType( type_, slope_ ) );
        }

        Slope getSlope() const noexcept { return slope_; }
        void  setSlope( Slope Slp ) noexcept
        {
            slope_ = Slp;
            setFilterType( toFilterType( type_, slope_ ) );
        }

        FilterType getFilterType() const noexcept { return filterType_; }
        void       setFilterType( FilterType Typ ) noexcept
        {
            if( Typ < LOWPASS_1_POLE || Typ >= NUM_FILTER_TYPES )
                Typ = LOWPASS_1_POLE;

            if( Typ != filterType_ )
            {
                filterType_ = Typ;
                clearState(); // The sections are wired differently...
                dirty_ = true;
            }
        }

        void setParameters( const Params& P ) noexcept
        {
            setType( P.type );
            setCutoff( P.cutoff );
            setResonance( P.resonance );
            setDepth( P.depth );
        }

        //! Cutoff as a position in freqRange ( 0.0 - 1.0 ).
        inline void setCutoff( double newCutoff )
        {
            newCutoff = utils::clamp( newCutoff, 0.0, 1.0 );
            if( newCutoff != cutoff_ )
            {
                cutoff_ = newCutoff;
                dirty_  = true;
            }
        };
        inline double getCutoff() const noexcept { return cutoff_; }

        //! Cutoff in Hz.
        inline void setCutoffFrequency( double Freq ) { setCutoff( freqRange.convertTo0to1( Freq ) ); }

        //! Effective cutoff in Hz, including the modulation.
        inline double getCutoffFrequency() const noexcept { return freqRange.convertFrom0to1( getModulatedCutoff() ); }

        //! Cutoff modulation, added to the cutoff position ( -1.0 - 1.0 ), scaled by the depth.
        inline void setCutoffModulate( double newCutoff )
        {
            newCutoff = utils::clamp( newCutoff, -1.0, 1.0 );
            if( newCutoff != cutoffMod_ )
            {
                cutoffMod_ = newCutoff;
                dirty_     = true;
            }
        };

        inline void setDepth( double newDepth )
        {
            newDepth = utils::clamp( newDepth, 0.0, 1.0 );
            if( newDepth != depth_ )
            {
                depth_ = newDepth;
                dirty_ = true;
            }
        }

        //! Resonance ( 0.0 - 1.0 ). It has no effect on the 1 pole types.
        inline void setResonance( double newResonance )
        {
            newResonance = utils::clamp( newResonance, 0.0, 1.0 );
            if( newResonance != resonance_ )
            {
                resonance_ = newResonance;
                dirty_     = true;
            }
        };
        inline double getResonance() const noexcept { return resonance_; }

        void reset() noexcept override { clearState(); }

        //! Runs the filter on silence (lets it ring out).
        T advance( unsigned int NumSamples ) noexcept override
        {
            T out = 0;
            for( auto i = 0u; i < NumSamples; ++i )
                out = processSample( 0, (T)0 );
            return out;
        }

        T process() noexcept override { return processSample( 0, (T)0 ); }

        //! Filters one sample of the first channel.
        T process( T inputValue ) noexcept { return processSample( 0, inputValue ); }

        //! Filters one sample of the given channel.
        T processSample( int Channel, T inputValue ) noexcept
        {
            if( this->skipProcess() )
                return inputValue;

            if( dirty_ )
                updateCoefficients();

            auto& st = state_[(size_t)utils::clamp( Channel, 0, maxChannels - 1 )];
            return static_cast<T>( processInternal( st, (double)inputValue ) );
        }

        void process( Buffer<float>& Buffer ) noexcept override
        {
            auto bSize  = Buffer.getBlockSize();
            auto nChans = Buffer.getNumInputChannels();

            for( uint32_t channel = 0; channel < nChans; channel++ )
            {
                auto in  = Buffer.getInputBuffer( channel );
                auto out = Buffer.getOutputBuffer( channel );

                if( in && out )
                {
                    for( uint32_t sample = 0; sample < bSize; ++sample )
                        *out++ = (float)processSample( (int)channel, (T)*in++ );
                }
            }
        }

        //! Filters, in place, a buffer with the juce::AudioBuffer interface.
        template<typename AudioBufferType>
        auto process( AudioBufferType& Buf ) noexcept
            -> decltype( Buf.getWritePointer( 0 ), Buf.getNumChannels(), Buf.getNumSamples(), void() )
        {
            const auto nChans   = Buf.getNumChannels();
            const auto nSamples = Buf.getNumSamples();

            for( int channel = 0; channel < nChans; ++channel )
            {
                auto data = Buf.getWritePointer( channel );
                for( int sample = 0; sample < nSamples; ++sample, ++data )
                    *data = static_cast<std::remove_reference_t<decltype( *data )>>( processSample( channel, (T)*data ) );
            }
        }

    private:
        struct SvfCoeffs
        {
            double k{ Math<double>::sqrt2 }; // Damping ( 1 / Q ).
            double a1{ 0 }, a2{ 0 }, a3{ 0 };
            double m1{ 0 }; // Band gain for the band boost.
        };

        struct SvfState
        {
            double ic1{ 0 }, ic2{ 0 };
        };

        struct ChannelState
        {
            std::array<SvfState, 4> svf{};
            std::array<double, 4>   pole{}; // One pole / ladder stage states.
        };

        Type  type_{ Type::LOW_PASS };
        Slope slope_{ Slope::_12DB };

        FilterType filterType_{ LOWPASS_2_POLE };
        double     cutoff_{ 1.0 }, resonance_{ 0.0 }, depth_{ 1.0 }, cutoffMod_{ 0.0 };
        bool       dirty_{ true };

        // Coefficients...
        int                      numSections_{ 1 };
        std::array<SvfCoeffs, 4> svf_{};
        double                   onePoleG_{ 0 }; // g / ( 1 + g )
        double                   ladderK_{ 0 }; // Ladder feedback ( 0 - 4 ).
        double                   ladderDrive_{ 1 };
        double                   ladderGain_{ 1 }; // Pass band gain compensation.

        std::array<ChannelState, maxChannels> state_{};

        static constexpr double kMinDamping = 0.1; // Q of 10 at full resonance.

        static FilterType toFilterType( Type Typ, Slope Slp ) noexcept
        {
            const bool steep = ( Slope::_24DB == Slp );
            switch( Typ )
            {
                case Type::HIGH_PASS: return steep ? HIGHPASS_4_POLE : HIGHPASS_2_POLE;
                case Type::BAND_PASS: return steep ? BANDPASS_4_POLE : BANDPASS_2_POLE;
                default: return steep ? LOWPASS_4_POLE : LOWPASS_2_POLE;
            }
        }

        //! Number of 2 pole sections of the cascaded types.
        static int sectionsOf( FilterType Typ ) noexcept
        {
            switch( Typ )
            {
                case LOWPASS_4_POLE:
                case HIGHPASS_4_POLE:
                case BANDPASS_4_POLE:
                case BANDSTOP_4_POLE:
                case BANDBOOST_4_POLE: return 2;
                case LOWPASS_6_POLE:
                case HIGHPASS_6_POLE:
                case BANDPASS_6_POLE:
                case BANDSTOP_6_POLE:
                case BANDBOOST_6_POLE: return 3;
                case LOWPASS_8_POLE:
                case HIGHPASS_8_POLE:
                case BANDPASS_8_POLE:
                case BANDSTOP_8_POLE:
                case BANDBOOST_8_POLE: return 4;
                default: return 1;
            }
        }

        void recalculate() noexcept override { dirty_ = true; }

        inline double getModulatedCutoff() const noexcept
        {
            return utils::clamp( cutoff_ + ( depth_ * cutoffMod_ ), 0.0, 1.0 );
        }

        void clearState() noexcept
        {
            for( auto& st : state_ )
                st = ChannelState{};
        }

        static void setSvf( SvfCoeffs& c, double g, double k ) noexcept
        {
            c.k  = k;
            c.a1 = 1.0 / ( 1.0 + g * ( g + k ) );
            c.a2 = g * c.a1;
            c.a3 = g * c.a2;
        }

        void updateCoefficients() noexcept
        {
            const double sr  = ( this->sampleRate() > 0 ) ? (double)this->sampleRate() : 48000.0;
            const double fc  = utils::min( freqRange.convertFrom0to1( getModulatedCutoff() ), 0.45 * sr );
            const double g   = std::tan( Math<double>::pi * fc / sr );
            const double res = resonance_;

            onePoleG_    = g / ( 1.0 + g );
            numSections_ = sectionsOf( filterType_ );

            if( filterType_ <= HIGHPASS_8_POLE )
            { // Butterworth cascade, the resonance raises the Q of the last ( highest Q ) section...
                const int order = 2 * numSections_;
                for( int i = 0; i < numSections_; ++i )
                {
                    double k = 2.0 * std::cos( Math<double>::pi * ( 2 * i + 1 ) / ( 2.0 * order ) );
                    if( i == numSections_ - 1 )
                        k = ( k * ( 1.0 - res ) ) + ( kMinDamping * res );
                    setSvf( svf_[i], g, k );
                }
            }
            else if( filterType_ <= BANDSTOP_8_POLE )
            { // Identical sections, the resonance narrows the band...
                const double k = ( Math<double>::sqrt2 * ( 1.0 - res ) ) + ( kMinDamping * res );
                for( int i = 0; i < numSections_; ++i )
                    setSvf( svf_[i], g, k );
            }
            else if( filterType_ <= BANDBOOST_8_POLE )
            { // Bell sections boosting 12 dB in total, the resonance narrows the band...
                const double A = std::pow( 10.0, ( 12.0 / numSections_ ) / 40.0 );
                const double k = ( ( Math<double>::sqrt2 * ( 1.0 - res ) ) + ( kMinDamping * res ) ) / A;
                for( int i = 0; i < numSections_; ++i )
                {
                    setSvf( svf_[i], g, k );
                    svf_[i].m1 = k * ( ( A * A ) - 1.0 );
                }
            }
            else if( MODEL_4 == filterType_ )
            {
                setSvf( svf_[0], g, ( Math<double>::sqrt2 * ( 1.0 - res ) ) + ( kMinDamping * res ) );
            }
            else
            { // Ladders...
                const bool classic = ( LOWPASS_CLASSIC == filterType_ );
                ladderK_           = ( classic ? 4.0 : 3.9 ) * res;
                ladderDrive_       = classic ? 2.5 : 1.5;
                ladderGain_        = classic ? 1.0 : ( 1.0 + ( 0.5 * ladderK_ ) );
            }

            dirty_ = false;
        }

        static inline double svfTick( SvfState& s, const SvfCoeffs& c, double v0, double& band ) noexcept
        {
            const double v3 = v0 - s.ic2;
            const double v1 = ( c.a1 * s.ic1 ) + ( c.a2 * v3 );
            const double v2 = s.ic2 + ( c.a2 * s.ic1 ) + ( c.a3 * v3 );
            s.ic1           = ( 2.0 * v1 ) - s.ic1;
            s.ic2           = ( 2.0 * v2 ) - s.ic2;
            band            = v1;
            return v2; // Low pass...
        }

        inline double onePole( double& s, double x ) const noexcept
        {
            const double v = ( x - s ) * onePoleG_;
            const double y = v + s;
            s              = y + v;
            return y;
        }

        inline double ladder( ChannelState& st, double x ) const noexcept
        {
            // Zero delay feedback: solve the feedback loop for the linear
            // ladder, then saturate the input of the first stage.
            const double G  = onePoleG_;
            const double G2 = G * G;
            const double S  = ( 1.0 - G ); // Each stage: y = G * x + ( 1 - G ) * s
            const double sigma =
                ( G2 * G * S * st.pole[0] ) + ( G2 * S * st.pole[1] ) + ( G * S * st.pole[2] ) + ( S * st.pole[3] );

            double u = ( ( x * ladderGain_ ) - ( ladderK_ * sigma ) ) / ( 1.0 + ( ladderK_ * G2 * G2 ) );
            u        = std::tanh( ladderDrive_ * u ) / ladderDrive_;

            const double y1 = onePole( st.pole[0], u );
            const double y2 = onePole( st.pole[1], y1 );
            const double y3 = onePole( st.pole[2], y2 );
            const double y4 = onePole( st.pole[3], y3 );

            switch( filterType_ )
            {
                case MODEL_2: return ( 4.0 * y2 ) - ( 8.0 * y3 ) + ( 4.0 * y4 ); // 4 pole band pass...
                case MODEL_3: return u - ( 4.0 * y1 ) + ( 6.0 * y2 ) - ( 4.0 * y3 ) + y4; // 4 pole high pass...
                default: return y4; // 4 pole low pass...
            }
        }

        inline double processInternal( ChannelState& st, double x ) noexcept
        {
            double band = 0.0;
            switch( filterType_ )
            {
                case LOWPASS_1_POLE: return onePole( st.pole[0], x );
                case HIGHPASS_1_POLE: return x - onePole( st.pole[0], x );

                case LOWPASS_2_POLE:
                case LOWPASS_4_POLE:
                case LOWPASS_6_POLE:
                case LOWPASS_8_POLE:
                    for( int i = 0; i < numSections_; ++i )
                        x = svfTick( st.svf[i], svf_[i], x, band );
                    return x;

                case HIGHPASS_2_POLE:
                case HIGHPASS_4_POLE:
                case HIGHPASS_6_POLE:
                case HIGHPASS_8_POLE:
                    for( int i = 0; i < numSections_; ++i )
                    {
                        const double low = svfTick( st.svf[i], svf_[i], x, band );
                        x                = x - ( svf_[i].k * band ) - low;
                    }
                    return x;

                case BANDPASS_2_POLE:
                case BANDPASS_4_POLE:
                case BANDPASS_6_POLE:
                case BANDPASS_8_POLE:
                    for( int i = 0; i < numSections_; ++i )
                    {
                        svfTick( st.svf[i], svf_[i], x, band );
                        x = svf_[i].k * band; // Unity gain at the centre frequency...
                    }
                    return x;

                case BANDSTOP_2_POLE:
                case BANDSTOP_4_POLE:
                case BANDSTOP_6_POLE:
                case BANDSTOP_8_POLE:
                    for( int i = 0; i < numSections_; ++i )
                    {
                        svfTick( st.svf[i], svf_[i], x, band );
                        x = x - ( svf_[i].k * band );
                    }
                    return x;

                case BANDBOOST_2_POLE:
                case BANDBOOST_4_POLE:
                case BANDBOOST_6_POLE:
                case BANDBOOST_8_POLE:
                    for( int i = 0; i < numSections_; ++i )
                    {
                        svfTick( st.svf[i], svf_[i], x, band );
                        x = x + ( svf_[i].m1 * band );
                    }
                    return x;

                case MODEL_4:
                    return svfTick( st.svf[0], svf_[0], std::tanh( 2.0 * x ) * 0.5, band );

                default: // MODEL_1, MODEL_2, MODEL_3, LOWPASS_CLASSIC
                    return ladder( st, x );
            }
        }
    };

    //
}; // Namespace AudIO
#endif // Header guard
