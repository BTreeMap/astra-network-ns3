/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* Copyright (c) 2006 Georgia Tech Research Corporation, INRIA
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <iostream>
#include <stdio.h>
#include "ns3/log.h"
#include "ns3/enum.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/simulator.h"
#include "drop-tail-queue.h"
#include "red-queue.h"
#include "broadcom-egress-queue.h"


namespace ns3 {

	NS_LOG_COMPONENT_DEFINE("BEgressQueue");
	NS_OBJECT_ENSURE_REGISTERED(BEgressQueue);

	TypeId BEgressQueue::GetTypeId(void)
	{
		static TypeId tid = TypeId("ns3::BEgressQueue")
			.SetParent<Queue>()
			.AddConstructor<BEgressQueue>()
			.AddAttribute("MaxBytes",
				"The maximum number of bytes accepted by this BEgressQueue.",
				DoubleValue(1000.0 * 1024 * 1024),
				MakeDoubleAccessor(&BEgressQueue::m_maxBytes),
				MakeDoubleChecker<double>())
			.AddAttribute("MediumPriorityQueue",
				"Index of the TC_med queue, served with strict priority below queue 0 "
				"(TC_high) and above the round-robin TC_low queues. qCnt disables the tier.",
				UintegerValue(BEgressQueue::qCnt),
				MakeUintegerAccessor(&BEgressQueue::m_medPriorityQueue),
				MakeUintegerChecker<uint32_t>(0, BEgressQueue::qCnt))
			.AddAttribute("MediumPriorityWeight",
				"Percent of egress bandwidth TC_med may consume when TC_low has traffic. "
				"UEC 1.0.3 section 4.1 recommends WDRR at 25% and warns that an "
				"unrestricted trimmed class can cause congestion collapse; 100 restores "
				"strict priority over TC_low.",
				UintegerValue(25),
				MakeUintegerAccessor(&BEgressQueue::m_medPriorityWeight),
				MakeUintegerChecker<uint32_t>(1, 100))
			.AddTraceSource ("BeqEnqueue", "Enqueue a packet in the BEgressQueue. Multiple queue",
					MakeTraceSourceAccessor (&BEgressQueue::m_traceBeqEnqueue),
					"ns3::BeqDequeue::BEgressQueue")
			.AddTraceSource ("BeqDequeue", "Dequeue a packet in the BEgressQueue. Multiple queue",
					MakeTraceSourceAccessor (&BEgressQueue::m_traceBeqDequeue),
					"ns3::BeqDequeue::BEgressQueue")
			;

		return tid;
	}

	BEgressQueue::BEgressQueue() :
		Queue(),
  		NS_LOG_TEMPLATE_DEFINE("BEgressQueue")
	{
		NS_LOG_FUNCTION_NOARGS();
		m_bytesInQueueTotal = 0;
		m_rrlast = 0;
		m_medPriorityQueue = qCnt;
		m_medPriorityWeight = 25;
		m_medDeficit = 0;
		for (uint32_t i = 0; i < fCnt; i++)
		{
			m_bytesInQueue[i] = 0;
			//m_queues.push_back(CreateObject<RedQueue>());
			m_queues.push_back(CreateObject<DropTailQueuePacket>());
		}
	}

	BEgressQueue::~BEgressQueue()
	{
		NS_LOG_FUNCTION_NOARGS();
	}

	bool
	BEgressQueue::Enqueue (Ptr<Packet> p)
	{
	NS_LOG_FUNCTION (this << p);

	//
	// If DoEnqueue fails, Queue::Drop is called by the subclass
	//
	bool retval = DoEnqueue (p);
	if (retval)
		{
		NS_LOG_LOGIC ("m_traceEnqueue (p)");
		m_traceEnqueue (p);

		uint32_t size = p->GetSize ();
		m_nBytes += size;
		m_nTotalReceivedBytes += size;

		m_nPackets++;
		m_nTotalReceivedPackets++;
		}
	return retval;
	}

	Ptr<Packet>
	BEgressQueue::Dequeue (void)
	{
	NS_LOG_FUNCTION (this);

	Ptr<Packet> packet = DoDequeue ();

	if (packet)
		{
		NS_ASSERT (m_nBytes >= packet->GetSize ());
		NS_ASSERT (m_nPackets > 0);

		m_nBytes -= packet->GetSize ();
		m_nPackets--;

		NS_LOG_LOGIC ("m_traceDequeue (packet)");
		m_traceDequeue (packet);
		}
	return packet;
	}

