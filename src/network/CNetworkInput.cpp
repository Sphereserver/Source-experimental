#include "../common/crypto/CCrypto.h"
#include "../game/clients/CClient.h"
#include "../game/CServer.h"
#include "../game/CWorldGameTime.h"
#include "../sphere/threads.h"
#include "../sphere/ProfileTask.h"
#include "packet.h"
#include "send.h"
#include "CNetState.h"
#include "CNetworkManager.h"
#include "CNetworkThread.h"
#include "CNetworkInput.h"

#define NETWORK_BUFFERSIZE		0xF000	// size of receive buffer
#define NETWORK_SEEDLEN_OLD		(sizeof( dword ))
#define NETWORK_SEEDLEN_NEW		(1 + (sizeof( dword ) * 5))


CNetworkInput::CNetworkInput(void) : m_thread(nullptr)
{
    m_receiveBuffer = new byte[NETWORK_BUFFERSIZE];
    m_decryptBuffer = new byte[NETWORK_BUFFERSIZE];
}

CNetworkInput::~CNetworkInput()
{
    if (m_receiveBuffer != nullptr)
        delete[] m_receiveBuffer;
    if (m_decryptBuffer != nullptr)
        delete[] m_decryptBuffer;
}

void CNetworkInput::setOwner(CNetworkThread* thread)
{
    m_thread = thread;
}

bool CNetworkInput::processInput()
{
    ADDTOCALLSTACK("CNetworkInput::processInput");
    ASSERT(m_thread != nullptr);

    // when called from within the thread's context, we just receive data
    if ((m_thread->isActive() && m_thread->isCurrentThread()) ||	// check for multi-threaded network
        !m_thread->isActive())										// check for single-threaded network
    {
        receiveData();
        processData();
    }

    // when called from outside the thread's context, we process data
    else if (!m_thread->isActive() || !m_thread->isCurrentThread())
    {
        // if the thread does not receive ticks, we must perform a quick select to see if we should
        // wake up the thread
        if (m_thread->isActive() && m_thread->getPriority() == IThread::Disabled)
        {
            fd_set fds;
            if (checkForData(fds))
                m_thread->awaken();
        }

        processData();
    }
    return true;
}

void CNetworkInput::receiveData()
{
    ADDTOCALLSTACK("CNetworkInput::receiveData");
    ASSERT(m_thread != nullptr);
    ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());
    EXC_TRY("ReceiveData");

    // check for incoming data
    EXC_SET_BLOCK("select");
    fd_set fds;
    if (checkForData(fds) == false)
        return;

    EXC_SET_BLOCK("messages");
    NetworkThreadStateIterator states(m_thread);
    while (CNetState* state = states.next())
    {
        EXC_SET_BLOCK("check socket");
        if (state->isReadClosed())
            continue;

        EXC_SET_BLOCK("start network profile");
        const ProfileTask networkTask(PROFILE_NETWORK_RX);
        if (!FD_ISSET(state->m_socket.GetSocket(), &fds))
        {
            state->m_incoming.rawPackets.clean();
            continue;
        }

        // receive data
        EXC_SET_BLOCK("messages - receive");
        int received = state->m_socket.Receive(m_receiveBuffer, NETWORK_BUFFERSIZE, 0);
        if (received <= 0 || received > NETWORK_BUFFERSIZE)
        {
            state->markReadClosed();
            EXC_SET_BLOCK("next state");
            continue;
        }

        EXC_SET_BLOCK("start client profile");
        CurrentProfileData.Count(PROFILE_DATA_RX, received);

        EXC_SET_BLOCK("messages - parse");

        // our objective here is to take the received data and separate it into packets to
        // be stored in CNetState::m_incoming.rawPackets
        byte* buffer = m_receiveBuffer;
        while (received > 0)
        {
            // currently we just take the data and push it into a queue for the main thread
            // to parse into actual packets
            // todo: if possible, it would be useful to be able to perform that separation here,
            // but this is made difficult due to the variety of client types and encryptions that
            // may be connecting
            uint length = (uint)received;

            Packet* packet = new Packet(buffer, length);
            state->m_incoming.rawPackets.push(packet);
            buffer += length;
            received -= (int)(length);
        }
    }

    EXC_CATCH;
}

