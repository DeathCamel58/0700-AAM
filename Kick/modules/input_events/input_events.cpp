
#include <moodycamel/moodycamel.h>
#include <juce_serialport/juce_serialport.h>

#include "input_events.h"
#include "contracts/dcl/dcl_contracts.h"
#include "contracts/dcl/dcl_contracts_helper.h"
#include "kick_SimulatorInterface.h"

#include "ComConfig.hpp"
#include "kick_McuFrame.h"

// The front-panel MCU's serial port; the build system can override it (see
// CMakeLists.txt).
#ifndef KICK_MCU_SERIAL_DEVICE
    #define KICK_MCU_SERIAL_DEVICE "/dev/ttymxc0"
#endif

namespace Controls
{
    std::atomic<bool> shiftBtnState{ false };
    std::atomic<bool> halfLevelState{ false };

    ///
    /// Static members...
    static std::array<uint8_t, BUFFER_RX_SIZE> ms_arrui8RxBufferUsb;
    static std::array<uint8_t, BUFFER_RX_SIZE> ms_arrui8TxBufferUsb;
    static SimulatorInterface*                 simInterface{ nullptr };
    static CPktMgr                             ms_objPktMgrUsb{ CPktMgr::EN_COM_CHANNEL_USB, ms_arrui8RxBufferUsb.data(), BUFFER_RX_SIZE, ms_arrui8TxBufferUsb.data(), BUFFER_RX_SIZE, 60 };

    sigslot::signal<CPktMgr::HWMsg_t&> Controls::InputEvents::dispatchHardwareEvent = {};

    ///
    /// IMPL...
    class HwSerialData : private juce::ChangeListener
        , private juce::Thread
        , private juce::AsyncUpdater
    {
    public:
        HwSerialData()
            : Thread( "Queue" ) { startThread(); }

        ~HwSerialData()
        {
#ifndef __APPLE__
            close();
#endif
        }

        void connect()
        {
#if( JUCE_LINUX || __linux__ )
            if( juce::File( KICK_MCU_SERIAL_DEVICE ).exists() )
            {
                juce::StringPairArray portlist = SerialPort::getSerialPortPaths();

                if( !portlist.size() )
                    return;

                bool         hasSerialPort = false;
                juce::String serialDev     = KICK_MCU_SERIAL_DEVICE;

                auto serialDebug = []( juce::String str1, juce::String str2 ) {
                    DBG( str1 );
                    DBG( str2 );
                };
                SerialPortConfig config( 230400, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_NONE );

    #if( JUCE_LINUX || __linux__ )
                juce::String devPath = serialDev;
    #endif
                if( sPort )
                {
                    input.reset( nullptr );
                    output.reset( nullptr );
                    sPort.reset( nullptr );
                }
                rxPending.clear();
                {
    #if( JUCE_LINUX || __linux__ )
                    CPktMgr::HWMsg_t hwMsg;
                    hwMsg.pktType = CPktMgr::EN_PKT_TYPE_SYSTEM;
                    hwMsg.pktCmd  = contracts::DeviceControlContracts::SysCmd::EN_SYS_COMM_INIT;
                    hwMsg.pktDir  = CPktMgr::EN_PKT_REQUEST;

                    sPort.reset( new SerialPort( devPath, config, serialDebug ) );
                    input.reset( new SerialPortInputStream( sPort.get() ) );
                    input->addChangeListener( this );
                    input->setNotify( SerialPortInputStream::NOTIFY_ALWAYS );

                    hwMsg.data.push_back( (int)contracts::DeviceControlContracts::SysCommCmd::EN_SYS_COMM_CONNECTED );
                    output.reset( new SerialPortOutputStream( sPort.get() ) );
                    Controls::InputEvents::sendMessageToHardware( hwMsg );
    #endif
                }
            }
            else
            {
                DBG( "Serial port " << KICK_MCU_SERIAL_DEVICE << " not found" );
            }
#endif
        }

        void close()
        {
            if( sPort ) sPort->close();
            stopThread( 1000 );
            waitForThreadToExit( 1000 );
        }

        void reconnect()
        {
#ifndef __APPLE__
            close();
            connect();
#endif
        }

        void sendMessageToControlSurface( CPktMgr::HWMsg_t& f_HwMsg )
        {
            // A frame's size byte counts the data and the CRC: a longer
            // message would go out with a wrong size and be misread.
            if( f_HwMsg.data.size() > Controls::mcu::maxDataSize )
            {
                juce::Logger::writeToLog( "Not sending a message with " + juce::String( (int)f_HwMsg.data.size() ) + " data bytes to the control surface: the most is "
                                          + juce::String( (int)Controls::mcu::maxDataSize ) );
                return;
            }

            // Enqueue in the queue...
            txQueue.enqueue( f_HwMsg );
            //semaphoreQueue.signal();
            triggerAsyncUpdate();
        }

        void messageReceivedFromControlSurface( CPktMgr::HWMsg_t& f_rxMsg )
        {
            juce::String log;
            log += " Data[0] : " + ( f_rxMsg.data.empty() ? juce::String( "-" ) : juce::String( f_rxMsg.data[0] ) ) + " Cmd : " + juce::String( f_rxMsg.pktCmd ) + +" type : " + juce::String( f_rxMsg.pktType );
            juce::Logger::writeToLog( log );

            Controls::InputEvents::dispatchHardwareEvent( f_rxMsg );
        }

    private:
        void handleAsyncUpdate() override { txEvent.signal(); }
        void sendSerialOutData( const std::vector<char>& data )
        {
#if( JUCE_LINUX || __linux__ )
            if( !output || data.empty() )
                return;

            output->write( data.data(), data.size() );
            output->flush();
#endif
        }

        void changeListenerCallback( juce::ChangeBroadcaster* source ) override
        {
            if( !input )
                return;

            // Take what the reader thread has received so far. A frame can
            // arrive in pieces, so the start of one is kept for the next
            // time.
            const int available = (int)input->getTotalLength();
            if( available > 0 )
            {
                const auto kept = rxPending.size();
                rxPending.resize( kept + (size_t)available );
                const int got = input->read( rxPending.data() + kept, available );
                rxPending.resize( kept + (size_t)juce::jmax( 0, got ) );
            }

            for( const auto& frame : Controls::mcu::extractFrames( rxPending, []( std::vector<char>& bytes ) { return CPktMgr::CrcGen( bytes ); } ) )
            {
                CPktMgr::HWMsg_t rxMsg;
                rxMsg.pktType = (CPktMgr::enumPacketType)frame.type;
                rxMsg.pktCmd  = frame.command;
                rxMsg.pktDir  = (CPktMgr::enumPacketDir)frame.direction;
                for( auto byte : frame.data )
                    rxMsg.data.push_back( (int)byte );

                if( rxMsg.pktDir == CPktMgr::enumPacketDir::EN_PKT_REQUEST )
                {
                    if( contracts::DclContractsHelper::isButtonMessage( rxMsg ) )
                    {
                        DBG( "Button state = " << contracts::DclContractsHelper::getButtonState( rxMsg ) );
                        if( ( contracts::DeviceControlContracts::EN_SHIFT == contracts::DclContractsHelper::getButtonCode( rxMsg ) ) )
                            Controls::shiftBtnState.store( contracts::DclContractsHelper::getButtonState( rxMsg ) );
                    }
                    messageReceivedFromControlSurface( rxMsg );
                }
                else
                {
                    //If the data is Ack (response) for the App message, ignore
                    // Do Nothing
                }
            }

            // Whatever is left is at most the start of a frame; more than
            // the longest frame is noise.
            if( rxPending.size() > Controls::mcu::headerSize + Controls::mcu::maxDataSize + 1 )
                rxPending.clear();
        }

        void run() override
        {
            static CPktMgr::HWMsg_t  item;
            static std::vector<char> txData;
            while( !threadShouldExit() )
            {
                txEvent.wait( 1000 );
                //semaphoreQueue.wait( 1 );

                while( txQueue.try_dequeue( item ) && !threadShouldExit() )
                {
#if( JUCE_LINUX || __linux__ )
                    txData.clear();
                    ms_objPktMgrUsb.BuildAndTransmitPacket( item, txData );
                    sendSerialOutData( txData );

                    item.data.clear();
#endif
                }
            } //DBG("Out of While");
        }

    private:
        juce::WaitableEvent txEvent;

        std::unique_ptr<SerialPort>             sPort;
        std::unique_ptr<SerialPortInputStream>  input;
        std::unique_ptr<SerialPortOutputStream> output;

        // Received bytes that are not a complete frame yet.
        std::vector<uint8_t> rxPending;

        moodycamel::ConcurrentQueue<CPktMgr::HWMsg_t> txQueue;
    };
    static std::unique_ptr<HwSerialData> _FPPort;

