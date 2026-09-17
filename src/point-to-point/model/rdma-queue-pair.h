#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <ns3/object.h>
#include <ns3/packet.h>
#include <ns3/ipv4-address.h>
#include <ns3/data-rate.h>
#include <ns3/event-id.h>
#include <ns3/custom-header.h>
#include <ns3/int-header.h>
#include <map>
#include <vector>

namespace ns3 {

class RdmaQueuePair : public Object {
public:
	Time startTime;
	Ipv4Address sip, dip;
	uint16_t sport, dport;
	uint64_t m_size, m_init_size, m_tag;
	uint32_t m_src, m_dest;
	uint64_t snd_nxt, snd_una; // next seq to send, the highest unacked seq
	uint64_t m_highest_sent;
	uint64_t m_data_attempted_bytes;
	uint64_t m_retransmitted_bytes;
	uint64_t m_trimmed_payload_bytes;
	uint32_t m_recovery_events;
	uint32_t m_trim_notifications;
	uint32_t m_trim_lasthop_notifications;
	uint32_t m_trim_recovery_events;
	uint32_t m_stale_trim_notifications;
	uint32_t m_recovery_retries;
	// Cumulative retransmission-timeout firings. m_recovery_retries resets on
	// every acknowledgement advance, so it cannot answer how often the sender
	// waited out a timeout over the life of the transfer.
	uint32_t m_timeouts;
	// Rate cuts taken. Only CC mode 1 (DCQCN) reacts, so this is zero in every
	// other mode and separates a CC-driven tail from a repair-driven one.
	uint32_t m_cnp_received;
	// Congestion response as a two-variant sum {obey, exempt}. A bool carries
	// it because the value is read on the congestion path: it follows the
	// latest report from the receiver, set while the budget has room and
	// cleared while it has none. An exempt queue pair pays for congestion in
	// bounded loss instead of rate.
	bool m_cc_exempt;
	// Congestion signals withheld from the controller while exempt, and
	// reports that the entry had no allowance left.
	uint32_t m_cc_signals_withheld;
	uint32_t m_allowance_gone_reports;
	// When the receiver granted this queue pair its exemption. Zero means
	// never: no report can arrive before the first send.
	uint64_t m_cc_exempt_granted_ns;
	// What the exemption cost over the transfer's life, now that it follows
	// the report both ways: how many reports changed the bit, and how much
	// simulated time the sender spent delivering signals to its controller
	// after it had been granted. A flow that never saw a set bit spends none.
	uint32_t m_cc_transitions;
	uint64_t m_cc_obeying_ns;
	// When the current stretch of obeying began, and the last report's bit
	// with whether one has been seen at all. Zero means the queue pair is not
	// obeying: no report can arrive at time zero.
	uint64_t m_cc_obey_since_ns;
	bool m_cc_report_seen;
	bool m_cc_last_report;
	// Simulated times of the first trim notification received and the first
	// repair packet sent. Zero means never: no packet can be trimmed or
	// repaired before the transfer's first send.
	uint64_t m_first_trim_ns;
	uint64_t m_first_repair_ns;
	// Simulated time of the last cumulative-acknowledgement advance (or of
	// queue-pair creation). The forward-progress deadline measures from here.
	uint64_t m_last_progress_ns;
	uint32_t m_failure_reason;
	bool m_failed;
	// Selective repair: merged byte ranges awaiting retransmission, always
	// clamped above snd_una. GetNxtPacket serves these before new data.
	std::map<uint64_t, uint64_t> m_repair_ranges;
	EventId m_retransmissionTimer;
	uint16_t m_pg;
	uint16_t m_ipid;
	uint32_t m_win; // bound of on-the-fly packets
	uint64_t m_baseRtt; // base RTT of this qp
	DataRate m_max_rate; // max rate
	bool m_var_win; // variable window size
	Time m_nextAvail;	//< Soonest time of next send
	uint32_t wp; // current window of packets
	uint32_t lastPktSize;
	Callback<void> m_notifyAppFinish;
	Callback<void> m_notifyAppSent;
	/******************************
	 * runtime states
	 *****************************/
	DataRate m_rate;	//< Current rate
	struct {
		DataRate m_targetRate;	//< Target rate
		EventId m_eventUpdateAlpha;
		double m_alpha;
		bool m_alpha_cnp_arrived; // indicate if CNP arrived in the last slot
		bool m_first_cnp; // indicate if the current CNP is the first CNP
		EventId m_eventDecreaseRate;
		bool m_decrease_cnp_arrived; // indicate if CNP arrived in the last slot
		uint32_t m_rpTimeStage;
		EventId m_rpTimer;
	} mlx;
	struct {
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		IntHop hop[IntHeader::maxHop];
		uint32_t keep[IntHeader::maxHop];
		uint32_t m_incStage;
		double m_lastGap;
		double u;
		struct {
			double u;
			DataRate Rc;
			uint32_t incStage;
		}hopState[IntHeader::maxHop];
	} hp;
	struct{
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
		uint64_t lastRtt;
		double rttDiff;
	} tmly;
	struct{
		uint32_t m_lastUpdateSeq;
		uint32_t m_caState;
		uint32_t m_highSeq; // when to exit cwr
		double m_alpha;
		uint32_t m_ecnCnt;
		uint32_t m_batchSizeOfAlpha;
	} dctcp;
	struct{
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
	}hpccPint;

