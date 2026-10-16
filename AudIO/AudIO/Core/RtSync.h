#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace AudIO
{
    //
    // AudioThreadSync: lets control threads find out when the audio thread has
    // stopped using state they have just replaced.
    //
    // The top-level design brackets every block with beginBlock() and
    // endBlock(), which bump a counter: the counter is odd while a block runs.
    // A control thread that has unpublished something (swapped an atomic
    // pointer to a new snapshot, say) only has to wait for the block that was
    // running at that moment, if any, to end; blocks that start later see the
    // new state. That wait is short: at most the rest of one block's
    // processing time, and none at all when no block is running or the audio
    // device is stopped.
    //
    // A control thread can also take exclusive access to the graph; blocks
    // that start while it holds it are skipped. That is only meant for the
    // rare case where the audio thread has stopped calling back but has not
    // been stopped.
    //
    // There is one audio thread per process (the device callback that
    // processes the top-level design).
    //
    class AudioThreadSync
    {
    public:
        static AudioThreadSync& get()
        {
            static AudioThreadSync instance;
            return instance;
        }

        // Audio thread: call at the start of each top-level block. Returns
        // false if the block must be skipped (a control thread holds
        // exclusive access); endBlock() must not be called then.
        bool beginBlock() noexcept
        {
            audioThread.store( std::this_thread::get_id(), std::memory_order_relaxed );
            counter.fetch_add( 1 );
            if( exclusive.load() )
            {
                counter.fetch_add( 1 );
                return false;
            }
            return true;
        }

        // Audio thread: call at the end of each block begun with beginBlock().
        void endBlock() noexcept { counter.fetch_add( 1 ); }

        bool isAudioThread() const noexcept
        {
            return audioThread.load( std::memory_order_relaxed ) == std::this_thread::get_id();
        }

        // A stamp to check later with hasPassed(), taken after unpublishing.
        uint32_t stamp() const noexcept { return counter.load(); }

        // True once no block that was running when Stamp was taken is still running.
        bool hasPassed( uint32_t Stamp ) const noexcept
        {
            return ( 0 == ( Stamp & 1u ) ) || ( counter.load() != Stamp );
        }

        // Control thread: returns true once no block that was running at the
        // time of the call is still running. False on timeout, or when called
        // from the audio thread itself (which would wait for itself).
        bool synchronize( std::chrono::milliseconds Timeout = std::chrono::milliseconds( 1000 ) ) const
        {
            const auto s = stamp();
            if( hasPassed( s ) )
                return true;
            if( isAudioThread() )
                return false;
            return waitUntil( [&] { return hasPassed( s ); }, Timeout );
        }

        // Control thread: returns true once a whole block has started and
        // finished after the call. False on timeout (also when the audio
        // device is not running).
        bool waitForBlock( std::chrono::milliseconds Timeout ) const
        {
            if( isAudioThread() )
                return false;
            const auto s      = stamp();
            const auto target = s + ( ( s & 1u ) ? 3u : 2u ); // next begin/end pair
            return waitUntil( [&] { return (int32_t)( counter.load() - target ) >= 0; }, Timeout );
        }

        // Control thread: take or give back exclusive access. While held, no
        // block runs. Returns false (and does not hold it) on timeout.
        bool acquireExclusive( std::chrono::milliseconds Timeout = std::chrono::milliseconds( 1000 ) )
        {
            if( exclusive.exchange( true ) )
                return false; // Not re-entrant, and only one holder at a time.
            if( synchronize( Timeout ) )
                return true;
            exclusive.store( false );
            return false;
        }

        void releaseExclusive() noexcept { exclusive.store( false ); }

    private:
        AudioThreadSync() = default;

        template<typename Condition>
        static bool waitUntil( Condition&& Done, std::chrono::milliseconds Timeout )
        {
            const auto deadline = std::chrono::steady_clock::now() + Timeout;
            for( int spins = 0; !Done(); ++spins )
            {
                if( std::chrono::steady_clock::now() >= deadline )
                    return Done();
                // The wait is normally well under a millisecond: yield first,
                // then back off to short sleeps.
                if( spins < 64 )
                    std::this_thread::yield();
                else
                    std::this_thread::sleep_for( std::chrono::microseconds( 200 ) );
            }
            return true;
        }

        std::atomic<uint32_t>        counter{ 0 };
        std::atomic<bool>            exclusive{ false };
        std::atomic<std::thread::id> audioThread{};
    };

    // Control thread: free something the audio thread may still be using in
    // the block running now (it has just been unpublished). If the audio
    // thread is stuck in that block, it is kept instead, deliberately:
    // freeing it could crash the audio thread.
    inline void releaseAfterBlock( std::shared_ptr<void> Garbage )
    {
        if( AudioThreadSync::get().synchronize() )
            return; // Freed here.
        static_cast<void>( new std::shared_ptr<void>( std::move( Garbage ) ) );
    }

    //
    // RtPublished: a snapshot that a control thread replaces and the audio
    // thread reads, without locks. The audio thread uses get() during a block;
    // the snapshot it got stays valid until the end of that block. Replaced
    // snapshots are freed by later publish() calls (or the destructor), on the
    // control thread, once the audio thread has moved past them.
    //
    template<typename T>
    class RtPublished
    {
    public:
        RtPublished() = default;
        ~RtPublished() = default;

        RtPublished( const RtPublished& )            = delete;
        RtPublished& operator=( const RtPublished& ) = delete;

        // Audio thread (or control thread).
        T* get() const noexcept { return live.load(); }

        // Control thread: the snapshot last published.
        T* current() const noexcept { return owned.get(); }

        // Control thread: make Next the live snapshot.
        void publish( std::unique_ptr<T> Next )
        {
            auto previous = std::move( owned );
            owned         = std::move( Next );
            live.store( owned.get() );

            collect();
            if( previous )
            {
                const auto s = AudioThreadSync::get().stamp();
                if( !AudioThreadSync::get().hasPassed( s ) )
                    retired.push_back( { s, std::move( previous ) } );
            }
        }

        // Control thread: free replaced snapshots the audio thread is done with.
        void collect()
        {
            auto& sync = AudioThreadSync::get();
            for( auto it = retired.begin(); it != retired.end(); )
            {
                if( sync.hasPassed( it->first ) )
                    it = retired.erase( it );
                else
                    ++it;
            }
        }

    private:
        std::atomic<T*>                                      live{ nullptr };
        std::unique_ptr<T>                                   owned;
        std::vector<std::pair<uint32_t, std::unique_ptr<T>>> retired;
    };
} // Namespace AudIO
