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

#include "juce_serialport.h"

#if JUCE_LINUX
    #include <cerrno>
    #include <cstring>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/eventfd.h>
    #include <termios.h>
    #include <unistd.h>
#endif

namespace
{
    // How long the reader thread waits for data before checking whether it
    // should exit, and how long close() may therefore have to wait for it.
    constexpr int readPollMs = 50;

    // How long write() waits for the driver to accept more data before
    // giving up on the port.
    constexpr int writeTimeoutMs = 2000;

#if JUCE_LINUX
    bool baudToSpeed( juce::uint32 bps, speed_t& speed )
    {
        struct Rate
        {
            juce::uint32 bps;
            speed_t      speed;
        };
        static const Rate rates[] = {
            { 50, B50 },
            { 75, B75 },
            { 110, B110 },
            { 134, B134 },
            { 150, B150 },
            { 200, B200 },
            { 300, B300 },
            { 600, B600 },
            { 1200, B1200 },
            { 1800, B1800 },
            { 2400, B2400 },
            { 4800, B4800 },
            { 9600, B9600 },
            { 19200, B19200 },
            { 38400, B38400 },
            { 57600, B57600 },
            { 115200, B115200 },
            { 230400, B230400 },
            { 460800, B460800 },
            { 500000, B500000 },
            { 576000, B576000 },
            { 921600, B921600 },
            { 1000000, B1000000 },
            { 1152000, B1152000 },
            { 1500000, B1500000 },
            { 2000000, B2000000 },
    #ifdef B2500000
            { 2500000, B2500000 },
    #endif
    #ifdef B3000000
            { 3000000, B3000000 },
    #endif
    #ifdef B3500000
            { 3500000, B3500000 },
    #endif
    #ifdef B4000000
            { 4000000, B4000000 },
    #endif
        };

        for( const auto& r : rates )
        {
            if( r.bps == bps )
            {
                speed = r.speed;
                return true;
            }
        }
        return false;
    }

    juce::String errnoString( int err )
    {
        return juce::String( std::strerror( err ) );
    }
#endif
}

//==============================================================================
SerialPort::SerialPort( DebugFunction debugFunction )
    : debug( std::move( debugFunction ) )
{
#if JUCE_LINUX
    wakeFd = eventfd( 0, EFD_CLOEXEC | EFD_NONBLOCK );
#endif
}

SerialPort::SerialPort( const juce::String& path, const SerialPortConfig& config, DebugFunction debugFunction )
    : SerialPort( std::move( debugFunction ) )
{
    portConfig = config;
    open( path );
}

SerialPort::~SerialPort()
{
    close();
#if JUCE_LINUX
    if( wakeFd >= 0 )
        ::close( wakeFd );
#endif
}

void SerialPort::reportError( const juce::String& message ) const
{
    const auto text = portPath + ": " + message;
    if( debug )
        debug( "SerialPort", text );
    else
        DBG( "SerialPort: " << text );
}

