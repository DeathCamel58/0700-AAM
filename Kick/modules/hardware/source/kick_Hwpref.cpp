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

#include "kick_Hwpref.h"
#include "kick_RemovableMedia.h"

// The power key's input device; the build system can override it (see
// CMakeLists.txt).
#ifndef KICK_POWER_KEY_DEVICE
    #define KICK_POWER_KEY_DEVICE "/dev/input/event2"
#endif

// The backlight, by its name in /sys/class/backlight or its sysfs
// directory; the build system can set it (see CMakeLists.txt). Empty: the
// first backlight there is.
#ifndef KICK_BACKLIGHT_DEVICE
    #define KICK_BACKLIGHT_DEVICE ""
#endif

// A systemd unit that performs the factory reset, or empty if the
// application only removes its own state; the build system can set it.
#ifndef KICK_FACTORY_RESET_UNIT
    #define KICK_FACTORY_RESET_UNIT ""
#endif

namespace mt
{
    namespace fs = std::filesystem;


    HwPref::HwPref()
        : juce::Thread( "powerbutton" )
    {
        DBG( "HwPref Constructor called" );
        //startThread();
    }

    void HwPref::run()
    {
        const char* devicePath = KICK_POWER_KEY_DEVICE;

        // How long to wait before trying the device again after it could
        // not be opened or read; doubled on every failure, up to 30 s.
        int retryMs = 500;

        while( !threadShouldExit() )
        {
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
            if( powerKeyFd < 0 )
            {
                powerKeyFd = open( devicePath, O_RDONLY | O_CLOEXEC | O_NONBLOCK );
                if( powerKeyFd < 0 )
                {
                    DBG( "Error opening Input device " << devicePath );
                    // It may appear later, or not exist on this hardware:
                    // try again, without spinning on a CPU core.
                    wait( retryMs );
                    retryMs = juce::jmin( retryMs * 2, 30000 );
                    continue;
                }
            }

            // Wait for events, but not so long that a request to stop the
            // thread goes unnoticed.
            struct pollfd pfd {};
            pfd.fd     = powerKeyFd;
            pfd.events = POLLIN;
            const int ready = poll( &pfd, 1, 200 );
            if( ready == 0 || ( ready < 0 && errno == EINTR ) )
                continue;

            // The device delivers whole events, several at a time if they
            // queued up.
            struct input_event events[16];
            const ssize_t      n = ready > 0 ? read( powerKeyFd, events, sizeof( events ) ) : -1;
            if( n < 0 && ( errno == EAGAIN || errno == EINTR ) )
                continue;

            if( n <= 0 || ( pfd.revents & ( POLLERR | POLLHUP | POLLNVAL ) ) != 0 )
            {
                // The device went away or cannot be read: open it again
                // after a while.
                DBG( "Error reading Input device " << devicePath );
                close( powerKeyFd );
                powerKeyFd = -1;
                wait( retryMs );
                retryMs = juce::jmin( retryMs * 2, 30000 );
                continue;
            }

            retryMs = 500;

            for( size_t i = 0; i < (size_t)n / sizeof( struct input_event ); ++i )
            {
                const auto& ev = events[i];

                // The key is KEY_STOP or, from a power key driver such as
                // the i.MX SNVS one, KEY_POWER.
                if( ev.type == EV_KEY && ( ev.code == KEY_STOP || ev.code == KEY_POWER ) )
                {
                    if( ev.value == 1 )
                    {
                        DBG( "Power key pressed" );
                    }
                    else if( ev.value == 0 )
                    {
                        DBG( "Power key released" );
                        if( powerButtonUI )
                            powerButtonUI();
                    }
                }
            }
#else
            wait( 1000 );
#endif
        }

#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        if( powerKeyFd >= 0 )
        {
            close( powerKeyFd );
            powerKeyFd = -1;
        }
#endif
    }

