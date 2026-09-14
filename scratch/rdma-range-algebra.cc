// Self-checking fixture for the receive-side range algebra that the recovery
// domain charges its budget from, and for the sender's congestion exemption.
// Exits 0 when every law holds and 1 with the failing law on stderr otherwise.
//
// A fabric run cannot reach these branches. The sender segments from byte
// zero at the MTU and every repair segment starts at a packet boundary, so a
// trim that straddles ReceiverNextExpectedSeq or partly overlaps an accepted
// range is unreachable through the switch. The clip exists so the charge
// equals the absorbed bytes by construction rather than by that alignment
// assumption, and that is what this asserts directly.
//
// The exemption is here for a different reason: it ends on one event, and a
// bundle reports only how many flows re-armed, never on which event.

#include "ns3/core-module.h"
#include "ns3/custom-header.h"
#include "ns3/qbb-header.h"
#include "ns3/qbb-net-device.h"
#include "ns3/rdma-hw.h"
#include "ns3/rdma-queue-pair.h"

#include <cstdint>
#include <iostream>
#include <string>

using ns3::CreateObject;
using ns3::CustomHeader;
using ns3::Ipv4Address;
using ns3::Ptr;
using ns3::QbbNetDevice;
using ns3::RdmaHw;
using ns3::RdmaInterfaceMgr;
using ns3::RdmaQueuePair;
using ns3::RdmaRxQueuePair;

namespace {

int failures = 0;

void Expect(const std::string &law, uint64_t actual, uint64_t expected){
	if (actual == expected)
		return;
	std::cerr << "range algebra failed: " << law << ": got " << actual
		<< ", want " << expected << "\n";
	failures++;
}

int cc_rearmed_events = 0;

void CountTransportEvent(const char *event, uint64_t bytes){
	(void)bytes;
	if (std::string(event) == "cc_rearmed")
		cc_rearmed_events++;
}

}  // namespace

int main(){
	// A trim of an untouched range charges its whole length.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		Expect("untouched range is charged in full",
			q->UnsettledBytes(1000, 2000), 1000);
	}
	// A trim straddling the cumulative sequence charges only the bytes above
	// it. Charging 1000 here is the double-spend the clip removes.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->ReceiverNextExpectedSeq = 1500;
		Expect("straddling trim is clipped at the frontier",
			q->UnsettledBytes(1000, 2000), 500);
		Expect("range wholly below the frontier is settled",
			q->UnsettledBytes(500, 1500), 0);
	}
	// A trim overlapping an accepted out-of-order range charges the remainder.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->AddOutOfOrderRange(2000, 2500);
		Expect("partial overlap charges the uncovered bytes",
			q->UnsettledBytes(2000, 3000), 500);
		Expect("partial overlap on the left charges the uncovered bytes",
			q->UnsettledBytes(1500, 2200), 500);
		Expect("covered range is settled", q->UnsettledBytes(2100, 2400), 0);
	}
	// Two disjoint accepted ranges each subtract once, and no byte twice.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->AddOutOfOrderRange(3000, 3200);
		q->AddOutOfOrderRange(3400, 3600);
		Expect("every accepted range subtracts once",
			q->UnsettledBytes(3000, 3600), 200);
	}
	// The frontier and the accepted ranges are one budget, not two.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->ReceiverNextExpectedSeq = 1000;
		q->AddOutOfOrderRange(1200, 1400);
		Expect("frontier and accepted ranges do not double-subtract",
			q->UnsettledBytes(800, 1600), 400);
	}
	// Forgiving absorbs exactly what it was charged, so a repeated trim of the
	// same range is settled and charges nothing.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		const uint64_t charged = q->UnsettledBytes(4000, 5000);
		q->AddOutOfOrderRange(4000, 5000);
		Expect("forgiving absorbs what it charged", charged, 1000);
		Expect("a repeated trim of a forgiven range charges nothing",
			q->UnsettledBytes(4000, 5000), 0);
	}
	// The sender's exemption. A repair request says the receiver did not
	// forgive this range, which a replay of an outstanding request also says,
	// so only the allowance report may end it.
	{
		const Ipv4Address sender("11.0.0.1");
		const Ipv4Address receiver("11.0.1.1");
		Ptr<RdmaHw> hw = CreateObject<RdmaHw>();
		hw->m_cc_mode = 0;
		hw->m_ack_interval = 1;
		hw->m_backto0 = false;
		hw->m_retransmission_timeout_ns = 0;
		hw->m_no_progress_timeout_ns = 0;
		hw->m_max_retransmission_retries = 1024;
		hw->m_selective_retransmission = true;
		hw->m_transportEventCallback = ns3::MakeCallback(&CountTransportEvent);
		Ptr<QbbNetDevice> device = CreateObject<QbbNetDevice>();
		RdmaInterfaceMgr nic;
		nic.dev = device;
		hw->m_nic.push_back(nic);
		hw->m_rtTable[receiver.Get()].push_back(0);

		Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(
			3, sender, receiver, 10000, 10001);
		qp->m_size = 3000;
		qp->snd_nxt = 2000;
		qp->m_highest_sent = 2000;
		qp->m_cc_exempt = true;
		hw->m_qpMap[RdmaHw::GetQpKey(receiver.Get(), 10000, 3)] = qp;

		CustomHeader request;
		request.sip = receiver.Get();
		request.dip = sender.Get();
		request.ack.flags = 0;
		request.ack.dport = 10000;
		request.ack.sport = 10001;
		request.ack.pg = 3;
		request.ack.seq = 0;
		request.ack.trim_payload_size = 1000;

		hw->RecoverTrimmedQueue(qp, request);
		hw->RecoverTrimmedQueue(qp, request);
		Expect("a replayed repair request leaves the exemption in place",
			qp->m_cc_exempt, 1);

		// The report that the entry is spent, then two more of them. The
		// exemption ends once, so a re-arm cannot be counted twice.
		request.ack.seq = 1000;
		request.ack.flags =
			1 << ns3::qbbHeader::FLAG_ALLOWANCE_EXHAUSTED;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("an allowance report ends the exemption", qp->m_cc_exempt, 0);
		request.ack.seq = 2000;
		hw->RecoverTrimmedQueue(qp, request);
		hw->RecoverTrimmedQueue(qp, request);
		Expect("the exemption ends once", cc_rearmed_events, 1);
		Expect("every report is counted", qp->m_allowance_spent_signalled, 3);
	}
	ns3::Simulator::Destroy();
	if (failures > 0)
		return 1;
	std::cout << "range algebra laws hold\n";
	return 0;
}
