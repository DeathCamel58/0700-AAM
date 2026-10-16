#pragma once

#include <unordered_map>

#include "ObjectMeta.h"
#include "RtSync.h"
#include "../Includes/AudioDataTypes.h"
#include "../Includes/Platform.h"
#include "../Objects/LevelMeter.h"

namespace AudIO
{

#define METERING_LOAD_BALANCE ( 1 )

    class MeterEngine : public Object
    {
    public:
        explicit MeterEngine( uint32_t count = 32 );

        bool addMeter( objectMeta& srcobj );
        bool removeMeter( uint32_t id );
        void clear();

#if defined( METERING_LOAD_BALANCE ) && ( METERING_LOAD_BALANCE > 0 )
        void setRateHz( float RateHz );
#endif

        MeterData& read( uint32_t id );
        void       readAll( const std::function<void( uint32_t meterid, MeterData& data )>& MeterCallback );

    protected:
        friend class DesignImpl;
        void prepareToPlay( uint32_t samplerate, uint32_t blocksize ) override;
        void process( Buffer<float>& buffer ) override;

    private:
        using MeterEntry   = std::pair<std::shared_ptr<LevelMeter>, objectMeta*>;
        using MeterMapType = std::unordered_map<uint32_t, MeterEntry>;

        // The meters the audio thread runs, in the map's order. Published
        // whole whenever the map changes; a removed meter lives on in the
        // list it was in until the audio thread is done with that list.
        struct MeterList
        {
            std::vector<MeterEntry> meters;
        };

        void publish();

        MeterMapType           _meters;
        RtPublished<MeterList> _list;
        MeterData              _data{ 2 };
        recursive_mutex        _lock;

#if defined( METERING_LOAD_BALANCE ) && ( METERING_LOAD_BALANCE > 0 )
        int              _loopCntr{ 1 };
        std::atomic<int> _metersPerBlock{ 1 };
        int              _meterIdx{ 0 }; // Audio thread.
        float            _rateMs{ 0.1f };
        void             recalculate();
#endif
    };
} // Namespace AudIO
