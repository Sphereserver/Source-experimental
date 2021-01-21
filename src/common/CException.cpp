
#include "CException.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <csignal>
#include <cstring>

#include "../game/CServer.h"
#include "../game/CWorld.h"

#ifdef _DEBUG
#include <fcntl.h>
int IsDebuggerPresent(void)
{
	char buf[1024];
	int debugger_present = 0;

	int status_fd = open("/proc/self/status", O_RDONLY);
	if (status_fd == -1)
		return 0;

	ssize_t num_read = read(status_fd, buf, sizeof(buf)-1);

	if (num_read > 0)
	{
		static const char TracerPid[] = "TracerPid:";
		char *tracer_pid;

		buf[num_read] = 0;
		tracer_pid    = strstr(buf, TracerPid);
		if (tracer_pid)
			debugger_present = !!atoi(tracer_pid + sizeof(TracerPid) - 1);
	}

	return debugger_present;
}
#endif // _DEBUG

#endif // !_WIN32


#ifdef _DEBUG
void NotifyDebugger()
{
    if (IsDebuggerPresent())
    {

#ifdef _WIN32
    #ifdef _MSC_VER
        __debugbreak();
    #else
        std::abort();
    #endif
#else
        std::raise(SIGINT);
#endif

    }
}
#endif // _DEBUG


#ifdef _WIN32
int CSError::GetSystemErrorMessage(dword dwError, lptstr lpszError, uint nMaxError) // static
{
    //	PURPOSE:  copies error message text to a string
    //
    //	PARAMETERS:
    //		lpszBuf - destination buffer
    //		dwSize - size of buffer
    //
    //	RETURN VALUE:
    //		destination buffer

    LPCVOID lpSource = nullptr;
    va_list* Arguments = nullptr;
    DWORD nChars = ::FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        lpSource,
        dwError, LANG_NEUTRAL,
        lpszError, nMaxError, Arguments);

    if (nChars > (DWORD)0)
    {     // successful translation -- trim any trailing junk
        DWORD index = nChars - 1;      // index of last character
        while ((lpszError[index] == '\n') || (lpszError[index] == '\r'))
            lpszError[index--] = '\0';
        nChars = index + 1;
    }

    return nChars;
}
#endif

bool CSError::GetErrorMessage( lptstr lpszError, uint uiMaxError ) const
{
#ifdef _WIN32
	// Compatible with CException and CSFileException
	if ( m_hError )
	{
		// return the message defined by the system for the error code
		tchar szCode[ 1024 ];
		int nChars = GetSystemErrorMessage( m_hError, szCode, sizeof(szCode) );
		if ( nChars )
		{
			if ( m_hError & 0x80000000 )
				snprintf( lpszError, uiMaxError, "Error Pri=%d, Code=0x%x(%s), Desc='%s'", m_eSeverity, m_hError, szCode, m_pszDescription );
			else
				snprintf( lpszError, uiMaxError, "Error Pri=%d, Code=%u(%s), Desc='%s'", m_eSeverity, m_hError, szCode, m_pszDescription );
			return true;
		}
	}
#endif

	if ( m_hError & 0x80000000 )
		snprintf( lpszError, uiMaxError, "Error Pri=%d, Code=0x%x, Desc='%s'", m_eSeverity, m_hError, m_pszDescription );
	else
		snprintf( lpszError, uiMaxError, "Error Pri=%d, Code=%u, Desc='%s'", m_eSeverity, m_hError, m_pszDescription );
	return true;
}

CSError::CSError( const CSError &e ) :
	m_eSeverity( e.m_eSeverity ),
	m_hError( e.m_hError ),
	m_pszDescription( e.m_pszDescription )
{
}

CSError::CSError( LOG_TYPE eSev, dword hErr, lpctstr pszDescription ) :
	m_eSeverity( eSev ),
	m_hError( hErr ),
	m_pszDescription( pszDescription )
{
}


// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------


bool CAssert::GetErrorMessage(lptstr lpszError, uint uiMaxError) const
{
	snprintf(lpszError, uiMaxError, "Assert severity=%d: '%s' file '%s', line %lld", m_eSeverity, m_pExp, m_pFile, m_llLine);
	return true;
}

// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------

#ifdef _WIN32

CWinException::CWinException(uint uCode, size_t pAddress) :
	CSError(LOGL_CRIT, uCode, "Exception"), m_pAddress(pAddress)
{
}

CWinException::~CWinException()
{
}

bool CWinException::GetErrorMessage(lptstr lpszError, uint uiMaxError) const
{
	lpctstr zMsg;
	switch ( m_hError )
	{
		case STATUS_BREAKPOINT:				zMsg = "Breakpoint";				break;
		case STATUS_ACCESS_VIOLATION:		zMsg = "Access Violation";			break;
		case STATUS_FLOAT_DIVIDE_BY_ZERO:	zMsg = "Float: Divide by Zero";		break;
		case STATUS_INTEGER_DIVIDE_BY_ZERO:	zMsg = "Integer: Divide by Zero";	break;
		case STATUS_STACK_OVERFLOW:			zMsg = "Stack Overflow";			break;
		default:
			snprintf(lpszError, uiMaxError, "code=0x%x, (0x%" PRIxSIZE_T ")", m_hError, m_pAddress);
			return true;
	}

	snprintf(lpszError, uiMaxError, "\"%s\" (0x%" PRIxSIZE_T ")", zMsg, m_pAddress);
	return true;
}

#endif

// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------
// --------------------------------------------------------------------------------

void Assert_Fail( lpctstr pExp, lpctstr pFile, long long llLine )
{
	EXC_NOTIFY_DEBUGGER;
	throw CAssert(LOGL_CRIT, pExp, pFile, llLine);
}