    // The system's network manager owns the Ethernet interface: it brings
    // the link up, runs DHCP on it and keeps the address and routes. A DHCP
    // client or an interface configuration of our own would fight it (and
    // needs root), so DHCP is always on and the rest is not done here.
    int HwPref::startDhcp( void )
    {
        DBG( "DHCP is run by the system's network manager" );
        return 0;
    }

    int HwPref::stopDhcp( void )
    {
        DBG( "DHCP is run by the system's network manager; not stopping it" );
        return -1;
    }

    int HwPref::factoryReset( void )
    {
        // A system that resets more than the application's own state (its
        // own configuration, say) provides a unit that does it all and
        // restarts the device. Started without waiting, as it stops the
        // application.
        if( std::string( KICK_FACTORY_RESET_UNIT ).size() > 0 )
            return process::run( { "systemctl", "start", "--no-block", KICK_FACTORY_RESET_UNIT } );

        // Forget the Wi-Fi networks, which wpa_supplicant keeps.
        int result = 0;
        if( !WifiManager::getWifiInterface()->removeAllNetworks() )
        {
            DBG( "Factory reset: could not remove the Wi-Fi networks" );
            result = -1;
        }

        // Remove the projects and the preferences from the state directory
        // (what "rm -rf <state>/projects* <state>/prefs" did).
        std::error_code       ec;
        std::vector<fs::path> paths;

        for( const auto& entry : fs::directory_iterator( KICK_STATE_DIR, ec ) )
        {
            const auto name = entry.path().filename().string();
            if( name.rfind( "projects", 0 ) == 0 || name == "prefs" )
                paths.push_back( entry.path() );
        }

        for( const auto& path : paths )
        {
            fs::remove_all( path, ec );
            if( ec )
                result = -1;
        }

        return result;
    }

    int HwPref::shutdown( void )
    {
        // Through systemd, which shuts down cleanly and lets an
        // unprivileged user power off where the system's policy allows it.
        return process::run( { "systemctl", "poweroff" } );
    }

    // Function to compare firmware versions
    mt::VersionComparisonResult HwPref::isLinuxFirmwareVersionLess( const mt::linuxFwInfo& current, const mt::linuxFwInfo& stored )
    {
        if( current.major < stored.major )
        {
            return mt::VersionComparisonResult::Less;
        }
        else if( current.major == stored.major )
        {
            if( current.minor < stored.minor )
            {
                return mt::VersionComparisonResult::Less;
            }
            else if( current.minor == stored.minor )
            {
                if( current.patch < stored.patch )
                {
                    return mt::VersionComparisonResult::Less;
                }
                else if( current.patch == stored.patch )
                {
                    return mt::VersionComparisonResult::Equal;
                }
            }
        }
        return mt::VersionComparisonResult::Greater;
    }

    // Function to compare mcu versions
    mt::VersionComparisonResult HwPref::isMCUFirmwareVersionLess( const mt::mcuFwInfo& current, const mt::mcuFwInfo& stored )
    {
        if( current.major < stored.major )
        {
            return mt::VersionComparisonResult::Less;
        }
        else if( current.major == stored.major )
        {
            if( current.minor < stored.minor )
            {
                return mt::VersionComparisonResult::Less;
            }
            else if( current.minor == stored.minor )
            {
                return mt::VersionComparisonResult::Equal;
            }
        }
        return mt::VersionComparisonResult::Greater;
    }

    std::string HwPref::isImageFound( juce::String Path, juce::String format )
    {
        DBG( "isImageFound checking in Folder path " << Path << "Format of file " << format );
        std::vector<std::string> swuFiles = getSWUFiles( Path.toStdString(), format.toStdString() );
        if( swuFiles.empty() )
        {
            DBG( "No SWU files found in the folder." );
            return "";
        }
        else
        {
            DBG( "SWU Files in the folder:" );
            for( const auto& swuFile : swuFiles )
            {
                DBG( swuFile );
            }
        }
        fwFileName     = swuFiles.at( 0 ); // taking whichever is first
        fwFileFullpath = Path.toStdString() + "/" + swuFiles.at( 0 );
        DBG( "FWFile Name -> " << fwFileName );
        DBG( "FWFile Name -> " << fwFileFullpath );
        return fwFileFullpath;
    }