bool SerialPort::open( const juce::String& path )
{
    close();

    const juce::ScopedLock rl( readLock );
    const juce::ScopedLock wl( writeLock );

    portPath = path;

#if JUCE_LINUX
    const int handle = ::open( path.toRawUTF8(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC );
    if( handle < 0 )
    {
        reportError( "cannot open: " + errnoString( errno ) );
        return false;
    }

    if( !isatty( handle ) )
    {
        reportError( "not a serial port" );
        ::close( handle );
        return false;
    }

    if( !applyConfig( handle, portConfig ) )
    {
        ::close( handle );
        return false;
    }

    // Drop whatever was received before the port was configured.
    tcflush( handle, TCIOFLUSH );

    // Clear a wake-up left over from an earlier close().
    if( wakeFd >= 0 )
    {
        eventfd_t value;
        eventfd_read( wakeFd, &value );
    }

    fd = handle;
    return true;
#else
    reportError( "serial ports are not supported on this platform" );
    return false;
#endif
}

void SerialPort::close()
{
    // Stop new reads and writes from starting, and wake those waiting in
    // poll(), then wait for them to finish under the locks so that the
    // descriptor is never closed (and possibly reused) while in use.
    const int handle = fd.exchange( -1 );
    if( handle < 0 )
        return;

#if JUCE_LINUX
    if( wakeFd >= 0 )
        eventfd_write( wakeFd, 1 );
#endif

    const juce::ScopedLock rl( readLock );
    const juce::ScopedLock wl( writeLock );
#if JUCE_LINUX
    ::close( handle );
#endif
}

bool SerialPort::setConfig( const SerialPortConfig& config )
{
    const juce::ScopedLock rl( readLock );
    const juce::ScopedLock wl( writeLock );

    const int handle = fd.load();
    if( handle >= 0 && !applyConfig( handle, config ) )
        return false;

    portConfig = config;
    return true;
}

bool SerialPort::getConfig( SerialPortConfig& config ) const
{
    config = portConfig;
    return true;
}

bool SerialPort::applyConfig( int handle, const SerialPortConfig& config )
{
#if JUCE_LINUX
    termios tio{};
    if( tcgetattr( handle, &tio ) != 0 )
    {
        reportError( "cannot read the line settings: " + errnoString( errno ) );
        return false;
    }

    // Raw mode: no line editing, echo, signals or character translation.
    cfmakeraw( &tio );
    tio.c_cflag |= CLOCAL | CREAD;

    speed_t speed;
    if( !baudToSpeed( config.bps, speed ) )
    {
        reportError( "unsupported baud rate " + juce::String( config.bps ) );
        return false;
    }
    cfsetispeed( &tio, speed );
    cfsetospeed( &tio, speed );

    tio.c_cflag &= ~(tcflag_t)CSIZE;
    switch( config.databits )
    {
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        case 8: tio.c_cflag |= CS8; break;
        default:
            reportError( "unsupported number of data bits " + juce::String( config.databits ) );
            return false;
    }

    tio.c_cflag &= ~(tcflag_t)( PARENB | PARODD );
    #ifdef CMSPAR
    tio.c_cflag &= ~(tcflag_t)CMSPAR;
    #endif
    tio.c_iflag &= ~(tcflag_t)( INPCK | ISTRIP );
    switch( config.parity )
    {
        case SerialPortConfig::SERIALPORT_PARITY_NONE: break;
        case SerialPortConfig::SERIALPORT_PARITY_ODD: tio.c_cflag |= PARENB | PARODD; break;
        case SerialPortConfig::SERIALPORT_PARITY_EVEN: tio.c_cflag |= PARENB; break;
    #ifdef CMSPAR
        case SerialPortConfig::SERIALPORT_PARITY_MARK: tio.c_cflag |= PARENB | CMSPAR | PARODD; break;
        case SerialPortConfig::SERIALPORT_PARITY_SPACE: tio.c_cflag |= PARENB | CMSPAR; break;
    #endif
        default:
            reportError( "unsupported parity" );
            return false;
    }
    if( tio.c_cflag & PARENB )
        tio.c_iflag |= INPCK;

    // termios has no 1.5 stop bits setting; UARTs use 1.5 when two stop bits
    // are selected with 5 data bits.
    if( config.stopbits == SerialPortConfig::STOPBITS_1 )
        tio.c_cflag &= ~(tcflag_t)CSTOPB;
    else
        tio.c_cflag |= CSTOPB;

    tio.c_cflag &= ~(tcflag_t)CRTSCTS;
    tio.c_iflag &= ~(tcflag_t)( IXON | IXOFF | IXANY );
    switch( config.flowcontrol )
    {
        case SerialPortConfig::FLOWCONTROL_NONE: break;
        case SerialPortConfig::FLOWCONTROL_HARDWARE: tio.c_cflag |= CRTSCTS; break;
        case SerialPortConfig::FLOWCONTROL_XONXOFF: tio.c_iflag |= IXON | IXOFF; break;
    }

    // Reads return whatever is available without waiting; the descriptor is
    // non-blocking and waiting is done with poll().
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if( tcsetattr( handle, TCSANOW, &tio ) != 0 )
    {
        reportError( "cannot apply the line settings: " + errnoString( errno ) );
        return false;
    }
    return true;
#else
    juce::ignoreUnused( handle, config );
    return false;
#endif
}

int SerialPort::readBytes( void* dest, int maxBytes, int timeoutMs )
{
    const juce::ScopedLock rl( readLock );

    const int handle = fd.load();
    if( handle < 0 )
        return -1;
    if( maxBytes <= 0 )
        return 0;

#if JUCE_LINUX
    pollfd    pfd[2] = { { handle, POLLIN, 0 }, { wakeFd, POLLIN, 0 } };
    const int ready  = ::poll( pfd, wakeFd >= 0 ? 2 : 1, timeoutMs );
    if( ready < 0 )
        return errno == EINTR ? 0 : -1;
    if( ready == 0 || fd.load() != handle )
        return 0;

    if( pfd[0].revents & POLLIN )
    {
        const auto n = ::read( handle, dest, (size_t)maxBytes );
        if( n > 0 )
            return (int)n;
        if( n < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) )
            return 0;
        reportError( n == 0 ? juce::String( "disconnected" ) : "read failed: " + errnoString( errno ) );
    }
    else if( pfd[0].revents == 0 )
    {
        return 0;
    }
    else
    {
        reportError( "disconnected" );
    }

    // The device has gone away (e.g. a USB adapter was unplugged) or failed.
    close();
    return -1;
#else
    juce::ignoreUnused( dest, timeoutMs );
    return -1;
#endif
}

