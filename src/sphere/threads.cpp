// this thing is somehow required to be able to initialise OLE
#define _WIN32_DCOM

#include <algorithm>
#include "../common/CException.h"
#include "../common/common.h"
#include "../common/CLog.h"
#include "../game/CServer.h"
#include "ProfileTask.h"
#include "threads.h"

#if defined(_WIN32)
	#include <process.h>
	#include <objbase.h>
#elif !defined(_BSD)
#ifndef __APPLE__
	#include <sys/prctl.h>
#endif
#endif


// number of exceptions after which we restart thread and think that the thread have gone in exceptioning loops
#define EXCEPTIONS_ALLOWED	10

// number of milliseconds to wait for a thread to close
#define THREADJOIN_TIMEOUT	60000


// Normal Buffer
SimpleMutex g_tmpStringMutex;
volatile int g_tmpStringIndex = 0;
char g_tmpStrings[THREAD_TSTRING_STORAGE][THREAD_STRING_LENGTH];

// TemporaryString Buffer
SimpleMutex g_tmpTemporaryStringMutex;
volatile int g_tmpTemporaryStringIndex = 0;

struct TemporaryStringStorage
{
	char m_buffer[THREAD_STRING_LENGTH];
	char m_state;
} g_tmpTemporaryStringStorage[THREAD_STRING_STORAGE];

#ifdef _WIN32
#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO
{
	DWORD dwType;
	LPCSTR szName;
	DWORD dwThreadID;
	DWORD dwFlags;
} THREADNAME_INFO;
#pragma pack(pop)

constexpr DWORD MS_VC_EXCEPTION = 0x406D1388;
#endif

void IThread::setThreadName(const char* name)
{
	// register the thread name

	// Unix uses prctl to set thread name
	// thread name must be 16 bytes, zero-padded if shorter
	char name_trimmed[m_nameMaxLength] = { '\0' };	// m_nameMaxLength = 16
    Str_CopyLimitNull(name_trimmed, name, m_nameMaxLength);

#if defined(_WIN32)
#if defined(_MSC_VER)	// TODO: support thread naming when compiling with compilers other than Microsoft's
	// Windows uses THREADNAME_INFO structure to set thread name
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = name_trimmed;
	info.dwThreadID = (DWORD)(-1);
	info.dwFlags = 0;

	__try
	{
		RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	}
#endif
#elif defined(__APPLE__)	// Mac
	pthread_setname_np(name_trimmed);
#elif !defined(_BSD)		// Linux
	prctl(PR_SET_NAME, name_trimmed, 0, 0, 0);
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
	pthread_set_name_np(getCurrentThreadId(), name_trimmed);
#elif defined(__NetBSD__)
	pthread_setname_np(getCurrentThreadId(), "%s", name_trimmed);
#endif
}


/**
 * ThreadHolder
**/
spherethreadlist_t ThreadHolder::m_threads;
size_t ThreadHolder::m_threadCount = 0;
bool ThreadHolder::m_inited = false;
SimpleMutex ThreadHolder::m_mutex;
TlsValue<IThread *> ThreadHolder::m_currentThread;

IThread *ThreadHolder::current()
{
    if (!m_inited)
	    init();

	IThread * thread = m_currentThread;
	if (thread == nullptr)
		return DummySphereThread::getInstance();

    // Uncomment it only for testing purposes, since this method is called very often and we don't need the additional overhead
	//ASSERT( thread->isSameThread(thread->getId()) );

	return thread;
}

void ThreadHolder::push(IThread *thread)
{
    if (!m_inited)
        init();

	SimpleThreadLock lock(m_mutex);
	m_threads.push_back(thread);
	++m_threadCount;
}

void ThreadHolder::pop(IThread *thread)
{
    if (!m_inited)
        init();

    ASSERT(m_threadCount > 0);	// Trying to dequeue thread while no threads are active?

    SimpleThreadLock lock(m_mutex);
    spherethreadlist_t::iterator it = std::find(m_threads.begin(), m_threads.end(), thread);
    ASSERT(it != m_threads.end());	// Ensure that the thread to dequeue is registered
    --m_threadCount;
    m_threads.erase(it);
}

