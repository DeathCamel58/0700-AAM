/*
 * Copyright (c) 2022 MUSIC Tribe
 *
 * This file is part of Kick.
 *
 * Kick is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Kick is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with Kick. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt
{
    ///
    /// Class: MIDIDevices definitions
    ///

    ///
    /// Static member definitions...
    ///
    std::vector<juce::String>                        MTMidiDeviceManager::midiInPorts{ "None" };
    std::vector<juce::String>                        MTMidiDeviceManager::midiOutPorts{ "None" };
    juce::ReferenceCountedArray<MidiDeviceListEntry> MTMidiDeviceManager::midiInputs;
    juce::ReferenceCountedArray<MidiDeviceListEntry> MTMidiDeviceManager::midiOutputs;

    ///
    /// Implementation
    ///
    MTMidiDeviceManager::MTMidiDeviceManager()
    {
        midiInPorts.reserve( 8 );
        midiOutPorts.reserve( 8 );

        createVirtualPorts();
        refreshMIDIDeviceLists();

        // Called on the message thread whenever a port comes or goes.
        deviceListConnection = juce::MidiDeviceListConnection::make( [this]() { refreshMIDIDeviceLists(); } );
    }

    MTMidiDeviceManager::~MTMidiDeviceManager()
    {
        deviceListConnection.reset();
        cancelPendingUpdate();

        juce::ReferenceCountedArray<MidiDeviceListEntry> outputs;
        {
            const juce::ScopedLock sl( outputLock );
            outputs.swapWith( midiOutputs );
        }

        for( auto mDev : midiInputs )
            closeMidiDevice( mDev );
        for( auto mDev : outputs )
            closeMidiDevice( mDev );

        midiInputs.clear();
        virtualInputs.clear();
        virtualOutputs.clear();
    }

    void MTMidiDeviceManager::createVirtualPorts()
    {
#if JUCE_LINUX || JUCE_BSD || JUCE_MAC || JUCE_IOS
        // The ports live as long as their MidiInput and MidiOutput, which
        // their list entries own.
        for( auto i = 0; i < 8; ++i )
        {
            if( auto port = juce::MidiOutput::createNewDevice( "Kick:Out:" + juce::String( i + 1 ) ) )
            {
                MidiDeviceListEntry::Ptr entry = new MidiDeviceListEntry( port->getDeviceInfo() );
                entry->outDevice               = std::move( port );
                virtualOutputs.add( entry );
            }
        }

        if( auto port = juce::MidiInput::createNewDevice( "Kick:In", this ) )
        {
            MidiDeviceListEntry::Ptr entry = new MidiDeviceListEntry( port->getDeviceInfo() );
            entry->isInput                 = true;
            entry->inDevice                = std::move( port );
            entry->inDevice->start();
            virtualInputs.add( entry );
        }
#endif
    }

    int                              MTMidiDeviceManager::getNumMidiInputDevices() noexcept { return midiInputs.size(); }
    int                              MTMidiDeviceManager::getNumMidiOutputDevices() noexcept { return midiOutputs.size(); }
    const std::vector<juce::String>& MTMidiDeviceManager::getMidiInputPortNames() noexcept { return midiInPorts; }
    const std::vector<juce::String>& MTMidiDeviceManager::getMidiOutputPortNames() noexcept { return midiOutPorts; }

    juce::ReferenceCountedObjectPtr<MidiDeviceListEntry> MTMidiDeviceManager::getMidiDevice( int index, bool isInput ) noexcept
    {
        return isInput ? midiInputs[index] : midiOutputs[index];
    }
    const juce::ReferenceCountedArray<MidiDeviceListEntry>& MTMidiDeviceManager::getMidiDeviceList( bool isInput ) noexcept
    {
        return isInput ? midiInputs : midiOutputs;
    }
    const std::vector<juce::String>& MTMidiDeviceManager::getMidiDeviceNames( bool isInput ) noexcept
    {
        return isInput ? midiInPorts : midiOutPorts;
    }

    void MTMidiDeviceManager::dumpMidiInfo( bool isInputDevice )
    {
        juce::String str;
        int          i = 0;

        str += "\n=======================================================\n";
        str += juce::Time::getCurrentTime().toString( true, true ) + "\n\n";
        if( isInputDevice )
        {
            str += "Midi Inputs: \n";
            for( auto in : midiInputs )
                str += juce::String( ++i ) + ": " + in->deviceInfo.name + " (" + in->deviceInfo.identifier + ") \n";
        }
        else
        {
            str += "Midi Outputs: \n";
            for( auto out : midiOutputs )
                str += juce::String( ++i ) + ": " + out->deviceInfo.name + " (" + out->deviceInfo.identifier + ") \n";
        }
        str += "\n=======================================================\n\n";

        DBG( str );
    }

    void MTMidiDeviceManager::refreshMIDIDeviceLists()
    {
        updateMidiDeviceList( true ); // Input devices...
        updateMidiDeviceList( false ); // Output devices...
    }

    ///
    ///
    bool MTMidiDeviceManager::hasMidiDeviceListChanged( const juce::Array<juce::MidiDeviceInfo>& availableDevices, bool isInputDevice )
    {
        // The system's ports follow Kick's own in the list.
        const juce::ReferenceCountedArray<MidiDeviceListEntry>& midiDevices = isInputDevice ? midiInputs : midiOutputs;
        const auto                                               first       = ( isInputDevice ? virtualInputs : virtualOutputs ).size();

        if( availableDevices.size() != midiDevices.size() - first )
            return true;

        for( auto i = 0; i < availableDevices.size(); ++i )
            if( availableDevices[i] != midiDevices[first + i]->deviceInfo )
                return true;

        return false;
    }

    ///
    ///
    void MTMidiDeviceManager::updateMidiDeviceList( bool isInputDevice )
    {
        auto availableDevices = isInputDevice ? juce::MidiInput::getAvailableDevices() : juce::MidiOutput::getAvailableDevices();

        // Kick's virtual ports are listed as ports of the other direction
        // (others read from Kick's outputs); opening them would loop Kick's
        // own MIDI back to it.
        for( auto& port : isInputDevice ? virtualOutputs : virtualInputs )
            availableDevices.removeAllInstancesOf( port->deviceInfo );

        if( hasMidiDeviceListChanged( availableDevices, isInputDevice ) )
        {
            juce::ReferenceCountedArray<MidiDeviceListEntry>& midiDevices     = isInputDevice ? midiInputs : midiOutputs;
            auto&                                             midiDeviceNames = isInputDevice ? midiInPorts : midiOutPorts;

            juce::ReferenceCountedArray<MidiDeviceListEntry> newDeviceList;
            std::vector<juce::String>                        newPortNames;

            newPortNames.push_back( "None" );

            for( auto& entry : isInputDevice ? virtualInputs : virtualOutputs )
            {
                newDeviceList.add( entry );
                newPortNames.push_back( entry->deviceInfo.name );
            }

            // add all currently plugged-in devices to the device list
            for( auto& newDevice : availableDevices )
            {
                MidiDeviceListEntry::Ptr entry = findMidiDevice( newDevice, isInputDevice );

                if( entry == nullptr )
                {
                    entry          = new MidiDeviceListEntry( newDevice );
                    entry->isInput = isInputDevice;
                    openMidiDevice( entry );
                }

                newDeviceList.add( entry );
                newPortNames.push_back( entry->deviceInfo.name );
            }

            juce::ReferenceCountedArray<MidiDeviceListEntry> unplugged;
            for( auto& entry : midiDevices )
                if( !newDeviceList.contains( entry ) )
                    unplugged.add( entry );

            // Actually update the device list
            if( isInputDevice )
            {
                midiDevices = newDeviceList;
            }
            else
            {
                const juce::ScopedLock sl( outputLock );
                midiDevices = newDeviceList;
            }

            // No sender can reach the unplugged devices any more.
            for( auto& entry : unplugged )
                closeMidiDevice( entry );

            // Update port names...
            midiDeviceNames = newPortNames;
            // Display information...
            dumpMidiInfo( isInputDevice );

            // Emit MIDI device changed signal
            if( isInputDevice )
                signals::midiInputsChanged();
        }
    }

    juce::ReferenceCountedObjectPtr<MidiDeviceListEntry> MTMidiDeviceManager::findMidiDevice( juce::MidiDeviceInfo device, bool isInputDevice ) const
    {
        const juce::ReferenceCountedArray<MidiDeviceListEntry>& midiDevices = isInputDevice ? midiInputs : midiOutputs;
        for( auto& d : midiDevices )
            if( d->deviceInfo == device )
                return d;

        return nullptr;
    }

    juce::ReferenceCountedObjectPtr<MidiDeviceListEntry> MTMidiDeviceManager::findMidiDevice( const juce::String& device, bool isInputDevice ) const
    {
        const juce::ReferenceCountedArray<MidiDeviceListEntry>& midiDevices = isInputDevice ? midiInputs : midiOutputs;
        for( auto& d : midiDevices )
            if( d->deviceInfo.identifier == device )
                return d;

        for( auto& d : midiDevices )
            if( d->deviceInfo.name == device )
                return d;

        return nullptr;
    }

    void MTMidiDeviceManager::openMidiDevice( MidiDeviceListEntry::Ptr entry )
    {
        if( entry )
        {
            if( entry->isInput && nullptr == entry->inDevice.get() )
            {
                entry->inDevice = juce::MidiInput::openDevice( entry->deviceInfo.identifier, this );
                if( entry->inDevice.get() == nullptr )
                {
                    DBG( "MidiDemo::openDevice: open input device [" << entry->deviceInfo.name << "] failed!" );
                    return;
                }
                entry->inDevice->start();
            }
            else if( !entry->isInput && nullptr == entry->outDevice.get() )
            {
                auto device = juce::MidiOutput::openDevice( entry->deviceInfo.identifier );
                if( device.get() == nullptr )
                {
                    DBG( "MidiDemo::openDevice: open output device [" << entry->deviceInfo.name << "] failed!" );
                    return;
                }
                device->startBackgroundThread();

                const juce::ScopedLock sl( outputLock );
                entry->outDevice = std::move( device );
            }
        }
    }

    void MTMidiDeviceManager::closeMidiDevice( MidiDeviceListEntry::Ptr entry )
    {
        if( entry )
        {
            if( entry->isInput && nullptr != entry->inDevice.get() )
            {
                entry->inDevice->stop();
                entry->inDevice.reset();
            }
            else if( !entry->isInput && nullptr != entry->outDevice.get() )
            {
                std::unique_ptr<juce::MidiOutput> device;
                {
                    const juce::ScopedLock sl( outputLock );
                    std::swap( device, entry->outDevice );
                }
                device->stopBackgroundThread();
            }
        }
    }

    void MTMidiDeviceManager::openMidiDevice( bool isInput, int index )
    {
        if( isInput && ( index < midiInputs.size() ) )
        {
            openMidiDevice( midiInputs[index] );
        }
        else if( !isInput && ( index < midiOutputs.size() ) )
        {
            openMidiDevice( midiOutputs[index] );
        }
    }

    void MTMidiDeviceManager::closeMidiDevice( bool isInput, int index )
    {
        if( isInput && ( index < midiInputs.size() ) )
        {
            closeMidiDevice( midiInputs[index] );
        }
        else if( !isInput && ( index < midiOutputs.size() ) )
        {
            closeMidiDevice( midiOutputs[index] );
        }
    }

    void MTMidiDeviceManager::handleIncomingMidiMessage( juce::MidiInput* source, const juce::MidiMessage& message )
    {
        // This is called on the MIDI thread
        if( midiInputFifoEnabled && message.getRawDataSize() <= 3 )
        {
            const auto scope = midiInputFifo.write( 1 );
            if( scope.blockSize1 > 0 )
            {
                auto& event     = midiInputEvents[(size_t)scope.startIndex1];
                event.source    = source;
                event.timeStamp = message.getTimeStamp();
                event.size      = message.getRawDataSize();
                std::copy_n( message.getRawData(), event.size, event.data );
            }
        }

        const juce::ScopedLock sl( midiMonitorLock );
        MTMidiMessage          msg;
        msg.source  = source;
        msg.message = message;
        incomingMessages.add( msg );
        triggerAsyncUpdate();
    }

    void MTMidiDeviceManager::handleAsyncUpdate()
    {
        // This is called on the message loop
        {
            const juce::ScopedLock sl( midiMonitorLock );
            midiMessages.swapWith( incomingMessages );
        }

        onMidiMessages( midiMessages );
        for( auto& m : midiMessages )
        {
            onMidiMessage( m );
        }

        //midiMonitor.insertTextAtCaret( messageText );
    }

    bool MTMidiDeviceManager::popMidiInputEvent( MidiInputEvent& Event ) noexcept
    {
        const auto scope = midiInputFifo.read( 1 );
        if( scope.blockSize1 <= 0 )
            return false;

        Event = midiInputEvents[(size_t)scope.startIndex1];
        return true;
    }

    void MTMidiDeviceManager::sendToOutputs( const juce::MidiMessage& Msg )
    {
        const juce::ScopedLock sl( outputLock );

        for( auto midiOutput : midiOutputs )
            if( midiOutput->outDevice.get() != nullptr )
                midiOutput->outDevice->sendMessageNow( Msg );
    }

    void MTMidiDeviceManager::sendToOutput( juce::MidiDeviceInfo Device, const juce::MidiMessage& Msg )
    {
        const juce::ScopedLock sl( outputLock );

        auto midiOutput = findMidiDevice( Device, false );
        if( midiOutput )
            if( midiOutput->outDevice.get() != nullptr )
                midiOutput->outDevice->sendMessageNow( Msg );
    }

    void MTMidiDeviceManager::sendToOutput( const juce::String& Device, const juce::MidiMessage& Msg )
    {
        const juce::ScopedLock sl( outputLock );

        auto midiOutput = findMidiDevice( Device, false );
        if( midiOutput )
            if( midiOutput->outDevice.get() != nullptr )
                midiOutput->outDevice->sendMessageNow( Msg );
    }

}
