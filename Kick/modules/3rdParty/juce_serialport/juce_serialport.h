/*******************************************************************************
 The block below describes the properties of this module, and is read by
 the Projucer to automatically generate project code that uses it.
 For details about the syntax and how to create or use a module, see the
 JUCE Module Format.txt file.

 BEGIN_JUCE_MODULE_DECLARATION

  ID:                 juce_serialport
  vendor:             Kick
  version:            1.0.0
  name:               Serial port access
  description:        Buffered, asynchronous access to serial ports (POSIX termios)
  website:
  license:            AGPLv3

  dependencies:       juce_core, juce_events

 END_JUCE_MODULE_DECLARATION

*******************************************************************************/

/*
 * Copyright (c) 2026 Dylan Corrales <deathcamel58@gmail.com>
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

/*
 * Serial port access for Kick.
 *
 * Provides SerialPort, SerialPortConfig, SerialPortInputStream and
 * SerialPortOutputStream with the interface of the "juce_serialport" module
 * that Kick was originally written against. That module was not part of the
 * Kick source release and carries no license, so this is an independent
 * implementation of the parts of that interface that Kick uses.
 *
 * Only Linux (termios) is implemented. On other platforms no ports are listed
 * and opening a port fails.
 *
 * Typical use:
 *
 *     SerialPort port ("/dev/ttyUSB0", SerialPortConfig (115200, 8,
 *                       SerialPortConfig::SERIALPORT_PARITY_NONE,
 *                       SerialPortConfig::STOPBITS_1,
 *                       SerialPortConfig::FLOWCONTROL_NONE));
 *     if (port.exists())
 *     {
 *         SerialPortInputStream in (&port);   // reads on its own thread
 *         in.addChangeListener (listener);    // called on the message thread
 *         in.setNotify (SerialPortInputStream::NOTIFY_ALWAYS);
 *         SerialPortOutputStream out (&port);
 *         out.write (data, size);
 *     }
 *
 * The streams must be destroyed before the SerialPort they use.
 */

#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>

/** Receives diagnostic messages from a SerialPort: (source, message). */
using DebugFunction = std::function<void( juce::String, juce::String )>;

//==============================================================================
/** Line settings of a serial port. */
class SerialPortConfig
{
public:
    enum SerialPortParity
    {
        SERIALPORT_PARITY_NONE = 0,
        SERIALPORT_PARITY_ODD,
        SERIALPORT_PARITY_EVEN,
        SERIALPORT_PARITY_MARK,
        SERIALPORT_PARITY_SPACE
    };

    enum SerialPortStopBits
    {
        STOPBITS_1 = 0,
        STOPBITS_1ANDHALF,
        STOPBITS_2
    };

    enum SerialPortFlowControl
    {
        FLOWCONTROL_NONE = 0,
        FLOWCONTROL_HARDWARE,
        FLOWCONTROL_XONXOFF
    };

    SerialPortConfig() = default;

    SerialPortConfig( juce::uint32 bitsPerSecond, int dataBits, SerialPortParity parityMode,
                      SerialPortStopBits stopBitsMode, SerialPortFlowControl flowControlMode )
        : bps( bitsPerSecond ), databits( dataBits ), parity( parityMode ), stopbits( stopBitsMode ), flowcontrol( flowControlMode )
    {
    }

    juce::uint32          bps         = 9600;
    int                   databits    = 8;
    SerialPortParity      parity      = SERIALPORT_PARITY_NONE;
    SerialPortStopBits    stopbits    = STOPBITS_1;
    SerialPortFlowControl flowcontrol = FLOWCONTROL_NONE;
};

//==============================================================================
/** An open serial port. Reading and writing go through SerialPortInputStream
    and SerialPortOutputStream. */
class SerialPort
{
public:
    /** Creates an object that is not connected to a port; call open(). */
    explicit SerialPort( DebugFunction debugFunction = nullptr );