IThread * ThreadHolder::getThreadAt(size_t at)
{
	if ( at > getActiveThreads() )
		return nullptr;

	SimpleThreadLock lock(m_mutex);
	for ( spherethreadlist_t::const_iterator it = m_threads.begin(), end = m_threads.end(); it != end; ++it )
	{
		if ( at == 0 )
			return *it;

		--at;
	}

	return nullptr;
}

void ThreadHolder::init()
{
	ASSERT(!m_inited);
    memset(g_tmpStrings, 0, sizeof(g_tmpStrings));
    memset(g_tmpTemporaryStringStorage, 0, sizeof(g_tmpTemporaryStringStorage));

    m_inited = true;
}

/*
 * AbstractThread
*/
int AbstractThread::m_threadsAvailable = 0;

AbstractThread::AbstractThread(const char *name, IThread::Priority priority)
{
	if( AbstractThread::m_threadsAvailable == 0 )
	{
		// no threads were started before - initialise thread subsystem
#ifdef _WIN32
		if( CoInitializeEx(nullptr, COINIT_MULTITHREADED) != S_OK )
		{
			throw CSError(LOGL_FATAL, 0, "OLE is not available, threading model unimplementable");
		}
#endif
		++AbstractThread::m_threadsAvailable;
	}
	m_id = 0;
	m_name = name;
	m_handle = 0;
	m_hangCheck = 0;
    _thread_selfTerminateAfterThisTick = true;
	m_terminateRequested = true;
	setPriority(priority);
}

AbstractThread::~AbstractThread()
{
	terminate(false);
	--AbstractThread::m_threadsAvailable;
	if( AbstractThread::m_threadsAvailable == 0 )
	{
		// all running threads have gone, the thread subsystem is no longer needed
#ifdef _WIN32
		CoUninitialize();
#else
		// No pthread equivalent
#endif
	}
}

void AbstractThread::start()
{
#ifdef _WIN32
	m_handle = reinterpret_cast<spherethread_t>(_beginthreadex(nullptr, 0, &runner, this, 0, nullptr));
#else
	pthread_attr_t threadAttr;
	pthread_attr_init(&threadAttr);
	pthread_attr_setdetachstate(&threadAttr, PTHREAD_CREATE_DETACHED);
	int result = pthread_create( &m_handle, &threadAttr, &runner, this );
	pthread_attr_destroy(&threadAttr);

	if (result != 0)
	{
		m_handle = 0;
		throw CSError(LOGL_FATAL, 0, "Unable to spawn a new thread");
	}
#endif

	m_terminateEvent.reset();
	ThreadHolder::push(this);
}

void AbstractThread::terminate(bool ended)
{
	if( isActive() )
	{
		bool wasCurrentThread = isCurrentThread();
		if (ended == false)
		{
			g_Log.Event(LOGL_WARN, "Forcing thread '%s' to terminate...\n", getName());

			// if the thread is current then terminating here will prevent cleanup from occurring
			if (wasCurrentThread == false)
			{
#ifdef _WIN32
				TerminateThread(m_handle, 0);
				CloseHandle(m_handle);
#else
				pthread_cancel(m_handle); // IBM say it so
#endif
			}
		}

		// Common things
		ThreadHolder::pop(this);
		m_id = 0;
		m_handle = 0;

		// let everyone know we have been terminated
		m_terminateEvent.set();

		// current thread can be terminated now
		if (ended == false && wasCurrentThread)
		{
#ifdef _WIN32
			_endthreadex(EXIT_SUCCESS);
#else
			pthread_exit(EXIT_SUCCESS);
#endif
		}
	}
}

