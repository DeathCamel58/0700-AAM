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

#include "juce_core/juce_core.h"
#include "ipc_application.h"
#include "termination_signals.h"
#include "ipc/subsystem/mt_ipc_subsystem.h"
#include "contracts/logger/logger.h"
#include "ipc/utils/mt_ipc_helpers.h"
#include "../../main/system_ipc_interface.h"
#include "contracts/main/system_ipc_helpers.h"
#include "contracts/main/system_signals.h"
#include "ipc/utils/mt_ipc_json.h"
#include "ipc/utils/mt_ipc_exception.h"

#include <csignal>

namespace contracts
{
    const int HEART_BEAT_TIMEOUT_IN_MS( 1000 );

    // Sends a heartbeat to KickMain every second from the message thread,
    // so a heartbeat shows that the message loop is still running. (A
    // dedicated thread kept beating while the message thread was stuck.)
    class IPCHeartBeat : private juce::Timer
    {
    public:
        IPCHeartBeat( const int hostId )
        {
            heartBeatMessage = contracts::SystemIPCHelpers::prepareHeartBeatMessage();
            heartBeatMessage->setProperty(ipc::IPC_KEY_HOST_ID, hostId);
        }

        ~IPCHeartBeat() override
        {
            stopTimer();
        }

        void start()
        {
            startTimer( HEART_BEAT_TIMEOUT_IN_MS );
        }

        void stop()
        {
            stopTimer();
        }

    private:
        void timerCallback() override
        {
            contracts::SystemSignals::SendSystemCommand( heartBeatMessage );
        }

        std::shared_ptr<ipc::MTIPCJson>  heartBeatMessage;

    };

    class IPCApplicationImpl
    {
    public:
        IPCApplicationImpl( const juce::String& name,
                            const juce::String& logFileBaseName,
                            const juce::String& metricsFileBaseName,
                            bool                createSystemInterface,
                            bool                enableHeartBeat,
                            bool                logToServer,
                            const juce::String& version )
            : name( name ),
            logFileBaseName( logFileBaseName ),
            metricsFileBaseName( metricsFileBaseName ),
            version( version ),
            logToServer( logToServer ),
            createSystemInterface( createSystemInterface ),
            enableHeartBeat(enableHeartBeat)
        {
        }

        ~IPCApplicationImpl()
        {
            shutdown();
        }

        const juce::String getApplicationName()
        {
            return name;
        }

        const juce::String getApplicationVersion()
        {
            return version;
        }

        bool initialise( const juce::var& config, std::vector<std::shared_ptr<ipc::MTIIPCMessageHandler>>& handlers )
        {
           initializeLogger( logFileBaseName, metricsFileBaseName, name, logToServer );

            if( config.isUndefined() )
            {
                juce::Logger::writeToLog( "Failed to Initailize. Reason - Undefined configuration" );
                return false;
            }

            contracts::ConnectionIdStore::initialize( config );
            
            subsystem.reset( new ipc::MTIPCSubsystem( name,
                                                      config,
                                                      handlers,
                                                      contracts::ConnectionIdStore::PARENT_CONNECTION_ID ) );

            if(createSystemInterface)
                systemIPCIface.reset( new contracts::SystemIPCIterface( subsystem->dispatcher(), contracts::ConnectionIdStore::MAIN_CONNECTION_ID ) );

            if( enableHeartBeat )
            {
                hostId = config.getProperty( ipc::IPC_KEY_HOST_ID, -1 );
                heartBeat.reset( new IPCHeartBeat(hostId) );
                heartBeat->start();
            }

            return true;
        }

        void shutdown()
        {
            if( heartBeat )
                heartBeat->stop();

            // Stop the IPC, and with it any child processes (which get
            // SIGTERM and time to save), while the logger still works.
            if( subsystem )
                subsystem->stop();

            if( logger )
            {
                juce::Logger::setCurrentLogger( nullptr );
            }
        }

