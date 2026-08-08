#include <ns3/hash.h>
#include <ns3/uinteger.h>
#include <ns3/seq-ts-header.h>
#include <ns3/udp-header.h>
#include <ns3/ipv4-header.h>
#include <ns3/simulator.h>
#include "ns3/ppp-header.h"
#include "rdma-queue-pair.h"

namespace ns3 {

/**************************
 * RdmaQueuePair
 *************************/
TypeId RdmaQueuePair::GetTypeId (void)
{
static TypeId tid = TypeId ("ns3::RdmaQueuePair")
		.SetParent<Object> ()
		;
	return tid;
}

RdmaQueuePair::RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport){
	startTime = Simulator::Now();
	sip = _sip;
	dip = _dip;
	sport = _sport;
	dport = _dport;
	m_size = 0;
	m_init_size = 0;
	m_src = -1;
	m_dest = -1;
	m_tag = -1;
	snd_nxt = snd_una = 0;
	m_highest_sent = 0;
	m_data_attempted_bytes = 0;
	m_retransmitted_bytes = 0;
	m_trimmed_payload_bytes = 0;
	m_recovery_events = 0;
	m_trim_notifications = 0;
	m_trim_ftd_repairs = 0;
	m_trim_bts_notifications = 0;
	m_trim_lasthop_notifications = 0;
	m_trim_recovery_events = 0;
	m_stale_trim_notifications = 0;
	m_recovery_retries = 0;
	m_last_progress_ns = Simulator::Now().GetNanoSeconds();
	m_failure_reason = 0;
	m_failed = false;
	m_pg = pg;
	m_ipid = 0;
	m_win = 0;
	m_baseRtt = 0;
	m_max_rate = 0;
	m_var_win = false;
	m_rate = 0;
	m_nextAvail = Time(0);
	mlx.m_alpha = 1;
	mlx.m_alpha_cnp_arrived = false;
	mlx.m_first_cnp = true;
	mlx.m_decrease_cnp_arrived = false;
	mlx.m_rpTimeStage = 0;
	hp.m_lastUpdateSeq = 0;
	for (uint32_t i = 0; i < sizeof(hp.keep) / sizeof(hp.keep[0]); i++)
		hp.keep[i] = 0;
	hp.m_incStage = 0;
	hp.m_lastGap = 0;
	hp.u = 1;
	for (uint32_t i = 0; i < IntHeader::maxHop; i++){
		hp.hopState[i].u = 1;
		hp.hopState[i].incStage = 0;
	}

	tmly.m_lastUpdateSeq = 0;
	tmly.m_incStage = 0;
	tmly.lastRtt = 0;
	tmly.rttDiff = 0;

	dctcp.m_lastUpdateSeq = 0;
	dctcp.m_caState = 0;
	dctcp.m_highSeq = 0;
	dctcp.m_alpha = 1;
	dctcp.m_ecnCnt = 0;
	dctcp.m_batchSizeOfAlpha = 0;

	hpccPint.m_lastUpdateSeq = 0;
	hpccPint.m_incStage = 0;
}

void RdmaQueuePair::SetSize(uint64_t size){
	m_size = size;
}

void RdmaQueuePair::SetSrc(uint32_t src){
	m_src = src;
}

void RdmaQueuePair::SetDest(uint32_t dest){
	m_dest = dest;
}

uint32_t RdmaQueuePair::GetSrc(){
	return m_src;
}

uint32_t RdmaQueuePair::GetDest(){
	return m_dest;
}

void RdmaQueuePair::SetTag(uint64_t tag){
	m_tag = tag;
}

uint64_t RdmaQueuePair::GetTag(){
	return m_tag;
}

void RdmaQueuePair::SetInitialSize(uint64_t size){
	m_init_size = size;
}

uint64_t RdmaQueuePair::GetInitialSize(){
	return m_init_size;
}

void RdmaQueuePair::SetWin(uint32_t win){
	m_win = win;
}

void RdmaQueuePair::SetBaseRtt(uint64_t baseRtt){
	m_baseRtt = baseRtt;
}