void AbstractThread::run()
{
	// is the very first since there is a possibility of something being altered there
	onStart();

	int exceptions = 0;
	bool lastWasException = false;
    _thread_selfTerminateAfterThisTick = false;
	m_terminateRequested = false;

	for (;;)
	{
        if (shouldExit())
            break;

		bool gotException = false;

		//	report me being alive if I am being checked for status
		if (m_hangCheck != 0)
		{
			m_hangCheck = 0;
		}

		try
		{
			tick();

            if (shouldExit())
                break;

            // ensure this is recorded as 'idle' time for this thread (ideally this should
            // be in tick() but we cannot guarantee it to be called there
            CurrentProfileData.Start(PROFILE_IDLE);
		}
        /*
        catch( const CAssert& e )
        {
            gotException = true;
            g_Log.CatchEvent(&e, "[TR] ExcType=CAssert in %s::tick", getName());
            CurrentProfileData.Count(PROFILE_STAT_FAULTS, 1);
        }
        */
		catch( const CSError& e )
		{
			gotException = true;
			g_Log.CatchEvent(&e, "[TR] ExcType=CSError in %s::tick", getName());
			CurrentProfileData.Count(PROFILE_STAT_FAULTS, 1);
		}
        catch( const std::exception& e )
        {
            gotException = true;
            g_Log.CatchStdException(&e, "[TR] ExcType=std::exception in %s::tick", getName());
            CurrentProfileData.Count(PROFILE_STAT_FAULTS, 1);
        }
		catch( ... )
		{
			gotException = true;
			g_Log.CatchEvent(nullptr, "[TR] ExcType=pure in %s::tick", getName());
			CurrentProfileData.Count(PROFILE_STAT_FAULTS, 1);
		}

		if( gotException )
		{
			if( lastWasException )
			{
				++exceptions;
			}
			else
			{
				lastWasException = true;
				exceptions = 0;
			}
			if( exceptions >= EXCEPTIONS_ALLOWED )
			{
				// a bad thing really happened. ALL previous EXCEPTIONS_ALLOWED ticks resulted in exception
				// almost for sure we have looped somewhere and have no way to get out from this situation
				// probably a thread restart can fix the problems
				// but there is no real need to restart a thread, we will just simulate a thread restart,
				// notifying a subclass like we have been just restarted, so it will restart it's operations
				g_Log.Event(LOGL_CRIT, "'%s' thread raised too many exceptions, restarting...\n", getName());
				onStart();
				lastWasException = false;
			}
		}
		else
		{
			lastWasException = false;
		}

		if( shouldExit() )
			break;

		m_sleepEvent.wait(m_tickPeriod);
	}
}

SPHERE_THREADENTRY_RETNTYPE AbstractThread::runner(void *callerThread)
{
	// If the caller thread is an AbstractThread, call run, which starts it and make it enter in its main loop
	AbstractThread * caller = reinterpret_cast<AbstractThread*>(callerThread);
	if (caller != nullptr)
	{
		caller->run();
		caller->terminate(true);
	}

	return 0;
}

bool AbstractThread::isActive() const
{
	return (m_handle != 0);
}

void AbstractThread::waitForClose()
{
    // Another thread has requested us to close and it's waiting for us to complete the current tick, 
    //  or to forcefully be forcefully terminated after a THREADJOIN_TIMEOUT, which of the two happens first.

    // TODO? add a mutex here to protect at least the changes to m_terminateRequested?
	if (isActive())
	{
		if (isCurrentThread() == false)
		{
			// flag that we want the thread to terminate
			m_terminateRequested = true;
			awaken();

			// give the thread a chance to close on its own, and then
			// terminate anyway
			m_terminateEvent.wait(THREADJOIN_TIMEOUT);
		}

		terminate(false);
	}
}

void AbstractThread::awaken()
{
	m_sleepEvent.signal();
}

bool AbstractThread::isCurrentThread() const
{
#ifdef _WIN32
	return (getId() == ::GetCurrentThreadId());
#else
	return pthread_equal(m_handle,pthread_self());
#endif
}

bool AbstractThread::checkStuck()
{
	if( isActive() )
	{
		if( m_hangCheck == 0 )
		{
			//	initiate hang check
			m_hangCheck = 0xDEAD;
		}
		else if( m_hangCheck == 0xDEAD )
		{
			//	one time period was not answered, wait a bit more
			m_hangCheck = 0xDEADDEADl;
			//	TODO:
			//g_Log.Event(LOGL_CRIT, "'%s' thread seems being hang (frozen) at '%s'?\n", m_name, m_action);
		}
		else if( m_hangCheck == 0xDEADDEADl )
		{
			//	TODO:
			//g_Log.Event(LOGL_CRIT, "'%s' thread hang, restarting...\n", m_name);
			#ifdef THREAD_TRACK_CALLSTACK
				static_cast<AbstractSphereThread*>(this)->printStackTrace();
			#endif
			terminate(false);
			run();
			start();
			return true;
		}
	}

	return false;
}