	Ptr<Packet>
	BEgressQueue::Remove (void)
	{
	NS_LOG_FUNCTION (this);

	Ptr<Packet> packet = DoDequeue ();

	if (packet)
		{
		NS_ASSERT (m_nBytes >= packet->GetSize ());
		NS_ASSERT (m_nPackets > 0);

		m_nBytes -= packet->GetSize ();
		m_nPackets--;

		NS_LOG_LOGIC ("m_traceDequeue (packet)");
		m_traceDequeue (packet);
		m_traceDrop (packet);
		}
	return packet;
	}

	Ptr<const Packet>
	BEgressQueue::Peek (void) const
	{
	NS_LOG_FUNCTION (this);
	return DoPeek ();
	}


	bool
		BEgressQueue::DoEnqueue(Ptr<Packet> p, uint32_t qIndex)
	{
		NS_LOG_FUNCTION(this << p);

		if (m_bytesInQueueTotal + p->GetSize() < m_maxBytes)  //infinite queue
		{
			m_queues[qIndex]->Enqueue(p);
			m_bytesInQueueTotal += p->GetSize();
			m_bytesInQueue[qIndex] += p->GetSize();
		}
		else
		{
			return false;
		}
		return true;
	}

	// Weighted deficit between TC_med and the TC_low aggregate. Serving TC_med
	// costs (100 - weight) per byte and serving TC_low earns weight per byte, so
	// the steady state gives TC_med exactly `weight` percent of the link.
	//
	// The deficit is clamped to roughly one MTU of imbalance in either direction.
	// Without the clamp, an idle TC_low period would let TC_med bank unbounded
	// credit (or debt), and the correction would land exactly when congestion
	// resumes — delaying loss notification at the worst moment. Bounding it keeps
	// the scheduler memoryless beyond a single round.
	bool BEgressQueue::MedWithinShare() const
	{
		if (m_medPriorityWeight >= 100)
		{
			return true; // strict priority over TC_low
		}
		return m_medDeficit >= 0;
	}

	void BEgressQueue::AccountWeightedShare(uint32_t qIndex, uint32_t bytes)
	{
		if (m_medPriorityQueue >= qCnt || m_medPriorityWeight >= 100)
		{
			return;
		}
		const int64_t weight = int64_t(m_medPriorityWeight);
		if (qIndex == m_medPriorityQueue)
		{
			m_medDeficit -= int64_t(bytes) * (100 - weight);
		}
		else
		{
			m_medDeficit += int64_t(bytes) * weight;
		}
		const int64_t kMaxImbalance = int64_t(9000) * 100; // ~one jumbo frame
		if (m_medDeficit > kMaxImbalance)
		{
			m_medDeficit = kMaxImbalance;
		}
		else if (m_medDeficit < -kMaxImbalance)
		{
			m_medDeficit = -kMaxImbalance;
		}
	}