void _cdecl Sphere_Purecall_Handler()
{
	// catch this special type of C++ exception as well.
	Assert_Fail("purecall", "unknown", 1);
}

void SetPurecallHandler()
{
	// We don't want sphere to immediately exit if a pure call is done.
#ifdef _MSC_VER
	_set_purecall_handler(Sphere_Purecall_Handler);
#else
	// GCC handler for pure calls is __cxxabiv1::__cxa_pure_virtual.
	// Its code calls std::terminate(), which then calls abort(), so we set the terminate handler to redefine this behaviour.
	std::set_terminate(Sphere_Purecall_Handler);
#endif
}

#if defined(_WIN32) && !defined(_DEBUG)

#include "crashdump/crashdump.h"

void _cdecl Sphere_Exception_Windows( unsigned int id, struct _EXCEPTION_POINTERS* pData )
{
#ifndef _NO_CRASHDUMP
	if ( CrashDump::IsEnabled() )
		CrashDump::StartCrashDump(GetCurrentProcessId(), GetCurrentThreadId(), pData);

#endif
	// WIN32 gets an exception.
	size_t pCodeStart = (size_t)(byte *) &globalstartsymbol;	// sync up to my MAP file.

	size_t pAddr = (size_t)pData->ExceptionRecord->ExceptionAddress;
	pAddr -= pCodeStart;

	throw CWinException(id, pAddr);
}

#endif // _WIN32 && !_DEBUG

void SetExceptionTranslator()
{
#if defined(_MSC_VER) && !defined(_DEBUG)
	_set_se_translator( Sphere_Exception_Windows );
#endif
}

#ifndef _WIN32
void _cdecl Signal_Hangup(int sig = 0) // If shutdown is initialized
{
    UNREFERENCED_PARAMETER(sig);

#ifdef THREAD_TRACK_CALLSTACK
    static bool _Signal_Hangup_stack_printed = false;
    if (!_Signal_Hangup_stack_printed)
    {
        StackDebugInformation::printStackTrace();
        _Signal_Hangup_stack_printed = true;
    }
#endif

    if (!g_Serv.m_fResyncPause)
        g_World.Save(true);

    g_Serv.SetExitFlag(SIGHUP);
}

void _cdecl Signal_Terminate(int sig = 0) // If shutdown is initialized
{
    sigset_t set;

    g_Log.Event(LOGL_FATAL, "Server Unstable: %s\n", strsignal(sig));
#ifdef THREAD_TRACK_CALLSTACK
    static bool _Signal_Terminate_stack_printed = false;
    if (!_Signal_Terminate_stack_printed)
    {
        StackDebugInformation::printStackTrace();
        _Signal_Terminate_stack_printed = true;
    }
#endif

    if (sig)
    {
        signal(sig, &Signal_Terminate);
        sigemptyset(&set);
        sigaddset(&set, sig);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
    }

    g_Serv.SetExitFlag(SIGABRT);
    for (size_t i = 0; i < ThreadHolder::getActiveThreads(); ++i)
        ThreadHolder::getThreadAt(i)->terminate(false);
    //exit(EXIT_FAILURE);
}

void _cdecl Signal_Break(int sig = 0)		// signal handler attached when using secure mode
{
    // Shouldn't be needed, since gdb consumes the signals itself and this code won't be executed
    //#ifdef _DEBUG
    //		if (IsDebuggerPresent())
    //			return;		// do not block this signal, so that the debugger can catch it
    //#endif

    g_Log.Event(LOGL_FATAL, "Secure Mode prevents CTRL+C\n");

    sigset_t set;
    if (sig)
    {
        signal(sig, &Signal_Break);
        sigemptyset(&set);
        sigaddset(&set, sig);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
    }
}

void _cdecl Signal_Illegal_Instruction(int sig = 0)
{
#ifndef __APPLE__
    StackDebugInformation::freezeCallStack(true);
    sigset_t set;

    g_Log.Event(LOGL_FATAL, "%s\n", strsignal(sig));
    StackDebugInformation::printStackTrace();

    if (sig)
    {
        signal(sig, &Signal_Illegal_Instruction);
        sigemptyset(&set);
        sigaddset(&set, sig);
        sigprocmask(SIG_UNBLOCK, &set, nullptr);
    }

    StackDebugInformation::freezeCallStack(false);

    throw CSError(LOGL_FATAL, sig, strsignal(sig));
#endif
}

void _cdecl Signal_Children(int sig = 0)
{
    UNREFERENCED_PARAMETER(sig);
    while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) {}
}
#endif

#ifndef _WIN32
void SetUnixSignals( bool bSet )    // Signal handlers are installed only in secure mode
{
	signal( SIGHUP,		bSet ? &Signal_Hangup : SIG_DFL );
	signal( SIGTERM,	bSet ? &Signal_Terminate : SIG_DFL );
	signal( SIGQUIT,	bSet ? &Signal_Terminate : SIG_DFL );
	signal( SIGABRT,	bSet ? &Signal_Terminate : SIG_DFL );
	signal( SIGILL,		bSet ? &Signal_Terminate : SIG_DFL );
	signal( SIGINT,		bSet ? &Signal_Break : SIG_DFL );
	signal( SIGSEGV,	bSet ? &Signal_Illegal_Instruction : SIG_DFL );
	signal( SIGFPE,		bSet ? &Signal_Illegal_Instruction : SIG_DFL );
	signal( SIGPIPE,	bSet ? SIG_IGN : SIG_DFL );
	signal( SIGCHLD,	bSet ? &Signal_Children : SIG_DFL );
}
#endif