    bool HwPref::extractImage( std::string path )
    {
        bool            status      = true;
        std::string     imageFormat = ".swu";
        std::error_code ec;
        DBG( "FwUpdate ->extractImage-> creating " << path );
        fs::create_directory( path, ec );
        std::string             outFullpath = decrptionDir + decryptedFwname;
        const juce::StringArray mount { "mount", "-o", "loop", "-t", "ext4", outFullpath, extractDir };
        DBG( "FwUpdate ->extractImage->  " << mount.joinIntoString( " " ) );
        process::run( mount );
        std::vector<std::string> swuFiles = getSWUFiles( extractDir, imageFormat );
        if( swuFiles.empty() )
        {
            DBG( "FwUpdate -> After exraction image file In " << extractDir << " Is not Found" );
            return false;
        }
        else
        {
            DBG( "FwUpdate -> After exraction image file In " << extractDir << " Is Found" );
            for( const auto& swuFile : swuFiles )
            {
                DBG( swuFile );
            }
        }
        return status;
    }
    juce::String HwPref::getDevSerial()
    {
        std::string serialNo = "000000000";
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        std::string path = "/sys/devices/soc0/serial_number";

        std::ifstream serialFile( path.c_str() );
        if( serialFile.is_open() )
        {
            serialFile >> serialNo;
        }
#endif
        return juce::String( serialNo );
    }

    juce::String HwPref::getFirmwareVersion()
    {
        std::string     fwVersion;
        std::string     path = "/etc/";
        mt::linuxFwInfo linfo;

        getLinuxVersion( path, linfo );
        juce::String FwVersion = juce::String::formatted( "%d.%d.%d",linfo.major, linfo.minor, linfo.patch );
        return FwVersion;
    }

    bool HwPref::getLinuxVersion( std::string& path, mt::linuxFwInfo& linfo )
    {
        std::string   filePath = path + "version.txt";
        std::ifstream file( filePath );

        if( !file.is_open() )
        {
            DBG( "FwUpdate -> getFwVersion Error opening file: " << filePath );
            return false;
        }

        std::string line;

        // Read the first line for Linux firmware information
        if( std::getline( file, line ) )
        {
            // Define a regular expression for parsing Linux firmware information
            std::regex  linuxRegex( "^([a-zA-Z]+) (\\d+)\\.(\\d+)\\.(\\d+)$" );
            std::smatch match;

            // Try to match the line against the regular expression
            if( std::regex_match( line, match, linuxRegex ) )
            {
                linfo.linuxname = match[1];
                linfo.major     = std::stoi( match[2] );
                linfo.minor     = std::stoi( match[3] );
                linfo.patch     = std::stoi( match[4] );
                return true;
                // Display parsed information
            }
            else
            {
                DBG( "Error parsing Linux firmware information. Invalid format." );
                file.close();
                return false;
            }
        }
        else
        {
            DBG( "Error reading Linux firmware information from file." );
            file.close();
            return false;
        }
    }