void RdmaQueuePair::SetVarWin(bool v){
	m_var_win = v;
}

void RdmaQueuePair::SetAppNotifyCallback(Callback<void> notifyAppFinish){
	m_notifyAppFinish = notifyAppFinish;
}

void RdmaQueuePair::SetAppSentCallback(Callback<void> notifyAppSent){
	m_notifyAppSent = notifyAppSent;
}


uint64_t RdmaQueuePair::GetBytesLeft(){
	// Pending selective repairs count as sendable bytes: a queue pair whose
	// tail is fully transmitted must stay schedulable until its repair
	// ranges have been resent. This runs inside the egress queue's per-packet
	// scan over every registered queue pair, so the overwhelmingly common
	// no-repairs case must stay one comparison — RepairBytesLeft() walks and
	// prunes the range map and is only entered when ranges exist, which
	// requires selective retransmission to be enabled and active.
	uint64_t tail = m_size >= snd_nxt ? m_size - snd_nxt : 0;
	if (m_repair_ranges.empty())
		return tail;
	return tail + RepairBytesLeft();
}

void RdmaQueuePair::AddRepairRange(uint64_t start, uint64_t end){
	if (start < snd_una)
		start = snd_una;
	if (end > m_size)
		end = m_size;
	if (start >= end)
		return;
	// Merge with any overlapping or adjacent recorded ranges.
	auto it = m_repair_ranges.lower_bound(start);
	if (it != m_repair_ranges.begin()){
		auto prev = std::prev(it);
		if (prev->second >= start){
			start = prev->first;
			if (prev->second > end)
				end = prev->second;
			m_repair_ranges.erase(prev);
		}
	}
	it = m_repair_ranges.lower_bound(start);
	while (it != m_repair_ranges.end() && it->first <= end){
		if (it->second > end)
			end = it->second;
		it = m_repair_ranges.erase(it);
	}
	m_repair_ranges[start] = end;
}

uint64_t RdmaQueuePair::TakeRepairSegment(uint64_t max_bytes, uint64_t &start){
	DropAcknowledgedRepairs();
	if (m_repair_ranges.empty() || max_bytes == 0)
		return 0;
	auto it = m_repair_ranges.begin();
	start = it->first;
	uint64_t size = it->second - it->first;
	if (size > max_bytes)
		size = max_bytes;
	uint64_t new_start = start + size;
	uint64_t end = it->second;
	m_repair_ranges.erase(it);
	if (new_start < end)
		m_repair_ranges[new_start] = end;
	return size;
}

void RdmaQueuePair::DropAcknowledgedRepairs(){
	while (!m_repair_ranges.empty()){
		auto it = m_repair_ranges.begin();
		if (it->second <= snd_una){
			m_repair_ranges.erase(it);
			continue;
		}
		if (it->first < snd_una){
			uint64_t end = it->second;
			m_repair_ranges.erase(it);
			m_repair_ranges[snd_una] = end;
		}
		break;
	}
}

uint64_t RdmaQueuePair::RepairBytesLeft(){
	DropAcknowledgedRepairs();
	uint64_t total = 0;
	for (auto const &range : m_repair_ranges)
		total += range.second - range.first;
	return total;
}

uint32_t RdmaQueuePair::GetHash(void){
	union{
		struct {
			uint32_t sip, dip;
			uint16_t sport, dport;
		};
		char c[12];
	} buf;
	buf.sip = sip.Get();
	buf.dip = dip.Get();
	buf.sport = sport;
	buf.dport = dport;
	return Hash32(buf.c, 12);
}

void RdmaQueuePair::Acknowledge(uint64_t ack){
	if (ack > snd_una){
		snd_una = ack;
		// A cumulative ACK can outrun a go-back-N rewind: resent duplicates
		// make the receiver repeat its frontier ACK, which lands above the
		// rewound snd_nxt. Unclamped, GetOnTheFly() underflows and the window
		// check blocks the queue pair from ever sending again.
		if (snd_nxt < snd_una){
			snd_nxt = snd_una;
		}
	}
}

