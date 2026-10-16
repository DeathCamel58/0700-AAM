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
#ifndef AUDIO_ADSR_H_INCLUDE
    #define AUDIO_ADSR_H_INCLUDE

    #include <cmath>

    #include "../Includes/idsp.h"
    #include "../Includes/Utils.h"

namespace AudIO
{
    template<typename T>
    class ADSR : public IDSP<T>
    {
    public:
        struct Params
        {
            double A; // Time in seconds...
            double H; // Time in seconds...
            double D; // Time in seconds...
            double S; // Sustain level ( 0.0 - 1.0 )
            double R; // Time in seconds...
        };

        enum State
        { //! ADSR envelope states.
            IDLE = 0,
            ATTACK,
            HOLD,
            DECAY,
            SUSTAIN,
            RELEASE,
            MAX
        };

        enum class Type
        { //! ADSR curve.
            LINEAR,
            EXPONENTIAL
        };

        explicit ADSR( Type Typ = Type::EXPONENTIAL ) noexcept
            : type_( Typ ) {}

        explicit ADSR( const Params& P ) noexcept
        {
            setParameters( P );
        }

        bool         isActive() noexcept { return ( this->isEnabled() && currentStage_ != State::IDLE ); }
        inline State getCurrentState() const noexcept { return currentStage_; }
        Type         getType() noexcept { return type_; }

        void setType( Type Typ ) noexcept { type_ = Typ; }
        void setParameters( const Params& P ) noexcept
        {
            stageTime_[State::IDLE]    = 0.0;
            stageTime_[State::ATTACK]  = utils::max( P.A, minimumLevel_ );
            stageTime_[State::HOLD]    = utils::max( P.H, 0.0 );
            stageTime_[State::DECAY]   = utils::max( P.D, 0.0 );
            stageTime_[State::SUSTAIN] = utils::clamp( P.S, 0.0, 1.0 );
            stageTime_[State::RELEASE] = utils::max( P.R, minimumLevel_ );
        }

        //! Sets the stage times (in seconds) and the sustain level (0.0 - 1.0).
        //! Note the order of the arguments: release comes before sustain.
        void setParameters( double Attack, double Hold, double Decay, double Release, double Sustain ) noexcept
        {
            setParameters( Params{ Attack, Hold, Decay, Sustain, Release } );
        }

        Params getParameters() const noexcept
        {
            return Params{ stageTime_[State::ATTACK], stageTime_[State::HOLD], stageTime_[State::DECAY],
                           stageTime_[State::SUSTAIN], stageTime_[State::RELEASE] };
        }

        //! Current output level of the envelope, without advancing it.
        inline T getLevel() const noexcept { return static_cast<T>( currentStage_ == State::IDLE ? 0.0 : currentLevel_ ); }

        void gate( bool On ) noexcept
        {
            if( On )
                keyOn();
            else
                keyOff();
        }

        void keyOn() noexcept
        {
            if( this->isEnabled() )
                enterStage( State::ATTACK );
        }
        void keyOff() noexcept
        {
            if( this->isEnabled() )
                enterStage( State::RELEASE );
            else
                enterStage( State::IDLE );
        }

        void reset() noexcept override { enterStage( State::IDLE ); }

        T advance( unsigned int NumSamples ) noexcept override
        {
            if( this->skipProcess() )
                return 0.0;

            T val = 0.0;
            for( auto i = 0u; i < NumSamples; ++i )
                val = static_cast<T>( process_internal() );
            return val;
        }

        T process() noexcept override
        {
            return static_cast<T>( this->skipProcess() ? 1.0 : process_internal() );
        }

        void process( Buffer<float>& buffer ) noexcept override
        {
            if( this->skipProcess() )
                return;

            auto   outs = buffer.getOutputs();
            float* outL = outs[0];
            float* outR = outs[1];

            if( !outL )
                return;

            const auto nSamples = (int)buffer.getBlockSize();
            for( auto sample = 0; sample < nSamples; ++sample )
            {
                auto adsrOut = static_cast<float>( this->process_internal() );
                // iterate over all available output channels
                *outL++ *= adsrOut;
                if( outR )
                    *outR++ *= adsrOut;
            }
        }

    private:
        void recalculate() noexcept override
        {
            enterStage( currentStage_ );
        }