int SerialPort::writeBytes( const void* src, int numBytes, int timeoutMs )
{
    const juce::ScopedLock wl( writeLock );

    const int handle = fd.load();
    if( handle < 0 )
        return -1;
    if( numBytes <= 0 )
        return 0;

#if JUCE_LINUX
    pollfd    pfd[2] = { { handle, POLLOUT, 0 }, { wakeFd, POLLIN, 0 } };
    const int ready  = ::poll( pfd, wakeFd >= 0 ? 2 : 1, timeoutMs );
    if( ready < 0 )
        return errno == EINTR ? 0 : -1;
    if( ready == 0 )
        return 0;
    if( fd.load() != handle )
        return -1;

    if( pfd[0].revents & POLLOUT )
    {
        const auto n = ::write( handle, src, (size_t)numBytes );
        if( n >= 0 )
            return (int)n;
        if( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR )
            return 0;
        reportError( "write failed: " + errnoString( errno ) );
    }
    else if( pfd[0].revents == 0 )
    {
        return 0;
    }
    else
    {
        reportError( "disconnected" );
    }
    // Leave closing the port to the reader, or to the owner: close() takes
    // the read lock, which must not be taken while holding the write lock.
    return -1;
#else
    juce::ignoreUnused( src, timeoutMs );
    return -1;
#endif
}

bool SerialPort::drain()
{
    const juce::ScopedLock wl( writeLock );

    const int handle = fd.load();
    if( handle < 0 )
        return false;
#if JUCE_LINUX
    return tcdrain( handle ) == 0;
#else
    return false;
#endif
}

juce::StringPairArray SerialPort::getSerialPortPaths()
{
    juce::StringPairArray ports;

#if JUCE_LINUX
    // Every tty backed by a device (a UART, a USB serial adapter, ...) has a
    // "device" link in sysfs; virtual consoles and pseudo terminals do not.
    for( const auto& entry : juce::RangedDirectoryIterator( juce::File( "/sys/class/tty" ), false, "*",
                                                            juce::File::findDirectories | juce::File::findFiles ) )
    {
        const auto ttyDir = entry.getFile();
        if( !ttyDir.getChildFile( "device" ).exists() )
            continue;

        const auto name = ttyDir.getFileName();
        const auto dev  = juce::File( "/dev" ).getChildFile( name );
        if( dev.exists() )
            ports.set( name, dev.getFullPathName() );
    }
#endif

    return ports;
}