uint64_t RdmaQueuePair::GetOnTheFly(){
	return snd_nxt - snd_una;
}

bool RdmaQueuePair::IsWinBound(){
	uint64_t w = GetWin();
	return w != 0 && GetOnTheFly() >= w;
}

uint64_t RdmaQueuePair::GetWin(){
	if (m_win == 0)
		return 0;
	uint64_t w;
	if (m_var_win){
		w = m_win * m_rate.GetBitRate() / m_max_rate.GetBitRate();
		if (w == 0)
			w = 1; // must > 0
	}else{
		w = m_win;
	}
	return w;
}

uint64_t RdmaQueuePair::HpGetCurWin(){
	if (m_win == 0)
		return 0;
	uint64_t w;
	if (m_var_win){
		w = m_win * hp.m_curRate.GetBitRate() / m_max_rate.GetBitRate();
		if (w == 0)
			w = 1; // must > 0
	}else{
		w = m_win;
	}
	return w;
}

bool RdmaQueuePair::IsFinished(){
	return !m_failed && snd_una >= m_size;
}

bool RdmaQueuePair::IsFailed(){
	return m_failed;
}

/*********************
 * RdmaRxQueuePair
 ********************/
TypeId RdmaRxQueuePair::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaRxQueuePair")
		.SetParent<Object> ()
		;
	return tid;
}

RdmaRxQueuePair::RdmaRxQueuePair(){
	sip = dip = sport = dport = 0;
	m_ipid = 0;
	ReceiverNextExpectedSeq = 0;
	m_nackTimer = Time(0);
	m_milestone_rx = 0;
	m_lastNACK = 0;
}

uint32_t RdmaRxQueuePair::GetHash(void){
	union{
		struct {
			uint32_t sip, dip;
			uint16_t sport, dport;
		};
		char c[12];
	} buf;
	buf.sip = sip;
	buf.dip = dip;
	buf.sport = sport;
	buf.dport = dport;
	return Hash32(buf.c, 12);
}

void RdmaRxQueuePair::AddOutOfOrderRange(uint64_t start, uint64_t end){
	if (start >= end)
		return;
	auto it = m_ooo_ranges.lower_bound(start);
	if (it != m_ooo_ranges.begin()){
		auto prev = std::prev(it);
		if (prev->second >= start){
			start = prev->first;
			if (prev->second > end)
				end = prev->second;
			m_ooo_ranges.erase(prev);
		}
	}
	it = m_ooo_ranges.lower_bound(start);
	while (it != m_ooo_ranges.end() && it->first <= end){
		if (it->second > end)
			end = it->second;
		it = m_ooo_ranges.erase(it);
	}
	m_ooo_ranges[start] = end;
}

uint64_t RdmaRxQueuePair::AbsorbContiguousFrom(uint64_t expected){
	auto it = m_ooo_ranges.begin();
	while (it != m_ooo_ranges.end() && it->first <= expected){
		if (it->second > expected)
			expected = it->second;
		it = m_ooo_ranges.erase(it);
	}
	return expected;
}

/*********************
 * RdmaQueuePairGroup
 ********************/
TypeId RdmaQueuePairGroup::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaQueuePairGroup")
		.SetParent<Object> ()
		;
	return tid;
}

RdmaQueuePairGroup::RdmaQueuePairGroup(void){
}

uint32_t RdmaQueuePairGroup::GetN(void){
	return m_qps.size();
}

Ptr<RdmaQueuePair> RdmaQueuePairGroup::Get(uint32_t idx){
	return m_qps[idx];
}

Ptr<RdmaQueuePair> RdmaQueuePairGroup::operator[](uint32_t idx){
	return m_qps[idx];
}

void RdmaQueuePairGroup::AddQp(Ptr<RdmaQueuePair> qp){
	m_qps.push_back(qp);
}

#if 0
void RdmaQueuePairGroup::AddRxQp(Ptr<RdmaRxQueuePair> rxQp){
	m_rxQps.push_back(rxQp);
}
#endif

void RdmaQueuePairGroup::Clear(void){
	m_qps.clear();
}

}