    bool HwPref::getFwVersion( std::string& path, mt::linuxFwInfo& linfo, mt::mcuFwInfo& minfo )
    {
        std::string   filePath = path + "version.txt";
        std::ifstream file( filePath );

        if( !file.is_open() )
        {
            DBG( "FwUpdate -> getFwVersion Error opening file: " << filePath );
            return false;
        }

        std::string line;

        // Read the first line for Linux firmware information
        if( std::getline( file, line ) )
        {
            // Define a regular expression for parsing Linux firmware information
            std::regex  linuxRegex( "^([a-zA-Z]+) (\\d+)\\.(\\d+)\\.(\\d+)$" );
            std::smatch match;

            // Try to match the line against the regular expression
            if( std::regex_match( line, match, linuxRegex ) )
            {
                linfo.linuxname = match[1];
                linfo.major     = std::stoi( match[2] );
                linfo.minor     = std::stoi( match[3] );
                linfo.patch     = std::stoi( match[4] );

                // Display parsed information
            }
            else
            {
                DBG( "Error parsing Linux firmware information. Invalid format." );
                file.close();
                return false;
            }
        }
        else
        {
            DBG( "Error reading Linux firmware information from file." );
            file.close();
            return false;
        }

        // Read the second line for MCU firmware information
        if( std::getline( file, line ) )
        {
            // Define a regular expression for parsing MCU firmware information
            std::regex  mcuRegex( "^([a-zA-Z]+) (\\d+)\\.(\\d+)$" );
            std::smatch mcuMatch;

            // Try to match the line against the regular expression
            if( std::regex_match( line, mcuMatch, mcuRegex ) )
            {
                minfo.mcuname = mcuMatch[1];
                minfo.major   = std::stoi( mcuMatch[2] );
                minfo.minor   = std::stoi( mcuMatch[3] );

                // Display parsed information
            }
            else
            {
                DBG( "Error parsing MCU firmware information. Invalid format." );
                file.close();
                return false;
            }
        }
        else
        {
            DBG( "Error reading MCU firmware information from file." );
            file.close();
            return false;
        }

        file.close();
        return true;
    }

    bool HwPref::addVersionNotoFile( std::string filename )
    {
        return true;
    }

    bool HwPref::getVersionfromFile( std::string filename, linuxFwInfo& info )
    {
        fw_ver_index = -1;
        // Open the encrypted file
        std::ifstream encrypted_file( filename, std::ios::binary );
        if( !encrypted_file )
        {
            DBG( "Error opening encrypted filei" );
            return false;
        }

        // Define buffer to store the version information
        char version_buffer[20]; // 20 characters for version, 1 for null terminator

        // Read the version information from the file
        if( !encrypted_file.read( version_buffer, 20 ) )
        {
            DBG( "Error reading version information." );
            return false;
        }

        // Close the file
        encrypted_file.close();

        for( auto i = 0; i < 20; i++ )
            if( ( version_buffer[i] == 'E' ) || ( version_buffer[i] == 'R' ) )
                fw_ver_index = ( i + 2 );

        if( fw_ver_index < 0 )
        {
            DBG( "Version is not correct" );
            return false;
        }
        version_buffer[fw_ver_index] = '\0';

        // Parse the version buffer and fill the linuxFwInfo structure
        std::istringstream version_stream( std::string( version_buffer, fw_ver_index ) );

        if( !( version_stream >> info.major ) )
        {
            DBG( "Error parsing major version." );
            return false;
        }

        if( version_stream.get() != '.' || !( version_stream >> info.minor ) )
        {
            DBG( "Error parsing minor version." );
            return false;
        }

        if( version_stream.get() != '.' || !( version_stream >> info.patch ) )
        {
            DBG( "Error parsing patch version." );
            return false;
        }

        // Output the version information
        DBG( "Version from future Update File is=" << filename << ":" << info.major << "." << info.minor << "." << info.patch );
        return true;
    }

    bool HwPref::isImgFile( std::string filepath )
    {
        // Check if the file extension is ".img"
        return fs::path( filepath ).extension() == ".img";
    }

    bool HwPref::removeHeaderFromImage( std::string fwfilepath )
    {
        // Copy everything from byte fw_ver_index on (counting from 1, as
        // "tail -c +N" did) into the file that is decrypted next.
        const std::string outFilepath = decrptionDir + fwHeaderFile;
        DBG( "removing Header from image " << fwfilepath << " -> " << outFilepath );
        {
            std::ifstream in( fwfilepath, std::ios::binary );
            std::ofstream out( outFilepath, std::ios::binary | std::ios::trunc );

            if( fw_ver_index > 0 )
                in.seekg( fw_ver_index - 1 );

            if( !in || !out || !( out << in.rdbuf() ) || !out.flush() )
            {
                DBG( "Failed to remove header from image." );
                return false;
            }
        }

    // Now remove the original file
    if ( std::remove( fwfilepath.c_str() ) != 0 )
    {
        DBG( "Error deleting original file: " << fwfilepath );
        return false;
    }
    else
    {
        DBG( "Successfully deleted original file: " << fwfilepath );
    }
    	return true;

    }

