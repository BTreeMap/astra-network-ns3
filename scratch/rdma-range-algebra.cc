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
// The exemption is here for a different reason: it follows a one-bit report
// both ways, and a bundle reports only how many flows changed state, never on
// which report.
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
using AstraSimNs3::affords_soft;
using AstraSimNs3::budget_gone;
using AstraSimNs3::kDecisionScale;
using AstraSimNs3::kPacketPayload;
using AstraSimNs3::range_coin;
using AstraSimNs3::remainder_verdict;
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

int cc_transition_events = 0;

void CountTransportEvent(const char *event, uint64_t bytes){
	(void)bytes;
	if (std::string(event) == "cc_transition")
		cc_transition_events++;
}

// The frontend's two answers, as a stopped sender and a spent cell would give
// them. The fixture drives the transport, so it stands in for the frontend.
uint64_t StopWholeFlow(uint32_t, uint32_t, uint16_t, uint16_t, uint64_t,
		uint64_t){
	return 8192;
}

bool AlwaysGone(uint32_t, uint32_t, uint16_t, uint16_t, uint64_t){
	return true;
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
	// The plan is part of every forgiving cell, because the report is
	// measured against the step's total rather than against the cap that has
	// vested so far.
	cell.owed = eligible;
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
		if (trim_verdict(SettledCell(eligible, spend), threshold,
				Pacing{NoPacing{}}, 0, spend).first)
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
		Expect("a range inside the cap is forgiven", answer.first, 1);
		Expect("a forgiven range is charged", answer.second.forgiven, 1000);
		Expect("a cell with room reports none gone",
			budget_gone(answer.second, kTenPercent) ? 1 : 0, 0);
	}
	// A range above the cap is refused and nothing is charged. The refusal
	// itself says nothing about the report: what the report reads is the
	// holes, and a refused range becomes one only once the receiver counts it.
	{
		const auto answer = trim_verdict(SettledCell(1000000, 200000),
			kTenPercent, Pacing{NoPacing{}}, 0, 200000);
		Expect("a range above the cap is repaired", answer.first, 0);
		Expect("a refused range is not charged", answer.second.forgiven, 0);
		Expect("a refusal on its own reports nothing",
			budget_gone(answer.second, kTenPercent) ? 1 : 0, 0);
	}
	// The report against the step's total: even if every byte now missing
	// were forgiven, one more packet would not fit. Holes and forgiven bytes
	// enter it the same way, because a forgiven range is absorbed as received
	// and has stopped being a hole.
	{
		StepLedger cell;
		cell.owed = 100000;  // p x owed is 10000 bytes at kTenPercent
		cell.forgiven = 5000;
		cell.holes = 10000 - kPacketPayload - 5000 - 1;
		Expect("one packet still fits under the line",
			budget_gone(cell, kTenPercent) ? 1 : 0, 0);
		cell.holes++;
		Expect("a packet that exactly fills the line still fits",
			budget_gone(cell, kTenPercent) ? 1 : 0, 0);
		cell.holes++;
		Expect("the report fires when no packet fits",
			budget_gone(cell, kTenPercent) ? 1 : 0, 1);
		// Not a latch. The holes drain as their repairs land, so the same
		// cell reports room again without anything being refunded.
		cell.holes = 0;
		Expect("a repaired hole turns the report green again",
			budget_gone(cell, kTenPercent) ? 1 : 0, 0);
	}
	// A soft refusal charges nothing and says nothing: the vesting cap will
	// grow, so the refusal is about now and not about the rest of the step.
	{
		StepLedger cell;
		cell.eligible = 1000000;
		cell.owed = 1000000;
		cell.delivered = 100000;  // affords 11111 under the soft cap
		const auto answer = trim_verdict(cell, kTenPercent,
			Pacing{NoPacing{}}, 0, 20000);
		Expect("the soft cap refuses what has not vested", answer.first, 0);
		Expect("a soft refusal charges nothing", answer.second.forgiven, 0);
		Expect("a soft refusal sets no report",
			budget_gone(answer.second, kTenPercent) ? 1 : 0, 0);
	}
	// The flooding sender. Delivery stalls, so the soft cap stalls with it and
	// `forgiven` never grows; the holes are what reach the line instead, and
	// they are what the report is measured on.
	{
		StepLedger cell;
		cell.eligible = 1000000;
		cell.owed = 1000000;  // the step's allowance is 100000 bytes
		bool reported = false;
		for (int trim = 0; trim < 5; trim++){
			const auto answer = trim_verdict(cell, kTenPercent,
				Pacing{NoPacing{}}, 0, 30000);
			Expect("nothing vests, so nothing is forgiven", answer.first, 0);
			cell = answer.second;
			// The refused range stays missing, so the receiver's holes grow.
			cell.holes += 30000;
			reported = budget_gone(cell, kTenPercent);
			Expect("the report fires once the holes pass the allowance",
				reported ? 1 : 0,
				cell.holes + kPacketPayload > 100000 ? 1 : 0);
		}
		Expect("the flooding sender is told to obey its controller",
			reported ? 1 : 0, 1);
	}
	// A coin refusal charges nothing and reports nothing: the receiver chose
	// it, and the fabric asked for nothing it could not have.
	{
		const Pacing never{Bernoulli{1}};
		const auto answer = trim_verdict(SettledCell(1000000, 1000),
			kTenPercent, never, kDecisionScale - 1, 1000);
		Expect("the coin can refuse a range the cap affords", answer.first, 0);
		Expect("a coin refusal charges nothing", answer.second.forgiven, 0);
		Expect("a coin refusal reports no spent allowance",
			budget_gone(answer.second, kTenPercent) ? 1 : 0, 0);
	}
	// The charge that fills the cap leaves no room for one more packet, so
	// the same answer carries the report.
	{
		const auto answer = trim_verdict(SettledCell(1000000, 100000),
			kTenPercent, Pacing{NoPacing{}}, 0, 100000);
		Expect("the charge that fills the cap is still forgiven",
			answer.first, 1);
		Expect("a cap with no room for one packet is gone",
			budget_gone(answer.second, kTenPercent) ? 1 : 0, 1);
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
			cell.owed = 1000000;
			cell.delivered = delivered;
			const bool now = trim_verdict(cell, kTenPercent,
				Pacing{NoPacing{}}, 0, bytes).first;
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
			cell.owed = 1000000;
			cell.delivered = 800000;
			cell.forgiven = forgiven;
			const bool now = trim_verdict(cell, kTenPercent,
				Pacing{NoPacing{}}, 0, bytes).first;
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
		cell.owed = 1000000;
		for (uint64_t bytes = 1; bytes <= 100000; bytes *= 10){
			Expect("nothing is forgiven before anything arrives",
				trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
					bytes).first, 0);
		}
		Expect("the floor lifts only at a tolerance of one",
			trim_verdict(cell, kDecisionScale, Pacing{NoPacing{}}, 0,
				1000).first, 1);
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
			trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
				100000).first, 1);
		Expect("the owed base refuses a byte above the cap",
			trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
				100001).first, 0);
		cell.delivered = 1000000;
		Expect("arrivals do not lift the owed ceiling",
			trim_verdict(cell, kTenPercent, Pacing{NoPacing{}}, 0,
				100001).first, 0);
	}
	// The step stop, per sender. The receiver ends one sender's step once
	// 1 - p of what that sender owes it has arrived, and takes the hole it
	// leaves; a sender still owed bytes keeps its remainder, however well the
	// pool could afford it.
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
		Expect("without the stop no remainder is taken at all",
			remainder_verdict(cell, kTenPercent, 100, 2, false).first, 0);
		Expect("the pool still bounds a stopped sender",
			remainder_verdict(cell, kTenPercent, 300, 1, true).first, 0);
	}
	// The stop is judged against the step pool, not against what has vested,
	// and it composes with either base. This cell runs on the soft base and
	// has accounted for nothing, so the vesting cap refuses every byte of the
	// remainder; the pool is p x owed less what the step has already
	// forgiven, and that is what the stop spends.
	{
		StepLedger cell;
		cell.owed = 2000;
		cell.by_sender[1].owed = 1000;
		cell.by_sender[1].delivered = 900;
		Expect("the stop runs on the soft base here", cell.owed_base ? 1 : 0,
			0);
		Expect("the vesting cap would refuse the whole remainder",
			affords_soft(cell, kTenPercent, 200) ? 1 : 0, 0);
		Expect("a remainder inside the pool is forgiven whole",
			remainder_verdict(cell, kTenPercent, 200, 1, true).first, 200);
		cell.forgiven = 100;
		Expect("what the step already forgave leaves the pool",
			remainder_verdict(cell, kTenPercent, 200, 1, true).first, 0);
		Expect("what is left of the pool is still spendable",
			remainder_verdict(cell, kTenPercent, 100, 1, true).first, 100);
		// The same cell on the owed base answers the same, because the pool
		// is the plan either way.
		cell.forgiven = 0;
		cell.owed_base = true;
		Expect("the owed base does not change what the stop may take",
			remainder_verdict(cell, kTenPercent, 200, 1, true).first, 200);
	}
	// The coin is drawn per trimmed arrival: the same range asked twice draws
	// twice, so a refusal is not remembered and the range meets the cap as it
	// stands on its next trim. Two arms at one seed draw the same sequence,
	// because the attempt number is the flow's own count of verdicts.
	{
		const uint64_t flow_hash = 0x5eed1234abcdULL;
		uint64_t admitted = 0;
		uint64_t redrawn = 0;
		for (uint64_t start = 0; start < 4096 * 64; start += 4096){
			const uint32_t attempt =
				static_cast<uint32_t>(start / 4096) + 1;
			const uint64_t coin = range_coin(flow_hash, start, attempt);
			Expect("one arm repeats the other's draw",
				range_coin(flow_hash, start, attempt), coin);
			Expect("the coin is inside the decision scale",
				coin < kDecisionScale, 1);
			redrawn += range_coin(flow_hash, start, attempt + 1) != coin ? 1
				: 0;
			const Pacing half{Bernoulli{kDecisionScale / 2}};
			admitted += trim_verdict(SettledCell(1000000, 1000),
				kTenPercent, half, coin, 1000).first ? 1 : 0;
		}
		// 64 ranges. A coin that ignored the attempt would redraw none of
		// them; collisions at one in a million make a few plausible.
		Expect("the same range asked twice draws two coins", redrawn > 60, 1);
		// 64 draws at p = 0.5. A coin that ignored its inputs would sit at 0
		// or 64, which is all this needs to exclude.
		Expect("the coin admits some ranges and refuses others",
			admitted > 8 && admitted < 56, 1);
	}
	// The remainder is whole or nothing, and only a stopped sender has one.
	// The coin does not reach it at all, because the rule is not one of its
	// arguments: pacing keeps allowance for the end of the step, and the
	// remainder is asked at the end of the step.
	{
		StepLedger cell = SettledCell(1000000, 50000);
		cell.by_sender[1].owed = 1000000;
		cell.by_sender[1].delivered = 950000;
		const auto affordable =
			remainder_verdict(cell, kTenPercent, 50000, 1, true);
		Expect("a remainder inside the pool is forgiven whole",
			affordable.first, 50000);
		Expect("a forgiven remainder is charged in full",
			affordable.second.forgiven, 50000);
		const auto refused =
			remainder_verdict(cell, kTenPercent, 150000, 1, true);
		Expect("a remainder above the pool is refused whole", refused.first, 0);
		Expect("a refused remainder charges nothing",
			refused.second.forgiven, 0);
		const auto empty = remainder_verdict(cell, kTenPercent, 0, 1, true);
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
	// The holes, and their exclusion from the forgiven bytes. A trim header
	// raises the frontier without any data arriving, which is what makes the
	// trimmed range a hole; forgiving it absorbs it as received, so it stops
	// being one and the two terms of the report never count the same byte.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		Expect("a fresh queue pair has no holes", q->Holes(), 0);
		q->NoteSeen(8192);
		Expect("a trim header advances the highest sequence seen",
			q->Holes(), 8192);
		q->NoteForgiven(4096, 8192);
		q->AddOutOfOrderRange(4096, 8192);
		Expect("a forgiven range is never a hole", q->Holes(), 4096);
		Expect("what was given up is on record",
			q->ForgivenBytes(4096, 8192), 4096);
		Expect("only what was given up is on record",
			q->ForgivenBytes(0, 4096), 0);
		// The repair of the range that was refused drains the last hole.
		q->AddOutOfOrderRange(0, 4096);
		q->ReceiverNextExpectedSeq =
			static_cast<uint32_t>(q->AbsorbContiguousFrom(0));
		Expect("a repaired hole drains", q->Holes(), 0);
		Expect("data arriving for a forgiven range is late, not new",
			q->ForgivenBytes(4096, 8192), 4096);
	}
	// A forgiveness records only the bytes it gave up, so a range the
	// receiver already held is not charged to the sender twice when it
	// arrives again.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->AddOutOfOrderRange(2000, 2500);
		q->NoteForgiven(1500, 3000);
		Expect("the held part of a forgiven range is not given up",
			q->ForgivenBytes(2000, 2500), 0);
		Expect("the rest of it is", q->ForgivenBytes(1500, 3000), 1000);
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
	// The table's bounds, and the certification that waits for the step's
	// last collective. A rank running two collectives in a step is still
	// receiving the second while the first is done, so the first leaves the
	// cell alone.
	{
		ForgivenessLedger ledger = ForgivenessLedger::make(4, 3);
		ledger.register_owed(2, 1, 3, 1000000);
		ledger.register_collectives(2, 1, 2);
		ledger.register_eligible(2, 1, 1000000);
		Expect("a cell inside the table is available",
			ledger.cell(2, 1) != nullptr, 1);
		Expect("a rank outside the table is not",
			ledger.cell(9, 1) == nullptr, 1);
		Expect("a step outside the table is not",
			ledger.cell(2, 9) == nullptr, 1);
		ledger.note_collective_completed(2, 1);
		Expect("the first collective leaves the cell open",
			ledger.cell(2, 1)->completed, 1);
		ledger.note_collective_completed(2, 1);
		Expect("the last collective certifies it",
			ledger.cell(2, 1)->completed, 2);
	}
	// A launch into a cell the plan did not name ends the run, because every
	// decision downstream is measured against that plan.
	{
		ForgivenessLedger ledger = ForgivenessLedger::make(1, 1);
		bool threw = false;
		try {
			ledger.register_eligible(0, 1, 4096);
		} catch (const std::exception &) {
			threw = true;
		}
		Expect("a launch the plan did not name is refused", threw ? 1 : 0, 1);
	}
	// Delivered bytes accumulate separately from launched ones, which is what
	// releases the budget as the step proceeds, and each arrival is credited
	// to the sender that sent it, which is what the step stop reads.
	{
		ForgivenessLedger ledger = ForgivenessLedger::make(2, 1);
		ledger.register_owed(0, 1, 1, 1000000);
		ledger.register_eligible(0, 1, 1000000);
		Expect("an entry that has received nothing has nothing to spend",
			trim_verdict(*ledger.cell(0, 1), kTenPercent,
				Pacing{NoPacing{}}, 0, 1000).first, 0);
		ledger.register_delivered(0, 1, 1, 500000);
		Expect("a delivered message releases budget",
			trim_verdict(*ledger.cell(0, 1), kTenPercent,
				Pacing{NoPacing{}}, 0, 1000).first, 1);
		Expect("the arrival is credited to its sender",
			ledger.cell(0, 1)->by_sender.at(1).delivered, 500000);
		Expect("no other sender is credited",
			ledger.cell(0, 1)->by_sender.count(2), 0);
	}
	// The arrival that carries a sender across 1 - p is the one that stops it,
	// and it stops that sender alone. The receiver acts on that arrival
	// because a flow waiting on a repair receives nothing.
	{
		AstraSimNs3::experiment_config.enabled = true;
		AstraSimNs3::experiment_config.domain =
			AstraSimNs3::SheddingDomain::Recovery;
		AstraSimNs3::experiment_config.step_stop = true;
		AstraSimNs3::experiment_config.p_high_threshold = kTenPercent;
		AstraSimNs3::experiment_config.clr_mask_by_step[1] = false;
		AstraSimNs3::forgiveness_ledger = ForgivenessLedger::make(4, 1);
		AstraSimNs3::forgiveness_ledger.register_owed(0, 1, 1, 1000);
		AstraSimNs3::forgiveness_ledger.register_owed(0, 1, 2, 1000);
		AstraSimNs3::forgiveness_ledger.register_eligible(0, 1, 2000);

		// Two open flows from sender 1 and one from sender 2.
		AstraSimNs3::FlowRecord first;
		first.admission_eligible = true;
		first.src = 1;
		first.dst = 0;
		first.operation.training_step = 1;
		AstraSimNs3::FlowRecord second = first;
		AstraSimNs3::FlowRecord other = first;
		other.src = 2;

		Expect("an arrival short of 1 - p stops nobody",
			AstraSimNs3::note_delivered(first, 899, 0) ? 1 : 0, 0);
		Expect("the arrival that crosses 1 - p stops its sender",
			AstraSimNs3::note_delivered(second, 1, 0) ? 1 : 0, 1);
		Expect("the next arrival from that sender stops nobody again",
			AstraSimNs3::note_delivered(first, 1, 0) ? 1 : 0, 0);
		Expect("the other sender is untouched",
			AstraSimNs3::note_delivered(other, 899, 0) ? 1 : 0, 0);
		Expect("late bytes for a forgiven range are counted on the flow",
			AstraSimNs3::note_delivered(first, 0, 512) ? 1 : 0, 0);
		Expect("and only those", first.late_forgiven_bytes, 512);

		// Both of the stopped sender's flows are forgiven their holes on that
		// crossing; the other sender's is not.
		const StepLedger *cell = AstraSimNs3::forgiveness_ledger.cell(0, 1);
		Expect("the stopped sender's first flow is forgiven its hole",
			remainder_verdict(*cell, kTenPercent, 50, 1, true).first, 50);
		Expect("the stopped sender's second flow is forgiven its hole",
			remainder_verdict(*cell, kTenPercent, 40, 1, true).first, 40);
		Expect("the other sender's flow keeps its remainder",
			remainder_verdict(*cell, kTenPercent, 50, 2, true).first, 0);
		AstraSimNs3::experiment_config = AstraSimNs3::ExperimentConfig{};
		AstraSimNs3::forgiveness_ledger = ForgivenessLedger{};
	}
	// The sender's exemption follows the latest report, both ways. A packet
	// from a receiver that does not mark the flow says nothing, a marked one
	// with room grants it, a marked one with none returns the sender to its
	// controller, and the next one with room grants it again.
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
		hw->m_congestionExemption = true;
		hw->m_reengage = true;
		hw->m_transportEventCallback = ns3::MakeCallback(&CountTransportEvent);
		Ptr<QbbNetDevice> device = CreateObject<QbbNetDevice>();
		RdmaInterfaceMgr nic;
		nic.dev = device;
		hw->m_nic.push_back(nic);
		hw->m_rtTable[receiver.Get()].push_back(0);

		Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(
			3, sender, receiver, 10000, 10001);
		qp->m_size = 5000;
		qp->snd_nxt = 4000;
		qp->m_highest_sent = 4000;
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
		Expect("an unmarked repair request grants nothing", qp->m_cc_exempt,
			0);

		const uint32_t eligible = 1 << ns3::qbbHeader::FLAG_FORGIVENESS_ELIGIBLE;
		const uint32_t spent = 1 << ns3::qbbHeader::FLAG_ALLOWANCE_EXHAUSTED;
		request.ack.seq = 1000;
		request.ack.flags = eligible;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("a marked report with room grants the exemption",
			qp->m_cc_exempt, 1);
		Expect("the grant is not a transition", qp->m_cc_transitions, 0);
		request.ack.seq = 2000;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("a repeated report leaves the exemption in place",
			qp->m_cc_exempt, 1);
		Expect("a repeated bit is not a transition", qp->m_cc_transitions, 0);

		request.ack.seq = 3000;
		request.ack.flags = eligible | spent;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("a report with no room returns the sender to its controller",
			qp->m_cc_exempt, 0);
		Expect("the changed bit is one transition", qp->m_cc_transitions, 1);

		request.ack.seq = 4000;
		request.ack.flags = eligible;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("a later report with room withholds again", qp->m_cc_exempt, 1);
		Expect("both changes are counted", qp->m_cc_transitions, 2);
		Expect("both changes are reported", cc_transition_events, 2);
		Expect("every report of no room is counted",
			qp->m_allowance_gone_reports, 1);
		// Simulated time never advanced, so the stretch of obeying is empty;
		// what this pins is that a flow which saw a set bit still spends none
		// of the time it held a grant obeying.
		Expect("obeying time covers only the stretches under the controller",
			qp->m_cc_obeying_ns, 0);
	}
	// A flow that never saw a set bit spends no time obeying, and the arm
	// that never re-engages keeps its exemption through one.
	{
		const Ipv4Address sender("11.0.0.2");
		const Ipv4Address receiver("11.0.1.2");
		Ptr<RdmaHw> hw = CreateObject<RdmaHw>();
		hw->m_cc_mode = 0;
		hw->m_ack_interval = 1;
		hw->m_backto0 = false;
		hw->m_retransmission_timeout_ns = 0;
		hw->m_no_progress_timeout_ns = 0;
		hw->m_max_retransmission_retries = 1024;
		hw->m_selective_retransmission = true;
		hw->m_congestionExemption = true;
		hw->m_reengage = false;
		Ptr<QbbNetDevice> device = CreateObject<QbbNetDevice>();
		RdmaInterfaceMgr nic;
		nic.dev = device;
		hw->m_nic.push_back(nic);
		hw->m_rtTable[receiver.Get()].push_back(0);

		Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(
			3, sender, receiver, 10000, 10001);
		qp->m_size = 5000;
		qp->snd_nxt = 4000;
		qp->m_highest_sent = 4000;
		hw->m_qpMap[RdmaHw::GetQpKey(receiver.Get(), 10000, 3)] = qp;

		CustomHeader request;
		request.sip = receiver.Get();
		request.dip = sender.Get();
		request.ack.dport = 10000;
		request.ack.sport = 10001;
		request.ack.pg = 3;
		request.ack.trim_payload_size = 1000;
		request.ack.seq = 0;
		request.ack.flags = 1 << ns3::qbbHeader::FLAG_FORGIVENESS_ELIGIBLE;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("the reference arm is granted like any other", qp->m_cc_exempt,
			1);
		request.ack.seq = 1000;
		request.ack.flags |= 1 << ns3::qbbHeader::FLAG_ALLOWANCE_EXHAUSTED;
		hw->RecoverTrimmedQueue(qp, request);
		Expect("the reference arm ignores a set bit", qp->m_cc_exempt, 1);
		Expect("it still counts the report",
			qp->m_allowance_gone_reports, 1);
		Expect("a flow that never obeys spends no time obeying",
			qp->m_cc_obeying_ns, 0);
		Expect("and never opened a stretch of it", qp->m_cc_obey_since_ns, 0);
	}
	// After its stop a receive queue pair says nothing more about
	// forgiveness, so nothing it acknowledges can grant an exemption or move
	// one.
	{
		const Ipv4Address sender("11.0.0.3");
		const Ipv4Address receiver("11.0.1.3");
		Ptr<RdmaHw> hw = CreateObject<RdmaHw>();
		hw->m_ack_interval = 1;
		hw->m_selective_retransmission = true;
		Ptr<QbbNetDevice> device = CreateObject<QbbNetDevice>();
		RdmaInterfaceMgr nic;
		nic.dev = device;
		hw->m_nic.push_back(nic);
		hw->m_rtTable[sender.Get()].push_back(0);
		hw->m_remainderVerdictCallback = ns3::MakeCallback(&StopWholeFlow);
		hw->m_allowanceGoneCallback = ns3::MakeCallback(&AlwaysGone);

		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->sip = receiver.Get();
		q->dip = sender.Get();
		q->sport = 100;
		q->dport = 10001;
		q->m_forgiveness_eligible = true;
		q->ReceiverNextExpectedSeq = 4096;
		q->NoteSeen(4096);
		Expect("an eligible queue pair reports on its budget",
			hw->AllowanceGone(q) ? 1 : 0, 1);
		hw->AskRemainderOnArrival(q);
		Expect("the stop absorbs the remainder",
			q->ReceiverNextExpectedSeq, 8192);
		Expect("the stop records what it gave up",
			q->ForgivenBytes(4096, 8192), 4096);
		Expect("after its stop the queue pair grants nothing",
			q->m_forgiveness_eligible, 0);
		Expect("and reports nothing", hw->AllowanceGone(q) ? 1 : 0, 0);
	}
	ns3::Simulator::Destroy();
	if (failures > 0)
		return 1;
	std::cout << "range algebra and forgiveness core laws hold\n";
	return 0;
}
