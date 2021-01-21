#include "../game/clients/CClient.h"
#include "../game/CServerConfig.h"
#include "../sphere/threads.h"
#include "../sphere/ProfileTask.h"
#include "packet.h"
#include "CNetState.h"
#include "CNetworkThread.h"
#include "CNetworkOutput.h"


#ifdef _WIN32
#include <WinSock2.h>

/***************************************************************************
*
*	void SendCompleted_Winsock			Winsock event handler for when async operation completes
*
***************************************************************************/
void CALLBACK SendCompleted_Winsock(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
{
	UNREFERENCED_PARAMETER(dwFlags);
	ADDTOCALLSTACK("SendCompleted_Winsock");

	CNetState* state = reinterpret_cast<CNetState*>(lpOverlapped->hEvent);
	if (state == nullptr)
	{
		DEBUGNETWORK(("Async i/o operation (Winsock) completed without client context.\n"));
		return;
	}

	CNetworkThread* thread = state->getParentThread();
	if (thread == nullptr)
	{
		DEBUGNETWORK(("%x:Async i/o operation (Winsock) completed (single-threaded, or no thread context).\n", state->id()));
		// This wasn't called by a CNetworkThread, so we can't do onAsyncSendComplete.
		state->setSendingAsync(false);	// new addition. is it breaking something or instead fixing things?
		return;
	}

	if (dwError != 0)
		DEBUGNETWORK(("%x:Async i/o operation (Winsock) completed with error code 0x%x, %d bytes sent.\n", state->id(), dwError, cbTransferred));
	//else
	//	DEBUGNETWORK(("%x:Async i/o operation (Winsock) completed successfully, %d bytes sent.\n", state->id(), cbTransferred));

	thread->onAsyncSendComplete(state, dwError == 0 && cbTransferred > 0);
}

#endif



CNetworkOutput::CNetworkOutput() : m_thread(nullptr)
{
	m_encryptBuffer = new byte[MAX_BUFFER];
}

CNetworkOutput::~CNetworkOutput()
{
	if (m_encryptBuffer != nullptr)
		delete[] m_encryptBuffer;
}

bool CNetworkOutput::processOutput()
{
	ADDTOCALLSTACK("CNetworkOutput::processOutput");
	ASSERT(m_thread != nullptr);
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	const ProfileTask networkTask(PROFILE_NETWORK_TX);

	static uchar tick = 0;
	EXC_TRY("CNetworkOutput");
	++tick;

	// decide which queues need to be processed
	bool toProcess[PacketSend::PRI_QTY];
	if (m_thread->isActive())
	{
		toProcess[PacketSend::PRI_HIGHEST]  = true;
		toProcess[PacketSend::PRI_HIGH]     = ((tick %  2) ==  1);
		toProcess[PacketSend::PRI_NORMAL]   = ((tick %  4) ==  3);
		toProcess[PacketSend::PRI_LOW]      = ((tick %  8) ==  7);
		toProcess[PacketSend::PRI_IDLE]     = ((tick % 16) == 15);
	}
	else
	{
		// we receive less ticks in single-threaded mode, so process packet
		// queues a bit faster
		toProcess[PacketSend::PRI_HIGHEST]	= true;
		toProcess[PacketSend::PRI_HIGH]		= true;
		toProcess[PacketSend::PRI_NORMAL]	= true;
		toProcess[PacketSend::PRI_LOW]		= ((tick % 2) == 1);
		toProcess[PacketSend::PRI_IDLE]		= ((tick % 4) == 3);
	}

	// process clients which have been marked for flushing
	EXC_SET_BLOCK("flush");
	checkFlushRequests();

	size_t packetsSent = 0;
	NetworkThreadStateIterator states(m_thread);
	while (CNetState* state = states.next())
	{
		if (state->isWriteClosed())
			continue;

		// process packet queues
		for (int priority = PacketSend::PRI_HIGHEST; priority >= 0; --priority)
		{
			if (toProcess[priority] == false)
				continue;
			else if (state->isWriteClosed())
				break;
			packetsSent += processPacketQueue(state, priority);
		}

		// process asynchronous queue
		if (state->isWriteClosed() == false)
			packetsSent += processAsyncQueue(state);

		// process byte queue
		if (state->isWriteClosed() == false && processByteQueue(state))
			++packetsSent;
	}

	if (packetsSent > 0)
	{
		// notify thread there could be more to process
		if (m_thread->getPriority() == IThread::Disabled)
			m_thread->awaken();
	}

	return packetsSent > 0;
	EXC_CATCH;
	EXC_DEBUG_START;
	g_Log.EventDebug("ActiveThread=%d, TickCount=%d\n", m_thread->isActive()? 1 : 0, tick);
	EXC_DEBUG_END;
	return false;
}

void CNetworkOutput::checkFlushRequests(void)
{
	// check for clients who need data flushing
	ADDTOCALLSTACK("CNetworkOutput::checkFlushRequests");
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	NetworkThreadStateIterator states(m_thread);
	while (CNetState* state = states.next())
	{
		if (state->isWriteClosed())
			continue;

		if (state->needsFlush())
			flush(state);

		if (state->isClosing() && state->hasPendingData() == false)
			state->markWriteClosed();
	}
}

size_t CNetworkOutput::flush(CNetState* state)
{
	// process all queues for a client
	ADDTOCALLSTACK("CNetworkOutput::flush");
	ASSERT(state);
	ASSERT(m_thread);

	if (state->isInUse() == false || state->isClosed())
		return 0;

	if (m_thread->isActive() && m_thread->isCurrentThread() == false)
	{
		// when this isn't the active thread, all we can do is raise a request to flush this
		// client later
		state->markFlush(true);
		if (m_thread->getPriority() == IThread::Disabled)
			m_thread->awaken();

		return 0;
	}

	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());
	size_t packetsSent = 0;
	for (int priority = PacketSend::PRI_HIGHEST; priority >= 0; --priority)
	{
		if (state->isWriteClosed())
			break;
		packetsSent += processPacketQueue(state, priority);
	}

	if (state->isWriteClosed() == false)
		packetsSent += processAsyncQueue(state);

	if (state->isWriteClosed() == false && processByteQueue(state))
		++packetsSent;

	state->markFlush(false);
	return packetsSent;
}

