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

#pragma once

namespace mt
{
    ///
    /// MTMidiDeviceManager
    ///
    /// Opens every MIDI port of the system, and Kick's own virtual ports:
    /// eight outputs other programs can read from and an input they can
    /// write to. The lists are updated when the system's ports change.
    ///
    /// The output list is changed on the message thread and read by the
    /// threads that send (the sequencer's), so both take outputLock.
    ///
    class MTMidiDeviceManager
        : private juce::MidiInputCallback
        , private juce::AsyncUpdater
    {
    public:
        std::function<void( const MTMidiMessage& )>        onMidiMessage  = []( const MTMidiMessage& ) {};
        std::function<void( juce::Array<MTMidiMessage>& )> onMidiMessages = []( juce::Array<MTMidiMessage>& ) {};

        MTMidiDeviceManager();
        ~MTMidiDeviceManager();

        void dumpMidiInfo( bool isInputDevice );

        ///
        /// STATIC functions
        ///
        // MIDI Device related API's...
        static int                              getNumMidiInputDevices() noexcept;
        static int                              getNumMidiOutputDevices() noexcept;
        static const std::vector<juce::String>& getMidiInputPortNames() noexcept;
        static const std::vector<juce::String>& getMidiOutputPortNames() noexcept;

        static juce::ReferenceCountedObjectPtr<MidiDeviceListEntry>    getMidiDevice( int index, bool isInput ) noexcept;
        static const juce::ReferenceCountedArray<MidiDeviceListEntry>& getMidiDeviceList( bool isInput ) noexcept;
        static const std::vector<juce::String>&                        getMidiDeviceNames( bool isInput ) noexcept;

        ///
        /// Public functions
        ///
        void openMidiDevice( MidiDeviceListEntry::Ptr entry );
        void closeMidiDevice( MidiDeviceListEntry::Ptr entry );
        void openMidiDevice( bool isInput, int index );
        void closeMidiDevice( bool isInput, int index );

        // Send MIDI message; from any thread. A device is named by its
        // identifier or, as projects store it, by its name.
        void sendToOutputs( const juce::MidiMessage& msg );
        void sendToOutput( juce::MidiDeviceInfo device, const juce::MidiMessage& msg );
        void sendToOutput( const juce::String& Device, const juce::MidiMessage& Msg );

        // Incoming short messages for the audio thread, without a trip
        // through the message thread. Off until a reader enables it; the
        // MIDI thread then also queues every message of up to three bytes
        // (no SysEx) in a fixed lock-free FIFO, and drops them when it is
        // full. timeStamp is JUCE's, in seconds on the
        // Time::getMillisecondCounter() clock.
        struct MidiInputEvent
        {
            juce::MidiInput* source{ nullptr };
            double           timeStamp{ 0 };
            int              size{ 0 };
            juce::uint8      data[3]{};
        };

        void setMidiInputFifoEnabled( bool Enabled ) noexcept { midiInputFifoEnabled = Enabled; }
        bool popMidiInputEvent( MidiInputEvent& Event ) noexcept;

        /// MIDI device list updater...
        void refreshMIDIDeviceLists();

    private:
        // MIDI devices...
        static std::vector<juce::String>                        midiInPorts;
        static std::vector<juce::String>                        midiOutPorts;
        static juce::ReferenceCountedArray<MidiDeviceListEntry> midiInputs;
        static juce::ReferenceCountedArray<MidiDeviceListEntry> midiOutputs;

        juce::CriticalSection      midiMonitorLock;
        juce::Array<MTMidiMessage> incomingMessages;

        // Taken to change midiOutputs and to send to its devices.
        juce::CriticalSection outputLock;

        // Kick's virtual ports; they lead midiInputs and midiOutputs.
        juce::ReferenceCountedArray<MidiDeviceListEntry> virtualInputs;
        juce::ReferenceCountedArray<MidiDeviceListEntry> virtualOutputs;

        juce::MidiDeviceListConnection deviceListConnection;

        static constexpr int                         midiInputFifoSize = 1024;
        std::atomic<bool>                            midiInputFifoEnabled{ false };
        juce::AbstractFifo                           midiInputFifo{ midiInputFifoSize };
        std::array<MidiInputEvent, midiInputFifoSize> midiInputEvents;

        void createVirtualPorts();

        juce::ReferenceCountedObjectPtr<MidiDeviceListEntry> findMidiDevice( juce::MidiDeviceInfo device, bool isInputDevice ) const;
        juce::ReferenceCountedObjectPtr<MidiDeviceListEntry> findMidiDevice( const juce::String& device, bool isInputDevice ) const;

        bool hasMidiDeviceListChanged( const juce::Array<juce::MidiDeviceInfo>& availableDevices, bool isInputDevice );
        void updateMidiDeviceList( bool isInputDevice );
        void handleIncomingMidiMessage( juce::MidiInput* source, const juce::MidiMessage& message ) override;
        void handleAsyncUpdate() override;

    private:
        juce::Array<MTMidiMessage> midiMessages;
    };

}
