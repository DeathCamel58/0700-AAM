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
#include "juce_events/juce_events.h"
#include "mt_ipc_socket_server.h"
#include "mt_ipc_connection.h"
#include "mti_ipc_server_callback.h"
#include "mti_ipc_connection_callback.h"
#include "../utils/mt_ipc_constants.h"
#include "../utils/mt_ipc_keys.h"
#include "../utils/mt_ipc_json.h"
#include "../utils/mt_ipc_message_constructor.h"
#include "../utils/mt_ipc_message_types.h"
#include "../utils/mt_ipc_enums.h"
#include "../utils/mt_ipc_close_on_exec.h"

namespace ipc
{
    class MTIPCClientConnection : public ipc::MTIPCConnection
    {
    public:
        MTIPCClientConnection( int connectionId, std::shared_ptr<ipc::MTIIPCConnectionCallback> pCallback )
            : ipc::MTIPCConnection( connectionId, pCallback, false )

        {
        }

        virtual void disconnect() override
        {
            if( disconnectNotifier )
                disconnectNotifier( connectionIdentifier() );
            ipc::MTIPCConnection::disconnect();
        }

        void setDisconnectionNotifier( std::function<void( int connectionId )> notifier )
        {
            disconnectNotifier = notifier;
        }

    protected:
        virtual void onDisconnected()
        {
            if( disconnectNotifier )
                disconnectNotifier( connectionIdentifier() );

            ipc::MTIPCConnection::onDisconnected();
        }

    private:
        std::function<void( int connectionId )> disconnectNotifier{ nullptr };
    };

    // connection callback
    class MTIPCSeverConnectionCallback : public MTIIPCConnectionCallback
    {
    public:
        MTIPCSeverConnectionCallback( std::function<void( int )>                             disconnectedCB,
                                      std::function<void( int, std::shared_ptr<MTIPCJson> )> messageCB )
            : disconnectedCB( disconnectedCB ), messageCB( messageCB )
        {}
        void onConnected( [[maybe_unused]] int connectionId, const juce::String& connectionName ) override
        {

        }

        void onDisconnected( const int connectionId, const juce::String& connectionName ) override
        {
            disconnectedCB( connectionId );
        }

        void onMessage( const int connectionId, std::shared_ptr<MTIPCJson> message) override
        {
            messageCB( connectionId, message );
        }

    private:
        std::function<void( int )>                             connectedCB;
        std::function<void( int )>                             disconnectedCB;
        std::function<void( int, std::shared_ptr<MTIPCJson> )> messageCB;
    };

    // server impl
    class MTIPCSocketServerImpl : public juce::InterprocessConnectionServer
    {
    public:
        MTIPCSocketServerImpl( const juce::var& config, std::shared_ptr<ipc::MTIIPCServerCallback> serverCallback )
            : serverCallback( serverCallback ), connectionCallBackPtr( new MTIPCSeverConnectionCallback( std::bind( &MTIPCSocketServerImpl::onClientDisconnected, this, std::placeholders::_1 ), std::bind( &MTIPCSocketServerImpl::processMessage, this, std::placeholders::_1, std::placeholders::_2 ) ) )
        {
            serverPort = config.getProperty( ipc::IPC_KEY_PORT, -1 );
            if( serverPort <= 0 ) return;

            bindAddress = config.getProperty( IPC_KEY_BIND_ADDRESS, "" );

            enableValidation = config.getProperty( ipc::IPC_KEY_CLIENT_ENABLE_VALIDATION, true );

            juce::var connections = config.getProperty( ipc::IPC_KEY_CONNECTIONS, juce::Array<juce::var>() );

            for( const juce::var& connection : *connections.getArray() )
            {
                int connId = connection.getProperty( ipc::IPC_KEY_CONNECTION_ID, -1 );
                validationData.set( connId, connection.getProperty( ipc::IPC_KEY_UUID, "" ) );
            }
        }

        ~MTIPCSocketServerImpl()
        {
            // No more callbacks to the connection manager, which is being
            // destroyed when this is.
            destroying = true;
            alive.reset();
            stopServer();
        }

        void clearData()
        {
            const juce::ScopedLock sl( lock );
            validationData.clear();
            registered.clear();
            unregistered.clear();
        }