    /// <summary>
    ///
    /// Class Controls::InputEvents
    ///
    /// </summary>

    void Controls::InputEvents::initialize()
    {
#ifdef SIM_BUILD
        //creating multiple instance of SimulatorInterface using factoty is somehow clearing the connected signals need to find the reason
        simInterface = Factory<SimulatorInterface>::makeObject( 0 );
        simInterface->hwInputReceived.connect( []( int id, const std::vector<char>& data ) {
            Controls::InputEvents::dispatchHardwareEvent( id, data );
        } );
#else
        if( !_FPPort )
        {
            _FPPort.reset( new HwSerialData );
            ms_objPktMgrUsb.Init();
        }
        _FPPort->connect();
        // Send Rebooted command to GD
#endif
    }

    void Controls::InputEvents::reconnect()
    {
#ifndef __APPLE__
        if( _FPPort ) _FPPort->reconnect();
#endif
    }

    void Controls::InputEvents::shutdown()
    {
        _FPPort.reset();
    }

    void Controls::InputEvents::messageReceived( CPktMgr::HWMsg_t& f_rxMsg )
    {
        if( _FPPort ) _FPPort->messageReceivedFromControlSurface( f_rxMsg );
    }

    void Controls::InputEvents::sendMessageToHardware( CPktMgr::HWMsg_t& f_HwMsg )
    {
        if( f_HwMsg.pktType == CPktMgr::enumPacketType::EN_PKT_TYPE_SYSTEM )
        {
            if( f_HwMsg.pktCmd == contracts::DeviceControlContracts::EN_SYS_RECONNECT )
            {
                reconnect();
                return;
            }
            if( f_HwMsg.pktCmd == contracts::DeviceControlContracts::EN_SYS_SHUTDOWN )
            {
                shutdown();
                return;
            }
        }

#ifdef SIM_BUILD
        if( simInterface ) simInterface->sendMessageToHW( f_HwMsg );
#endif
        if( _FPPort ) _FPPort->sendMessageToControlSurface( f_HwMsg );
        else
        {
            juce::Logger::outputDebugString( "Null Serial Port " );
        }
    }
}