size_t CNetworkOutput::processPacketQueue(CNetState* state, uint priority)
{
	// process a client's packet queue
	ADDTOCALLSTACK("CNetworkOutput::processPacketQueue");
	ASSERT(state != nullptr);
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	if (state->isWriteClosed() ||
		(state->m_outgoing.queue[priority].empty() && state->m_outgoing.currentTransaction == nullptr))
		return 0;

	CClient* client = state->getClient();
	ASSERT(client != nullptr);

	size_t maxPacketsToProcess = NETWORK_MAXPACKETS;
	size_t maxLengthToProcess = NETWORK_MAXPACKETLEN;
	size_t packetsProcessed = 0;
	size_t lengthProcessed = 0;

	while (packetsProcessed < maxPacketsToProcess && lengthProcessed < maxLengthToProcess)
	{
		// select next transaction
		while (state->m_outgoing.currentTransaction == nullptr)
		{
			if (state->m_outgoing.queue[priority].empty())
				break;

			state->m_outgoing.currentTransaction = state->m_outgoing.queue[priority].front();
			state->m_outgoing.queue[priority].pop();
		}

		PacketTransaction* transaction = state->m_outgoing.currentTransaction;
		if (transaction == nullptr)
			break;

		// acquire next packet from transaction
		PacketSend* packet = transaction->front();
		transaction->pop();

		// if the transaction is now empty we can clear it now so we can move
		// on to the next transaction later
		if (transaction->empty())
		{
			state->m_outgoing.currentTransaction = nullptr;
			delete transaction;
		}

		// check if the packet is allowed this packet
		if (state->canReceive(packet) == false || packet->onSend(client) == false)
		{
			delete packet;
			continue;
		}

		EXC_TRY("processPacketQueue");
		lengthProcessed += packet->getLength();
		++packetsProcessed;

		EXC_SET_BLOCK("sending");
		if (sendPacket(state, packet) == false)
		{
			state->clearQueues();
			state->markWriteClosed();
			break;
		}

		EXC_CATCH;
		EXC_DEBUG_START;
		g_Log.EventDebug("id='%x', pri='%u', packet '%" PRIuSIZE_T "' of '%" PRIuSIZE_T "' to send, length '%" PRIuSIZE_T "' of '%" PRIuSIZE_T "'\n",
			state->id(), priority, packetsProcessed, maxPacketsToProcess, lengthProcessed, maxLengthToProcess);
		EXC_DEBUG_END;
	}

	if (packetsProcessed >= maxPacketsToProcess)
	{
		DEBUGNETWORK(("Reached maximum packet count limit for this tick (%" PRIuSIZE_T "/%" PRIuSIZE_T ").\n", packetsProcessed, maxPacketsToProcess));
	}
	if (lengthProcessed >= maxLengthToProcess)
	{
		DEBUGNETWORK(("Reached maximum packet length limit for this tick (%" PRIuSIZE_T "/%" PRIuSIZE_T ").\n", lengthProcessed, maxLengthToProcess));
	}

	return packetsProcessed;
}

