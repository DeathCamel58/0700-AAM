#include "mt_sample_player.h"

#define broadcastMessageLengthInMs 3
#define cuePreviewLengthInMs       30

namespace mt
{
    // Moves pos to where a loop goes on from when it reaches an end of
    // [first, last), or turns a bidirectional loop around. False when the
    // playback does not loop.
    static bool wrapPlayback( MTSamplePlayMode mode, bool looping, juce::int64 first, juce::int64 last, juce::int64& pos, bool& backwards )
    {
        if( !looping )
            return false;

        if( mode == MTSamplePlayMode::samplePlayModeLoopForward )
            pos = first;
        else if( mode == MTSamplePlayMode::samplePlayModeLoopBackward )
            pos = last;
        else
            backwards = !backwards;

        return true;
    }

    // Reads the file on a background thread, in chunks, where the player
    // will play next: in the direction it plays and across a loop's wrap.
    // The audio thread only copies chunks that have been read, so it never
    // waits for the disk, and the memory used does not grow with the
    // file's length.
    class MTSamplePlayer::ReadAhead : public juce::TimeSliceClient
    {
    public:
        static constexpr int numChunks = 12;
        static constexpr int chunkSize = 32768;

        // About 3.4 s at 48 kHz, read before it is played.
        static constexpr juce::int64 readAheadSamples = 5 * (juce::int64)chunkSize;

        explicit ReadAhead( std::unique_ptr<juce::AudioFormatReader> r )
            : reader( std::move( r ) )
            , numChannels( (int)reader->numChannels )
            , length( reader->lengthInSamples )
        {
            for( auto& c : chunks )
                c.samples.setSize( numChannels, chunkSize );
            setPlan( MTSamplePlayMode::samplePlayFromStart, false, 0, length, 0, false );
        }

        juce::int64 getLength() const { return length; }
        int         getNumChannels() const { return numChannels; }

        // Called on the message thread when the playback starts.
        void setPlan( MTSamplePlayMode mode, bool looping, juce::int64 first, juce::int64 last, juce::int64 pos, bool backwards )
        {
            {
                const juce::ScopedLock sl( planLock );
                plan = { mode, looping, first, last };
            }
            setPlayhead( pos, backwards );
        }

        // Called on the audio thread after each block.
        void setPlayhead( juce::int64 pos, bool backwards )
        {
            playhead         = pos;
            playingBackwards = backwards;
        }

        // Called on the audio thread. Copies [start, start + num) to dest,
        // leaving the parts outside the file silent. Returns false, with
        // that part of dest cleared, if some of it has not been read yet.
        bool read( juce::AudioBuffer<float>& dest, int destStart, int num, juce::int64 start )
        {
            const auto first = juce::jlimit( (juce::int64)0, length, start );
            const auto last  = juce::jlimit( (juce::int64)0, length, start + num );

            for( auto p = first; p < last; )
            {
                const auto index = p / chunkSize;
                const auto end   = juce::jmin( last, ( index + 1 ) * chunkSize );
                if( !copyFromChunk( index, dest, destStart + (int)( p - start ), (int)( p - index * chunkSize ), (int)( end - p ) ) )
                {
                    dest.clear( destStart, num );
                    return false;
                }
                p = end;
            }
            return true;
        }

        int useTimeSlice() override
        {
            const auto wanted = getWantedChunks();

            for( int i = 0; i < wanted.size(); ++i )
            {
                if( findChunk( wanted[i] ) != nullptr )
                    continue;

                for( auto& c : chunks )
                {
                    if( !wanted.contains( c.index.load() ) )
                    {
                        load( c, wanted[i] );
                        return 0;
                    }
                }
            }
            return 10;
        }

    private:
        struct Chunk
        {
            // Held by the audio thread while it copies, and by the reading
            // thread while it changes index; -1 while the chunk is read.
            juce::SpinLock           lock;
            std::atomic<juce::int64> index{ -1 };
            juce::AudioBuffer<float> samples;
        };

        struct Plan
        {
            MTSamplePlayMode mode    = MTSamplePlayMode::samplePlayFromStart;
            bool             looping = false;
            juce::int64      first   = 0;
            juce::int64      last    = 0;
        };

        Chunk* findChunk( juce::int64 index )
        {
            for( auto& c : chunks )
                if( c.index.load() == index )
                    return &c;
            return nullptr;
        }

        bool copyFromChunk( juce::int64 index, juce::AudioBuffer<float>& dest, int destStart, int offset, int count )
        {
            auto* c = findChunk( index );
            if( c == nullptr )
                return false;

            const juce::SpinLock::ScopedTryLockType tl( c->lock );
            if( !tl.isLocked() || c->index.load() != index )
                return false;

            // Mono files play on every channel, as AudioFormatReader::read() does.
            for( int ch = 0; ch < dest.getNumChannels(); ++ch )
                dest.copyFrom( ch, destStart, c->samples, juce::jmin( ch, numChannels - 1 ), offset, count );
            return true;
        }