    bool HwPref::decryptopenSSLImage( std::string fwfilepath )
    {
        bool        status        = false;
        std::string outFwfilepath = decrptionDir + decryptedFwname;
        std::string inFwfilepath  = decrptionDir + fwHeaderFile;
#if( JUCE_LINUX || __linux__ )
        std::error_code ec;
        fs::create_directories( decrptionDir, ec ); // Create decryption directory if not exists

        if( isImgFile( fwfilepath ) )
        { // Check if it's an img file
            if( removeHeaderFromImage( fwfilepath ) )
            { // Remove header from image
                // The passphrase goes to openssl on its standard input, not
                // on its command line, where any user could read it.
                const juce::StringArray command { "openssl", "enc", "-aes-256-cbc", "-pbkdf2", "-d", "-salt",
                                                  "-in", inFwfilepath, "-out", outFwfilepath, "-pass", "stdin" };

                std::cout << "Decrypting image: " << inFwfilepath << " -> " << outFwfilepath << std::endl;
                int result = process::runWithInput( command, decrptKey + "\n" );

                if( result == 0 )
                {
                    std::cout << "Decryption successful" << std::endl;
                    return true;
                }
                else
                {
                    std::cout << "Incorrect password or decryption failed" << std::endl;
                    return false;
                }
            }
            else
            {
                std::cout << "Header removal failed" << std::endl;
                return false;
            }
        }
        else
        {
            std::cout << "Not an IMG file" << std::endl;
            return false;
        }
#endif
        return true;
    }
    std::vector<std::pair<std::string, linuxFwInfo>> HwPref::getSWUFileswithVersion( const std::string& folderPath, const std::string& imageFormat )
    {
        std::vector<std::pair<std::string, linuxFwInfo>> swuFiles;
        if( fs::exists( folderPath ) && fs::is_directory( folderPath ) )
        {
            try
            {
                bool foundSWU = false;
                for( const auto& entry : fs::directory_iterator( folderPath ) )
                {
                    if( entry.path().extension() == imageFormat )
                    {
                        std::string fullpath = folderPath + "/" + entry.path().filename().string();
                        linuxFwInfo info;
                        if( getVersionfromFile( fullpath, info ) )
                        {
                            swuFiles.push_back( std::make_pair( fullpath, info ) );
                            DBG( "FwUpdate file -> " << fullpath << " Version: " << info.major << "." << info.minor << "." << info.patch );
                            foundSWU = true;
                        }
                        else
                        {
                            DBG( "Error extracting version information from file: " << entry.path().string() );
                        }
                    }
                }
                if( !foundSWU )
                {
                    DBG( "No SWU files found in the specified directory." );
                }
            } catch( const std::filesystem::filesystem_error& e )
            {
                DBG( "Error accessing directory: " << e.what() );
            }
        }
        else
        {
            DBG( "The path is not valid or does not exist." );
        }
        return swuFiles;
    }

    std::vector<std::string> HwPref::getSWUFiles( const std::string& folderPath, const std::string& imageFormat )
    {
        std::vector<std::string> swuFiles;
        if( fs::exists( folderPath ) && fs::is_directory( folderPath ) )
        {
            try
            {
                for( const auto& entry : fs::directory_iterator( folderPath ) )
                {
                    if( entry.path().extension() == imageFormat )
                    {
                        swuFiles.push_back( entry.path().filename().string() );
                        DBG( "FwUpate files -> " << entry.path().filename().string() );
                    }
                }
            } catch( const std::filesystem::filesystem_error& e )
            {
                DBG( "Error accessing directory: " << e.what() );
            }
        }
        else
        {
            DBG( "The path is not valid or does not exist." );
        }
        return swuFiles;
    }