void AbstractThread::onStart()
{
	// start-up actions for each thread
	// when implemented in derived classes this method must always be called too, preferably before
	// the custom implementation

	// we set the id here to ensure it is available before the first tick, otherwise there's
	// a small delay when setting it from AbstractThread::start and it's possible for the id
	// to not be set fast enough (particular when using pthreads)
	m_id = getCurrentThreadId();
	ThreadHolder::m_currentThread = this;

	if (isActive())		// This thread has actually been spawned and the code is executing on a different thread
		setThreadName(getName());
}

void AbstractThread::setPriority(IThread::Priority pri)
{
	ASSERT(((pri >= IThread::Idle) && (pri <= IThread::RealTime)) || (pri == IThread::Disabled));
	m_priority = pri;

	// detect a sleep period for thread depending on priority
	switch( m_priority )
	{
		case IThread::Idle:
			m_tickPeriod = 1000;
			break;
		case IThread::Low:
			m_tickPeriod = 200;
			break;
		case IThread::Normal:
			m_tickPeriod = 100;
			break;
		case IThread::High:
			m_tickPeriod = 50;
			break;
		case IThread::Highest:
			m_tickPeriod = 5;
			break;
		case IThread::RealTime:
			m_tickPeriod = 0;
			break;
		case IThread::Disabled:
			m_tickPeriod = AutoResetEvent::_kiInfinite;
			break;
	}
}

bool AbstractThread::shouldExit()
{
	return m_terminateRequested || _thread_selfTerminateAfterThisTick;
}

/*
 * AbstractSphereThread
*/
AbstractSphereThread::AbstractSphereThread(const char *name, Priority priority)
	: AbstractThread(name, priority)
{
#ifdef THREAD_TRACK_CALLSTACK
	m_stackPos = 0;
	memset(m_stackInfo, 0, sizeof(m_stackInfo));
	m_freezeCallStack = false;
    m_exceptionStackUnwinding = false;
#endif

	// profiles that apply to every thread
	m_profile.EnableProfile(PROFILE_IDLE);
	m_profile.EnableProfile(PROFILE_OVERHEAD);
	m_profile.EnableProfile(PROFILE_STAT_FAULTS);
}

// IMHO we need a lock on allocateBuffer and allocateStringBuffer

char *AbstractSphereThread::allocateBuffer()
{
	SimpleThreadLock stlBuffer(g_tmpStringMutex);

	char * buffer = nullptr;
	++g_tmpStringIndex;

	if( g_tmpStringIndex >= THREAD_TSTRING_STORAGE )
	{
		g_tmpStringIndex %= THREAD_TSTRING_STORAGE;
	}

	buffer = g_tmpStrings[g_tmpStringIndex];
	*buffer = '\0';

	return buffer;
}

TemporaryStringStorage *AbstractSphereThread::allocateStringBuffer()
{
	int initialPosition = g_tmpTemporaryStringIndex;
	int index;
	for (;;)
	{
		index = ++g_tmpTemporaryStringIndex;
		if( g_tmpTemporaryStringIndex >= THREAD_STRING_STORAGE )
		{
			index = g_tmpTemporaryStringIndex %= THREAD_STRING_STORAGE;
		}

		if( g_tmpTemporaryStringStorage[index].m_state == 0 )
		{
			TemporaryStringStorage * store = &g_tmpTemporaryStringStorage[index];
			*store->m_buffer = '\0';
			return store;
		}

		// a protection against deadlock. All string buffers are marked as being used somewhere, so we
		// have few possibilities (the case shows that we have a bug and temporary strings used not such):
		// a) return nullptr and wait for exceptions in the program
		// b) allocate a string from a heap
		if( initialPosition == index )
		{
			// but the best is to throw an exception to give better formed information for end users
			// rather than access violations
			DEBUG_WARN(( "Thread temporary string buffer is full.\n" ));
			throw CSError(LOGL_FATAL, 0, "Thread temporary string buffer is full");
		}
	}
}

void AbstractSphereThread::allocateString(TemporaryString &string)
{
	SimpleThreadLock stlBuffer(g_tmpTemporaryStringMutex);

	TemporaryStringStorage * store = allocateStringBuffer();
	string.init(store->m_buffer, &store->m_state);
}