        inline double process_internal()
        {
            switch( currentStage_ )
            {
                default: // IDLE...
                    return 0.0;
                case State::SUSTAIN: // Follows changes of the sustain level...
                    currentLevel_ = utils::max( stageTime_[State::SUSTAIN], minimumLevel_ );
                    return stageTime_[State::SUSTAIN];
                case State::ATTACK:
                case State::HOLD:
                case State::DECAY:
                case State::RELEASE:
                {
                    const auto out = currentLevel_;
                    if( Type::LINEAR == type_ )
                        currentLevel_ += increment_;
                    else
                        currentLevel_ *= multiplier_;

                    if( ++numCurrentSamples >= numSamplesToNextStage )
                        enterStage( (State)( currentStage_ + 1 ) );

                    return out;
                }
            }
        }

        inline unsigned long calcSamplesToState( State Stage ) noexcept
        {
            switch( Stage )
            {
                default:
                    return 0;
                case State::ATTACK:
                case State::HOLD:
                case State::DECAY:
                case State::RELEASE:
                    return ( stageTime_[Stage] * this->sampleRate() );
            }
        }

        //! Per-sample multiplier of an exponential segment from startLevel to endLevel.
        inline double calculateCoefficient( double startLevel, double endLevel, unsigned long lengthInSamples )
        {
            if( 0 == lengthInSamples )
                return 1.0;
            return std::exp( ( std::log( endLevel ) - std::log( startLevel ) ) / (double)lengthInSamples );
        }

        //! Per-sample increment of a linear segment from startLevel to endLevel.
        inline double calculateIncrement( double startLevel, double endLevel, unsigned long lengthInSamples )
        {
            if( 0 == lengthInSamples )
                return 0.0;
            return ( endLevel - startLevel ) / (double)lengthInSamples;
        }

        void enterStage( State newStage ) noexcept
        {
            double endLevel   = 0;
            numCurrentSamples = 0;

            numSamplesToNextStage = calcSamplesToState( newStage );
            currentStage_         = newStage;

            switch( currentStage_ )
            {
                default:
                    currentStage_ = State::IDLE;
                    currentLevel_ = endLevel = minimumLevel_;
                    break;
                case State::ATTACK:
                    currentLevel_ = minimumLevel_;
                    endLevel      = 1.0;
                    break;
                case State::HOLD:
                    endLevel = currentLevel_;
                    break;
                case State::DECAY:
                    endLevel = utils::max( stageTime_[State::SUSTAIN], minimumLevel_ );
                    break;
                case State::SUSTAIN:
                    if( utils::isEqual<double>( 0.0, stageTime_[currentStage_] ) )
                    { // No sustain, the envelope has ended...
                        enterStage( State::IDLE );
                        return;
                    }
                    currentLevel_ = endLevel = stageTime_[currentStage_];
                    break;
                case State::RELEASE:
                    endLevel = minimumLevel_;
                    break;
            }

            currentLevel_ = utils::max( currentLevel_, minimumLevel_ );
            endLevel      = utils::max( endLevel, minimumLevel_ );
            multiplier_   = calculateCoefficient( currentLevel_, endLevel, numSamplesToNextStage );
            increment_    = calculateIncrement( currentLevel_, endLevel, numSamplesToNextStage );
            switch( currentStage_ )
            {
                case State::ATTACK:
                case State::HOLD:
                case State::DECAY:
                    if( 0 == numSamplesToNextStage )
                    {
                        currentLevel_ = endLevel;
                        enterStage( State( currentStage_ + 1 ) );
                    }
                    break;
                case State::RELEASE:
                    if( 0 == numSamplesToNextStage )
                        enterStage( State::IDLE );
                    break;
                default:
                    break;
            }
        }

    private:
        Type  type_{ Type::EXPONENTIAL };
        State currentStage_{ State::IDLE };
        // Internal...
        static constexpr double        minimumLevel_ = 0.0001;
        std::array<double, State::MAX> stageTime_{ 0.0, 0.01, 0.0, 0.5, 0.1, 1.0 };
        double                         currentLevel_{ minimumLevel_ };
        double                         multiplier_{ 1.0 };
        double                         increment_{ 0.0 };
        unsigned long                  numCurrentSamples{ 0 };
        unsigned long                  numSamplesToNextStage{ 0 };
    };

    //
}; // Namespace AudIO
#endif // AUDIO_ADSR_H_INCLUDE