    bool HwPref::isEthernetCableConnected()
    {
        return net::hasCarrier( net::ethernetInterface() );
    }

    bool HwPref::ethernetEnable( bool status )
    {
#if( JUCE_LINUX || __linux__ )
        // The system's network manager brings the link up and down.
        juce::ignoreUnused( status );
        DBG( "Ethernet is managed by the system's network manager; not turning it " << ( status ? "on" : "off" ) );
        return false;
#else
        return true; // Not supported on non-Linux systems
#endif
    }

    bool HwPref::isInterfaceUp()
    {
#if( JUCE_LINUX || __linux__ )
        return net::isUp( net::ethernetInterface() );
#else
        return true;
#endif
    }

    int HwPref::setIP( const std::string& interface, const std::string& ipAddress, const std::string& subnetMask, const std::string& gateway )
    {
        DBG( "SetIP Fucntion Called" );
        int result = 0;
#if( JUCE_LINUX || __linux__ )
        // The system's network manager owns the interface's addresses.
        juce::ignoreUnused( interface, ipAddress, subnetMask, gateway );
        DBG( "Not setting " << interface << " to " << ipAddress << "/" << subnetMask << ": addresses come from the system's network manager" );
        result = -1;
#endif
        return result;
    }

    std::string HwPref::getSubnetMask( const std::string& interface )
    {
        return net::ipv4Address( interface ).netmask;
    }

    std::string HwPref::getGateway( const std::string& interface )
    {
        const std::string gateway = net::defaultGateway( interface );
        return gateway.empty() ? "0.0.0.0" : gateway;
    }

    void HwPref::getIP( const std::string interface, sysEthInfo& info )
    {
        const auto address = net::ipv4Address( interface );
        info.IP            = address.address;
        info.subnet        = address.netmask;
        info.status        = net::isUp( interface );
        info.gateway       = getGateway( interface );
        DBG( "Network " << interface << ": " << info.IP << "/" << info.subnet << " via " << info.gateway << ( info.status ? ", up" : ", down" ) );
    }

    int HwPref::getDateTime( dateInfo& date, timeInfo& time )
    {
        DBG( "GetDate Fucntion Called" );

        // The system's time zone, /etc/localtime: tzset() reads it again if
        // it has changed since (TZ being unset), and localtime_r(), unlike
        // localtime(), is safe with other threads.
        tzset();

        std::time_t currentTime = std::time( nullptr );
        std::tm     timeInfo {};

        if( localtime_r( &currentTime, &timeInfo ) != nullptr )
        {
            // Extract and display each component separately
            date.YYYY = timeInfo.tm_year + 1900; // Add 1900 to get the actual year
            date.MM   = timeInfo.tm_mon + 1; // Add 1 since months are 0-based
            date.DD   = timeInfo.tm_mday;
            time.HH   = timeInfo.tm_hour;
            time.MM   = timeInfo.tm_min;

            DBG( "Date-> " << date.YYYY << "/" << date.MM << "/" << date.DD );
            DBG( "Time-> " << time.HH << "/" << time.MM );

            return 1;
        }
        return 0;
    }

    juce::String HwPref::getTimeZone()
    {
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        // /etc/localtime links to the zone's file in the time zone database
        // (.../zoneinfo/Europe/Berlin), which is how systemd's timedated
        // sets and reports it.
        char          target[PATH_MAX];
        const ssize_t n = readlink( "/etc/localtime", target, sizeof( target ) - 1 );
        if( n > 0 )
        {
            std::string zone( target, (size_t)n );
            const auto  pos = zone.find( "zoneinfo/" );
            if( pos != std::string::npos )
            {
                zone = zone.substr( pos + 9 );
                for( const char* prefix : { "posix/", "right/" } )
                    if( zone.rfind( prefix, 0 ) == 0 )
                        zone = zone.substr( std::strlen( prefix ) );
                return zone;
            }
        }

        // Debian's way; also written by some builds.
        return sysfile::readLine( "/etc/timezone" );
#else
        return {};
#endif
    }

