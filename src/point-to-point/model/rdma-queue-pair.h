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
	uint32_t m_trim_ftd_repairs;
	uint32_t m_trim_bts_notifications;
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
	// Priority pulls the sender served. Recovery domain only.
	uint32_t m_priority_pulls;
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
	// Recovery-domain forgiveness state. Declared here with the rest of the
	// receive state so the queue-pair layout changes once; the transitions
	// that fill it land with the ReceiveTrim fork.
	// Trimmed ranges with a PULL outstanding, pruned below
	// ReceiverNextExpectedSeq on every advance.
	std::map<uint64_t, uint64_t> m_pulled_ranges;
	uint64_t m_forgiven_bytes;
	uint32_t m_forgiven_ranges;
	// A forgiven non-last-hop trim owes congestion control one CNP, carried on
	// the next ACK. Without it a forgiven trim hides congestion.
	bool m_pending_cnp;

	static TypeId GetTypeId (void);
	RdmaRxQueuePair();
	uint32_t GetHash(void);
	void AddOutOfOrderRange(uint64_t start, uint64_t end);
	uint64_t AbsorbContiguousFrom(uint64_t expected);
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