        // Stops accepting, and closes the connections that have not
        // registered; the registered ones belong to the connection manager.
        void stopServer()
        {
            stop();

            juce::HashMap<int, std::shared_ptr<ipc::MTIPCClientConnection>> pending;
            {
                const juce::ScopedLock sl( lock );
                pending.swapWith( unregistered );
            }
            for( auto conn : pending )
                conn->disconnect();
        }

        // if the all the clients are connected then start will not start the server
        // notify server about some clients being disconnected and then start.
        void start()
        {
            if( beginWaitingForSocket( serverPort, bindAddress ) )
            {
                ipc::setListeningSocketCloseOnExec( serverPort );
                return;
            }

            // Most likely another process still holds the port. Nothing can
            // connect to this process, so fail, and let the supervisor start
            // it again later.
            juce::Logger::writeToLog( "Cannot listen on port " + juce::String( serverPort ) + " (is another process using it?); quitting" );
            if( auto* app = juce::JUCEApplicationBase::getInstance() )
                app->setApplicationReturnValue( EXIT_FAILURE );
            juce::MessageManager::callAsync( juce::JUCEApplicationBase::quit );
        }

        bool areAllValidClientsConnected()
        {
            const juce::ScopedLock sl( lock );
            for( auto itr = validationData.begin(); itr != validationData.end(); itr.next() )
            {
                int key = itr.getKey();

                if( registered.contains( key ) && registered[key] )
                    continue;
                return false;
            }
            return true;
        }


        void updateUnConnectedConnectionIds( std::vector<int>& ids )
        {
            const juce::ScopedLock sl( lock );
            for( auto itr = validationData.begin(); itr != validationData.end(); itr.next() )
            {
                int key = itr.getKey();
                if( registered.contains( key ) )
                    continue;
                ids.push_back( key );
            }            
        }


    protected:
        // Called on the server's thread for each accepted connection.
        juce::InterprocessConnection* createConnectionObject()
        {
            const juce::ScopedLock sl( lock );
            // Negative, so a temporary id never collides with a client's
            // connection id.
            tempConnectionId--;
            std::shared_ptr<ipc::MTIPCClientConnection> conn( new ipc::MTIPCClientConnection( tempConnectionId, connectionCallBackPtr ) );
            if( !enableValidation )
                registered.set( tempConnectionId, conn );
            unregistered.set( tempConnectionId, conn );
            return conn->getConnectionObject();
        }

    private:
        // Called on the connection's own thread with its first messages.
        void processMessage( const int connectionId, std::shared_ptr<MTIPCJson> message )
        {
            try
            {
                int type = message->getProperty( ipc::IPC_KEY_MESSAGE_TYPE );
                if( type != ipc::MTIPCMessageType::IPC_MESSAGE_TYPE_REGISTER ) return;

                int          clientConnectionId = message->getProperty( ipc::IPC_KEY_CONNECTION_ID );
                juce::String uuid               = message->getProperty( ipc::IPC_KEY_UUID );
                juce::String connectionName     = message->getProperty( ipc::IPC_KEY_CONNECTION_NAME );

                std::shared_ptr<ipc::MTIPCClientConnection> conn;
                bool                                        validConnection;
                {
                    const juce::ScopedLock sl( lock );
                    if( !unregistered.contains( connectionId ) )
                        return;
                    conn = unregistered[connectionId];

                    validConnection = enableValidation ? ( validationData.contains( clientConnectionId ) && validationData[clientConnectionId] == uuid )
                                                             && ( registered[clientConnectionId] == nullptr ) :
                                                         true;
                    if( validConnection )
                        registered.set( clientConnectionId, conn );
                    unregistered.remove( connectionId );
                    registered.remove( connectionId );
                }

                std::shared_ptr<ipc::MTIPCJson>
                    handshakeMessage = ipc::MTIPCMessageConstructor::constructHeaderMessage( ipc::MTIPCMessageType::IPC_MESSAGE_TYPE_REGISTERATION_STATUS );

                if( validConnection )
                {
                    handshakeMessage->setProperty( ipc::IPC_KEY_REGISTERATION_STATUS, ipc::MTIPCRegistrationStatus::IPC_MESSAGE_REGISTERATION_SUCCESS );

                    conn->setConnectionIdentifier( clientConnectionId );
                    conn->setConnectionName( connectionName );
                    conn->setCallback( nullptr );
                    conn->setDisconnectionNotifier( disconnectionNotifier() );
                    conn->publish( handshakeMessage );

                    if( serverCallback )
                        serverCallback->newConnectionRegistered( clientConnectionId, std::static_pointer_cast<ipc::MTIPCConnection>( conn ) );
                }
                else
                {
                    juce::Logger::writeToLog( "Rejected the registration of " + connectionName );
                    handshakeMessage->setProperty( ipc::IPC_KEY_REGISTERATION_STATUS, ipc::MTIPCRegistrationStatus::IPC_MESSAGE_REGISTERATION_FAILED );
                    conn->publish( handshakeMessage );
                    // This is the connection's own thread, which destroying
                    // the connection would wait for: release it on the
                    // message thread.
                    release( conn );
                }

            } catch( const std::exception& e )
            {
                juce::Logger::writeToLog( "Exception handling a registration: " + juce::String( e.what() ) );
            } catch( ... )
            {
                juce::Logger::writeToLog( "Unknown exception handling a registration" );
            }
        }

