
/*
 * Copyright (c) 2024 MUSIC Tribe
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

#include "kick_log_message_handler.h"
#include "kick_logger_config.h"

#include "contracts/config/main_ipc_config.h"
#include "contracts/config/ipc_config_constants.h"
#include "contracts/utils/app/ipc_application.h"
#include "kick_heart_beat_message_handler.h"
#include "kick_heart_beat_monitor.h"
#include "ipc/subsystem/mt_ipc_subsystem.h"
#include "kick_heart_beat_data.h"
#include "ipc/process/mt_ipc_process_manager.h"
#include "ipc/connection/mt_ipc_connection_manager.h"
#include "kick_exit_message_handler.h"
#include "kick_service_notifier.h"

#define COMMAND_LINE_ARGUMENT_STANDALONE "standalone"

class KickMain : public contracts::IPCApplication
{
public:
    //==============================================================================
    KickMain()
        : IPCApplication( contracts::MAIN_PROCESS_NAME,
                          kick::logger::LOG_FILE_BASE_NAME,
                          kick::logger::METRICS_FILE_BASE_NAME,
                          false ),
        heartBeatData( new kick::main::KickHeartBeatData() )
    {}

    bool moreThanOneInstanceAllowed() override
    {
        return false;
    }

    void initialise( const juce::String& commandLine ) override
    {
        reportStatus( "Starting" );
        IPCApplication::initialise( commandLine );

        // A process that exits, or is restarted, leaves a heartbeat behind
        // that its successor must not be judged by.
        if( auto ss = subsystem() )
        {
            std::weak_ptr<kick::main::KickHeartBeatData> weakData = heartBeatData;
            ss->setChildProcessListener( [weakData]( const int hostId, const bool running ) {
                if( auto data = weakData.lock() )
                    running ? data->hostStarted( hostId ) : data->hostExited( hostId );
            } );
        }

        monitor.reset( new kick::main::KickHearBeatMonitor( heartBeatData, std::bind( &KickMain ::onHeartBeatNotReceived, this, std::placeholders::_1 ),
                                                            std::bind( &KickMain::onHeartBeatsVerified, this, std::placeholders::_1 ) ) );
        monitor->startMonitoring();
    }

    void shutdown()
    {
        serviceNotifier.notify( "STOPPING=1" );
        monitor->stopMonintoring();
        monitor.reset();
        heartBeatData.reset();
        IPCApplication::shutdown();
    }

    juce::var getConfig( const juce::String& commandLine ) override
    {
        juce::var config = (commandLine == COMMAND_LINE_ARGUMENT_STANDALONE ?
            contracts::KickIPCConfig().getStandAloneConfig()
            : contracts::KickIPCConfig().getConfig());

        // Expect a heartbeat from every child the process manager launches
        // from this configuration (none in the standalone one).
        juce::Array<int> childHostIds;
        juce::var children = config.getProperty( ipc::IPC_KEY_PROCESS_CONFIG, juce::var() ).getProperty( ipc::IPC_KEY_CHILDREN, juce::var() );
        if( auto* childConfigs = children.getArray() )
        {
            for( const juce::var& child : *childConfigs )
            {
                int hostId = child.getProperty( ipc::IPC_KEY_HOST_ID, -1 );
                if( hostId < 0 )
                    continue;
                childHostIds.add( hostId );
                childNames.set( hostId, child.getProperty( ipc::IPC_KEY_PROCESS_NAME, hostId ).toString() );
            }
        }
        heartBeatData->expectHeartBeatsFrom( childHostIds );

        return config;
    }

    void addMessageHandlers( std::vector<std::shared_ptr<ipc::MTIIPCMessageHandler>>& handlers )
    {
        handlers.push_back( std::shared_ptr<ipc::MTIIPCMessageHandler>( new kick::main::KickLogMessageHandler() ) );
        handlers.push_back( std::shared_ptr<ipc::MTIIPCMessageHandler>(new kick::main::KickHeartBeatMessageHandler( heartBeatData) ));
        handlers.push_back( std::shared_ptr<ipc::MTIIPCMessageHandler>( new kick::main::KickExitMessageHandler() ) );
    }
private:
    // Runs on the message thread, called by the monitor for a host that has
    // not sent a heartbeat for HEART_BEAT_RESTART_TIMEOUT_IN_MS, or within
    // HEART_BEAT_STARTUP_GRACE_IN_MS of a restart.
    // Restarts that process only; the others keep running and reconnect to
    // it. (Stopping and starting the whole subsystem destroyed its data in
    // stop(), and start() then dereferenced it.)
    void onHeartBeatNotReceived( const int hostId )
    {
        if( !childNames.contains( hostId ) || juce::MessageManager::getInstance()->hasStopMessageBeenSent() )
            return;

        auto ss = subsystem();
        if( !ss )
            return;

        // The host may have recovered since the check.
        if( heartBeatData->beginRestartIfSilent( hostId, kick::main::KickHeartBeatData::now(),
                                                 HEART_BEAT_RESTART_TIMEOUT_IN_MS, HEART_BEAT_STARTUP_GRACE_IN_MS ) )
            ss->restartHost( hostId );
    }

    // Runs on the message thread after every check, so WATCHDOG=1 is only
    // sent while KickMain's message loop runs and every child's heartbeat
    // (sent from the child's message loop) is recent enough, or the child
    // is being restarted and still within its startup grace period. Until
    // it is told READY=1, the service manager only enforces its start
    // timeout; after that, missing WATCHDOG=1 notifications let it restart
    // KickMain when restarting the process in onHeartBeatNotReceived() does
    // not help.
    void onHeartBeatsVerified( const kick::main::KickHeartBeatData::Verification& result )
    {
        const bool ready = reportedReady || result.startingHostIds.isEmpty();
        if( result.missingHostIds.isEmpty() && ready )
        {
            if( !reportedReady )
            {
                reportedReady = true;
                serviceNotifier.notify( "READY=1" );
            }
            reportStatus( result.startingHostIds.isEmpty() ? "Running" : "Restarting " + hostNames( result.startingHostIds ) );
            serviceNotifier.notify( "WATCHDOG=1" );
            return;
        }

        juce::Array<int> waiting( result.missingHostIds );
        waiting.addArray( result.startingHostIds );
        reportStatus( "Waiting for " + hostNames( waiting ) );
    }

    juce::String hostNames( const juce::Array<int>& hostIds ) const
    {
        juce::StringArray names;
        for( const int hostId : hostIds )
            names.add( childNames[hostId] );
        return names.joinIntoString( ", " );
    }

    void reportStatus( const juce::String& status )
    {
        if( status == reportedStatus )
            return;
        reportedStatus = status;
        serviceNotifier.notify( "STATUS=" + status );
    }

private:
    std::shared_ptr<kick::main::KickHeartBeatData> heartBeatData;
    std::unique_ptr<kick::main::KickHearBeatMonitor> monitor;

    // Used on the message thread only.
    kick::main::KickServiceNotifier  serviceNotifier;
    juce::HashMap<int, juce::String> childNames;
    bool                             reportedReady{ false };
    juce::String                     reportedStatus;

};

//==============================================================================
// This macro generates the main() routine that launches the app.
START_JUCE_APPLICATION( KickMain )
