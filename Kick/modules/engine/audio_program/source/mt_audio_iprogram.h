/*
 * Copyright (c) 2021 MUSIC Tribe
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

#include <bitset>

#include "mt_midi_meter.h"
#include "mt_audio_iprogram_atomics.h"
#include "contracts/dcl/dcl_contracts.h"

namespace mt
{
    class ISynthObject;

    class MTIProgram;
    static sigslot::signal<MTIProgram*> programNameChanged;
    static sigslot::signal<MTIProgram*> programBeingDeleted;

    const std::array<int, 128> defaultDrumColours = { contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::PadColourOrange1, contracts::DeviceControlContracts::PadColourOrange, contracts::DeviceControlContracts::PadColourPink,
                                                      contracts::DeviceControlContracts::PadColourNeutralWhite, contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::padColourDarkRed, contracts::DeviceControlContracts::PadColourOrange,
                                                      contracts::DeviceControlContracts::PadColourPink, contracts::DeviceControlContracts::padColourlightGreen4, contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::PadColourLightSkyBlue,
                                                      contracts::DeviceControlContracts::PadColourPink2, contracts::DeviceControlContracts::padColourlightGreen4, contracts::DeviceControlContracts::PadColourOrange, contracts::DeviceControlContracts::PadColourRed };
    const std::array<int, 128> defaultClipColours = { contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::PadColourOrange1, contracts::DeviceControlContracts::PadColourOrange, contracts::DeviceControlContracts::PadColourPink,
                                                      contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::PadColourOrange1, contracts::DeviceControlContracts::PadColourOrange, contracts::DeviceControlContracts::PadColourPink,
                                                      contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::PadColourOrange1, contracts::DeviceControlContracts::PadColourOrange, contracts::DeviceControlContracts::PadColourPink,
                                                      contracts::DeviceControlContracts::PadColourRed, contracts::DeviceControlContracts::PadColourOrange1, contracts::DeviceControlContracts::PadColourOrange, contracts::DeviceControlContracts::PadColourPink };
    const std::array<int, 128> defaultPadColours{ contracts::DeviceControlContracts::PadColourRed };

    struct IMidiMsgProcessor
    {
        // Pure virtual functions to be implemented by derived classes...
        virtual void noteProcess( juce::Array<juce::MidiMessage>& Msg ) = 0;

        virtual void noteOn( int Note, int Velocity ) { return; };
        virtual void noteOff( int Note ) { return; };
        virtual void noteOff( std::vector<int>& Notes ) { return; };
        virtual void noteOffAll() { return; };
        virtual void noteMute( int Note, bool Mute ) { return; };
        virtual void noteReset( int Note ) { return; };
        virtual void noteReset( std::vector<int>& Notes ) { return; };
        virtual void noteLoop( int Note, bool Loop ) { return; };
        virtual void modWheel( int Value ) { return; };
    };

    class MTIProgram
        : public MTIProgramData
        , public IMidiMsgProcessor
        , public MTIInsertsInterface
        , public juce::ValueTree::Listener
    {
    public:
        explicit MTIProgram( juce::ValueTree& VT, juce::UndoManager* UM = nullptr );
        virtual ~MTIProgram();

        ///
        /// What playing a note reads, for the threads that play notes (the
        /// sequencer's tick thread, MIDI input, the pads). The samples, their
        /// layers and the map that holds them belong to the message thread,
        /// which adds, removes and edits them while notes play; so the
        /// program publishes an immutable copy of what note handling needs,
        /// and replaces it whenever that changes.
        ///
        struct PlayableLayer
        {
            bool                         hasSource{ false };
            std::shared_ptr<ASoundSlice> slice; // Kept alive while the copy is.
            int                          velStart{ MIDI::MIN };
            int                          velEnd{ MIDI::MAX };
        };

        struct PlayableNote
        {
            bool                                  assigned{ false }; // A sample exists for the note.
            bool                                  mute{ false };
            int                                   keyNote{ -1 };
            int                                   muteGroup{ -1 };
            int                                   polyphony{ 1 };
            LayerPlayType                         layerPlay{ LayerPlayType::CYCLE };
            ClipPlayType                          padPlay{ ClipPlayType::CLIP_ONE_SHOT };
            std::array<int, MAX_SIMUL_ACTIONS>    simultaneousPlay{};
            std::array<int, MAX_SIMUL_ACTIONS>    simultaneousMute{};
            std::array<PlayableLayer, MAX_LAYERS> layers;
        };

        struct PlayableNotes
        {
            std::array<PlayableNote, MIDI::NUMERIC_MAX>   notes;
            std::array<SamplePlayType, MIDI::NUMERIC_MAX> samplePlay{};
            std::bitset<MIDI::NUMERIC_MAX>                soloNotes;

            // A note outside 0..127 has no sample and is not soloed.
            const PlayableNote& operator[]( int Note ) const
            {
                static const PlayableNote none{};
                return CommonUtils::isValidMidiNote( Note ) ? notes[(size_t)Note] : none;
            }

            // As the program's functions of the same names.
            bool isNoteSolo( int Note ) const { return CommonUtils::isValidMidiNote( Note ) && soloNotes[(size_t)Note]; }
            bool isNoteMutedDueToSolo( int Note ) const { return soloNotes.any() && !isNoteSolo( Note ); }
            bool isNoteEffectivelyMuted( int Note ) const { return ( *this )[Note].mute || isNoteMutedDueToSolo( Note ); }

            // Calls Func with the key note of every other sample in mute group MG.
            template<typename Function>
            void forNotesInMuteGroup( int Note, int MG, Function&& Func ) const
            {
                if( 0 <= MG )
                    for( const auto& it : notes )
                        if( it.assigned && MG == it.muteGroup && Note != it.keyNote )
                            Func( it.keyNote );
            }
        };

        // Holds the published copy for as long as it exists; any thread.
        class PlayableNotesReader
        {
        public:
            explicit PlayableNotesReader( const MTIProgram& Program );
            ~PlayableNotesReader();

            const PlayableNotes& operator*() const noexcept { return *notes; }
            const PlayableNotes* operator->() const noexcept { return notes; }

        private:
            const MTIProgram&    program;
            const PlayableNotes* notes;

            JUCE_DECLARE_NON_COPYABLE( PlayableNotesReader )
        };

        std::function<void()>             onNameChanged     = []() {};
        std::function<void()>             onGainChanged     = []() {};
        std::function<void()>             onPanChanged      = []() {};
        std::function<void()>             onMuteChanged     = []() {};
        std::function<void()>             onOutChanged      = []() {};
        std::function<void( int, float )> onSendGainChanged = []( int, float ) {};

        // Index API's
        inline void setIndex( int Index ) { MTIProgramData::index = Index; };
        inline int  getIndex() const { return MTIProgramData::index; }

        // Prog type.
        inline bool isDrum() const { return MTProgramType::Drum == getType(); }
        inline bool isKeys() const { return MTProgramType::Keygroup == getType(); }
        inline bool isCV() const { return MTProgramType::Cv == getType(); }
        inline bool isMidi() const { return MTProgramType::Midi == getType(); }
        inline bool isClip() const { return MTProgramType::Clip == getType(); }
        inline bool isAudio() const { return MTProgramType::Audio == getType(); }

        MTIInsertsInterface* getInsertsInterface() { return dynamic_cast<MTIInsertsInterface*>( this ); };

        ///
        /// Note related...
        ///
        MTISample*   getSample( int Note );
        MTISample*   getOrCreate( int Note );
        void         removeSample( int Note );
        MTILayer*    getLayer( int Note, int Layer );
        ASoundSlice* getLayerSourceSlice( int Note, int Layer );

        void                      forAllSamples( const std::function<void( MTISample* Sample )>& func );
        void                      forSamplesInSameMuteGroup( int Note, int MG, const std::function<void( int Notes )>& func );
        void                      getSamples( std::vector<MTISample*>& Samples );
        std::vector<juce::String> getSampleNames();
        inline int                getNumSamples() { return (int)iSampleMap.size(); }
        std::vector<int>          getAssignedNotes();

        void addSample( int KeyNote, const juce::String& Sample, int Slice, int Layer );
        void removeSample( int KeyNote, int Layer );
        void removeSample( int KeyNote, const juce::String& Source );

        // Virtual functions...
        virtual void noteProcess( juce::Array<juce::MidiMessage>& Msgs );
        virtual void refresh() {}

        std::array<int, 4> getSimultaneousNotePlay( int Note );
        std::array<int, 4> getSimultaneousNoteMute( int Note );

        void setSimultaneousNotePlay( int Note, const std::vector<int>& PlayNotes );
        void setSimultaneousNoteMute( int Note, const std::vector<int>& MuteTargets );

        /// Getters
        bool   isNoteMute( int Note );
        int    getNoteMuteGroup( int Note );
        int    getNoteMin( int Note );
        int    getNoteKey( int Note );
        int    getNoteMax( int Note );
        int    getNoteSemi( int Note );
        int    getNoteFine( int Note );
        double getNoteGain( int Note );
        double getNotePan( int Note );

        /// Setters
        bool toggleNoteMute( int Note );
        void setNoteMute( int Note, bool Mute );
        void setNoteGain( int Note, double Gain );
        void setNotePan( int Note, double Pan );
        void setNoteSemi( int Note, int Semi );
        void setNoteFine( int Note, int Fine );
        void setNotePitch( int Note, int Semi, int Fine );
        void setNoteMuteGroup( int Note, int Group );
        void setNoteMin( int Note, int Val );
        void setNoteKey( int Note, int Val );
        void setNoteMax( int Note, int Val );

        // Metering....
        bool                       IsInputMeteringEnabled() { return midiMeter.isEnabled(); };
        bool                       IsOutputMeteringEnabled() { return outMeterEnabled.load(); };
        void                       enableInputMetering( bool Enable ) { midiMeter.enable( Enable ); };
        const MTMeterData&         readInMeter() const { return midiMeter.getMeter(); };
        virtual void               enableOutputMetering( bool Enable ) { outMeterEnabled = Enable; };
        virtual const MTMeterData& readOutMeter() const { return AudIO::_dummyMeter; };
        virtual void               processMidiMeter() { midiMeter.process(); }

        inline juce::ValueTree&   getState() { return vTree; }
        inline juce::UndoManager* getUndoManager() { return um; }

        inline juce::String  getID() const { return MTIProgramData::id.get(); }
        inline juce::String  getName() const { return MTIProgramData::name.get(); }
        inline MTProgramType getType() const { return MTIProgramData::type.get(); }
        inline bool          getMute() const { return MTIProgramData::mute.get(); }
        inline bool          getSolo() const { return MTIProgramData::solo.get(); }
        inline int           getLevel() const { return MTIProgramData::level.get(); }
        inline int           getSemi() const { return MTIProgramData::semi.get(); }
        inline int           getFine() const { return MTIProgramData::fine.get(); }
        inline int           getTranspose() const { return MTIProgramData::transpose.get(); }
        inline int           getPolyphony() const { return MTIProgramData::polyphony.get(); }
        inline double        getPan() const { return MTIProgramData::pan.get(); }
        inline double        getGain() const { return CommonUtils::midiToGain( getLevel() ); }

        inline PadColorMode      getColourMode() const { return MTIProgramData::colourMode.get(); }
        inline EmptyPadColorMode getEmptyPadColourMode() const { return MTIProgramData::emptyPadColourMode.get(); }

        inline int getPadColour( int Pad ) const
        {
            return ( juce::isPositiveAndBelow( Pad, MIDI::NUMERIC_MAX ) ) ? MTIPadColors::padColour[Pad].get() : 0;
        }

        int  setName( const juce::String& newName );
        void setMute( bool Mute );
        void setSolo( bool Solo );
        void setLevel( int Level );
        void setSemi( int Semi );
        void setFine( int Fine );
        void setPitch( int Semi, int Fine );
        void setTranspose( int Transpose );
        void setPolyphony( int Polyphony );
        void setPan( double Pan );

        // API's related to mute status...
        inline bool      isProgramEffectivelyMuted() { return MTIProgramData::mute.get() || isMutedDueToSolo(); }
        inline bool      isNoteEffectivelyMuted( int Note ) { return isNoteMute( Note ) || isNoteMutedDueToSolo( Note ); }
        std::vector<int> getNotesInSameMuteGroup( int Note );

        // Routing...
        juce::String       getOutput() const { return output.get(); }
        float              getSendGain( int SendChannel );
        std::vector<float> getSendGains();

        void setOutput( const juce::String& outName );
        void setSendGain( int SendChannel, const double Gain );
        void setSendGains( const std::vector<double>& Gains );

        virtual SamplePlayType getNoteSamplePlay( int Note ) { return SamplePlayType::NOTE_ON; }

        int setColorMode( PadColorMode Mode );
        int setEmptyColorMode( EmptyPadColorMode Mode );
        int setPadColor( int Color, int Pad = -1 ); // Negative, set color for all pads. // 0 and Positive, pad index.

        ///
        /// SOLO API's
        ///
        /// Program
        bool        isMutedDueToSolo() { return !soloingPrograms.isZero() && !soloingPrograms[MTIProgramData::index]; }
        static void resetSoloingPrograms() { soloingPrograms.clear(); }

        /// Program Note
        bool isNoteSolo( int Note ) { return soloingNotes[Note]; }
        bool isNoteMutedDueToSolo( int Note ) { return !soloingNotes.isZero() && !soloingNotes[Note]; }
        void setNoteSolo( int Note, bool Solo );
        int  resetNotesSolo();
        int  getNumSoloNotes();

        // API's Related Sample Layer
        //Getters
        std::array<juce::String, MAX_LAYERS> getLayerSounds( int Note );
        juce::String                         getLayerSource( int Note, int Layer );

        bool   isLayerMuted( int Note, int Layer );
        bool   isLayerReversed( int Note, int Layer );
        bool   isLayerLooping( int Note, int Layer );
        bool   isLayerFade( int Note, int Layer );
        bool   isLayerWarp( int Note, int Layer );
        int    getLayerSlice( int Note, int Layer );
        int    getLayerOffset( int Note, int Layer );
        int    getLayerLoopStart( int Note, int Layer );
        int    getLayerLoopEnd( int Note, int Layer );
        int    getLayerVelocityStart( int Note, int Layer );
        int    getLayerVelocityEnd( int Note, int Layer );
        double getLayerPan( int Note, int Layer );
        double getLayerGain( int Note, int Layer );
        int    getLayerSemi( int Note, int Layer );
        int    getLayerFine( int Note, int Layer );
        int    getLayerTailLength( int Note, int Layer );
        int    getLayerTailStart( int Note, int Layer );

        //Setters
        void setLayerSource( int Note, int Layer, const juce::String& Value );
        void setLayerMute( int Note, int Layer, bool Value );
        void setLayerReverse( int Note, int Layer, bool Value );
        void setLayerLoop( int Note, int Layer, bool Value );
        void setLayerFade( int Note, int Layer, bool Value );
        void setLayerWarp( int Note, int Layer, bool Value );
        void setLayerSlice( int Note, int Layer, int Value );
        void setLayerOffset( int Note, int Layer, int Value );
        void setLayerLoopStart( int Note, int Layer, int Value );
        void setLayerLoopEnd( int Note, int Layer, int Value );
        void setLayerVelocityStart( int Note, int Layer, int Value );
        void setLayerVelocityEnd( int Note, int Layer, int Value );
        void setLayerPan( int Note, int Layer, double Value );
        void setLayerGain( int Note, int Layer, double Value );
        void setLayerSemi( int Note, int Layer, int Value );
        void setLayerFine( int Note, int Layer, int Value );
        void setLayerTailLength( int Note, int Layer, int miiliSec );
        void setLayerTailStart( int Note, int Layer, int miiliSec );

    protected:
        void       rebuildSampleMap();
        MTISample* createSample( int Note );
        void       publishPlayableNotes();

        inline void reloadMidiMeter( float normLevel ) { midiMeter.reload( normLevel ); }

        virtual std::unique_ptr<MTISample> iSampleCreator( int Note ) { return nullptr; };
        virtual std::unique_ptr<MTISample> iSampleCreator( juce::ValueTree& VT, juce::UndoManager* UM ) { return nullptr; };
        virtual void                       valueTreePropertyChanged( juce::ValueTree& treeWhosePropertyHasChanged, const juce::Identifier& Property );
        void                               valueTreeChildAdded( juce::ValueTree& Parent, juce::ValueTree& Child ) override;
        void                               valueTreeChildRemoved( juce::ValueTree& Parent, juce::ValueTree& Child, int Index ) override;
        void                               valueTreeChildOrderChanged( juce::ValueTree& Parent, int OldIndex, int NewIndex ) override;

    private:
        inline void soloChangeHandler( bool Solo )
        {
            soloingPrograms.setBit( MTIProgramData::index, Solo );
        }

        struct PlayableNotesUpdater : public juce::AsyncUpdater
        {
            explicit PlayableNotesUpdater( MTIProgram& P )
                : program( P ) {}
            void        handleAsyncUpdate() override { program.publishPlayableNotes(); }
            MTIProgram& program;
        };

        void sampleTreeChanged( const juce::ValueTree& Tree );
        void freeRetiredPlayableNotes();

    protected:
        std::map<int, std::unique_ptr<MTISample>> iSampleMap;
        juce::ValueTree                           iSampleVTree;

    public:
        juce::ValueTree    vTree;
        juce::UndoManager* um;

    private:
        std::atomic<bool>       outMeterEnabled{ false };
        juce::BigInteger        soloingNotes;
        static juce::BigInteger soloingPrograms;

        MTMidiMeter midiMeter{ 40 };

        // The published copy: a reader counts itself in playableNotesReaders,
        // then loads playableNotes. A replaced copy is freed, on the message
        // thread, once no reader has been seen after the replacement.
        std::atomic<const PlayableNotes*>                 playableNotes{ nullptr };
        mutable std::atomic<int>                          playableNotesReaders{ 0 };
        std::unique_ptr<const PlayableNotes>              currentPlayableNotes;
        std::vector<std::unique_ptr<const PlayableNotes>> retiredPlayableNotes;
        PlayableNotesUpdater                              playableNotesUpdater{ *this };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( MTIProgram )
    };

} // namespace MT