        std::shared_ptr<ipc::MTIPCMessageDispatcher> dispatcher()
        {
            if( subsystem )
                return subsystem->dispatcher();
            return nullptr;
        }

        std::shared_ptr<ipc::MTIPCSubsystem> getSubsystem()
        {
            return subsystem;
        }

    private:
        void initializeLogger( const juce::String& logFileBaseName,
                               const juce::String& metricsFileBaseName,
                               const juce::String& processName,
                               bool                logToServer = false )
        {
            logger.reset( new contracts::Logger( logFileBaseName, metricsFileBaseName, processName, logToServer ) );
            juce::Logger::setCurrentLogger( logger.get() );
        }

    private:
        juce::String                         name;
        juce::String                         logFileBaseName;
        juce::String                         metricsFileBaseName;
        juce::String                         version;
        bool                                 logToServer;
        bool                                 createSystemInterface;
        bool                                 enableHeartBeat;
        int                                  hostId;
        std::shared_ptr<ipc::MTIPCSubsystem> subsystem;
        std::unique_ptr<contracts::Logger>   logger;

        std::unique_ptr<contracts::SystemIPCIterface> systemIPCIface;
        std::unique_ptr<IPCHeartBeat>                 heartBeat;
       
    };

    IPCApplication::IPCApplication( const juce::String& name,
                                    const juce::String& logFileBaseName,
                                    const juce::String& metricsFileBaseName,
                                    bool                createSystemInterface,
                                    bool                enableHeartBeat,
                                    bool                logToServer,
                                    const juce::String& version )
        : impl( new IPCApplicationImpl( name,
                                        logFileBaseName,
                                        metricsFileBaseName,
                                        createSystemInterface,
                                        enableHeartBeat,
                                        logToServer,
                                        version ) )
    {
        // The IPC writes to sockets whose peer may have exited. Without this
        // the write raises SIGPIPE and kills this process as well, so one
        // crash takes the others down; ignored, the write fails with EPIPE
        // and the connection's reader reports the disconnect.
        std::signal( SIGPIPE, SIG_IGN );

        // The service manager stops Kick with SIGTERM: quit through the
        // message loop, so that shutdown() runs, as it does for an exit
        // message.
        terminationSignals.reset( new contracts::TerminationSignals( [this]( int signalNumber ) {
            juce::Logger::writeToLog( "Received " + contracts::TerminationSignals::signalName( signalNumber ) + ", quitting" );
            systemRequestedQuit();
        } ) );
    }

    IPCApplication::~IPCApplication()
    {
        terminationSignals.reset();
        impl.reset();
    }
    const juce::String IPCApplication::getApplicationName()
    {
        return impl->getApplicationName();
    }

    const juce::String IPCApplication::getApplicationVersion()
    {
        return impl->getApplicationVersion();
    }

    void IPCApplication::initialise( const juce::String& commandLine )
    {
        bool initialised = false;
        try
        {
            std::vector<std::shared_ptr<ipc::MTIIPCMessageHandler>> handlers{};
            addMessageHandlers( handlers );
            juce::var config = getConfig( commandLine );
            initialised      = impl->initialise( config, handlers );
        } catch( ... )
        {
            ipc::logCurrentException( "Exception initialising the IPC" );
        }

        // Without its IPC the process is no use: fail, so that the
        // supervisor sees it.
        if( !initialised )
        {
            juce::Logger::writeToLog( "Failed to initialise " + getApplicationName() + "; quitting" );
            setApplicationReturnValue( EXIT_FAILURE );
            quit();
        }
    }

    void IPCApplication::shutdown()
    {
        impl.reset();
    }

    std::shared_ptr<ipc::MTIPCMessageDispatcher> IPCApplication::dispatcher()
    {
        auto subsystem = impl->getSubsystem();
        if( subsystem )
            return subsystem->dispatcher();
        return nullptr;
    }

    std::shared_ptr<ipc::MTIPCSubsystem> IPCApplication::subsystem()
    {
        return impl->getSubsystem();
    }

}