void CNetworkInput::processData()
{
    ADDTOCALLSTACK("CNetworkInput::processData");
    ASSERT(m_thread != nullptr);
    ASSERT(!m_thread->isActive() || m_thread->isCurrentThread());
    EXC_TRY("ProcessData");

    // check which states have data
    EXC_SET_BLOCK("messages");
    NetworkThreadStateIterator states(m_thread);
    while (CNetState* state = states.next())
    {
        EXC_SET_BLOCK("check socket");
        if (state->isReadClosed())
            continue;

        EXC_SET_BLOCK("start network profile");
        const ProfileTask networkTask(PROFILE_NETWORK_RX);

        const CClient* client = state->getClient();
        ASSERT(client != nullptr);

        EXC_SET_BLOCK("check message");
        if (state->m_incoming.rawPackets.empty())
        {
            const CONNECT_TYPE connecttype = client->GetConnectType();
            if ((connecttype != CONNECT_TELNET) && (connecttype != CONNECT_AXIS))
            {
                // check for timeout
                EXC_SET_BLOCK("check frozen");
                const int64 iLastEventDiff = CWorldGameTime::GetCurrentTime().GetTimeDiff(client->m_timeLastEvent);
                if ((g_Cfg.m_iDeadSocketTime > 0) && (iLastEventDiff > g_Cfg.m_iDeadSocketTime))
                {
                    g_Log.Event(LOGM_CLIENTS_LOG | LOGL_EVENT, "%x:Frozen client disconnected (DeadSocketTime reached).\n", state->id());
                    state->m_client->addLoginErr(PacketLoginError::Other);		//state->markReadClosed();
                }
            }

            if (state->m_incoming.rawBuffer == nullptr)
            {
                EXC_SET_BLOCK("next state");
                continue;
            }
        }

        EXC_SET_BLOCK("messages - process");
        // we've already received some raw data, we just need to add it to any existing data we have
        while (state->m_incoming.rawPackets.empty() == false)
        {
            Packet* packet = state->m_incoming.rawPackets.front();
            state->m_incoming.rawPackets.pop();
            ASSERT(packet != nullptr);

            EXC_SET_BLOCK("packet - queue data");
            if (state->m_incoming.rawBuffer == nullptr)
            {
                // create new buffer
                state->m_incoming.rawBuffer = new Packet(packet->getData(), packet->getLength());
            }
            else
            {
                // append to buffer
                uint pos = state->m_incoming.rawBuffer->getPosition();
                state->m_incoming.rawBuffer->seek(state->m_incoming.rawBuffer->getLength());
                state->m_incoming.rawBuffer->writeData(packet->getData(), packet->getLength());
                state->m_incoming.rawBuffer->seek(pos);
            }

            delete packet;
        }

        if (g_Serv.IsLoading() == false)
        {
            EXC_SET_BLOCK("start client profile");
            const ProfileTask clientTask(PROFILE_CLIENTS);

            EXC_SET_BLOCK("packets - process");
            Packet* buffer = state->m_incoming.rawBuffer;
            if (buffer != nullptr)
            {
                // we have a buffer of raw bytes, we need to go through them all and process as much as we can
                while (state->isReadClosed() == false && buffer->getRemainingLength() > 0)
                {
                    if (processData(state, buffer))
                        continue;

                    // processData didn't want to use any data, which means we probably
                    // received some invalid data or that the packet was malformed
                    // best course of action right now is to close the connection
                    state->markReadClosed();
                    break;
                }

                if (buffer->getRemainingLength() <= 0)
                {
                    EXC_SET_BLOCK("packets - clear buffer");
                    delete buffer;
                    state->m_incoming.rawBuffer = nullptr;
                }
            }
        }
        EXC_SET_BLOCK("next state");
    }

    EXC_CATCH;
}

