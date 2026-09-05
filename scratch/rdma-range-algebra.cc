// Self-checking fixture for the receive-side range algebra that the recovery
// domain charges its budget from. Exits 0 when every law holds and 1 with the
// failing law on stderr otherwise.
//
// A fabric run cannot reach these branches. The sender segments from byte
// zero at the MTU and every repair segment starts at a packet boundary, so a
// trim that straddles ReceiverNextExpectedSeq or partly overlaps an accepted
// range is unreachable through the switch. The clip exists so the charge
// equals the absorbed bytes by construction rather than by that alignment
// assumption, and that is what this asserts directly.

#include "ns3/core-module.h"
#include "ns3/rdma-queue-pair.h"

#include <cstdint>
#include <iostream>
#include <string>

using ns3::CreateObject;
using ns3::Ptr;
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

void ExpectPulled(const std::string &law,
		const RdmaRxQueuePair::PulledRange *actual, bool expected){
	if ((actual != nullptr) == expected)
		return;
	std::cerr << "range algebra failed: " << law << "\n";
	failures++;
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
	// A pulled range answers for every offset inside it, not only its start.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->RecordPulledRange(6000, 7000, true);
		ExpectPulled("the recorded start is pulled",
			q->FindPulledRange(6000), true);
		ExpectPulled("an interior offset is pulled",
			q->FindPulledRange(6500), true);
		ExpectPulled("the exclusive end is not pulled",
			q->FindPulledRange(7000), false);
		ExpectPulled("an offset below the range is not pulled",
			q->FindPulledRange(5999), false);
	}
	// Pruning erases every settled entry, including one behind an entry that
	// still straddles the frontier. A front-only pop leaves those forever.
	{
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		q->RecordPulledRange(1000, 3000, false);
		q->RecordPulledRange(1500, 2000, false);
		q->RecordPulledRange(4000, 5000, false);
		q->ReceiverNextExpectedSeq = 2500;
		q->PruneSettledPulls();
		Expect("pruning erases settled entries behind an unsettled one",
			q->m_pulled_ranges.size(), 2);
		Expect("the straddling entry survives",
			q->m_pulled_ranges.count(1000), 1);
		Expect("the settled entry behind it is gone",
			q->m_pulled_ranges.count(1500), 0);
		Expect("the entry above the frontier survives",
			q->m_pulled_ranges.count(4000), 1);
	}
	if (failures > 0)
		return 1;
	std::cout << "range algebra laws hold\n";
	return 0;
}
