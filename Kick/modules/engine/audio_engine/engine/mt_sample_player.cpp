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

        if( samples.getNumSamples() <= 0 )
            return;

        // Drops what the resampler kept of the last playback.
        transportSource.setNextReadPosition( 0 );
        finished        = false;
        positionChanged = false;

        region      = r;
        playMode    = pm;
        regionStart = region.start;
        regionEnd   = region.end;

        if( region.isValid() )
        {
            if( playMode == MTSamplePlayMode::samplePlayFromStart || playMode == MTSamplePlayMode::samplePlayModeLoopForward
                || playMode == MTSamplePlayMode::samplePlayModeLoopBidirectional )
            {
                pos    = region.start;
                length = region.end;
            }
            else if( playMode == MTSamplePlayMode::samplePlayToStart )
            {
                pos    = 0;
                length = region.start;
            }
            else if( playMode == MTSamplePlayMode::samplePlayToEnd )
            {
                pos    = 0;
                length = region.end;
            }
            else if( playMode == MTSamplePlayMode::samplePlayFromEnd )
            {
                pos    = region.end;
                length = samples.getNumSamples();
            }
            else if( playMode == MTSamplePlayMode::samplePlayModeLoopBackward )
            {
                playingBackwords = true;
                length           = region.end;
                pos              = length - samplesPerBlock;
            }
            else if( playMode == MTSamplePlayMode::samplePlayModeCue )
            {
                int previewOffSet = static_cast<int>( ( sourceSampleRate * cuePreviewLengthInMs ) / 60 );
                pos               = region.getStart();
                length            = pos + previewOffSet;
                pos               = pos - previewOffSet;
            }
            length = juce::jmin( length, (juce::int64)samples.getNumSamples() );
        }
        else
        {
            pos    = 0;
            length = samples.getNumSamples();
        }

        //length = juce::jmin( length, reader->lengthInSamples );

        looping = ( playMode == MTSamplePlayMode::samplePlayModeLoopForward || playMode == MTSamplePlayMode::samplePlayModeLoopBackward || playMode == MTSamplePlayMode::samplePlayModeLoopBidirectional );

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

    void MTSamplePlayer::prepareToPlay( int samplesPerBlockExpected, double /*sr*/ )
    {
        samplesPerBlock = samplesPerBlockExpected;
    }

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

    void MTSamplePlayer::getNextAudioBlock( const juce::AudioSourceChannelInfo& bufferToFill )
    {
        bufferToFill.clearActiveBufferRegion();
        int sampleCopySize = 0;
        if( playingBackwords )
        {
            sampleCopySize = ( pos - regionStart < bufferToFill.numSamples ) ? static_cast<int>( pos - regionStart ) : bufferToFill.numSamples;
            sampleCopySize = juce::jmax( 0, sampleCopySize );
            readSamples( *bufferToFill.buffer, bufferToFill.startSample, sampleCopySize, pos );
            bufferToFill.buffer->reverse( bufferToFill.startSample, sampleCopySize );
            pos -= sampleCopySize;
        }
        else
        {
            sampleCopySize = ( length - pos < bufferToFill.numSamples ) ? static_cast<int>( length - pos ) : bufferToFill.numSamples;
            sampleCopySize = juce::jmax( 0, sampleCopySize );
            readSamples( *bufferToFill.buffer, bufferToFill.startSample, sampleCopySize, pos );
            pos += sampleCopySize;
        }

        // send playheade position message
        numSamplesElapsed += sampleCopySize;
        if( pos == length || numSamplesElapsed >= broadcastElapsedSamples )
        {
            numSamplesElapsed = 0;
            positionChanged   = true;
        }

        if( !looping && ( playingBackwords ? pos <= regionStart : pos >= length ) )
            finished = true;

        // reset pos if playmode is loop
        if( looping && ( pos >= regionEnd || pos <= regionStart ) )
        {
            if( playMode == MTSamplePlayMode::samplePlayModeLoopForward )
            {
                pos = regionStart;
            }
            else if( playMode == MTSamplePlayMode::samplePlayModeLoopBackward )
            {
                pos = regionEnd;
            }
            else if( playMode == MTSamplePlayMode::samplePlayModeLoopBidirectional )
            {
                if( pos <= regionStart )
                {
                    playingBackwords = false;
                }
                else
                {
                    pos              = length - samplesPerBlock;
                    playingBackwords = true;
                }
            }
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
