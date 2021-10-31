//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.

// ulxd -- The universal reflector
// Copyright © 2021 Thomas A. Early N7TAE
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "Main.h"
#include <string.h>
#include "CodecStream.h"
#include "DVFramePacket.h"
#include "Reflector.h"

////////////////////////////////////////////////////////////////////////////////////////
// constructor

CCodecStream::CCodecStream(CPacketStream *PacketStream, uint16_t uiId, uint8_t uiCodecIn, uint8_t uiCodecOut)
{
	keep_running = true;
	m_uiStreamId = uiId;
	m_uiPid = 0;
	m_uiCodecIn = uiCodecIn;
	m_uiCodecOut = uiCodecOut;
	m_bConnected = false;
	m_fPingMin = -1;
	m_fPingMax = -1;
	m_fPingSum = 0;
	m_fPingCount = 0;
	m_uiTotalPackets = 0;
	m_uiTimeoutPackets = 0;
	m_PacketStream = PacketStream;
}

////////////////////////////////////////////////////////////////////////////////////////
// destructor

CCodecStream::~CCodecStream()
{
	// close socket
	m_Socket.Close();

	// kill threads
	keep_running = false;
	if ( m_Future.valid() )
	{
		m_Future.get();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// initialization

bool CCodecStream::Init(uint16_t uiPort)
{
	m_bConnected = keep_running = false;	// prepare for the worst

	// create the send to address
	m_uiPort = uiPort;
	auto s = g_Reflector.GetTranscoderIp();
	m_Ip.Initialize(strchr(s, ':') ? AF_INET6 : AF_INET, m_uiPort, s);

	if (0 == strncasecmp(s, "none", 4))
	{
		return false;	// the user has disabled the transcoder
	}

	// create socket address, family based on transcoder listen address
#ifdef LISTEN_IPV4
#ifdef LISTEN_IPV6
	auto paddr = (AF_INET == m_Ip.GetFamily()) ? LISTEN_IPV4 : LISTEN_IPV6;
#else
	auto paddr = LISTEN_IPV4;
#endif
#else
	auto paddr = LISTEN_IPV6;
#endif
	CIp ip(m_Ip.GetFamily(), m_uiPort, paddr);

	// create our socket
	if (ip.IsSet())
	{
		if (! m_Socket.Open(ip))
		{
			std::cerr << "Error opening socket on IP address " << m_Ip << std::endl;
			return false;
		}
	}
	else
	{
		std::cerr << "Could not initialize Codec Stream on " << paddr << std::endl;
		return false;
	}

	keep_running = m_bConnected = true;
	m_Future = std::async(std::launch::async, &CCodecStream::Thread, this);

	return true;
}

void CCodecStream::Close(void)
{
	// close socket
	keep_running = m_bConnected = false;
	m_Socket.Close();

	// kill threads
	if ( m_Future.valid() )
	{
		m_Future.get();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// get

bool CCodecStream::IsEmpty(void) const
{
	return (m_LocalQueue.empty() && m_PacketStream->empty());
}

////////////////////////////////////////////////////////////////////////////////////////
// thread

void CCodecStream::Thread()
{
	while (keep_running)
	{
		Task();
	}
}

void CCodecStream::Task(void)
{
	CBuffer Buffer;
	CIp     Ip;
	uint8_t   Ambe[AMBE_SIZE];
	uint8_t   DStarSync[] = { 0x55,0x2D,0x16 };

	// any packet from transcoder
	if ( m_Socket.Receive(Buffer, Ip, 5) )
	{
		// crack
		if ( IsValidAmbePacket(Buffer, Ambe) )
		{
			// tickle
			m_TimeoutTimer.start();

			// update statistics
			double ping = m_StatsTimer.time();
			if ( m_fPingMin == -1 )
			{
				m_fPingMin = ping;
				m_fPingMax = ping;

			}
			else
			{
				m_fPingMin = MIN(m_fPingMin, ping);
				m_fPingMax = MAX(m_fPingMax, ping);

			}
			m_fPingSum += ping;
			m_fPingCount += 1;

			// pop the original packet
			if ( !m_LocalQueue.empty() )
			{
				auto Packet = m_LocalQueue.pop();
				auto Frame = (CDvFramePacket *)Packet.get();
				// todo: check the PID
				// update content with transcoded ambe
				Frame->SetAmbe(m_uiCodecOut, Ambe);
				// tag syncs in DvData
				if ( (m_uiCodecOut == CODEC_AMBEPLUS) && (Frame->GetPacketId() % 21) == 0 )
				{
					Frame->SetDvData(DStarSync);
				}
				// and push it back to client
				m_PacketStream->Lock();
				m_PacketStream->push(Packet);
				m_PacketStream->Unlock();
			}
			else
			{
				std::cout << "Unexpected transcoded packet received from transcoder" << std::endl;
			}
		}
	}

	// anything in our queue
	while ( !empty() )
	{
		// yes, pop it from queue
		auto Packet = pop();
		auto Frame = (CDvFramePacket *)Packet.get();

		// yes, send to ambed
		// this assume that thread pushing the Packet
		// have verified that the CodecStream is connected
		// and that the packet needs transcoding
		m_StatsTimer.start();
		m_uiTotalPackets++;
		EncodeAmbePacket(&Buffer, Frame->GetAmbe(m_uiCodecIn));
		m_Socket.Send(Buffer, m_Ip, m_uiPort);

		// and push to our local queue
		m_LocalQueue.push(Packet);
	}

	// handle timeout
	if ( !m_LocalQueue.empty() && (m_TimeoutTimer.time() >= (TRANSCODER_AMBEPACKET_TIMEOUT/1000.0f)) )
	{
		//std::cout << "ambed packet timeout" << std::endl;
		m_uiTimeoutPackets++;
	}
}

////////////////////////////////////////////////////////////////////////////////////////
/// packet decoding helpers

bool CCodecStream::IsValidAmbePacket(const CBuffer &Buffer, uint8_t *Ambe)
{
	bool valid = false;

	if ( (Buffer.size() == 11) && (Buffer.data()[0] == m_uiCodecOut) )
	{
		::memcpy(Ambe, &(Buffer.data()[2]), 9);
		valid = true;
	}
	return valid;
}

////////////////////////////////////////////////////////////////////////////////////////
/// packet encoding helpers

void CCodecStream::EncodeAmbePacket(CBuffer *Buffer, const uint8_t *Ambe)
{
	Buffer->clear();
	Buffer->Append(m_uiCodecIn);
	Buffer->Append(m_uiPid);
	Buffer->Append((uint8_t *)Ambe, 9);
}
