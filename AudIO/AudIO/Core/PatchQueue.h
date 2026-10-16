#pragma once

#include "../Includes/AudIOBuffer.h"
#include "SPSCQueue.h"
#include "RtSync.h"

#include <cstdio>

namespace AudIO
{

    enum class PatchType
    {
        input,
        output
    };

    // Where a patch's block comes from: the block given, or a channel of
    // another buffer, looked up only when the patch is applied, after the
    // patches queued before it (which may change that channel).
    enum class PatchSource
    {
        block,
        input,
        output
    };

    template<typename T>
    struct PatchData
    {
        Buffer<T>*  dest      = nullptr;
        T*          src       = nullptr;
        uint32_t    destch    = 0;
        PatchType   type      = PatchType::input;
        Buffer<T>*  srcBuffer = nullptr;
        uint32_t    srcch     = 0;
        PatchSource from      = PatchSource::block;
    };

    // Patches made while the audio thread runs are queued here and applied by
    // the audio thread at the start of a block, so that a buffer's channels
    // only change between blocks. Patches are queued by control threads,
    // which the design's lock serializes (a single producer).
    template<typename T>
    struct PatchQueue
    {
        static struct PatchQueue& get()
        {
            static struct PatchQueue _s;
            return _s;
        }

        void start()
        {
            process();
            _running.store( true );
        }

        void stop()
        {
            _running.store( false );
            process();
        }

        bool isRunning() const { return _running.load(); }

        // Audio thread (or a control thread when the queue is not running).
        void process()
        {
            if( _suspended.load() )
                return;

            PatchData<T> data;
            while( _queue.pop( data ) )
            {
                process( data );
            }
        }

        // Control thread: stop applying patches (they stay queued) until
        // resume(), so that a buffer can be resized. Returns false, without
        // suspending, if the audio thread does not finish its block in time.
        bool suspend()
        {
            _suspended.store( true );
            if( AudioThreadSync::get().synchronize() )
                return true;
            _suspended.store( false );
            return false;
        }

        void resume() { _suspended.store( false ); }

        // Control thread: returns once every queued patch has been applied,
        // so that buffers they refer to can be freed. The audio thread
        // empties the queue at the start of every block; if it is not running
        // blocks, the patches are applied here with the audio thread kept
        // out. False if that was not possible either.
        bool waitUntilEmpty()
        {
            if( !_running.load() || _queue.isEmpty() )
                return true;

            auto& sync = AudioThreadSync::get();
            for( int attempt = 0; attempt < 4; ++attempt )
            {
                sync.waitForBlock( std::chrono::milliseconds( 50 ) );
                if( _queue.isEmpty() )
                    return true;
            }
            return drainHere();
        }

        void patchInput( Buffer<T>* destination, T* src, uint32_t destCh )
        {
            if( destination )
            {
                destination->acquireInputForPatch( destCh );
                PatchData<T> data;
                data.dest   = destination;
                data.src    = src;
                data.destch = destCh;
                data.type   = PatchType::input;
                push( data );
            }
        }

        void patchOutput( Buffer<T>* destination, T* src, uint32_t destCh )
        {
            if( destination )
            {
                PatchData<T> data;
                data.dest   = destination;
                data.src    = src;
                data.destch = destCh;
                data.type   = PatchType::output;
                push( data );
            }
        }

        // Patch Destination's DestCh to Source's SrcCh (one of its inputs or
        // outputs) as it is once the patches queued before have been applied.
        // Source must outlive the patch (releaseWhenUnused() sees to that).
        void patchInputFrom( Buffer<T>* destination, uint32_t destCh, Buffer<T>* source, PatchSource from, uint32_t srcCh )
        {
            if( destination && source )
            {
                destination->acquireInputForPatch( destCh );
                PatchData<T> data;
                data.dest      = destination;
                data.destch    = destCh;
                data.type      = PatchType::input;
                data.srcBuffer = source;
                data.srcch     = srcCh;
                data.from      = from;
                push( data );
            }
        }

        void patchOutputFrom( Buffer<T>* destination, uint32_t destCh, Buffer<T>* source, PatchSource from, uint32_t srcCh )
        {
            if( destination && source )
            {
                PatchData<T> data;
                data.dest      = destination;
                data.destch    = destCh;
                data.type      = PatchType::output;
                data.srcBuffer = source;
                data.srcch     = srcCh;
                data.from      = from;
                push( data );
            }
        }

    private:
        void push( PatchData<T>& data )
        {
            if( _running )
            {
                if( !_queue.push( data ) )
                    pushWhenFull( data );
            }
            else
            {
                process( data );
            }
        }

        // The audio thread empties the queue every block, so a full queue
        // only means a burst of patches: wait for room. If the audio thread
        // is not running blocks at all (the device stalled without being
        // stopped), apply the queued patches here, with the audio thread
        // kept out. A patch is never dropped silently.
        void pushWhenFull( PatchData<T>& data )
        {
            auto& sync = AudioThreadSync::get();
            for( int attempt = 0; attempt < 10; ++attempt )
            {
                sync.waitForBlock( std::chrono::milliseconds( 50 ) );
                if( _queue.push( data ) )
                    return;
            }

            if( drainHere() && _queue.push( data ) )
                return;

            std::fprintf( stderr, "AudIO: the patch queue is full and the audio thread is stuck; a patch was lost\n" );
        }

        // Control thread: apply the queued patches here, with the audio
        // thread kept out. For an audio thread that has stopped running
        // blocks without being stopped.
        bool drainHere()
        {
            auto& sync = AudioThreadSync::get();
            if( !sync.acquireExclusive() )
                return false;

            std::fprintf( stderr, "AudIO: the audio thread is not running blocks; applying queued patches directly\n" );
            const auto suspended = _suspended.exchange( false );
            process();
            _suspended.store( suspended );
            sync.releaseExclusive();
            return true;
        }

        void process( PatchData<T>& data )
        {
            auto src = data.src;
            if( data.srcBuffer )
            {
                src = ( data.from == PatchSource::input ) ? data.srcBuffer->getInputBuffer( data.srcch )
                                                          : data.srcBuffer->getOutputBuffer( data.srcch );
            }

            if( data.type == PatchType::input )
            {
                data.dest->patchInput( src, data.destch );
            }
            else
            {
                data.dest->patchOutput( src, data.destch );
            }
        }

        // Large enough for a whole project's routing to be made between two
        // blocks.
        static constexpr size_t QUEUE_SIZE = 4096;

        std::atomic<bool>                   _running{ false };
        std::atomic<bool>                   _suspended{ false };
        SPSCQUeue<PatchData<T>, QUEUE_SIZE> _queue;
    };

    // Control thread: free something the audio thread may still be using:
    // an object or buffer just taken out of what the audio thread processes.
    // Patches still queued may refer to its buffers, and a block that started
    // before it was taken out may still be processing it; both waits are
    // short (at most one block).
    inline void releaseWhenUnused( std::shared_ptr<void> Garbage )
    {
        if( !PatchQueue<float>::get().waitUntilEmpty() )
        {
            // Freeing it could let a stuck patch write into freed memory.
            std::fprintf( stderr, "AudIO: queued patches were not applied; a removed object is kept\n" );
            static_cast<void>( new std::shared_ptr<void>( std::move( Garbage ) ) );
            return;
        }
        releaseAfterBlock( std::move( Garbage ) );
    }
}