        void load( Chunk& c, juce::int64 index )
        {
            {
                const juce::SpinLock::ScopedLockType sl( c.lock );
                c.index = -1;
            }

            const auto start = index * chunkSize;
            reader->read( &c.samples, 0, (int)juce::jmin( (juce::int64)chunkSize, length - start ), start, true, true );

            const juce::SpinLock::ScopedLockType sl( c.lock );
            c.index = index;
        }

        // The chunks the next readAheadSamples played come from, in the
        // order they are played, following the loop as the player does.
        juce::Array<juce::int64> getWantedChunks()
        {
            Plan p;
            {
                const juce::ScopedLock sl( planLock );
                p = plan;
            }
            auto pos       = juce::jlimit( p.first, juce::jmax( p.first, p.last ), playhead.load() );
            bool backwards = playingBackwards.load();

            juce::Array<juce::int64> wanted;
            const auto               add = [&]( juce::int64 from, juce::int64 to ) {
                for( auto i = from / chunkSize; i <= ( to - 1 ) / chunkSize && wanted.size() < numChunks; ++i )
                    wanted.addIfNotAlreadyThere( i );
            };

            // A short loop wraps many times within the samples read ahead.
            auto budget = readAheadSamples;
            for( int steps = 0; budget > 0 && wanted.size() < numChunks && steps < 64; ++steps )
            {
                const auto available = backwards ? pos - p.first : p.last - pos;
                if( available <= 0 )
                {
                    if( !wrapPlayback( p.mode, p.looping, p.first, p.last, pos, backwards ) )
                        break;
                    continue;
                }

                const auto step = juce::jmin( available, budget );
                if( backwards )
                {
                    add( pos - step, pos );
                    pos -= step;
                }
                else
                {
                    add( pos, pos + step );
                    pos += step;
                }
                budget -= step;
            }
            return wanted;
        }

        std::unique_ptr<juce::AudioFormatReader> reader;
        const int                                numChannels;
        const juce::int64                        length;
        Chunk                                    chunks[numChunks];

        juce::CriticalSection    planLock;
        Plan                     plan;
        std::atomic<juce::int64> playhead{ 0 };
        std::atomic<bool>        playingBackwards{ false };
    };

    MTSamplePlayer::MTSamplePlayer()
    {
        formatManager.registerBasicFormats();
        player.setSource( &transportSource );
        transportSource.addChangeListener( this );
        readThread.startThread();
    }

    MTSamplePlayer::~MTSamplePlayer()
    {
        stopTimer();
        transportSource.stop();
        devmgr.removeAudioCallback( &player );
        player.setSource( nullptr );
        transportSource.setSource( nullptr );
        readThread.removeAllClients();
        readThread.stopThread( 2000 );
    }

    void MTSamplePlayer::setSource( juce::File source )
    {
        transportSource.stop();

        // The audio thread no longer calls this source from here on.
        devmgr.removeAudioCallback( &player );
        transportSource.setSource( nullptr );

        // Waits for a read in progress to finish.
        readThread.removeAllClients();
        readAhead.reset();

        pos              = 0;
        length           = 0;
        sourceFile       = source;
        sourceSampleRate = 0.0;

        if( sourceFile == juce::File{} )
            return;

        // Only the header is read here; the samples are read by readThread.
        std::unique_ptr<juce::AudioFormatReader> reader( formatManager.createReaderFor( sourceFile ) );
        if( reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels <= 0 )
            return;

        sourceSampleRate        = reader->sampleRate;
        length                  = reader->lengthInSamples;
        broadcastElapsedSamples = static_cast<int>( ( sourceSampleRate * broadcastMessageLengthInMs ) / 60 );
        readAhead               = std::make_unique<ReadAhead>( std::move( reader ) );
        readThread.addTimeSliceClient( readAhead.get() );

        // The transport resamples from the file's rate to the device's.
        transportSource.setSource( this, 0, nullptr, sourceSampleRate, juce::jmax( 2, readAhead->getNumChannels() ) );
        devmgr.addAudioCallback( &player );
    }

    void MTSamplePlayer::start()
    {
        region = MTSampleRegion();
        start( region, MTSamplePlayMode::samplePlayFromStart );
    }

    void MTSamplePlayer::stop()
    {
        transportSource.stop();
    }

    bool MTSamplePlayer::isPlaying()
    {
        return transportSource.isPlaying();
    }