size_t CNetworkOutput::processAsyncQueue(CNetState* state)
{
	// process a client's async queue
	ADDTOCALLSTACK("CNetworkOutput::processAsyncQueue");
	ASSERT(state != nullptr);
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	if (state->isWriteClosed() || state->isAsyncMode() == false)
		return 0;

	if (state->m_outgoing.asyncQueue.empty() || state->isSendingAsync())
		return 0;

	const CClient* client = state->getClient();
	ASSERT(client != nullptr);

	// select the next packet to send
	PacketSend* packet = nullptr;
	while (state->m_outgoing.asyncQueue.empty() == false)
	{
		packet = state->m_outgoing.asyncQueue.front();
		state->m_outgoing.asyncQueue.pop();

		if (packet != nullptr)
		{
			// check if the client is allowed this
			if (state->canReceive(packet) && packet->onSend(client))
				break;

			// destroy the packet, we aren't going to use it
			delete packet;
			packet = nullptr;
		}
	}

	if (packet == nullptr)
		return 0;

	// start sending the packet we found
	if (sendPacketData(state, packet) == false)
	{
		state->clearQueues();
		state->markWriteClosed();
	}

	return 1;
}

bool CNetworkOutput::processByteQueue(CNetState* state)
{
	// process a client's byte queue
	ADDTOCALLSTACK("CNetworkOutput::processByteQueue");
	ASSERT(state != nullptr);
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	if (state->isWriteClosed() || state->m_outgoing.bytes.GetDataQty() <= 0)
		return false;

	size_t result = sendData(state, state->m_outgoing.bytes.RemoveDataLock(), state->m_outgoing.bytes.GetDataQty());
	if (result == _failed_result())
	{
		// error occurred
		state->clearQueues();
		state->markWriteClosed();
		return false;
	}

	if (result > 0)
		state->m_outgoing.bytes.RemoveDataAmount(result);

	return true;
}

bool CNetworkOutput::sendPacket(CNetState* state, PacketSend* packet)
{
	// send packet to client (can be queued for async operation)
	ADDTOCALLSTACK("CNetworkOutput::sendPacket");
	ASSERT(state != nullptr);
	ASSERT(packet != nullptr);
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	// check the client is allowed the packet
	if (state->canReceive(packet) == false)
	{
		delete packet;
		return false;
	}

	if (state->isAsyncMode())
	{
		state->m_outgoing.asyncQueue.push(packet);
		return true;
	}

	return sendPacketData(state, packet);
}