bool CNetworkInput::checkForData(fd_set& fds)
{
    // select() against each socket we own
    ADDTOCALLSTACK("CNetworkInput::checkForData");

    EXC_TRY("CheckForData");

    int count = 0;
    FD_ZERO(&fds);

    NetworkThreadStateIterator states(m_thread);
    while (CNetState* state = states.next())
    {
        EXC_SET_BLOCK("check socket");
        if (state->isReadClosed())
            continue;

        EXC_SET_BLOCK("check closing");
        if (state->isClosing() || state->m_socket.IsOpen() == false)
            continue;

        AddSocketToSet(fds, state->m_socket.GetSocket(), count);
    }

    EXC_SET_BLOCK("prepare timeout");
    timeval timeout; // time to wait for data.
    timeout.tv_sec = 0;
    timeout.tv_usec = 100; // micro seconds = 1/1000000

    EXC_SET_BLOCK("select");
    return select(count + 1, &fds, nullptr, nullptr, &timeout) > 0;

    EXC_CATCH;
    return false;
}

bool CNetworkInput::processData(CNetState* state, Packet* buffer)
{
    ADDTOCALLSTACK("CNetworkInput::processData");
    ASSERT(state != nullptr);
    ASSERT(buffer != nullptr);
    CClient* client = state->getClient();
    ASSERT(client != nullptr);

    if (client->GetConnectType() == CONNECT_UNK)
        return processUnknownClientData(state, buffer);

    client->m_timeLastEvent = CWorldGameTime::GetCurrentTime().GetTimeRaw();

    if (client->m_Crypt.IsInit() == false)
        return processOtherClientData(state, buffer);

    return processGameClientData(state, buffer);
}

