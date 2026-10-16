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

// module include files...

#include "drum_voice.h"

namespace mt
{
    /// @brief
    DrumProgram::DrumProgram( AudIO::Design& D, const juce::String& Name, MTIProgram* PDrum )
        : ISynthObject( D, POLYPHONY::MAX )
    {
        setName( Name.toStdString() );
        setProgram( PDrum );

        for( auto& it : noteLayerIdx )
            it.store( -1 );

        // Pre-reseve Memory...
        for( auto i = 0; i < getPolyphony(); ++i )
        {
            voices.emplace_back( std::make_unique<DrumVoice>( master ) );
        }
        ISynthObject::addVoices( voices );
    }

    DrumProgram::~DrumProgram()
    {
        setBypass( true );
    }

    void DrumProgram::setProgram( MTIProgram* PDrum )
    {
        // call base class method...
        ISynthObject::setProgram( PDrum );

        noteOffAll();
        pDrum = dynamic_cast<MTDrumProgram*>( PDrum );
        setBypass( ( pDrum == nullptr ) );
    }

    void DrumProgram::noteOn( int Note, int Velocity )
    {
        // Return if no audio synth available or invalid midi length
        if( !pDrum || !CommonUtils::isValidMidiNote( Note ) ) return;

        // Read the pads from the program's published copy: this runs on the
        // sequencer's tick thread too, while the UI edits the pads.
        const MTIProgram::PlayableNotesReader notes( *pDrum );
        noteOn( *notes, Note, Velocity );
    }

    void DrumProgram::noteOn( const MTIProgram::PlayableNotes& Notes, int Note, int Velocity )
    {
        if( !CommonUtils::isValidMidiNote( Note ) ) return;

        const auto& pNote = Notes[Note];
        if( pNote.assigned )
        {
            // Stop clips in same mute group.
            Notes.forNotesInMuteGroup( Note, pNote.muteGroup, [this]( int It ) {
                if( CommonUtils::isValidMidiNote( It ) )
                    noteOff( It );
            } );

            // Stop playback of clips simultaneously.
            for( auto it : pNote.simultaneousMute )
                noteOff( it );

            const auto hasSource = pNote.layers[0].hasSource;
            if( !Notes.isNoteEffectivelyMuted( Note ) && hasSource || Notes.isNoteSolo( Note ) && hasSource )
            {
                auto notePoly = pNote.polyphony;

                // Get layers that have valid source.
                std::vector<int> validSourceIndex;
                for( auto i = 0; i < MAX_LAYERS; ++i )
                {
                    if( pNote.layers[(size_t)i].hasSource )
                        validSourceIndex.push_back( i );
                }

                // Choose a layer to play sound
                auto layerToPlay = 0;
                switch( pNote.layerPlay )
                {
                    break;
                    case LayerPlayType::RANDOM:
                        layerToPlay = AudIO::utils::random<int>( 0, ( (int)validSourceIndex.size() - 1 ) );
                        break;
                    case LayerPlayType::CYCLE:
                    {
                        auto&      index = noteLayerIdx[(size_t)Note];
                        const auto last  = index.load();
                        layerToPlay      = ( last < 0 ) ? 0 : juce::jlimit( 0, (int)( validSourceIndex.size() - 1 ), last + 1 );
                        index.store( layerToPlay );
                        break;
                    }
                    case LayerPlayType::VELOCITY:
                    {
                        for( const auto& it : validSourceIndex )
                        {
                            const auto& layer = pNote.layers[(size_t)it];
                            if( layer.velStart <= Velocity && Velocity < layer.velEnd )
                            {
                                layerToPlay = it;
                                break;
                            }
                        }
                    }
                }; // switch

                if( auto soundSlice = pNote.layers[(size_t)layerToPlay].slice.get() )
                {
                    // Start playback.
                    postNoteOn( Note, Velocity, notePoly, soundSlice );
                }
            }

            // Trigger playback of clips simultaneously.
            for( auto it : pNote.simultaneousPlay )
                noteOn( Notes, it, Velocity );
        }
    }
};