bool CNetworkOutput::sendPacketData(CNetState* state, PacketSend* packet)
{
	// send packet data to client
	ADDTOCALLSTACK("CNetworkOutput::sendPacketData");
	ASSERT(state != nullptr);
	ASSERT(packet != nullptr);
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	CClient* client = state->getClient();
	ASSERT(client != nullptr);

	EXC_TRY("sendPacketData");
	xRecordPacket(client, packet, "server->client");

	EXC_SET_BLOCK("send trigger");
	if (packet->onSend(client) == false)
	{
		delete packet;
		return true;
	}

	if (client->xOutPacketFilter(packet->getData(), packet->getLength()) == true)
	{
		delete packet;
		return true;
	}

	EXC_SET_BLOCK("prepare data");
	byte* sendBuffer = nullptr;
	uint sendBufferLength = 0;

	if (client->GetConnectType() == CONNECT_GAME)
	{
		// game clients require encryption
		EXC_SET_BLOCK("compress and encrypt");

		// compress
		uint compressLength = client->xCompress(m_encryptBuffer, packet->getData(), MAX_BUFFER, packet->getLength());
        if (compressLength == 0)
        {
            g_Log.EventError("NET-OUT: Trying to compress (Huffman) too much data. Packet will not be sent. (Probably it's a dialog with a lot of data inside).\n");
            return false;
        }

		// encrypt
        if (client->m_Crypt.GetEncryptionType() == ENC_TFISH)
        {
            if (!client->m_Crypt.Encrypt(m_encryptBuffer, m_encryptBuffer, MAX_BUFFER, compressLength))
            {
                g_Log.EventError("NET-OUT: Trying to compress (TFISH/MD5) too much data. Packet will not be sent. (Probably it's a dialog with a lot of data inside).\n");
                return false;
            }
        }

		sendBuffer = m_encryptBuffer;
		sendBufferLength = compressLength;
	}
	else
	{
		// other clients expect plain data
		sendBuffer = packet->getData();
		sendBufferLength = packet->getLength();
	}

	// queue packet data
	EXC_SET_BLOCK("queue data");
	state->m_outgoing.bytes.AddNewData(sendBuffer, sendBufferLength);

	// if buffering is disabled then process the queue straight away
	// we need to do this rather than sending the packet data directly, otherwise if
	// the packet does not send all at once we will be stuck with an incomplete data
	// transfer (and no clean way to recover)
	if (g_Cfg.m_fUseExtraBuffer == false)
	{
		EXC_SET_BLOCK("send data");
		processByteQueue(state);
	}

	EXC_SET_BLOCK("sent trigger");
	packet->onSent(client);
	delete packet;
	return true;

	EXC_CATCH;
	EXC_DEBUG_START;
	g_Log.EventDebug("id='%x', packet '0x%x', length '%u'\n",
		state->id(), *packet->getData(), packet->getLength());
	EXC_DEBUG_END;
	return false;
}

size_t CNetworkOutput::sendData(CNetState* state, const byte* data, size_t length)
{
	// send raw data to client
	ADDTOCALLSTACK("CNetworkOutput::sendData");
	ASSERT(state != nullptr);
	ASSERT(data != nullptr);
	ASSERT(length > 0);
	ASSERT(length != _failed_result());
	ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());

	EXC_TRY("SendData");
	size_t result = 0;

	EXC_SET_BLOCK("sending");

#if defined(_WIN32) && !defined(_LIBEV)
	if (state->isAsyncMode())
	{
		// send via async winsock
		ZeroMemory(&state->m_overlapped, sizeof(WSAOVERLAPPED));
		state->m_overlapped.hEvent = state;
		state->m_bufferWSA.len = static_cast<ULONG>(length);
		state->m_bufferWSA.buf = reinterpret_cast<CHAR *>(const_cast<byte *>(data));

		DWORD bytesSent;
		if (state->m_socket.SendAsync(&state->m_bufferWSA, 1, &bytesSent, 0, &state->m_overlapped, (LPWSAOVERLAPPED_COMPLETION_ROUTINE)SendCompleted_Winsock) == 0)
		{
			result = bytesSent;
			state->setSendingAsync(true);
		}
		else
		{
			result = 0;
		}
	}
	else