bool CNetworkInput::processGameClientData(CNetState* state, Packet* buffer)
{
    ADDTOCALLSTACK("CNetworkInput::processGameClientData");
    EXC_TRY("ProcessGameData");
    ASSERT(state != nullptr);
    ASSERT(buffer != nullptr);
    CClient* client = state->getClient();
    ASSERT(client != nullptr);

    EXC_SET_BLOCK("decrypt message");
    if (!client->m_Crypt.Decrypt(m_decryptBuffer, buffer->getRemainingData(), MAX_BUFFER, buffer->getRemainingLength()))
    {
        g_Log.EventError("NET-IN: processGameClientData failed (Decrypt).\n");
        return false;
    }

    if (state->m_incoming.buffer == nullptr)
    {
        // create new buffer
        state->m_incoming.buffer = new Packet(m_decryptBuffer, buffer->getRemainingLength());
    }
    else
    {
        // append to buffer
        uint pos = state->m_incoming.buffer->getPosition();
        state->m_incoming.buffer->seek(state->m_incoming.buffer->getLength());
        state->m_incoming.buffer->writeData(m_decryptBuffer, buffer->getRemainingLength());
        state->m_incoming.buffer->seek(pos);
    }

    Packet* packet = state->m_incoming.buffer;
    uint remainingLength = packet->getRemainingLength();

    EXC_SET_BLOCK("record message");
    xRecordPacket(client, packet, "client->server");

    // process the message
    EXC_TRYSUB("ProcessMessage");

    while (remainingLength > 0 && state->isClosing() == false)
    {
        ASSERT(remainingLength == packet->getRemainingLength());
        byte packetId = packet->getRemainingData()[0];
        Packet* handler = m_thread->m_manager->getPacketManager().getHandler(packetId);

        if (g_Cfg.m_iDebugFlags & DEBUGF_PACKETS)
        {
#ifdef _DEBUG
            g_Log.Event(LOGM_DEBUG, "Parsing next packet into the received packet data stream (previous dump).\n");
#endif
            Packet packetPart;
            packetPart.writeData(packet->getRemainingData(), packet->getRemainingLength());
            packetPart.resize(packet->getRemainingLength());
            xRecordPacket(client, &packetPart, "sub-packet client->server");
        }

        if (handler != nullptr)
        {
            uint packetLength = handler->checkLength(state, packet);
            if (packetLength <= 0)
            {
                DEBUGNETWORK(("%x:Game packet (0x%x) does not match the expected length, waiting for more data...\n", state->id(), packetId));
                break;
            }

            remainingLength -= packetLength;

            // Packet filtering - check if any function trigger is installed
            //  allow skipping the packet which we do not wish to get
            if (client->xPacketFilter(packet->getRemainingData(), packetLength))
            {
                packet->skip((int)(packetLength));
                continue;
            }

            // copy data to handler
            handler->seek();
            handler->writeData(packet->getRemainingData(), packetLength);
            packet->skip((int)packetLength);

            // move to position 1 (no need for id) and fire onReceive()
            handler->resize(packetLength);
            handler->seek(1);
            handler->onReceive(state);
        }
        else
        {
            // Packet filtering - check if any function trigger is installed
            //  allow skipping the packet which we do not wish to get
            if (client->xPacketFilter(packet->getRemainingData(), remainingLength))
            {
                // todo: adjust packet filter to specify size!
                // packet has been handled by filter but we don't know how big the packet
                // actually is.. we can only assume the entire buffer is used.
                packet->skip((int)(remainingLength));
                remainingLength = 0;
                break;
            }

            // unknown packet.. we could skip 1 byte at a time but this can produce
            // strange behaviours (it's unlikely that only 1 byte is incorrect), so
            // it's best to just discard everything we have
            g_Log.Event(LOGL_WARN, "%x:Unknown game packet (0x%x) received.\n", state->id(), packetId);

#ifdef _DEBUG
            TemporaryString tsDump;
            packet->dump(tsDump);
            g_Log.EventDebug("%x:%s %s\n", client->GetSocketID(), "(unknown packet data) client -> server", (lpctstr)tsDump);
#endif

            packet->skip((int)(remainingLength));
            remainingLength = 0;
        }
    }

    EXC_CATCHSUB("Message");
    EXC_DEBUGSUB_START;
    TemporaryString tsDump;
    packet->dump(tsDump);

    g_Log.EventDebug("%x:Parsing %s", state->id(), static_cast<lpctstr>(tsDump));

    state->m_packetExceptions++;
    if (state->m_packetExceptions > 10)
    {
        g_Log.Event(LOGM_CLIENTS_LOG | LOGL_WARN, "%x:Disconnecting client from account '%s' since it is causing exceptions problems\n", state->id(), client != nullptr && client->GetAccount() ? client->GetAccount()->GetName() : "");
        if (client != nullptr)
            client->addKick(&g_Serv, false);
        else
            state->markReadClosed();
    }

    EXC_DEBUGSUB_END;

    // delete the buffer once it has been exhausted
    if (remainingLength <= 0)
    {
        state->m_incoming.buffer = nullptr;
        delete packet;
    }

    buffer->seek(buffer->getLength());
    return true;

    EXC_CATCH;
    return false;
}

