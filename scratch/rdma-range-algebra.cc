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
//
// The forgiveness core is here for a third: it is a pure function of a budget
// entry, and its laws hold over entries a fabric run would need a whole wave
// to reach, if it reached them at all.

#include "astra-sim/network_frontend/ns3/ExperimentConfig.hh"
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

using AstraSimNs3::Bernoulli;
using AstraSimNs3::ForgivenessLedger;
using AstraSimNs3::NoPacing;
using AstraSimNs3::Pacing;
using AstraSimNs3::StepLedger;
using AstraSimNs3::forgave;
using AstraSimNs3::kDecisionScale;
using AstraSimNs3::range_coin;
using AstraSimNs3::remainder_verdict;
using AstraSimNs3::revokes_exemption;
using AstraSimNs3::trim_verdict;

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

namespace {

// A budget entry whose rank has accounted for every launched byte except the
// range under test. The receiver measures the budget against what it has
// accounted for, kept or forgiven, so this is the entry at which the law
// reaches its ceiling of threshold / kDecisionScale of the launched bytes.
StepLedger SettledCell(uint64_t eligible, uint64_t outstanding){
	StepLedger cell;
	cell.eligible = eligible;
	cell.delivered = eligible - outstanding;
	return cell;
}

const uint64_t kTenPercent = kDecisionScale / 10;

// The largest total such an entry can spend. The law is monotone in the
// spend, which adds kDecisionScale to the charge for every threshold it adds
// to the base, so a bisection finds it.
uint64_t LargestSpend(uint64_t eligible, uint64_t threshold){
	uint64_t low = 0;
	uint64_t high = eligible;
	while (low < high){
		const uint64_t spend = low + (high - low + 1) / 2;
		if (forgave(trim_verdict(SettledCell(eligible, spend), threshold,
				Pacing{NoPacing{}}, 0, spend).first))
			low = spend;
		else
			high = spend - 1;
	}
	return low;
}

}  // namespace

