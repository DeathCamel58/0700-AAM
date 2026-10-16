#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#define USE_STATIC_RAMPER ( 0 )

#if defined( USE_STATIC_RAMPER ) && ( 0 < USE_STATIC_RAMPER )
    #include "../3rdparty/phmap/phmap.h"
#endif

namespace AudIO
{
    struct RampedParameterBase
    {
        virtual bool isRamping() const   = 0;
        virtual void ramp( int samples ) = 0;
    };

#if defined( USE_STATIC_RAMPER ) && ( 0 < USE_STATIC_RAMPER )
    struct SRamper
    {
        static struct SRamper& get()
        {
            static struct SRamper _s;
            return _s;
        }

        void ramp( int samples = 1 )
        {
            return;

            // Merge added/updated elements...
            processSet.merge( _add );
            _add.clear();

            // Remove destroyed elements...
            for( auto& it : _remove )
                processSet.erase( it );
            _remove.clear();

            // Process ramp quque...
            if( processSet.size() )
            {
                auto itr = processSet.begin();
                while( itr != processSet.end() )
                {
                    ( *itr )->ramp( samples );
                    if( !( *itr )->isRamping() )
                        itr = processSet.erase( itr );
                    else
                        ++itr;
                }
            }
        }

        void add( RampedParameterBase* p )
        {
            if( p )
            {
                _add.emplace( p );
            }
        }
        void remove( RampedParameterBase* p )
        {
            if( p )
            {
                _remove.emplace( p );
            }
        }

        phmap::flat_hash_set<RampedParameterBase*> processSet, _remove, _add;
    };
#endif

    //
    // RampedParameter: a value that the audio thread ramps towards its target.
    //
    // Control threads set the target (store(), operator=) and read it back
    // (value(), getTarget()). The audio thread ramps (getNext()) and reads
    // where the ramp is (getCurrent(), load()). A new target is handed over
    // through atomics and taken up by the audio thread at its next getNext(),
    // so that the ramp's state is the audio thread's alone. When nothing is
    // processing the parameter (its object is not playing), a control thread
    // may call getNext() or update() itself.
    //
    template<typename T>
    class RampedParameter : public RampedParameterBase
    {
    public:
        explicit RampedParameter( T V = .0f, int Steps = 12000, bool IsLinear = true )
            : // default ramp duration of 10ms @48K
            isLinear( IsLinear )
            , nSteps( Steps )
            , current( V )
            , target( V )
            , requested( V )
        {}

        // Copies the target, ramped to; for containers, before playing.
        RampedParameter( const RampedParameter& Other )
            : RampedParameter( Other.getTarget(), Other.nSteps, Other.isLinear )
        {}

        RampedParameter& operator=( const RampedParameter& ) = delete;

#if defined( USE_STATIC_RAMPER ) && ( 0 < USE_STATIC_RAMPER )
        virtual ~RampedParameter()
        {
            SRamper::get().remove( this );
        }
#else
        ~RampedParameter() = default;
#endif

        // Control threads (and the audio thread).
        inline T value() const { return requested.load( std::memory_order_relaxed ); }
        inline T getTarget() const { return value(); }

        // Audio thread (or a control thread, while the parameter is not processed).
        inline T getCurrent() const { return current.load( std::memory_order_relaxed ); }
        inline T load() const { return getCurrent(); }

        inline T getNext( int Samples = 1 )
        {
            update();
            ramp( Samples );
            return getCurrent();
        }

        void operator=( const T val )
        {
            store( val );
        }

        // Before playing.
        void setRampDuration( float sampleRate, float rampLengthInSeconds ) noexcept
        {
            nSteps = std::floor( rampLengthInSeconds * sampleRate );
        }

        void setRampDuration( int numSteps ) noexcept
        {
            nSteps = numSteps;
        }

        // Control threads: ramp to Val, or jump to it if Force. The latest
        // call wins.
        void store( const T val, bool force = false )
        {
            requested.store( val, std::memory_order_relaxed );
            // Bit 0 carries Force; the rest counts the calls.
            const auto serial = requestSerial.load( std::memory_order_relaxed );
            requestSerial.store( ( ( serial + 2u ) & ~1u ) | ( force ? 1u : 0u ), std::memory_order_release );
        }

        // Audio thread: take up a new target, if any.
        void update()
        {
            const auto serial = requestSerial.load( std::memory_order_acquire );
            if( serial == takenSerial )
                return;
            takenSerial = serial;

            const auto val = requested.load( std::memory_order_relaxed );
            target         = val;

            if( ( 0 != ( serial & 1u ) ) || utils::isEqual( val, getCurrent() ) )
            {
                current.store( val, std::memory_order_relaxed );
                nStepCounter = nSteps;
#if defined( USE_STATIC_RAMPER ) && ( 0 < USE_STATIC_RAMPER )
                SRamper::get().remove( this );
#endif
                return;
            }

            if( isLinear )
                stepSize = ( target - getCurrent() ) / (T)nSteps;
            // Add to ramper...
            nStepCounter = 0;
#if defined( USE_STATIC_RAMPER ) && ( 0 < USE_STATIC_RAMPER )
            SRamper::get().add( this );
#endif
        }

        // Audio thread.
        bool isRamping() const override
        {
            return ( nStepCounter < nSteps );
        }

    private:
        bool isLinear{ true };
        int  nSteps;
        int  nStepCounter{ 0 };

        // The ramp: the audio thread's (current is atomic only so that
        // getCurrent() may be called elsewhere).
        std::atomic<T> current{ 0 };
        T              target{ 0 };
        T              stepSize{ 0 };
        uint32_t       takenSerial{ 0 };

        // The target handed over by control threads.
        std::atomic<T>        requested{ 0 };
        std::atomic<uint32_t> requestSerial{ 0 };

        void ramp( int samples = 1 ) override
        {
            if( isRamping() )
            {
                auto availSteps = nSteps - nStepCounter;
                auto incSteps   = ( samples < availSteps ) ? samples : availSteps;

                current.store( isLinear ? linearInterpolator( incSteps ) : logInterpolator( incSteps ), std::memory_order_relaxed );
                nStepCounter += incSteps;
            }
            else
                current.store( target, std::memory_order_relaxed );
        }

        T linearInterpolator( int Samples )
        {
            return getCurrent() + stepSize * Samples;
        }

        T logInterpolator( int /*Samples*/ )
        {
            auto step = std::exp( ( std::log( std::abs( target ) ) - std::log( std::abs( getCurrent() ) ) ) / nSteps );
            return static_cast<T>( getCurrent() * std::pow( step, nStepCounter ) );
        }
    };
}
