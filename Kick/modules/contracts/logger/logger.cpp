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
#include "ipc/dispatcher/mt_ipc_message_dispatcher.h"
#include "ipc/utils/mt_ipc_message_constructor.h"
#include "logger_constants.h"
#include "logger_keys.h"
#include "logger_message_types.h"
#include "logger_enums.h"
#include "logger_helpers.h"
#include "logger.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace contracts
{
    namespace
    {
        // sd-daemon(3) priority prefixes, which the journal reads from the
        // start of each line on a service's stderr.
        const char* journalPriority( const int level )
        {
            switch( level )
            {
                case contracts::LogLevel::LOG_LEVEL_ERROR: return "<3>";
                case contracts::LogLevel::LOG_LEVEL_WARN:  return "<4>";
                case contracts::LogLevel::LOG_LEVEL_DEBUG: return "<7>";
                default:                                   return "<6>";
            }
        }

        // True if stderr is the journal stream the service manager set up
        // ($JOURNAL_STREAM names its device and inode, see systemd.exec(5)).
        // Children inherit both the variable and the stream.
        bool stderrIsJournal()
        {
            const char* stream = std::getenv( "JOURNAL_STREAM" );
            struct stat st;
            if( stream == nullptr || ::fstat( STDERR_FILENO, &st ) != 0 )
                return false;

            unsigned long long device = 0, inode = 0;
            return std::sscanf( stream, "%llu:%llu", &device, &inode ) == 2
                   && device == static_cast<unsigned long long>( st.st_dev )
                   && inode == static_cast<unsigned long long>( st.st_ino );
        }

        // The first directory in $LOGS_DIRECTORY (systemd's LogsDirectory=),
        // if it is set and writable.
        juce::File logsDirectory()
        {
            const juce::String directories( std::getenv( "LOGS_DIRECTORY" ) );
            if( directories.isEmpty() )
                return {};

            const juce::File directory( directories.upToFirstOccurrenceOf( ":", false, false ) );
            return directory.isDirectory() && directory.hasWriteAccess() ? directory : juce::File();
        }
    }

    /*
     * Writes every log line to stderr. Under systemd that is the journal,
     * which adds the time and keeps the priority given by an sd-daemon
     * prefix; elsewhere each line carries the time and level in text. Child
     * processes inherit KickMain's stderr, so they log the same way rather
     * than through the IPC.
     *
     * Only when $LOGS_DIRECTORY names a writable directory are the lines
     * also appended to a file there (and metrics to a CSV file), without
     * syncing each line to storage. The previous default, a "logs" directory
     * next to the executable, is not writable on the device, and every line
     * was opened, appended, synced and closed.
     */
    class LoggerImpl
    {
    public:
        LoggerImpl( const juce::String&                          logFileBaseName,
                    const juce::String&                          metricsFileBaseName,
                    const juce::String&                          processName,
                    bool                                         logToServer  = false,
                    int                                          connectionId = -1,
                    std::shared_ptr<ipc::MTIPCMessageDispatcher> dispatcher   = nullptr )
            : processName( processName ), logFileName( logFileBaseName ), metricsFileName( metricsFileBaseName ), toJournal( stderrIsJournal() )
        {
            juce::ignoreUnused( logToServer, connectionId, dispatcher );
            createMetricsFile();
            createLogFile();
        }

        ~LoggerImpl()
        {
            closeFile( pLogFile );
            closeFile( pMetricsFile );
        }

    public:
        void createLogFile()
        {
            const juce::ScopedLock lk( logCS );
            openFile( pLogFile, logFileName, LOGGER_FILE_EXTN );
        }

        void createMetricsFile()
        {
            const juce::ScopedLock lk( metricCS );
            openFile( pMetricsFile, metricsFileName, METRICS_FILE_EXTN );
        }

        const long getLogFileSize()
        {
            const juce::ScopedLock lk( logCS );
            return pLogFile ? std::ftell( pLogFile ) : 0;
        }

        const long getMetricsFileSize()
        {
            const juce::ScopedLock lk( metricCS );
            return pMetricsFile ? std::ftell( pMetricsFile ) : 0;
        }

        void logMessage( const juce::String& message, int level, const juce::String& origin = "", const juce::int64 timeInMilliseconds = 0 )
        {
            const juce::String name = origin.isEmpty() ? processName : origin;
            const juce::String text = message.trimEnd();

            const juce::String textLine = "[" + contracts::getFormattedTime( timeInMilliseconds ) + "] [" + name + "] "
                                          + contracts::logLevelString( level ) + " " + text + "\n";
            juce::String stderrLine = textLine;
            if( toJournal )
            {
                // The journal adds the time, and makes one entry per line:
                // give each line the priority, and the process name, as all
                // the processes share KickMain's stream.
                const juce::String prefix = juce::String( journalPriority( level ) ) + name + ": ";
                stderrLine = prefix + text.replace( "\n", "\n" + prefix ) + "\n";
            }

            const juce::ScopedLock lk( logCS );
            writeAll( STDERR_FILENO, stderrLine );
            if( pLogFile )
                appendToFile( pLogFile, textLine );
        }

        void logMetric( const juce::String& name, const juce::var& value, const juce::String& origin = "", const juce::int64 timeInMilliseconds = 0 )
        {
            const juce::String processOrName = origin.isEmpty() ? processName : origin;
            {
                const juce::ScopedLock lk( metricCS );
                if( pMetricsFile )
                {
                    appendToFile( pMetricsFile, contracts::getFormattedTime( timeInMilliseconds ) + "," + processOrName + ",METRIC," + name + "," + value.toString() + "\n" );
                    return;
                }
            }
            logMessage( "metric " + name + " = " + value.toString(), contracts::LogLevel::LOG_LEVEL_DEBUG, origin, timeInMilliseconds );
        }

    private:
        void openFile( std::FILE*& file, const juce::String& baseName, const char* extension )
        {
            closeFile( file );
            const juce::File directory = logsDirectory();
            if( directory == juce::File() )
                return;

            const juce::File path = directory.getChildFile( baseName + "_" + juce::Time::getCurrentTime().formatted( LOG_FILE_NAME_TIME_FORMAT ) + extension );
            file = std::fopen( path.getFullPathName().toRawUTF8(), "ae" );
        }

        static void closeFile( std::FILE*& file )
        {
            if( file )
                std::fclose( file );
            file = nullptr;
        }

        static void appendToFile( std::FILE* file, const juce::String& line )
        {
            std::fputs( line.toRawUTF8(), file );
            std::fflush( file );
        }

        // One write() per line, so lines from several threads and
        // processes on the same stream do not interleave.
        static void writeAll( int fd, const juce::String& line )
        {
            const char* data = line.toRawUTF8();
            size_t      left = line.getNumBytesAsUTF8();
            while( left > 0 )
            {
                const ssize_t written = ::write( fd, data, left );
                if( written < 0 && errno == EINTR )
                    continue;
                if( written <= 0 )
                    return;
                data += written;
                left -= static_cast<size_t>( written );
            }
        }

    private:
        std::FILE* pLogFile{ nullptr };
        std::FILE* pMetricsFile{ nullptr };

        juce::String          processName;
        juce::String          logFileName;
        juce::String          metricsFileName;
        const bool            toJournal;
        juce::CriticalSection logCS;
        juce::CriticalSection metricCS;
    };

    contracts::LoggerImpl* contracts::Logger::pImpl = nullptr;

    Logger::Logger( const juce::String&                          logFileBaseName,
                    const juce::String&                          metricsFileBaseName,
                    const juce::String&                          processName,
                    bool                                         logToServer,
                    int                                          connectionId,
                    std::shared_ptr<ipc::MTIPCMessageDispatcher> dispatcher )
    {
        if( !pImpl )
            pImpl = new contracts::LoggerImpl( logFileBaseName, metricsFileBaseName, processName, logToServer, connectionId, dispatcher );
    }

    Logger::~Logger()
    {
        delete pImpl;
        pImpl = nullptr;
    }

    void Logger::Info( const juce::String& message, const juce::String& origin, const juce::int64 timeInMillSeconds )
    {
        pImpl->logMessage( message, contracts::LogLevel::LOG_LEVEL_INFO, origin, timeInMillSeconds );
    }

    void Logger::Debug( const juce::String& message, const juce::String& origin, const juce::int64 timeInMillSeconds )
    {
        pImpl->logMessage( message, contracts::LogLevel::LOG_LEVEL_DEBUG, origin, timeInMillSeconds );
    }

    void Logger::Warning( const juce::String& message, const juce::String& origin, const juce::int64 timeInMillSeconds )
    {
        pImpl->logMessage( message, contracts::LogLevel::LOG_LEVEL_WARN, origin, timeInMillSeconds );
    }

    void Logger::Error( const juce::String& message, const juce::String& origin, const juce::int64 timeInMillSeconds )
    {
        pImpl->logMessage( message, contracts::LogLevel::LOG_LEVEL_ERROR, origin, timeInMillSeconds );
    }

    void Logger::Metric( const juce::String& name, const juce::var& value, const juce::String& origin, const juce::int64 timeInMillSeconds )
    {
        pImpl->logMetric( name, value, origin, timeInMillSeconds );
    }

    void Logger::log( const juce::String& message, const int level, const juce::String& origin, const juce::int64 timeInMillSeconds )
    {
        switch( level )
        {
            case contracts:: LogLevel::LOG_LEVEL_DEBUG:
                Debug( message, origin, timeInMillSeconds );
                break;

            case contracts::LogLevel::LOG_LEVEL_WARN:
                Warning( message, origin, timeInMillSeconds );
                break;

            case contracts::LogLevel::LOG_LEVEL_ERROR:
                Error( message, origin, timeInMillSeconds );
                break;
            default:
                Info( message, origin, timeInMillSeconds );
                break;
        }
    }

    // Logs are no longer forwarded to KickMain: every process writes them to
    // the stderr it inherited from KickMain.
    void Logger::setDispatcher( const int connId, std::shared_ptr<ipc::MTIPCMessageDispatcher> pDisptacter )
    {
        juce::ignoreUnused( connId, pDisptacter );
    }

    void Logger::logMessage( const juce::String& message )
    {
        pImpl->logMessage( message, contracts::LogLevel::LOG_LEVEL_INFO );
    }

    void Logger::createLogFile()
    {
        pImpl->createLogFile();
    }

    void Logger::createMetricsFile()
    {
        pImpl->createMetricsFile();
    }

    const long Logger::getLogFileSize()
    {
        return pImpl->getLogFileSize();
    }

    const long Logger::getLogMetricsSize()
    {
        return pImpl->getMetricsFileSize();
    }
}