bool CNetworkInput::processOtherClientData(CNetState* state, Packet* buffer)
{
    // process data from a non-game client
    ADDTOCALLSTACK("CNetworkInput::processOtherClientData");
    EXC_TRY("ProcessOtherClientData");
    ASSERT(state != nullptr);
    ASSERT(buffer != nullptr);
    CClient* client = state->getClient();
    ASSERT(client != nullptr);

    switch (client->GetConnectType())
    {
    case CONNECT_CRYPT:
    {
        if (buffer->getRemainingLength() < 5)
        {
            // not enough data to be a real client
            EXC_SET_BLOCK("ping #3");
            client->SetConnectType(CONNECT_UNK);
            if (client->OnRxPing(buffer->getRemainingData(), buffer->getRemainingLength()) == false)
                return false;
            break;
        }

        // first real data from client which we can use to log in
        EXC_SET_BLOCK("encryption setup");
        ASSERT(buffer->getRemainingLength() <= sizeof(CEvent));

        std::unique_ptr<CEvent> evt = std::make_unique<CEvent>();
        memcpy(evt.get(), buffer->getRemainingData(), buffer->getRemainingLength());

        if (evt->Default.m_Cmd == XCMD_EncryptionReply && state->isClientKR())
        {
            EXC_SET_BLOCK("encryption reply");

            // receiving response to 0xE3 packet
            uint iEncKrLen = evt->EncryptionReply.m_len;
            if (buffer->getRemainingLength() < iEncKrLen)
                return false; // need more data

            buffer->skip(iEncKrLen);
            return true;
        }

        client->xProcessClientSetup(evt.get(), buffer->getRemainingLength());
    }
    break;

    case CONNECT_HTTP:
        EXC_SET_BLOCK("http message");
        if (client->OnRxWebPageRequest(buffer->getRemainingData(), buffer->getRemainingLength()) == false)
            return false;
        break;

    case CONNECT_TELNET:
        EXC_SET_BLOCK("telnet message");
        if (client->OnRxConsole(buffer->getRemainingData(), buffer->getRemainingLength()) == false)
            return false;
        break;
    case CONNECT_AXIS:
        EXC_SET_BLOCK("Axis message");
        if (client->OnRxAxis(buffer->getRemainingData(), buffer->getRemainingLength()) == false)
            return false;
        break;

    default:
        g_Log.Event(LOGM_CLIENTS_LOG | LOGL_EVENT, "%x:Junk messages with no crypt\n", state->id());
        return false;
    }

    buffer->seek(buffer->getLength());
    return true;
    EXC_CATCH;
    return false;
}