	Ptr<Packet>
		BEgressQueue::DoDequeueRR(bool paused[]) //this is for switch only
	{
		NS_LOG_FUNCTION(this);

		if (m_bytesInQueueTotal == 0)
		{
			NS_LOG_LOGIC("Queue empty");
			return 0;
		}
		bool found = false;
		uint32_t qIndex;

		if (m_queues[0]->GetNPackets() > 0) //0 is the highest priority
		{
			found = true;
			qIndex = 0;
		}
		else
		{
			// TC_med (UEC 1.0.3 section 4.1.4.1) carries trimmed packets. It is
			// drained ahead of the round-robin TC_low data queues so trimmed
			// packets "bypass data packets and arrive quickly at the
			// destination", but section 4.1 also warns that unrestricted header
			// bandwidth causes congestion collapse and recommends capping the
			// trimmed share (WDRR at 25%, or fair-queueing at no more than 50%).
			// TC_med is therefore weighted against the TC_low aggregate rather
			// than given strict priority.
			const bool medReady = m_medPriorityQueue < qCnt &&
				!paused[m_medPriorityQueue] &&
				m_queues[m_medPriorityQueue]->GetNPackets() > 0;

			bool lowFound = false;
			uint32_t lowIndex = 0;
			for (uint32_t i = 1; i <= qCnt; i++)
			{
				const uint32_t candidate = (i + m_rrlast) % qCnt;
				if (candidate == m_medPriorityQueue)
					continue;
				if (!paused[candidate] && m_queues[candidate]->GetNPackets() > 0)  //round robin
				{
					lowFound = true;
					lowIndex = candidate;
					break;
				}
			}

			if (medReady && (!lowFound || MedWithinShare()))
			{
				found = true;
				qIndex = m_medPriorityQueue;
			}
			else if (lowFound)
			{
				found = true;
				qIndex = lowIndex;
			}
		}
		if (found)
		{
			Ptr<Packet> p = m_queues[qIndex]->Dequeue();
			m_traceBeqDequeue(p, qIndex);
			m_bytesInQueueTotal -= p->GetSize();
			m_bytesInQueue[qIndex] -= p->GetSize();
			if (qIndex != 0)
			{
				AccountWeightedShare(qIndex, p->GetSize());
			}
			if (qIndex != 0 && qIndex != m_medPriorityQueue)
			{
				m_rrlast = qIndex;
			}
			m_qlast = qIndex;
			NS_LOG_LOGIC("Popped " << p);
			NS_LOG_LOGIC("Number bytes " << m_bytesInQueueTotal);
			return p;
		}
		NS_LOG_LOGIC("Nothing can be sent");
		return 0;
	}

	bool
		BEgressQueue::Enqueue(Ptr<Packet> p, uint32_t qIndex)
	{
		NS_LOG_FUNCTION(this << p);
		//
		// If DoEnqueue fails, Queue::Drop is called by the subclass
		//
		bool retval = DoEnqueue(p, qIndex);
		if (retval)
		{
			NS_LOG_LOGIC("m_traceEnqueue (p)");
			m_traceEnqueue(p);
			m_traceBeqEnqueue(p, qIndex);

			uint32_t size = p->GetSize();
			m_nBytes += size;
			m_nTotalReceivedBytes += size;

			m_nPackets++;
			m_nTotalReceivedPackets++;
		}
		return retval;
	}

	Ptr<Packet>
		BEgressQueue::DequeueRR(bool paused[])
	{
		NS_LOG_FUNCTION(this);
		Ptr<Packet> packet = DoDequeueRR(paused);
		if (packet)
		{
			NS_ASSERT(m_nBytes >= packet->GetSize());
			NS_ASSERT(m_nPackets > 0);
			m_nBytes -= packet->GetSize();
			m_nPackets--;
			NS_LOG_LOGIC("m_traceDequeue (packet)");
			m_traceDequeue(packet);
		}
		return packet;
	}

	bool
		BEgressQueue::DoEnqueue(Ptr<Packet> p)	//for compatiability
	{
		//std:://cout << "Warning: Call Broadcom queues without priority\n";
		uint32_t qIndex = 0;
		NS_LOG_FUNCTION(this << p);
		if (m_bytesInQueueTotal + p->GetSize() < m_maxBytes)
		{
			m_queues[qIndex]->Enqueue(p);
			m_bytesInQueueTotal += p->GetSize();
			m_bytesInQueue[qIndex] += p->GetSize();
		}
		else
		{
			return false;

		}
		return true;
	}


	Ptr<Packet>
		BEgressQueue::DoDequeue(void)
	{
		NS_ASSERT_MSG(false, "BEgressQueue::DoDequeue not implemented");
		return 0;
	}


	Ptr<const Packet>
		BEgressQueue::DoPeek(void) const	//DoPeek doesn't work for multiple queues!!
	{
		//std:://cout << "Warning: Call Broadcom queues without priority\n";
		NS_LOG_FUNCTION(this);
		if (m_bytesInQueueTotal == 0)
		{
			NS_LOG_LOGIC("Queue empty");
			return 0;
		}
		NS_LOG_LOGIC("Number bytes " << m_bytesInQueue);
		return m_queues[0]->Peek();
	}

	uint32_t
		BEgressQueue::GetNBytes(uint32_t qIndex) const
	{
		return m_bytesInQueue[qIndex];
	}


	uint32_t
		BEgressQueue::GetNBytesTotal() const
	{
		return m_bytesInQueueTotal;
	}

	uint32_t
		BEgressQueue::GetLastQueue()
	{
		return m_qlast;
	}

}
