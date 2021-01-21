#include "sresetevents.h"

// AutoResetEvent:: Constructors, Destructor, Assign operator.

AutoResetEvent::AutoResetEvent()
{
#ifdef _WIN32
	m_handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
#else
	pthread_mutexattr_init(&m_criticalSectionAttr);
#ifndef __APPLE__
	pthread_mutexattr_settype(&m_criticalSectionAttr, PTHREAD_MUTEX_RECURSIVE_NP);
#endif
	pthread_mutex_init(&m_criticalSection, &m_criticalSectionAttr);

	pthread_condattr_init(&m_conditionAttr);
	pthread_cond_init(&m_condition, &m_conditionAttr);
#endif
}

AutoResetEvent::~AutoResetEvent()
{
#ifdef _WIN32
	CloseHandle(m_handle);
#else
	pthread_condattr_destroy(&m_conditionAttr);
	pthread_cond_destroy(&m_condition);
	pthread_mutexattr_destroy(&m_criticalSectionAttr);
	pthread_mutex_destroy(&m_criticalSection);
#endif
}

// AutoResetEvent:: Interaction.

void AutoResetEvent::wait(uint timeout)
{
	if (timeout == 0)
	{
		// if timeout is 0 then the thread's timeslice may not be given up as with normal
		// sleep methods - so we will check for this condition ourselves and use SleepEx
		// instead
		SleepEx(0, TRUE);
		return;
	}

#ifdef _WIN32
	// it's possible for WaitForSingleObjectEx to exit before the timeout and without
	// the signal. due to bAlertable=TRUE other events (e.g. async i/o) can cancel the
	// waiting period early.
	WaitForSingleObjectEx(m_handle, timeout, TRUE);
#else
	pthread_mutex_lock(&m_criticalSection);

	// it's possible for pthread_cond_wait/timedwait to exit before the timeout and
	// without a signal, but there's little we can actually do to check it since we
	// don't usually care about the condition - if the calling thread does care then
	// it needs to implement it's own checks
	if (timeout == _kiInfinite)
	{
		pthread_cond_wait(&m_condition, &m_criticalSection);
	}
	else
	{
		// pthread_cond_timedwait expects the timeout to be the actual time
		timespec time;
		clock_gettime(CLOCK_REALTIME, &time);
		time.tv_sec += timeout / 1000;
		time.tv_nsec += (timeout % 1000) * 1000000L;

		pthread_cond_timedwait(&m_condition, &m_criticalSection, &time);
	}

	pthread_mutex_unlock(&m_criticalSection);
#endif
}

void AutoResetEvent::signal()
{
#ifdef _WIN32
	SetEvent(m_handle);
#else
	pthread_mutex_lock(&m_criticalSection);
	pthread_cond_signal(&m_condition);
	pthread_mutex_unlock(&m_criticalSection);
#endif
}

// ManualResetEvent:: Constructors, Destructor, Asign operator.

ManualResetEvent::ManualResetEvent()
{
#ifdef _WIN32
	m_handle = CreateEvent(nullptr, TRUE, FALSE, nullptr);
#else
	m_value = false;
	pthread_mutexattr_init(&m_criticalSectionAttr);
#ifndef __APPLE__
	pthread_mutexattr_settype(&m_criticalSectionAttr, PTHREAD_MUTEX_RECURSIVE_NP);
#endif
	pthread_mutex_init(&m_criticalSection, &m_criticalSectionAttr);

	pthread_condattr_init(&m_conditionAttr);
	pthread_cond_init(&m_condition, &m_conditionAttr);
#endif
}

ManualResetEvent::~ManualResetEvent()
{
#ifdef _WIN32
	CloseHandle(m_handle);
#else
	pthread_condattr_destroy(&m_conditionAttr);
	pthread_cond_destroy(&m_condition);
	pthread_mutexattr_destroy(&m_criticalSectionAttr);
	pthread_mutex_destroy(&m_criticalSection);
#endif
}

// ManualResetEvent:: Interaction.

void ManualResetEvent::wait(uint timeout)
{
#ifdef _WIN32
	WaitForSingleObjectEx(m_handle, timeout, FALSE);
#else
	pthread_mutex_lock(&m_criticalSection);

	// pthread_cond_timedwait expects the timeout to be the actual time, calculate once here
	// rather every time inside the loop
	timespec time;
	if (timeout != _kiInfinite)
	{
		clock_gettime(CLOCK_REALTIME, &time);
		time.tv_sec += timeout / 1000;
		time.tv_nsec += (timeout % 1000) * 1000000L;
	}

	// it's possible for pthread_cond_wait/timedwait to exit before the timeout and
	// without a signal
	int result = 0;
	while (result == 0 && m_value == false)
	{
		if (timeout == _kiInfinite)
			result = pthread_cond_wait(&m_condition, &m_criticalSection);
		else
			result = pthread_cond_timedwait(&m_condition, &m_criticalSection, &time);
	}

	pthread_mutex_unlock(&m_criticalSection);
#endif
}

void ManualResetEvent::set()
{
#ifdef _WIN32
	SetEvent(m_handle);
#else
	pthread_mutex_lock(&m_criticalSection);
	m_value = true;
	pthread_cond_signal(&m_condition);
	pthread_mutex_unlock(&m_criticalSection);
#endif
}

void ManualResetEvent::reset()
{
#ifdef _WIN32
	ResetEvent(m_handle);
#else
	pthread_mutex_lock(&m_criticalSection);
	m_value = false;
	pthread_cond_signal(&m_condition);
	pthread_mutex_unlock(&m_criticalSection);
#endif
}
