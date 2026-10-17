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


#include "audio_engine/audio_engine.h"
#include "kick_ui/kick_ui.h"

#include "contracts/dcl/dcl_ipc_interface.h"
#include "contracts/config/gui_ipc_config.h"
#include "contracts/config/ipc_config_constants.h"
#include "contracts/utils/app/ipc_application.h"
#include "contracts/config/connection_id_store.h"
#include "contracts/dcl/dcl_base_message_handler.h"
#include "contracts/audioengine/audio_engine_base_message_handler.h"
#include "contracts/audioengine/audio_engine_ipc_interface.h"
#include "ipc/utils/mt_ipc_helpers.h"
#include "contracts/main/system_ipc_helpers.h"
#include "contracts/main/system_signals.h"

//==============================================================================
class GUI : public contracts::IPCApplication
{
public:
    //==============================================================================
    GUI()
        : IPCApplication( contracts::GUI_PROCESS_NAME,
                          contracts::GUI_PROCESS_NAME,
                          contracts::GUI_PROCESS_NAME,
                          true, true, true ) {}

    bool moreThanOneInstanceAllowed() override
    {
        return false;
    }

    //==============================================================================
    void initialise( const juce::String& ) override
    {
        // The window options are not part of the IPC configuration.
        windowOptions = kick::window::StartupOptions::fromProcess( getCommandLineParameterArray() );
        IPCApplication::initialise( windowOptions.getCommandLine() );
        windowOptions.log();

        dclInterface.reset( new contracts::DclIPCInterface( dispatcher(), contracts::ConnectionIdStore::DCL_CONNECTION_ID ) );
        audioEngineInterface.reset( new contracts::AudioEngineIPCInterface( dispatcher(), contracts::ConnectionIdStore::ENGINE_CONNECTION_ID ) );

        //juce::Thread::sleep( 5000);

        // Without a display (no X server, or DISPLAY not set) there is no
        // primary display, and the window code dereferences it. Fail with a
        // clear message instead, so that the supervisor restarts it later.
        if( juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() == nullptr )
        {
            juce::Logger::writeToLog( "No display is available (is DISPLAY set and the display server running?); quitting" );
            setApplicationReturnValue( EXIT_FAILURE );
            quit();
            return;
        }

        // Initialize Settings ...
        mt::getSettings();

        // Initialize Audio engine and interface ...
        mt::getAudioInterface();

        // Initialize User Interface...
        w = kick_ui::createMainWindow( getApplicationName(), windowOptions, nullptr );
    }

    void shutdown() override
    {
        // Add your application's shutdown code here ...
        if( w )
        {
            w.reset();
            // The window does not own the home screen, a static that would
            // otherwise destroy its screens after JUCE has shut down.
            kick_ui::KickHome::Instance().shutdown();
        }

        dclInterface.reset();
        audioEngineInterface.reset();
        IPCApplication::shutdown();
    }

    void addMessageHandlers( std::vector<std::shared_ptr<ipc::MTIIPCMessageHandler>>& handlers )
    {
        handlers.push_back( std::shared_ptr<ipc::MTIIPCMessageHandler>( new contracts::DclBaseMessageHandler() ) );
        handlers.push_back( std::shared_ptr < ipc::MTIIPCMessageHandler>( new contracts::AudioEngineBaseMessageHandler() ) );
    }

    juce::var getConfig( const juce::String& commandLine )
    {
        return ( commandLine.isEmpty() ?
            contracts::GUIIPCConfig().getStandAloneConfig() :
            ipc::createJsonObjectFromCommandLineString( commandLine ) );
    }

    //==============================================================================
    void systemRequestedQuit() override
    {
        // This is called when the app is being asked to quit: you can ignore this request and let the app
        // carry on running, or call quit() to allow the app to close.
        auto mgr = juce::ModalComponentManager::getInstance();
        if( mgr->cancelAllModalComponents() )
            juce::Timer::callAfterDelay( 100, [this] { quit(); } );
        else
            quit();
    }

    void anotherInstanceStarted( const juce::String& /*commandLine*/ ) override
    {
        // When another instance of the app is launched while this one is running, this method is invoked,
        // and the commandLine parameter tells you what the other instance's command-line arguments were.
    }

private:
   kick::window::StartupOptions                windowOptions;
   std::unique_ptr<kick::window::MainWindow>   w{ nullptr };
   std::unique_ptr<contracts::DclIPCInterface> dclInterface{ nullptr }; // create this to enable handling sendCommandToDcl signal.
   std::unique_ptr<contracts::AudioEngineIPCInterface> audioEngineInterface{ nullptr };
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION( GUI )