//==============================================================================
SerialPortInputStream::SerialPortInputStream( SerialPort* serialPort )
    : juce::Thread( "SerialInThread" ), port( serialPort )
{
    jassert( port != nullptr );
    startThread();
}

SerialPortInputStream::~SerialPortInputStream()
{
    signalThreadShouldExit();
    stopThread( 4 * readPollMs + 1000 );
}

void SerialPortInputStream::setNotify( notifyflag notifyMode, char c )
{
    notifyChar = c;
    notify     = notifyMode;
}

void SerialPortInputStream::cancel()
{
    const juce::ScopedLock sl( bufferLock );
    bufferedBytes = 0;
}

juce::int64 SerialPortInputStream::getTotalLength()
{
    const juce::ScopedLock sl( bufferLock );
    return (juce::int64)bufferedBytes;
}

bool SerialPortInputStream::isExhausted()
{
    const juce::ScopedLock sl( bufferLock );
    return bufferedBytes == 0;
}

int SerialPortInputStream::read( void* destBuffer, int maxBytesToRead )
{
    if( destBuffer == nullptr || maxBytesToRead <= 0 )
        return 0;

    const juce::ScopedLock sl( bufferLock );

    const auto n = juce::jmin( (size_t)maxBytesToRead, bufferedBytes );
    if( n == 0 )
        return 0;

    auto* data = static_cast<char*>( buffer.getData() );
    std::memcpy( destBuffer, data, n );
    bufferedBytes -= n;
    if( bufferedBytes > 0 )
        std::memmove( data, data + n, bufferedBytes );

    return (int)n;
}

void SerialPortInputStream::run()
{
    char chunk[512];

    while( !threadShouldExit() )
    {
        const int n = port->readBytes( chunk, (int)sizeof( chunk ), readPollMs );

        if( n < 0 )
        {
            // Closed or failed; wait in case the port is opened again.
            wait( 100 );
            continue;
        }
        if( n == 0 )
            continue;

        {
            const juce::ScopedLock sl( bufferLock );
            if( buffer.getSize() < bufferedBytes + (size_t)n )
                buffer.setSize( juce::jmax( (size_t)4096, ( bufferedBytes + (size_t)n ) * 2 ) );
            std::memcpy( static_cast<char*>( buffer.getData() ) + bufferedBytes, chunk, (size_t)n );
            bufferedBytes += (size_t)n;
        }

        const int mode = notify.load();
        if( mode == NOTIFY_ALWAYS
            || ( mode == NOTIFY_ON_CHAR && std::memchr( chunk, notifyChar.load(), (size_t)n ) != nullptr ) )
        {
            sendChangeMessage();
        }
    }
}

//==============================================================================
SerialPortOutputStream::SerialPortOutputStream( SerialPort* serialPort )
    : port( serialPort )
{
    jassert( port != nullptr );
}

SerialPortOutputStream::~SerialPortOutputStream()
{
    flush();
}

void SerialPortOutputStream::flush()
{
    if( port != nullptr )
        port->drain();
}

bool SerialPortOutputStream::write( const void* dataToWrite, size_t numberOfBytes )
{
    if( port == nullptr )
        return false;

    auto*      data      = static_cast<const char*>( dataToWrite );
    size_t     remaining = numberOfBytes;
    const auto deadline  = juce::Time::getMillisecondCounter() + (juce::uint32)writeTimeoutMs;

    while( remaining > 0 )
    {
        const auto now = juce::Time::getMillisecondCounter();
        if( now >= deadline )
            return false;

        const int chunk = (int)juce::jmin( remaining, (size_t)65536 );
        const int n     = port->writeBytes( data, chunk, (int)( deadline - now ) );
        if( n < 0 )
            return false;

        data += n;
        remaining -= (size_t)n;
        bytesWritten += n;
    }
    return true;
}