    int HwPref::setTimeZone( juce::String& str )
    {
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        int ret = process::run( { "timedatectl", "set-timezone", str } );
        if( ret == 0 )
        {
            DBG( "setting timezone Success " << str );
            return 1;
        }
        else
            DBG( "Failed to set timeZone" );
#endif
        return 0;
    }

    // The backlight's sysfs directory, or an empty string if there is none.
    static std::string findBacklight()
    {
        const std::string configured = KICK_BACKLIGHT_DEVICE;
        const std::string classDir   = "/sys/class/backlight";

        if( !configured.empty() )
            return configured[0] == '/' ? configured : classDir + "/" + configured;

        std::vector<std::string> names;
        std::error_code          ec;
        for( const auto& entry : fs::directory_iterator( classDir, ec ) )
            names.push_back( entry.path().filename().string() );

        if( names.empty() )
            return {};

        std::sort( names.begin(), names.end() );
        return classDir + "/" + names.front();
    }

    int HwPref::setBrightness( int value )
    {
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        // The setting is a percentage. Below 20% the screen is too dark to
        // find the setting again, so it goes no lower.
        value = juce::jlimit( 20, 100, value );

        const std::string backlight = findBacklight();
        if( backlight.empty() )
        {
            DBG( "No backlight found" );
            return 0;
        }

        // The levels go from 0 to max_brightness, which depends on the
        // driver (from a handful of PWM steps to thousands).
        const long maxBrightness = std::strtol( sysfile::readLine( backlight + "/max_brightness" ).c_str(), nullptr, 10 );
        if( maxBrightness <= 0 )
        {
            DBG( "Cannot read the maximum brightness of " << backlight );
            return 0;
        }

        const long level = juce::jmax( 1L, ( value * maxBrightness + 50 ) / 100 );
        if( sysfile::write( backlight + "/brightness", std::to_string( level ) ) )
            return 1;

        DBG( "Cannot set the brightness of " << backlight << " to " << level << ": " << strerror( errno ) );
#endif
        return 0;
    }

    DeviceInfo HwPref::getDeviceInfo()
    {
        DeviceInfo info;
        //Serial number
        juce::String serialnumber = getDevSerial().toRawUTF8();
        std::strncpy( info.serialNumber, serialnumber.toRawUTF8(), sizeof( info.serialNumber ) );

        //FirmwareVersion
        juce::String fwVersion = getFirmwareVersion().toRawUTF8();
        std::strncpy( info.firmwareVersion, fwVersion.toRawUTF8(), sizeof( info.firmwareVersion ) );

        //buildDate
        juce::String buildStamp = juce::String( __DATE__ ) + " :: " + juce::String( __TIME__ );
        std::strncpy( info.firmwareBuildDate, buildStamp.toRawUTF8(), sizeof( info.firmwareBuildDate ) );

        mt::sysRamInfo rInfo;
        mt::SystemResource::getResourceInterface().ramInfo( rInfo );

        std::strncpy( info.memoryStatus.available, juce::String( rInfo.freeRam ).toRawUTF8(), sizeof( info.memoryStatus.available ) );
        std::strncpy( info.memoryStatus.total, juce::String( rInfo.totalRam ).toRawUTF8(), sizeof( info.memoryStatus.total ) );
        std::strncpy( info.memoryStatus.used, juce::String( rInfo.usedRam ).toRawUTF8(), sizeof( info.memoryStatus.used ) );

        mt::sysSizeInfo sInfo;
        std::string     internalPath = KICK_STATE_DIR;
        mt::SystemResource::getResourceInterface().sizeInfo( internalPath, sInfo );

        std::strncpy( info.internalStorageStatus.availableSpace, juce::String( sInfo.avaSize ).toRawUTF8(), sizeof( info.internalStorageStatus.availableSpace ) );
        std::strncpy( info.internalStorageStatus.totalSpace, juce::String( sInfo.totalSize ).toRawUTF8(), sizeof( info.internalStorageStatus.totalSpace ) );
        std::strncpy( info.internalStorageStatus.usedSpace, juce::String( sInfo.usedSize ).toRawUTF8(), sizeof( info.internalStorageStatus.usedSpace ) );

        internalPath = getRemovableMediaPath( RemovableMedia::UsbDisk );

        if( !internalPath.empty() )
        {
            mt::SystemResource::getResourceInterface().sizeInfo( internalPath, sInfo );

            std::strncpy( info.usbStorageStatus.availableSpace, juce::String( sInfo.avaSize ).toRawUTF8(), sizeof( info.usbStorageStatus.availableSpace ) );
            std::strncpy( info.usbStorageStatus.totalSpace, juce::String( sInfo.totalSize ).toRawUTF8(), sizeof( info.usbStorageStatus.totalSpace ) );
        }
        else
        {
            memcpy( info.usbStorageStatus.totalSpace, "usb not connected", sizeof( info.usbStorageStatus.totalSpace ) );
            memcpy( info.usbStorageStatus.availableSpace, "usb not connected", sizeof( info.usbStorageStatus.availableSpace ) );
        }

        return info;
    }