    void MTSamplePlayer::start( const MTSampleRegion& r, MTSamplePlayMode pm )
    {
        playingBackwords = false;
        transportSource.stop();

        if( readAhead == nullptr )
            return;
        const juce::int64 numSamples = readAhead->getLength();

        // Drops what the resampler kept of the last playback.
        transportSource.setNextReadPosition( 0 );
        finished        = false;
        positionChanged = false;

        region   = r;
        playMode = pm;

        // The part of the file that is played, [first, last).
        juce::int64 first = 0;
        juce::int64 last  = numSamples;
        if( region.isValid() )
        {
            if( playMode == MTSamplePlayMode::samplePlayToStart )
            {
                last = region.start;
            }
            else if( playMode == MTSamplePlayMode::samplePlayToEnd )
            {
                last = region.end;
            }
            else if( playMode == MTSamplePlayMode::samplePlayFromEnd )
            {
                first = region.end;
            }
            else if( playMode == MTSamplePlayMode::samplePlayModeCue )
            {
                const auto previewOffSet = static_cast<juce::int64>( ( sourceSampleRate * cuePreviewLengthInMs ) / 60 );
                first                    = region.start - previewOffSet;
                last                     = region.start + previewOffSet;
            }
            else
            {
                first = region.start;
                last  = region.end;
            }
        }

        // A region reaching past the file's end would leave a loop playing
        // silence there, or never reach its end.
        regionStart = juce::jlimit( (juce::int64)0, numSamples, first );
        regionEnd   = juce::jlimit( regionStart, numSamples, last );
        length      = regionEnd;

        playingBackwords = ( playMode == MTSamplePlayMode::samplePlayModeLoopBackward );
        pos              = playingBackwords ? regionEnd : regionStart;

        // An empty region is not looped, so its playback ends at once.
        looping = regionStart < regionEnd
                  && ( playMode == MTSamplePlayMode::samplePlayModeLoopForward || playMode == MTSamplePlayMode::samplePlayModeLoopBackward
                       || playMode == MTSamplePlayMode::samplePlayModeLoopBidirectional );

        // Until the first chunk is read, the audio thread plays silence
        // without moving on, so nothing is skipped.
        readAhead->setPlan( playMode, looping, regionStart, regionEnd, pos, playingBackwords );
        readThread.moveToFrontOfQueue( readAhead.get() );

        transportSource.start();
        startTimerHz( 30 );
    }

    juce::int64 MTSamplePlayer::getNextReadPosition() const
    {
        // Never past the end: the transport would stop itself on the audio
        // thread and send its change message from there. The timer stops
        // it instead.
        return pos;
    }

    juce::int64 MTSamplePlayer::getTotalLength() const
    {
        return length;
    }

    bool MTSamplePlayer::isLooping() const
    {
        return looping;
    }

    void MTSamplePlayer::prepareToPlay( int /*samplesPerBlockExpected*/, double /*sr*/ ) {}

    void MTSamplePlayer::releaseResources() {}

    void MTSamplePlayer::getNextAudioBlock( const juce::AudioSourceChannelInfo& bufferToFill )
    {
        bufferToFill.clearActiveBufferRegion();
        if( readAhead == nullptr )
            return;

        // Plays [regionStart, regionEnd). Forwards, pos is the first sample
        // played next; backwards, it is the end of what is played next, so
        // a block reads [pos - n, pos) and reverses it. A loop wraps within
        // the block instead of leaving the rest of it silent.
        int done = 0;
        while( done < bufferToFill.numSamples )
        {
            const auto wanted    = (juce::int64)( bufferToFill.numSamples - done );
            const auto destStart = bufferToFill.startSample + done;
            int        count     = 0;

            if( playingBackwords )
            {
                count = (int)juce::jmin( wanted, pos - regionStart );
                if( count > 0 )
                {
                    if( !readAhead->read( *bufferToFill.buffer, destStart, count, pos - count ) )
                        break;
                    bufferToFill.buffer->reverse( destStart, count );
                    pos -= count;
                }
            }
            else
            {
                count = (int)juce::jmin( wanted, regionEnd - pos );
                if( count > 0 )
                {
                    if( !readAhead->read( *bufferToFill.buffer, destStart, count, pos ) )
                        break;
                    pos += count;
                }
            }

            if( count > 0 )
                done += count;
            else if( !wrapPlayback( playMode, looping, regionStart, regionEnd, pos, playingBackwords ) )
                break;
        }

        readAhead->setPlayhead( pos, playingBackwords );

        const bool atEnd = playingBackwords ? pos <= regionStart : pos >= regionEnd;
        if( !looping && atEnd )
            finished = true;

        // send playheade position message
        numSamplesElapsed += done;
        if( done > 0 && ( atEnd || numSamplesElapsed >= broadcastElapsedSamples ) )
        {
            numSamplesElapsed = 0;
            positionChanged   = true;
        }
    }

    double MTSamplePlayer::getSampleRate()
    {
        return sourceSampleRate;
    }

    void MTSamplePlayer::timerCallback()
    {
        // Stopping the transport sends its change message, which reaches
        // changeListenerCallback() and tells the listeners it stopped.
        if( finished.exchange( false ) )
        {
            positionChanged = false;
            stopTimer();
            transportSource.stop();
            return;
        }

        if( positionChanged.exchange( false ) )
            sendChangeMessage();

        if( !transportSource.isPlaying() )
            stopTimer();
    }

    void MTSamplePlayer::changeListenerCallback( ChangeBroadcaster* source )
    {
        // send playback stopped message
        if( source == &transportSource )
            sendChangeMessage();
    }
}
