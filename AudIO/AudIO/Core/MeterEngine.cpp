#pragma once

#include "MeterEngine.h"

namespace AudIO
{
    MeterEngine::MeterEngine( uint32_t count )
        : Object( 0, 0, 0 )
    {
        _meters.reserve( count );
        setRateHz( 20 );
    }

    bool MeterEngine::addMeter( objectMeta& srcobj )
    {
        if( srcobj.getObject().outputCount() < 2 )
        {
            return false;
        }
        auto objid = srcobj.getObject().getId();
        if( auto it = _meters.find( objid ); it == _meters.end() )
        {
            autolock guard{ _lock };
            _meters.try_emplace( objid, std::make_shared<LevelMeter>(), &srcobj );
            publish();
        }
#if defined( METERING_LOAD_BALANCE ) && ( METERING_LOAD_BALANCE > 0 )
        recalculate();
#endif
        return true;
    }

    bool MeterEngine::removeMeter( uint32_t id )
    {
        autolock guard{ _lock };
        if( 0 != _meters.erase( id ) )
            publish();
        return true;
    }

    void MeterEngine::clear()
    {
        autolock guard{ _lock };
        _meters.clear();
        publish();
    }

    void MeterEngine::publish()
    {
        auto list = std::make_unique<MeterList>();
        list->meters.reserve( _meters.size() );
        for( auto& [id, entry] : _meters )
            list->meters.push_back( entry );
        _list.publish( std::move( list ) );
    }

    void MeterEngine::prepareToPlay( uint32_t samplerate, uint32_t blocksize )
    {
        Object::prepareToPlay( samplerate, blocksize );
#if defined( METERING_LOAD_BALANCE ) && ( METERING_LOAD_BALANCE > 0 )
        recalculate();
#endif
    }

#if defined( METERING_LOAD_BALANCE ) && ( METERING_LOAD_BALANCE > 0 )
    void MeterEngine::setRateHz( float RateHz )
    {
        _rateMs = ( 1000.f / RateHz );
        recalculate();
    }

    void MeterEngine::recalculate()
    {
        // Blocks per metering period: blockRate() is in seconds. Not known
        // before prepareToPlay().
        const auto blockMs = ( ( 0 < sampleRate() ) && ( 0 < blockSize() ) ) ? ( blockRate() * 1000.f ) : 0.f;
        _loopCntr          = ( 0.f < blockMs ) ? utils::max( 1, (int)( _rateMs / blockMs ) ) : 1;
        _metersPerBlock.store( (int)( 1 + ( (float)_meters.size() / (float)_loopCntr ) ) );
    }
#endif

    void MeterEngine::process( Buffer<float>& /*buffer*/ )
    {
        auto list = _list.get();
        if( !list )
            return;
        auto& meters = list->meters;

#if defined( METERING_LOAD_BALANCE ) && ( METERING_LOAD_BALANCE > 0 )
        // Round-robin: a few meters each block, starting over after the
        // last one. The list may have shrunk since the last block.
        const auto count = (int)meters.size();
        if( 0 < count )
        {
            if( _meterIdx >= count )
                _meterIdx = 0;
            const auto numMeters = utils::min( _metersPerBlock.load(), count - _meterIdx );
            for( auto i = 0; i < numMeters; ++i, ++_meterIdx )
            {
                auto& [meter, objMeta] = meters[(size_t)_meterIdx];
                if( objMeta )
                    meter->process( objMeta->getBuffer() );
            }
            if( _meterIdx >= count )
                _meterIdx = 0;
        }
#else
        for( auto& [meter, objMeta] : meters )
        {
            if( objMeta )
                meter->process( objMeta->getBuffer() );
        }
#endif
    }

    MeterData& MeterEngine::read( uint32_t id )
    {
        if( auto it = _meters.find( id ); it != _meters.end() )
        {
            auto& [meter, objMeta] = it->second;
            meter->getMeter( _data );
        }
        else
        {
            for( auto& itr : _data )
                itr.reset();
        }
        return _data;
    }

    void MeterEngine::readAll( const std::function<void( uint32_t meterid, MeterData& data )>& MeterCallback )
    {
        if( MeterCallback )
            for( auto& [id, meteringInfo] : _meters )
            {
                auto& [meter, objMeta] = meteringInfo;
                meter->getMeter( _data );
                MeterCallback( id, _data );
            }
    }

} // Namespace AudIO