bool CNetworkInput::processUnknownClientData(CNetState* state, Packet* buffer)
{
    // process data from an unknown client type
    ADDTOCALLSTACK("CNetworkInput::processUnknownClientData");
    EXC_TRY("ProcessNewClient");
    ASSERT(state != nullptr);
    ASSERT(buffer != nullptr);
    CClient* client = state->getClient();
    ASSERT(client != nullptr);

    bool fHTTPReq = false;
    const uint uiOrigRemainingLength = buffer->getRemainingLength();
    const byte* const pOrigRemainingData = buffer->getRemainingData();
    if (state->m_seeded == false)
    {
        fHTTPReq = (uiOrigRemainingLength >= 5 && memcmp(pOrigRemainingData, "GET /", 5) == 0) ||
            (uiOrigRemainingLength >= 6 && memcmp(pOrigRemainingData, "POST /", 6) == 0);
    }
    if (!fHTTPReq && (uiOrigRemainingLength > INT8_MAX))
    {
        g_Log.EventWarn("%x:Client connected with a seed length of %u exceeding max length limit of %d, disconnecting.\n", state->id(), uiOrigRemainingLength, INT8_MAX);
        return false;
    }

    if (state->m_seeded == false)
    {
        // check for HTTP connection
        if (fHTTPReq)
        {
            EXC_SET_BLOCK("http request");
            if (g_Cfg.m_fUseHTTP != 2)
                return false;

            client->SetConnectType(CONNECT_HTTP);
            client->OnRxWebPageRequest(buffer->getRemainingData(), uiOrigRemainingLength);

            buffer->seek(buffer->getLength());
            return true;
        }

        // check for new seed (sometimes it's received on its own)
        else if (uiOrigRemainingLength == 1 && pOrigRemainingData[0] == XCMD_NewSeed)
        {
            state->m_newseed = true;
            buffer->skip(1);
            return true;
        }

        // check for ping data
        else if (uiOrigRemainingLength < 4)
        {
            EXC_SET_BLOCK("ping #1");
            if (client->OnRxPing(pOrigRemainingData, uiOrigRemainingLength) == false)
                state->markReadClosed();

            buffer->seek(buffer->getLength());
            return true;
        }

        // at least 4 bytes and not a web request, so must assume
        // it is a game client seed
        EXC_SET_BLOCK("game client seed");
        dword seed = 0;

        DEBUGNETWORK(("%x:Client connected with a seed length of %u ([0]=0x%x)\n", state->id(), uiOrigRemainingLength, (int)(pOrigRemainingData[0])));
        if (state->m_newseed || (pOrigRemainingData[0] == XCMD_NewSeed && uiOrigRemainingLength >= NETWORK_SEEDLEN_NEW))
        {
            DEBUGNETWORK(("%x:Receiving new client login handshake.\n", state->id()));

            if (state->m_newseed == false)
            {
                // we don't need the command byte
                state->m_newseed = true;
                buffer->skip(1);
            }

            if (buffer->getRemainingLength() >= (NETWORK_SEEDLEN_NEW - 1))
            {
                seed = buffer->readInt32();
                dword versionMajor = buffer->readInt32();
                dword versionMinor = buffer->readInt32();
                dword versionRevision = buffer->readInt32();
                dword versionPatch = buffer->readInt32();

                DEBUG_MSG(("%x:New Login Handshake Detected. Client Version: %u.%u.%u.%u\n", state->id(), versionMajor, versionMinor, versionRevision, versionPatch));
                state->m_reportedVersion = CCrypto::GetVerFromNumber(versionMajor, versionMinor, versionRevision, versionPatch);
            }
            else
            {
                DEBUGNETWORK(("%x:Not enough data received to be a valid handshake (%u).\n", state->id(), buffer->getRemainingLength()));
            }
        }
        else if (pOrigRemainingData[0] == XCMD_UOGRequest && uiOrigRemainingLength == 8)
        {
            DEBUGNETWORK(("%x:Receiving new UOG status request.\n", state->id()));
            buffer->skip(7);
            buffer->getRemainingData()[0] = 0x7F;
            return true;
        }
        else
        {
            // assume it's a normal client login
            DEBUGNETWORK(("%x:Receiving old client login handshake.\n", state->id()));

            seed = buffer->readInt32();
        }

        DEBUGNETWORK(("%x:Client connected with a seed of 0x%x (new handshake=%d, version=%u).\n", state->id(), seed, state->m_newseed ? 1 : 0, state->m_reportedVersion));

        if (seed == 0)
        {
            g_Log.Event(LOGM_CLIENTS_LOG | LOGL_EVENT, "%x:Invalid client detected, disconnecting.\n", state->id());
            return false;
        }

        state->m_seeded = true;
        state->m_seed = seed;

        if (buffer->getRemainingLength() <= 0 && state->m_seed == 0xFFFFFFFF)
        {
            // UO:KR Client opens connection with 255.255.255.255 and waits for the
            // server to send a packet before continuing
            EXC_SET_BLOCK("KR client seed");

            DEBUG_WARN(("UO:KR Client Detected.\n"));
            client->SetConnectType(CONNECT_CRYPT);
            state->m_clientType = CLIENTTYPE_KR;
            new PacketKREncryption(client);
        }

        return true;
    }

    if (uiOrigRemainingLength < 5)
    {
        // client has a seed assigned but hasn't send enough data to be anything useful,
        // some programs (ConnectUO) send a fake seed when really they're just sending
        // ping data
        EXC_SET_BLOCK("ping #2");
        if (client->OnRxPing(pOrigRemainingData, uiOrigRemainingLength) == false)
            return false;

        buffer->seek(buffer->getLength());
        return true;
    }

    // process login packet for client
    EXC_SET_BLOCK("messages  - setup");
    client->SetConnectType(CONNECT_CRYPT);
    client->xProcessClientSetup(reinterpret_cast<CEvent*>(buffer->getRemainingData()), uiOrigRemainingLength);
    buffer->seek(buffer->getLength());
    return true;

    EXC_CATCH;
    return false;
}
