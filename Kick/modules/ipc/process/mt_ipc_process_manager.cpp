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
#include "mt_ipc_process_manager.h"
#include "mt_ipc_child_process_executor.h"
#include "../utils/mt_ipc_constants.h"
#include "../utils/mt_ipc_keys.h"
#include "mti_ipc_process_manager_callback.h"

const juce::String DEFAULT_PROCESS_NAME( "" );
namespace ipc
{
	class MTIPCProcessManagerData
	{
	public:
		~MTIPCProcessManagerData()
		{
			executors.clear();
		}

        std::shared_ptr<ipc::MTIPCChildProcessExecutor> executor( const int processIdentifier )
        {
            const juce::ScopedLock sl( executorsLock );
            return executors.contains( processIdentifier ) ? executors[processIdentifier] : nullptr;
        }

        juce::String processName( const int processIdentifier )
        {
            if( configurations.contains( processIdentifier ) )
                return configurations[processIdentifier]->getProperty( ipc::IPC_KEY_PROCESS_NAME, DEFAULT_PROCESS_NAME );
            return DEFAULT_PROCESS_NAME;
        }

	public:
		// Started and stopped on the message thread, and reaped on the
		// monitor thread.
		juce::CriticalSection                                                executorsLock;
		juce::HashMap<int, std::shared_ptr<ipc::MTIPCChildProcessExecutor>> executors;
		// Set up by initialize(), read-only afterwards.
		juce::HashMap<int, std::shared_ptr<juce::var>> configurations;
		std::shared_ptr<ipc::MTIIPCProcessManagerCallback> callback;
	};

	class MTIPCProcessMonitorThread : public juce::Thread
	{
	public:
		MTIPCProcessMonitorThread(std::shared_ptr< MTIPCProcessManagerData> data)
			: juce::Thread("ProcessManagerThread"), data(data)
		{
		}
		~MTIPCProcessMonitorThread()
		{

		}

		virtual void run() override
		{
			// stopThread() wakes the wait.
			while( !threadShouldExit() && !wait( ipc::IPC_PROCESS_MONITOR_THREAD_TIMEOUT_IN_MILLI_SECONDS ) )
			{
				juce::Array<int>                                       exitedIds;
				juce::Array<std::shared_ptr<MTIPCChildProcessExecutor>> exitedExecutors;
				{
					const juce::ScopedLock sl( data->executorsLock );
					for( auto itr = data->executors.begin(); itr != data->executors.end(); itr.next() )
					{
						if( itr.getValue()->isRunning() )
							continue;
						exitedIds.add( itr.getKey() );
						exitedExecutors.add( itr.getValue() );
					}
					for( int pid : exitedIds )
						data->executors.remove( pid );
				}

				for( int i = 0; i < exitedIds.size(); ++i )
				{
					const int          pid         = exitedIds[i];
					const int          exitcode    = exitedExecutors[i]->exitCode();
					const juce::String processName = data->processName( pid );
					juce::Logger::writeToLog( processName + " " + exitedExecutors[i]->exitDescription() );

					// Holds the callback, not this thread, which may be gone
					// by the time the message is delivered.
					if( auto callback = data->callback )
						juce::MessageManager::callAsync( [callback, pid, exitcode, processName]() { callback->processExited( pid, exitcode, processName ); } );
				}
			}
		}


	private:
		std::shared_ptr< MTIPCProcessManagerData> data;
	};

   
	MTIPCProcessManager::MTIPCProcessManager(std::shared_ptr<ipc::MTIIPCProcessManagerCallback> callback)
		: data(new MTIPCProcessManagerData())
		, processMonitorThread(nullptr)
	{
		data->callback = callback;
	}

	MTIPCProcessManager::~MTIPCProcessManager()
	{
		data.reset();
		if( processMonitorThread )
			processMonitorThread->stopThread( 1500 );
		processMonitorThread.reset();
	}