    bool HwPref::isStorageconnected( std::string id ) // the mount point of a removable disk
    {
        // The mount point exactly: a substring of /proc/mounts would also
        // match other mount points, devices and mount options.
        const bool connected = !id.empty() && isMountPoint( id );
        DBG( " " << id << ( connected ? " Storage is connected" : " Storage is not connected" ) );
        return connected;
    }

    void HwPref::setDate( int day, int month, int year )
    {
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        // timedatectl sets the date and the time together: keep the time.
        std::time_t now = std::time( nullptr );
        std::tm     local {};
        localtime_r( &now, &local );

        DBG( "Setting Day -> " << year << "-" << month << "-" << day );
        if( setDateTime( year, month, day, local.tm_hour, local.tm_min, local.tm_sec ) )
        {
            DBG( "Date successfully updated to: " << day << "/" << month << "/" << year );
        }
        else
        {
            DBG( "Failed to update date." );
        }
#endif
    }

    void HwPref::setTime( int hour, int minute )
    {
#if defined( __linux__ ) || defined( __linux ) || defined( linux )
        // timedatectl sets the date and the time together: keep the date.
        std::time_t now = std::time( nullptr );
        std::tm     local {};
        localtime_r( &now, &local );

        DBG( "Setting Time -> " << hour << ":" << minute << ":00" );
        if( setDateTime( local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, hour, minute, 0 ) )
        {
            DBG( "Time successfully updated to: " << hour << "/" << minute );
        }
        else
        {
            DBG( "Failed to update Time." );
        }
#endif

        DBG( "Setting Day -> " << hour << "::" << minute );
    }

    bool HwPref::setDateTime( int year, int month, int day, int hour, int minute, int second )
    {
        char localTime[32];
        std::snprintf( localTime, sizeof( localTime ), "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second );

        // A clock set by hand would be overwritten by network time, and
        // timedated refuses to set it while network time is on, so turn it
        // off first; back on if the time cannot be set after all.
        juce::String ntp;
        process::run( { "timedatectl", "show", "--property=NTP", "--value" }, &ntp );
        const bool ntpWasOn = ntp.trim() == "yes";

        if( ntpWasOn )
            process::run( { "timedatectl", "set-ntp", "false" } );

        if( process::run( { "timedatectl", "set-time", localTime } ) == 0 )
            return true;

        if( ntpWasOn )
            process::run( { "timedatectl", "set-ntp", "true" } );
        return false;
    }

    // Get the number of days in a month, considering leap years
    int HwPref::getDaysInMonth( int month, int year )
    {
        if( month == 2 )
        {
            if( ( year % 4 == 0 && year % 100 != 0 ) || ( year % 400 == 0 ) )
                return 29; // Leap year
            else
                return 28; // Non-leap year
        }
        else if( month == 4 || month == 6 || month == 9 || month == 11 )
        {
            return 30;
        }
        else
        {
            return 31;
        }
    }
}
