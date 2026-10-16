#include "mt_sample_player.h"

#define broadcastMessageLengthInMs 3
#define cuePreviewLengthInMs       30

namespace mt
{
    MTSamplePlayer::MTSamplePlayer()
    {
        formatManager.registerBasicFormats();
        player.setSource( &transportSource );
        transportSource.addChangeListener( this );
    }

    MTSamplePlayer::~MTSamplePlayer()
    {
        stopTimer();
        transportSource.stop();
        devmgr.removeAudioCallback( &player );
        player.setSource( nullptr );
        transportSource.setSource( nullptr );
    }

    void MTSamplePlayer::setSource( juce::File source )
    {
        transportSource.stop();

        // The audio thread no longer calls this source from here on.
        devmgr.removeAudioCallback( &player );
        transportSource.setSource( nullptr );

        pos              = 0;
        length           = 0;
        sourceFile       = source;
        sourceSampleRate = 0.0;
        samples.setSize( 0, 0 );

        if( sourceFile == juce::File{} )
            return;

        std::unique_ptr<juce::AudioFormatReader> reader( formatManager.createReaderFor( sourceFile ) );
        if( reader == nullptr || reader->lengthInSamples <= 0 || reader->lengthInSamples > std::numeric_limits<int>::max() )
            return;

        samples.setSize( (int)reader->numChannels, (int)reader->lengthInSamples );
        reader->read( &samples, 0, (int)reader->lengthInSamples, 0, true, true );
        sourceSampleRate        = reader->sampleRate;
        length                  = reader->lengthInSamples;
        broadcastElapsedSamples = static_cast<int>( ( sourceSampleRate * broadcastMessageLengthInMs ) / 60 );

        // The transport resamples from the file's rate to the device's.
        transportSource.setSource( this, 0, nullptr, sourceSampleRate, juce::jmax( 2, samples.getNumChannels() ) );
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

        const juce::int64 numSamples = samples.getNumSamples();
        if( numSamples <= 0 )
            return;

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

    void MTSamplePlayer::readSamples( juce::AudioBuffer<float>& Dest, int DestStart, int NumSamples, juce::int64 Start ) const
    {
        // Mono files play on every channel, as AudioFormatReader::read() did.
        const auto numChannels = samples.getNumChannels();
        const auto first       = juce::jlimit( (juce::int64)0, (juce::int64)samples.getNumSamples(), Start );
        const auto last        = juce::jlimit( (juce::int64)0, (juce::int64)samples.getNumSamples(), Start + NumSamples );
        const auto offset      = (int)( first - Start );
        const auto count       = (int)( last - first );

        if( numChannels <= 0 || count <= 0 )
            return;

        for( int ch = 0; ch < Dest.getNumChannels(); ++ch )
            Dest.copyFrom( ch, DestStart + offset, samples, juce::jmin( ch, numChannels - 1 ), (int)first, count );
    }

    bool MTSamplePlayer::turnAround()
    {
        if( !looping )
            return false;

        if( playMode == MTSamplePlayMode::samplePlayModeLoopForward )
            pos = regionStart;
        else if( playMode == MTSamplePlayMode::samplePlayModeLoopBackward )
            pos = regionEnd;
        else
            playingBackwords = !playingBackwords;

        return true;
    }

    void MTSamplePlayer::getNextAudioBlock( const juce::AudioSourceChannelInfo& bufferToFill )
    {
        bufferToFill.clearActiveBufferRegion();

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
                    readSamples( *bufferToFill.buffer, destStart, count, pos - count );
                    bufferToFill.buffer->reverse( destStart, count );
                    pos -= count;
                }
            }
            else
            {
                count = (int)juce::jmin( wanted, regionEnd - pos );
                if( count > 0 )
                {
                    readSamples( *bufferToFill.buffer, destStart, count, pos );
                    pos += count;
                }
            }

            if( count > 0 )
                done += count;
            else if( !turnAround() )
                break;
        }

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
