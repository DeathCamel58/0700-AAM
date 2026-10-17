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

#include "kick_ui/kick_ui.h"
#include "audio_engine/audio_engine.h"
#include "kick_device_adapter.h"
#include "kick_audio_engine_adapter.h"
#include "kick_audio_interface.h"

#include "contracts/utils/app/termination_signals.h"

#include <csignal>

#ifdef PERFETTO
    #include "melatonin_perfetto/melatonin_perfetto.h"
#endif

//==============================================================================
class AudioEngineApplication : public juce::JUCEApplication
{
public:
    AudioEngineApplication()
    {
        // A write to a closed socket or pipe should fail with EPIPE, not
        // kill the application.
        std::signal( SIGPIPE, SIG_IGN );

        // Quit through the message loop on SIGTERM, SIGINT and SIGHUP, so
        // that shutdown() runs.
        terminationSignals = std::make_unique<contracts::TerminationSignals>( [this]( int ) { systemRequestedQuit(); } );
    }

    //==============================================================================
    const juce::String getApplicationName() override
    {
        return "Kick";
    }

    const juce::String getApplicationVersion() override
    {
        return "0.0.1";
    }

    bool moreThanOneInstanceAllowed() override
    {
        return false;
    }

    //==============================================================================
    void initialise( const juce::String& /*commandLine*/ ) override
    {
        windowOptions = kick::window::StartupOptions::fromProcess( getCommandLineParameterArray() );
        windowOptions.log();

        // Without a display (no X server, or DISPLAY not set) there is no
        // primary display, and the window code dereferences it. Fail with a
        // clear message instead.
        if( juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() == nullptr )
        {
            juce::Logger::writeToLog( "No display is available (is DISPLAY set and the display server running?); quitting" );
            setApplicationReturnValue( EXIT_FAILURE );
            quit();
            return;
        }

        audioInterface.reset(new KickAudioInterface());
        engineAdapter.reset(new KickAudioEngineAdapter(audioInterface));
        
        adapter.reset( new KickDeviceAdapter() );

        // Initialize Settings ...
        mt::getSettings();

        // Initialize Audio engine and interface ...
        mt::getAudioInterface();

        // Initialize User Interface...
        w = kick_ui::createMainWindow( getApplicationName(), windowOptions.windowMode, nullptr );

#if PERFETTO
        MelatoninPerfetto::get().beginSession();
#endif
    }

    void shutdown() override
    {
#if PERFETTO
        MelatoninPerfetto::get().endSession();
#endif
        // Add your application's shutdown code here ...
        if( w )
        {
            w = nullptr;
            // The window does not own the home screen, a static that would
            // otherwise destroy its screens after JUCE has shut down.
            kick_ui::KickHome::Instance().shutdown();
        }
        adapter = nullptr;
    }

    //==============================================================================
    void systemRequestedQuit() override
    {
#if PERFETTO
        MelatoninPerfetto::get().endSession();
#endif

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
    kick::window::StartupOptions          windowOptions;
    std::unique_ptr<kick::window::MainWindow> w;
    std::unique_ptr<KickDeviceAdapter>    adapter;
    std::unique_ptr<KickAudioEngineAdapter>    engineAdapter;
    std::shared_ptr<contracts::AudioInterface> audioInterface;
    std::unique_ptr<contracts::TerminationSignals> terminationSignals;
#if PERFETTO
    std::unique_ptr<perfetto::TracingSession> tracingSession;
#endif
};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION( AudioEngineApplication )