#endif
	{
		// send via standard api
		int sent = state->m_socket.Send(data, (int)length);
		if (sent > 0)
			result = (size_t)(sent);
		else
			result = 0;
	}

	// check for error
	if (result <= 0)
	{
		EXC_SET_BLOCK("error parse");
		int errorCode = CSocket::GetLastError(true);

#if defined(_WIN32) && !defined(_LIBEV)
		if (state->isAsyncMode() && errorCode == WSA_IO_PENDING)
		{
			// safe to ignore this, data has actually been sent (or will be)
			result = length;
		}
		else if (state->isAsyncMode() == false && errorCode == WSAEWOULDBLOCK)
#else
		if (errorCode == EAGAIN || errorCode == EWOULDBLOCK)
#endif
		{
			// send failed but it is safe to ignore and try again later
			result = 0;
		}
#if defined(_WIN32) && !defined(_LIBEV)
		else if (errorCode == WSAECONNRESET || errorCode == WSAECONNABORTED)
#else
		else if (errorCode == ECONNRESET || errorCode == ECONNABORTED)
#endif
		{
			// connection has been lost, client should be cleared
			result = _failed_result();
		}
		else
		{
			EXC_SET_BLOCK("unexpected connection error");
			if (state->isClosing() == false)
				g_Log.Event(LOGM_CLIENTS_LOG|LOGL_WARN, "%x:TX Error %d\n", state->id(), errorCode);

			// Connection error should clear the client too
			result = _failed_result();
		}
	}

	if (result > 0 && result != _failed_result())
		CurrentProfileData.Count(PROFILE_DATA_TX, (dword)(result));

	return result;
	EXC_CATCH;
	EXC_DEBUG_START;
	g_Log.EventDebug("id='%x', packet '0x%x', length '%" PRIuSIZE_T "'\n", state->id(), *data, length);
	EXC_DEBUG_END;
	return _failed_result();
}

void CNetworkOutput::onAsyncSendComplete(CNetState* state, bool success)
{
	// notify that async operation completed
	ADDTOCALLSTACK("CNetworkOutput::onAsyncSendComplete");
	ASSERT(state != nullptr);
#ifdef _LIBEV
	if (state->isSendingAsync() == true)
		DEBUGNETWORK(("%x: Changing SendingAsync from true to false.\n", state->id()));
#endif
	state->setSendingAsync(false);
	if (success == false)
		return;

	// we could process another batch of async data right now, but only if we
	// are in the output thread
	// - WinSock calls this from the same thread (so, enter the if)
	// - LinuxEv calls this from a completely different thread (cannot enter the if)
	if (m_thread->isActive() && m_thread->isCurrentThread())
	{
		if (processAsyncQueue(state) > 0)
			processByteQueue(state);
	}
}

void CNetworkOutput::QueuePacket(PacketSend* packet, bool appendTransaction)
{
	// queue a packet for sending
	ADDTOCALLSTACK("CNetworkOutput::QueuePacket");
	ASSERT(packet != nullptr);

	// don't bother queuing packets for invalid sockets
	CNetState* state = packet->m_target;
	if (state == nullptr || state->canReceive(packet) == false)
	{
		delete packet;
		return;
	}

	if (state->m_outgoing.pendingTransaction != nullptr && appendTransaction)
		state->m_outgoing.pendingTransaction->emplace_back(packet);
	else
		QueuePacketTransaction(new SimplePacketTransaction(packet));
}

void CNetworkOutput::QueuePacketTransaction(PacketTransaction* transaction)
{
	// queue a packet transaction for sending
	ADDTOCALLSTACK("CNetworkOutput::QueuePacketTransaction");
	ASSERT(transaction != nullptr);

	// don't bother queuing packets for invalid sockets
	CNetState* state = transaction->getTarget();
	if (state == nullptr || state->isInUse() == false || state->isWriteClosed())
	{
		delete transaction;
		return;
	}

	int priority = transaction->getPriority();
	ASSERT(priority >= PacketSend::PRI_IDLE && priority < PacketSend::PRI_QTY);

	// limit by max number of packets in queue
	size_t maxQueueSize = NETWORK_MAXQUEUESIZE;
	if (maxQueueSize > 0)
	{
		while (priority > PacketSend::PRI_IDLE && state->m_outgoing.queue[priority].size() >= maxQueueSize)
		{
			--priority;
			transaction->setPriority(priority);
		}
	}

	state->m_outgoing.queue[priority].push(transaction);

	// notify thread
	CNetworkThread* thread = state->getParentThread();
	if (thread != nullptr && thread->getPriority() == IThread::Disabled)
		thread->awaken();
}