int main(){
	// The forgiveness core. A range inside the cap is forgiven and charged,
	// and the entry that pays for it keeps the charge.
	{
		const auto answer = trim_verdict(SettledCell(1000000, 1000),
			kTenPercent, Pacing{NoPacing{}}, 0, 1000);
		Expect("a range inside the cap is forgiven", forgave(answer.first), 1);
		Expect("a forgiven range is charged", answer.second.forgiven, 1000);
		Expect("a cap with room reports none spent",
			revokes_exemption(answer.first), 0);
	}
	// A range above the cap is refused, nothing is charged, and the entry is
	// reported spent because it could not take the range in front of it.
	{
		const auto answer = trim_verdict(SettledCell(1000000, 200000),
			kTenPercent, Pacing{NoPacing{}}, 0, 200000);
		Expect("a range above the cap is repaired", forgave(answer.first), 0);
		Expect("a refused range is not charged", answer.second.forgiven, 0);
		Expect("a refusal reports the entry spent",
			revokes_exemption(answer.first), 1);
	}
	// The spent bit after a charge asks about one further byte, which is the
	// smallest range any later trim could carry.
	{
		const auto answer = trim_verdict(SettledCell(1000000, 100000),
			kTenPercent, Pacing{NoPacing{}}, 0, 100000);
		Expect("the charge that fills the cap is still forgiven",
			forgave(answer.first), 1);
		Expect("a cap with no room for one byte is spent",
			revokes_exemption(answer.first), 1);
	}
	// The ceiling. Once the rank has accounted for every byte it does not
	// spend, the law affords exactly threshold / kDecisionScale of the
	// launched bytes, which is the v1 cap. A denominator of delivered alone
	// would stop at eligible / 11 here, because the spend is then measured
	// against what is left rather than against the whole step, and that is
	// what this pins.
	{
		Expect("the law spends a tenth of the eligible bytes",
			LargestSpend(1000000, kTenPercent), 100000);
		Expect("the ceiling follows the threshold",
			LargestSpend(1000000, kDecisionScale / 4), 250000);
	}
	// Monotone in both counters the receiver holds: a byte that arrives never
	// withdraws budget, and a byte forgiven never buys room for another. The
	// two together are what makes the law absorbing.
	{
		const uint64_t bytes = 20000;
		bool afforded = false;
		bool crossed = false;
		for (uint64_t delivered = 0; delivered <= 1000000; delivered += 50000){
			StepLedger cell;
			cell.eligible = 1000000;
			cell.delivered = delivered;
			const bool now = forgave(trim_verdict(cell, kTenPercent,
				Pacing{NoPacing{}}, 0, bytes).first);
			Expect("a delivered byte never withdraws budget",
				!afforded || now, 1);
			crossed = crossed || (now && !afforded && delivered > 0);
			afforded = now;
		}
		Expect("the delivered sweep crosses the cap", crossed && afforded, 1);
		bool refused = false;
		crossed = false;
		for (uint64_t forgiven = 0; forgiven <= 200000; forgiven += 10000){
			StepLedger cell;
			cell.eligible = 1000000;
			cell.delivered = 800000;
			cell.forgiven = forgiven;
			const bool now = forgave(trim_verdict(cell, kTenPercent,
				Pacing{NoPacing{}}, 0, bytes).first);
			Expect("a forgiven byte never buys room", !refused || !now, 1);
			crossed = crossed || (!now && !refused && forgiven > 0);
			refused = !now;
		}
		Expect("the forgiven sweep crosses the cap", crossed && refused, 1);
	}
	// The floor. An entry that has accounted for nothing measures the budget
	// against the spend alone, so it affords a first range of b bytes only
	// when b x kDecisionScale <= b x threshold, which no tolerance below one
	// reaches. That floor is the reservation the delivered counter exists for.
	{
		StepLedger cell;
		cell.eligible = 1000000;
		for (uint64_t bytes = 1; bytes <= 100000; bytes *= 10){
			Expect("nothing is forgiven before anything arrives",
				forgave(trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
					bytes).first), 0);
		}
		Expect("the floor lifts only at a tolerance of one",
			forgave(trim_verdict(cell, kDecisionScale, Pacing{NoPacing{}}, 0,
				1000).first), 1);
	}
	// The owed base. The cap is the same tenth, available in full from the
	// step's first packet rather than as bytes are accounted for, so a cell
	// that has received nothing still affords it.
	{
		StepLedger cell;
		cell.eligible = 1000000;
		cell.owed = 1000000;
		cell.owed_base = true;
		Expect("the owed base affords the cap before anything arrives",
			forgave(trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
				100000).first), 1);
		Expect("the owed base refuses a byte above the cap",
			forgave(trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
				100001).first), 0);
		cell.delivered = 1000000;
		Expect("arrivals do not lift the owed ceiling",
			forgave(trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
				100001).first), 0);
	}
	// The step stop, per sender. The receiver ends one sender's step once
	// 1 - p of what that sender owes it has arrived, and takes the hole it
	// leaves; a sender still owed bytes keeps its remainder, however well the
	// cap could afford it.
	{
		StepLedger cell;
		cell.eligible = 2000;
		cell.owed = 2000;
		cell.owed_base = true;
		cell.by_sender[1].owed = 1000;
		cell.by_sender[2].owed = 1000;
		cell.by_sender[1].delivered = 899;
		Expect("a sender short of 1 - p keeps its remainder",
			remainder_verdict(cell, kTenPercent, 101, 1, true).first, 0);
		cell.by_sender[1].delivered = 900;
		Expect("a sender at 1 - p is stopped",
			remainder_verdict(cell, kTenPercent, 100, 1, true).first, 100);
		Expect("the second sender is not stopped by the first",
			remainder_verdict(cell, kTenPercent, 100, 2, true).first, 0);
		Expect("a sender the plan does not name is never stopped",
			remainder_verdict(cell, kTenPercent, 100, 3, true).first, 0);
		Expect("without the stop the cap alone decides",
			remainder_verdict(cell, kTenPercent, 100, 2, false).first, 100);
		Expect("the cap still bounds a stopped sender",
			remainder_verdict(cell, kTenPercent, 300, 1, true).first, 0);
	}
	// The coin is deterministic per (flow, start), so a range the coin refuses
	// stays refused however often it is re-trimmed, and a range it admits
	// stays admitted.
	{
		const uint64_t flow_hash = 0x5eed1234abcdULL;
		uint64_t admitted = 0;
		for (uint64_t start = 0; start < 4096 * 64; start += 4096){
			const uint64_t coin = range_coin(flow_hash, start);
			Expect("the coin repeats for one (flow, start)",
				range_coin(flow_hash, start), coin);
			Expect("the coin is inside the decision scale",
				coin < kDecisionScale, 1);
			const Pacing half{Bernoulli{kDecisionScale / 2}};
			const auto first = trim_verdict(SettledCell(1000000, 1000),
				kTenPercent, half, coin, 1000);
			const auto again = trim_verdict(SettledCell(1000000, 1000),
				kTenPercent, half, coin, 1000);
			Expect("a re-trimmed range gets the same verdict",
				first.first, again.first);
			admitted += forgave(first.first) ? 1 : 0;
		}
		// 64 draws at p = 0.5. A coin that ignored its inputs would sit at 0
		// or 64, which is all this needs to exclude.
		Expect("the coin admits some ranges and refuses others",
			admitted > 8 && admitted < 56, 1);
	}
	// A coin refusal is not an allowance report: the cap still has room, and
	// an exemption must not end on a decision the receiver made for pacing.
	{
		const Pacing never{Bernoulli{1}};
		const auto answer = trim_verdict(SettledCell(1000000, 1000),
			kTenPercent, never, kDecisionScale - 1, 1000);
		Expect("the coin can refuse a range the cap affords",
			forgave(answer.first), 0);
		Expect("a coin refusal reports no spent allowance",
			revokes_exemption(answer.first), 0);
		Expect("a coin refusal charges nothing", answer.second.forgiven, 0);
	}
	// The remainder is whole or nothing. The coin does not reach it at all,
	// because the rule is not one of its arguments: pacing keeps allowance for
	// the end of the step, and the remainder is asked at the end of the step.
	{
		const auto affordable = remainder_verdict(
			SettledCell(1000000, 50000), kTenPercent, 50000, 1, false);
		Expect("a remainder inside the cap is forgiven whole",
			affordable.first, 50000);
		Expect("a forgiven remainder is charged in full",
			affordable.second.forgiven, 50000);
		const auto refused = remainder_verdict(SettledCell(1000000, 150000),
			kTenPercent, 150000, 1, false);
		Expect("a remainder above the cap is refused whole", refused.first, 0);
		Expect("a refused remainder charges nothing",
			refused.second.forgiven, 0);
		const auto empty = remainder_verdict(SettledCell(1000000, 0),
			kTenPercent, 0, 1, false);
		Expect("an empty remainder is refused", empty.first, 0);
	}
	// The step stop charges the hole, not the span above the cumulative
	// sequence. Under selective repeat a stalled flow keeps accepting packets
	// past the gap, so the two differ in the common case, and charging the
	// span would spend the budget on bytes the receiver already holds.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->ReceiverNextExpectedSeq = 1000;
		q->AddOutOfOrderRange(3000, 4000);
		const uint64_t expected = q->ReceiverNextExpectedSeq;
		const uint64_t size = 5000;
		Expect("accepted bytes above the frontier are counted once",
			q->AcceptedBytesAbove(expected), 1000);
		Expect("the hole is the span less what arrived",
			size - expected - q->AcceptedBytesAbove(expected), 3000);
		// What the transport reports, and what the absorb below takes. The
		// frontend charges the line above, so the two sinks agree.
		Expect("the transport reports the same hole",
			q->UnsettledBytes(expected, size), 3000);
		q->AddOutOfOrderRange(expected, size);
		Expect("absorbing the remainder finishes the flow",
			q->AbsorbContiguousFrom(expected), size);
	}
	// A range straddling the frontier still counts only its part above it, so
	// a forgiven trim that reached below the frontier cannot be charged twice.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->ReceiverNextExpectedSeq = 1000;
		q->AddOutOfOrderRange(500, 1500);
		Expect("a straddling accepted range counts above the frontier only",
			q->AcceptedBytesAbove(1000), 500);
		Expect("a frontier with nothing above it has accepted nothing",
			q->AcceptedBytesAbove(2000), 0);
	}
	// A closed entry is eliminated before any verdict is computed, and the
	// eliminator is the only reader of the flag.
	{
		ForgivenessLedger ledger = ForgivenessLedger::make(4, 3);
		ledger.register_eligible(2, 1, 1000000);
		Expect("an open entry is available", ledger.open_cell(2, 1) != nullptr,
			1);
		ledger.close(2, 1);
		Expect("a closed entry is unavailable",
			ledger.open_cell(2, 1) == nullptr, 1);
		Expect("an entry outside the table is unavailable",
			ledger.open_cell(9, 1) == nullptr, 1);
		Expect("a step outside the table is unavailable",
			ledger.open_cell(2, 9) == nullptr, 1);
	}
	// Delivered bytes accumulate separately from launched ones, which is what
	// releases the budget as the step proceeds, and each arrival is credited
	// to the sender that sent it, which is what the step stop reads.
	{
		ForgivenessLedger ledger = ForgivenessLedger::make(2, 1);
		ledger.register_eligible(0, 1, 1000000);
		Expect("an entry that has received nothing has nothing to spend",
			forgave(trim_verdict(*ledger.open_cell(0, 1), kTenPercent,
				Pacing{NoPacing{}}, 0, 1000).first), 0);
		ledger.register_delivered(0, 1, 1, 500000);
		Expect("a delivered message releases budget",
			forgave(trim_verdict(*ledger.open_cell(0, 1), kTenPercent,
				Pacing{NoPacing{}}, 0, 1000).first), 1);
		Expect("the arrival is credited to its sender",
			ledger.open_cell(0, 1)->by_sender.at(1).delivered, 500000);
		Expect("no other sender is credited",
			ledger.open_cell(0, 1)->by_sender.count(2), 0);
	}
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
	std::cout << "range algebra and forgiveness core laws hold\n";
	return 0;
}