bool AbstractSphereThread::shouldExit()
{
	if ( g_Serv.GetExitFlag() != 0 )
		return true;

	return AbstractThread::shouldExit();
}

void AbstractSphereThread::exceptionCaught()
{
#ifdef THREAD_TRACK_CALLSTACK
    if (m_exceptionStackUnwinding == false)
        printStackTrace();
    else
        m_exceptionStackUnwinding = false;
#endif
}

#ifdef THREAD_TRACK_CALLSTACK
void AbstractSphereThread::pushStackCall(const char *name) noexcept
{
    if (m_freezeCallStack == false)
    {
        m_stackInfo[m_stackPos].functionName = name;
        m_stackInfo[m_stackPos].startTime = CSTime::GetPreciseSysTimeMicro();
        ++m_stackPos;
        m_stackInfo[m_stackPos].startTime = 0;
    }
}

void AbstractSphereThread::exceptionNotifyStackUnwinding(void)
{
    //ASSERT(isCurrentThread());
    if (m_exceptionStackUnwinding == false)
    {
        m_exceptionStackUnwinding = true;
        printStackTrace();
    }
}

void AbstractSphereThread::printStackTrace()
{
	// don't allow call stack to be modified whilst we're printing it
	freezeCallStack(true);
    
    const uint threadId = getId();
    const lpctstr threadName = getName();
    llong startTime = m_stackInfo[0].startTime;

	g_Log.EventDebug("Printing STACK TRACE for debugging purposes.\n");
    // On Windows versions prior to XP, startTime is the number of ticks, not microseconds
	g_Log.EventDebug(" __ thread (id) name __ |  # | _____________ function _____________ | microseconds passed from previous function start\n");
	for ( size_t i = 0; i < CountOf(m_stackInfo); ++i )
	{
		if( m_stackInfo[i].startTime == 0 )
			break;

        const bool origin = (i == (m_stackPos - 1));
        lpctstr extra = " ";
        if (origin)
        {
            if (m_exceptionStackUnwinding)
                extra = "<-- last function call (stack unwinding began here)";
            else
                extra = "<-- exception catch point (below is guessed and could be incorrect!)";
        }

        const int timedelta = (int)(m_stackInfo[i].startTime - startTime);
		g_Log.EventDebug("(%0.5u)%16.16s | %2u | %36.36s | +%d %s\n",
			threadId, threadName, (uint)i, m_stackInfo[i].functionName, timedelta, extra);

		startTime = m_stackInfo[i].startTime;
	}

	freezeCallStack(false);
}
#endif

/*
 * DummySphereThread
*/
DummySphereThread *DummySphereThread::_instance = nullptr;

DummySphereThread::DummySphereThread()
	: AbstractSphereThread("dummy", IThread::Normal)
{
}

void DummySphereThread::createInstance() // static
{
	// This dummy thread is created at the very beginning of the application startup.
	//  Before the server becomes operational, this won't be used anymore, since fully functional threads will be created.

	// Create this only once, it has to be one of the first operations to be done when the application starts.
	ASSERT(_instance == nullptr);
	_instance = new DummySphereThread();
}

DummySphereThread *DummySphereThread::getInstance() noexcept // static
{
	return _instance;
}

void DummySphereThread::tick()
{
}


/*
* StackDebugInformation
*/

#ifdef THREAD_TRACK_CALLSTACK

StackDebugInformation::StackDebugInformation(const char *name)
{
    m_context = static_cast<AbstractSphereThread *>(ThreadHolder::current());
    if (m_context != nullptr)
        m_context->pushStackCall(name);
}

StackDebugInformation::~StackDebugInformation()
{
    ASSERT(m_context != nullptr);

#if __cplusplus >= __cpp_lib_uncaught_exceptions
    if (std::uncaught_exceptions() != 0)
#else
    if (std::uncaught_exception()) // deprecated in C++17
#endif
    {
        // Exception was thrown and stack unwinding is in progress.
        m_context->exceptionNotifyStackUnwinding();
    }
    m_context->popStackCall();
}

#endif // THREAD_TRACK_CALLSTACK