	void MTIPCProcessManager::initialize(std::shared_ptr<juce::var> config)
	{
		juce::var               processes = config->getProperty(ipc::IPC_KEY_CHILDREN, juce::Array<juce::var>());
		juce::Array<juce::var>* arr = processes.getArray();
		for (juce::var& processConfig : *arr)
		{
			int id = processConfig.getProperty(ipc::IPC_KEY_HOST_ID, -1);
            if( id >= 0 )
            {
                data->configurations.set( id, std::make_shared<juce::var>( processConfig ) );
            }
		}
		processMonitorThread.reset(new ipc::MTIPCProcessMonitorThread(data));
	}

	void MTIPCProcessManager::start()
	{

		for (auto itr = data->configurations.begin(); itr != data->configurations.end(); itr.next())
		{
            startProcess( itr.getKey() );
        }
		processMonitorThread->startThread();
	}

	void MTIPCProcessManager::stop()
	{
		if(processMonitorThread && processMonitorThread->isThreadRunning())
			processMonitorThread->stopThread(1500);

		const juce::ScopedLock sl( data->executorsLock );
		for (auto executor : data->executors)
		{
			executor->stop();
		}

	}

	void MTIPCProcessManager::wait()
	{
		juce::Array<std::shared_ptr<ipc::MTIPCChildProcessExecutor>> executors;
		{
			const juce::ScopedLock sl( data->executorsLock );
			for( auto executor : data->executors )
				executors.add( executor );
		}
		for (auto executor : executors)
		{
			executor->join();
		}
	}

	int MTIPCProcessManager::exitCode( const int processIdentifier )
	{
		auto executor = data->executor( processIdentifier );
		return executor ? executor->exitCode() : 0;
	}

	bool MTIPCProcessManager::isRunning( const int processIdentifier )
	{
		auto executor = data->executor( processIdentifier );
		return executor && executor->isRunning();
	}

	bool MTIPCProcessManager::startProcess( const int processIdentifier )
	{
		if( !data->configurations.contains( processIdentifier ) )
			return false;

		std::shared_ptr<juce::var> processConfig = data->configurations[processIdentifier];
		std::shared_ptr< ipc::MTIPCChildProcessExecutor> exe(new ipc::MTIPCChildProcessExecutor(processConfig));		
		bool started = exe->start();

        if( started )
        {
            const juce::ScopedLock sl( data->executorsLock );
            data->executors.set( processIdentifier, exe );
        }

        juce::String processName = data->processName( processIdentifier );
        juce::Logger::writeToLog( started ? processName + " Started..." : "Failed to Start " + processName);


        if( auto callback = data->callback )
        {
            started ?
                juce::MessageManager::callAsync(
                    [callback, processIdentifier, processName]() { callback->processStarted( processIdentifier, processName ); } ) :
                juce::MessageManager::callAsync(
                    [callback, processIdentifier, processName]() { callback->failedToStartProcess( processIdentifier, processName ); } );
        }
		
		return started;
	}

	// The process monitor thread reports the exit, as it does for a process
	// that exits by itself. Reporting it here as well reported it twice: with
	// restart on kill enabled, that started two instances of the process, and
	// once the monitor thread had dropped the executor, the second report
	// dereferenced a null one.
	bool MTIPCProcessManager::stopProcess( const int processIdentifier )
	{
		std::shared_ptr<ipc::MTIPCChildProcessExecutor> executor = data->executor( processIdentifier );
		return executor == nullptr || executor->stop();
	}

	bool MTIPCProcessManager::waitForProcessToExit(const int processIdentifier)
	{
		std::shared_ptr<ipc::MTIPCChildProcessExecutor> executor = data->executor( processIdentifier );
		return executor == nullptr || executor->join();
	}

    juce::String MTIPCProcessManager::processName( const int processIdentifier )
    {
        return data->processName( processIdentifier );
    }
}