	/***********
	 * methods
	 **********/
	static TypeId GetTypeId (void);
	RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport);
	void SetSize(uint64_t size);
	void SetWin(uint32_t win);
	void SetBaseRtt(uint64_t baseRtt);
	void SetVarWin(bool v);
	void SetAppNotifyCallback(Callback<void> notifyAppFinish);
	void SetAppSentCallback(Callback<void> notifyAppSent);
	void AddRepairRange(uint64_t start, uint64_t end);
	uint64_t TakeRepairSegment(uint64_t max_bytes, uint64_t &start);
	void DropAcknowledgedRepairs();
	uint64_t RepairBytesLeft();

	uint64_t GetBytesLeft();
	uint64_t GetInitialSize();
	uint32_t GetSrc();
	uint32_t GetDest();
	uint64_t GetTag();
	void SetTag(uint64_t tag);void SetSrc(uint32_t src);void SetDest(uint32_t dest);void SetInitialSize(uint64_t size);
	uint32_t GetHash(void);
	void Acknowledge(uint64_t ack);
	uint64_t GetOnTheFly();
	bool IsWinBound();
	uint64_t GetWin(); // window size calculated from m_rate
	bool IsFinished();
	bool IsFailed();
	uint64_t HpGetCurWin(); // window size calculated from hp.m_curRate, used by HPCC
};

class RdmaRxQueuePair : public Object { // Rx side queue pair
public:
	struct ECNAccount{
		uint16_t qIndex;
		uint8_t ecnbits;
		uint16_t qfb;
		uint16_t total;

		ECNAccount() { memset(this, 0, sizeof(ECNAccount));}
	};
	ECNAccount m_ecn_source;
	uint32_t sip, dip;
	uint16_t sport, dport;
	uint16_t m_ipid;
	uint32_t ReceiverNextExpectedSeq;
	Time m_nackTimer;
	int32_t m_milestone_rx;
	uint32_t m_lastNACK;
	EventId QcnTimerEvent; // if destroy this rxQp, remember to cancel this timer
	// Out-of-order payload ranges accepted under selective retransmission.
	std::map<uint64_t, uint64_t> m_ooo_ranges;
	// Ranges the experiment layer forgave on this flow. They are absorbed
	// into m_ooo_ranges as though they had arrived, and AbsorbContiguousFrom
	// erases them as the cumulative sequence passes, so the record of what was
	// forgiven has to be kept apart from it. Its one reader counts the bytes
	// that arrive late for a range already given up.
	std::map<uint64_t, uint64_t> m_forgiven_ranges;
	// The end of the highest byte range this receiver has seen any evidence
	// of: a data packet that arrived and a trim header whose payload did not.
	// Everything below it and not settled is a hole, and the budget report is
	// measured against the sum of those.
	uint64_t m_highest_seen_end;
	// Whether the experiment layer may forgive this flow on this step, asked
	// once when the queue pair is created. Every acknowledgement this queue
	// pair emits carries it, and an acknowledgement carrying it without the
	// allowance report is what grants the sender its exemption.
	bool m_forgiveness_eligible;
	static TypeId GetTypeId (void);
	RdmaRxQueuePair();
	uint32_t GetHash(void);
	void AddOutOfOrderRange(uint64_t start, uint64_t end);
	uint64_t AbsorbContiguousFrom(uint64_t expected);
	// Bytes of [start, end) the receiver has not accepted: neither below the
	// cumulative sequence nor inside an accepted out-of-order range,
	// forgiveness included. Exactly the count AddOutOfOrderRange would
	// absorb, so a ledger charged this figure charges what it takes. Zero
	// means the range is settled and the trim is a duplicate.
	uint64_t UnsettledBytes(uint64_t start, uint64_t end) const;
	// Bytes at or above `expected` the receiver has already accepted out of
	// order, which under selective repeat is everything that arrived past the
	// gap the flow is stalled on. The step stop subtracts it, because a byte
	// that arrived is not a byte to forgive.
	uint64_t AcceptedBytesAbove(uint64_t expected) const;
	// Raise the highest sequence seen. Called for every accepted data packet
	// and every trim header, because a trimmed payload is evidence the range
	// exists just as an arrival is.
	void NoteSeen(uint64_t end);
	// Bytes below the highest sequence seen that are neither received nor
	// forgiven. A range trimmed three times is one hole, and a hole vanishes
	// when its repair lands, so the count falls on its own.
	uint64_t Holes() const;
	// Record what a forgiveness of [start, end) actually gave up: the part of
	// it the receiver did not already hold. Exactly the bytes the ledger was
	// charged, so a later arrival inside it is a byte the budget paid for and
	// the sender delivered anyway. Call before absorbing the range.
	void NoteForgiven(uint64_t start, uint64_t end);
	// Bytes of [start, end) that lie inside a range already forgiven, which is
	// what a late arrival for a given-up range costs.
	uint64_t ForgivenBytes(uint64_t start, uint64_t end) const;
};

class RdmaQueuePairGroup : public Object {
public:
	std::vector<Ptr<RdmaQueuePair> > m_qps;
	//std::vector<Ptr<RdmaRxQueuePair> > m_rxQps;

	static TypeId GetTypeId (void);
	RdmaQueuePairGroup(void);
	uint32_t GetN(void);
	Ptr<RdmaQueuePair> Get(uint32_t idx);
	Ptr<RdmaQueuePair> operator[](uint32_t idx);
	void AddQp(Ptr<RdmaQueuePair> qp);
	//void AddRxQp(Ptr<RdmaRxQueuePair> rxQp);
	void Clear(void);
};

}

#endif /* RDMA_QUEUE_PAIR_H */