    /** Opens the port at portPath and applies config; check exists(). */
    SerialPort( const juce::String& portPath, const SerialPortConfig& config,
                DebugFunction debugFunction = nullptr );

    virtual ~SerialPort();

    /** Opens the port at portPath, closing any port that was open, and applies
        the current configuration. Returns true on success. */
    bool open( const juce::String& portPath );

    /** Closes the port. Streams using it stop reading and writing. */
    void close();

    /** True while the port is open. */
    bool exists() const noexcept { return fd.load() >= 0; }

    /** Applies config to the port (and remembers it for later open() calls).
        Returns false if the port is open and the settings were refused. */
    bool setConfig( const SerialPortConfig& config );
    bool getConfig( SerialPortConfig& config ) const;

    const juce::String& getPortPath() const noexcept { return portPath; }

    /** Lists the serial ports on the system as (name, device path) pairs,
        e.g. ("ttymxc0", "/dev/ttymxc0"). */
    static juce::StringPairArray getSerialPortPaths();

    /** Low-level access used by the streams. Both return the number of bytes
        transferred, 0 if nothing could be transferred within timeoutMs, or -1
        if the port is closed or has failed (it is then closed). */
    int readBytes( void* dest, int maxBytes, int timeoutMs );
    int writeBytes( const void* src, int numBytes, int timeoutMs );

    /** Waits until all written data has been transmitted. */
    bool drain();

private:
    bool applyConfig( int handle, const SerialPortConfig& config );
    void reportError( const juce::String& message ) const;

    juce::String          portPath;
    SerialPortConfig      portConfig;
    DebugFunction         debug;
    std::atomic<int>      fd{ -1 };
    int                   wakeFd = -1; // eventfd that wakes poll() in close()
    juce::CriticalSection readLock, writeLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( SerialPort )
};

//==============================================================================
/** Reads from a SerialPort on a background thread into a buffer.

    read() and getTotalLength() work on the buffered data and never block.
    Depending on setNotify(), a change message is broadcast (and so delivered
    to ChangeListeners on the message thread) when data arrives. */
class SerialPortInputStream : public juce::InputStream
    , public juce::ChangeBroadcaster
    , private juce::Thread
{
public:
    enum notifyflag
    {
        NOTIFY_OFF = 0, ///< never broadcast
        NOTIFY_ON_CHAR, ///< broadcast when notifyChar is received
        NOTIFY_ALWAYS ///< broadcast whenever data is received
    };

    explicit SerialPortInputStream( SerialPort* port );
    ~SerialPortInputStream() override;

    void setNotify( notifyflag notifyMode = NOTIFY_ON_CHAR, char c = 0 );

    /** Discards the buffered data. */
    void cancel();

    /** Number of bytes received and not yet read. */
    juce::int64 getTotalLength() override;
    bool        isExhausted() override;
    int         read( void* destBuffer, int maxBytesToRead ) override;
    juce::int64 getPosition() override { return 0; }
    bool        setPosition( juce::int64 ) override { return false; }

private:
    void run() override;

    SerialPort*           port;
    juce::MemoryBlock     buffer;
    size_t                bufferedBytes = 0;
    juce::CriticalSection bufferLock;
    std::atomic<int>      notify{ NOTIFY_OFF };
    std::atomic<char>     notifyChar{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( SerialPortInputStream )
};

//==============================================================================
/** Writes to a SerialPort. write() blocks until the data has been handed to
    the driver (or the port fails). */
class SerialPortOutputStream : public juce::OutputStream
{
public:
    explicit SerialPortOutputStream( SerialPort* port );
    ~SerialPortOutputStream() override;

    void        flush() override;
    bool        setPosition( juce::int64 ) override { return false; }
    juce::int64 getPosition() override { return bytesWritten; }
    bool        write( const void* dataToWrite, size_t numberOfBytes ) override;

private:
    SerialPort* port;
    juce::int64 bytesWritten = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR( SerialPortOutputStream )
};