        static void release( std::shared_ptr<ipc::MTIPCClientConnection> conn )
        {
            juce::MessageManager::callAsync( [conn]() mutable { conn.reset(); } );
        }

        // Reports a registered connection's disconnect on the message thread,
        // if this server still exists then.
        std::function<void( int )> disconnectionNotifier()
        {
            std::weak_ptr<bool> weakAlive = alive;
            return [this, weakAlive]( int connectionId ) {
                juce::MessageManager::callAsync( [this, weakAlive, connectionId]() {
                    if( weakAlive.lock() )
                        onClientDisconnected( connectionId );
                } );
            };
        }

        // Called on a connection's thread, or on the message thread.
        void onClientDisconnected( int connectionId )
        {
            if( serverCallback && !destroying )
                serverCallback->connectionRemoved( connectionId );

            std::shared_ptr<ipc::MTIPCClientConnection> removed[2];
            {
                const juce::ScopedLock sl( lock );
                if( registered.contains( connectionId ) )
                {
                    removed[0] = registered[connectionId];
                    registered.remove( connectionId );
                }
                if( unregistered.contains( connectionId ) )
                {
                    removed[1] = unregistered[connectionId];
                    unregistered.remove( connectionId );
                }
            }
            for( auto& conn : removed )
                if( conn )
                    release( conn );
        }

    private:
        // Used on the server's thread, the connections' threads and the
        // message thread.
        juce::CriticalSection                                           lock;
        std::shared_ptr<bool>                                           alive{ std::make_shared<bool>( true ) };
        std::atomic<bool>                                               destroying{ false };
        juce::HashMap<int, std::shared_ptr<ipc::MTIPCClientConnection>> unregistered;
        juce::HashMap<int, std::shared_ptr<ipc::MTIPCClientConnection>> registered;
        juce::HashMap<int, juce::String>                               validationData;
        bool                                                           enableValidation{true};
        int                                                            serverPort{ -1 };
        juce::String                                                   bindAddress{ juce::String() };
        int                                                            tempConnectionId{ 0 };
        std::shared_ptr<ipc::MTIIPCServerCallback>                      serverCallback;
        std::shared_ptr<ipc::MTIIPCConnectionCallback>                  connectionCallBackPtr{ nullptr };
        std::function<void( int )>                                     disconnectionCallbackfPtr{ nullptr };
    };

    // facade class

    MTIPCSocketServer::MTIPCSocketServer( const juce::var& config, std::shared_ptr<ipc::MTIIPCServerCallback> serverCallback )
        : impl( new ipc::MTIPCSocketServerImpl( config, serverCallback ) )
    {
    }

    MTIPCSocketServer::~MTIPCSocketServer() { impl.reset(); };

    void MTIPCSocketServer::start()
    {
        impl->start();
    }

    void MTIPCSocketServer::stop()
    {
        impl->stopServer();
    }

    bool MTIPCSocketServer::areAllValidClientsConnected()
    {
        return impl->areAllValidClientsConnected();
    }

    void MTIPCSocketServer::updateUnConnectedConnectionIds( std::vector<int>& ids )
    {
        impl->updateUnConnectedConnectionIds( ids );
    }
}
