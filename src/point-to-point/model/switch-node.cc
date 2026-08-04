#include "ns3/ipv4.h"
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/flow-id-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "switch-node.h"
#include "qbb-net-device.h"
#include "qbb-header.h"
#include "ppp-header.h"
#include "ns3/simulator.h"
#include "ns3/int-header.h"
#include "ns3/channel.h"
#include <algorithm>
#include <cmath>

namespace ns3 {

TypeId SwitchNode::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::SwitchNode")
    .SetParent<Node> ()
    .AddConstructor<SwitchNode> ()
	.AddAttribute("EcnEnabled",
			"Enable ECN marking.",
			BooleanValue(false),
			MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
			MakeBooleanChecker())
	.AddAttribute("CcMode",
			"CC mode.",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ccMode),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("AckHighPrio",
			"Set high priority for ACK/NACK or not",
			UintegerValue(0),
			MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("PacketTrimMode",
			"0=disabled, 1=trim and forward to the destination (UEC 1.0.3 section 4.1), "
			"2=return trim metadata to the sender (not part of UEC 1.0.3).",
			UintegerValue(static_cast<uint32_t>(PacketTrimMode::Disabled)),
			MakeUintegerAccessor(&SwitchNode::m_packetTrimMode),
			MakeUintegerChecker<uint32_t>(
				static_cast<uint32_t>(PacketTrimMode::Disabled),
				static_cast<uint32_t>(PacketTrimMode::BackToSender)))
	.AddAttribute("TrimmedQueueIndex",
			"Egress queue (TC_med) that carries DSCP_TRIMMED packets. It must differ "
			"from queue 0 (TC_high, DSCP_CONTROL) and from every data priority group.",
			UintegerValue(2),
			MakeUintegerAccessor(&SwitchNode::m_trimmedQueueIndex),
			MakeUintegerChecker<uint32_t>(1, 7))
	.AddAttribute("MinTrimSize",
			"MIN_TRIM_SIZE in IP payload bytes. UEC 1.0.3 Table 4-1 requires 24 B for "
			"UET over UDP/IP so the UDP and PDS request headers survive trimming.",
			UintegerValue(24),
			MakeUintegerAccessor(&SwitchNode::m_minTrimSize),
			MakeUintegerChecker<uint32_t>())
	.AddAttribute("LastHopTrimCodepoint",
			"Mark packets trimmed on a directly attached host downlink with "
			"DSCP_TRIMMED_LAST_HOP (UEC 1.0.3 section 4.1.4.1).",
			BooleanValue(true),
			MakeBooleanAccessor(&SwitchNode::m_lastHopTrimCodepoint),
			MakeBooleanChecker())
	.AddAttribute("MaxRtt",
			"Max Rtt of the network",
			UintegerValue(9000),
			MakeUintegerAccessor(&SwitchNode::m_maxRtt),
			MakeUintegerChecker<uint32_t>())
	.AddTraceSource ("SwitchDrop", "A switch route or admission decision dropped a packet.",
			MakeTraceSourceAccessor (&SwitchNode::m_traceDrop),
			"ns3::Packet::TracedCallback")
	.AddTraceSource ("PacketTrim", "A congested switch converted RDMA data into trim metadata.",
			MakeTraceSourceAccessor (&SwitchNode::m_traceTrim),
			"ns3::Packet::TracedCallback")
  ;
  return tid;
}

SwitchNode::SwitchNode(){
	m_ecmpSeed = m_id;
	m_node_type = 1;
	m_packetTrimMode = static_cast<uint32_t>(PacketTrimMode::Disabled);
	m_trimmedQueueIndex = 2;
	m_minTrimSize = 24;
	m_lastHopTrimCodepoint = true;
	m_mmu = CreateObject<SwitchMmu>();
	for (uint32_t i = 0; i < pCnt; i++)
		for (uint32_t j = 0; j < pCnt; j++)
			for (uint32_t k = 0; k < qCnt; k++)
				m_bytes[i][j][k] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_txBytes[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_lastPktSize[i] = m_lastPktTs[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_u[i] = 0;
}

int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch){
	// look up entries
	auto entry = m_rtTable.find(ch.dip);

	// no matching entry
	if (entry == m_rtTable.end())
		return -1;

	// entry found
	auto &nexthops = entry->second;

	// pick one next hop based on hash
	union {
		uint8_t u8[4+4+2+2];
		uint32_t u32[3];
	} buf;
	buf.u32[0] = ch.sip;
	buf.u32[1] = ch.dip;
	if (ch.l3Prot == 0x6)
		buf.u32[2] = ch.tcp.sport | ((uint32_t)ch.tcp.dport << 16);
	else if (ch.l3Prot == 0x11)
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
	else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD ||
			 ch.l3Prot == kUecTrimRepairProtocol ||
			 ch.l3Prot == kUecTrimNotificationProtocol)
		buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);

	uint32_t idx = EcmpHash(buf.u8, 12, m_ecmpSeed) % nexthops.size();
	return nexthops[idx];
}

// UEC 1.0.3 section 4.1: "Switches that are configured to perform trimming will
// only trim packets that they know to be trimmable, as indicated by
// DSCP_TRIMMABLE." Control traffic (DSCP_CONTROL) and already trimmed packets
// (DSCP_TRIMMED) are therefore never trimmed.
bool SwitchNode::PacketTrimEnabledFor(const CustomHeader &ch) const{
	return m_packetTrimMode != static_cast<uint32_t>(PacketTrimMode::Disabled) &&
		ch.l3Prot == 0x11 && IsUetTrimmableDscp(ch.GetIpv4Dscp());
}

// A trimming switch is the last hop when the chosen egress port is a downlink to
// a directly connected host (UEC 1.0.3 section 4.1.4.1).
bool SwitchNode::IsLastHopTo(uint32_t outDev) const{
	Ptr<NetDevice> dev = m_devices[outDev];
	Ptr<Channel> channel = dev->GetChannel();
	if (!channel || channel->GetNDevices() != 2)
		return false;
	Ptr<NetDevice> peer = channel->GetDevice(0) == dev ? channel->GetDevice(1)
													  : channel->GetDevice(0);
	return peer->GetNode()->GetNodeType() == 0;
}

// Traffic class selection. DSCP_TRIMMED maps to TC_med, DSCP_CONTROL and the
// link-level control protocols map to TC_high (queue 0), and data rides its own
// priority group in TC_low (UEC 1.0.3 sections 3.6.4.7.2 and 4.1.4.1).
uint32_t SwitchNode::QueueIndexFor(const CustomHeader &ch) const{
	if (ch.l3Prot == 0x11)
		return IsUetTrimmedDscp(ch.GetIpv4Dscp()) ? m_trimmedQueueIndex : ch.udp.pg;
	if (ch.l3Prot == kUecTrimNotificationProtocol)
		return m_trimmedQueueIndex;
	if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE ||
		ch.l3Prot == kUecTrimRepairProtocol ||
		(m_ackHighPrio && (ch.l3Prot == 0xFD || ch.l3Prot == 0xFC)))
		return 0; // QCN, PFC, and UET control packets
	return ch.l3Prot == 0x06 ? 1 : ch.udp.pg; // if TCP, put to queue 1
}

// Truncate the IP payload in place and rewrite only the outer IP header, per the
// trim() pseudocode of UEC 1.0.3 section 4.1. Returns false when the packet is
// not worth trimming, in which case the caller applies the normal overflow
// procedure (the specification permits dropping a packet whose IP payload is
// already shorter than MIN_TRIM_SIZE).
bool SwitchNode::TrimInPlace(Ptr<Packet> p, const CustomHeader &ch, bool lastHop){
	const uint32_t l2l3Bytes = PppHeader::GetStaticSize() + ch.m_headerSize;
	if (p->GetSize() <= l2l3Bytes)
		return false;
	const uint32_t originalIpPayload = p->GetSize() - l2l3Bytes;
	// The retained prefix must cover every header the destination needs to
	// identify the original packet, and the frame must stay a legal minimum-size
	// Ethernet frame.
	const uint32_t kMinEthernetPayload = 60 - 14 - 20;
	uint32_t trimmedIpPayload = std::max(m_minTrimSize, CustomHeader::GetUdpHeaderSize());
	trimmedIpPayload = std::max(trimmedIpPayload, kMinEthernetPayload);
	// "The trimmed packet size MUST NOT be larger than the original packet size."
	if (originalIpPayload <= trimmedIpPayload)
		return false;

	p->RemoveAtEnd(originalIpPayload - trimmedIpPayload);

	PppHeader ppp;
	Ipv4Header ip;
	p->RemoveHeader(ppp);
	p->RemoveHeader(ip);
	// Trimming changes the DSCP and the length only. The ECN bits are carried
	// through unmodified (section 4.1.1) and TTL processing is unchanged.
	ip.SetDscp(static_cast<Ipv4Header::DscpType>(
		lastHop ? kUetDscpTrimmedLastHop : kUetDscpTrimmed));
	ip.SetPayloadSize(trimmedIpPayload);
	p->AddHeader(ip);
	p->AddHeader(ppp);
	return true;
}

// Trim the packet and re-run it through admission for the DSCP_TRIMMED queue.
// "The trimmed packet MUST be treated as a new incoming packet for
// DSCP_TRIMMED for any subsequent processing within the switch performing
// trimming" (UEC 1.0.3 section 4.1), so a congested TC_med drops it.
bool SwitchNode::TrimAndForward(Ptr<Packet> p, CustomHeader &ch, int outDev,
		PacketTrimTrigger trigger){
	const PacketTrimMode mode = static_cast<PacketTrimMode>(m_packetTrimMode);
	const bool lastHop = m_lastHopTrimCodepoint && IsLastHopTo(outDev);
	if (lastHop)
		trigger = trigger == PacketTrimTrigger::Admission
			? PacketTrimTrigger::AdmissionLastHop
			: PacketTrimTrigger::EgressQueueLastHop;

	if (mode == PacketTrimMode::BackToSender){
		const uint32_t dataHeaderBytes = ch.GetSerializedSize();
		if (p->GetSize() <= dataHeaderBytes)
			return false;
		return SendTrimNotification(p, ch, p->GetSize() - dataHeaderBytes, lastHop,
			trigger);
	}
	if (mode != PacketTrimMode::ForwardToDestination)
		return false;

	if (!TrimInPlace(p, ch, lastHop))
		return false;

	CustomHeader trimmedCh(CustomHeader::L2_Header | CustomHeader::L3_Header |
		CustomHeader::L4_Header);
	trimmedCh.getInt = 1;
	p->PeekHeader(trimmedCh);
	// The conversion itself is the observable event. Whether the resulting
	// trimmed packet survives TC_med admission is reported separately, because
	// "there is no guarantee that for each packet failing buffer admission
	// checks a trimmed packet will be delivered to the destination".
	m_traceTrim(p, static_cast<uint32_t>(trigger));
	SendToDev(p, trimmedCh);
	return true;
}

// Back-to-sender notification. This is deliberately *not* a UEC 1.0.3 mechanism
// (section 4.1: "Sending a trimmed packet back to the source ... is not part of
// this specification"); it models the FastLane/P802.1Qdw style of drop
// notification. It still rides TC_med and obeys its admission rules.
bool SwitchNode::SendTrimNotification(Ptr<const Packet> original,
		const CustomHeader &ch, uint32_t payloadSize, bool lastHop,
		PacketTrimTrigger trigger){
	qbbHeader trimHeader;
	trimHeader.SetSeq(ch.udp.seq);
	trimHeader.SetPG(ch.udp.pg);
	// Ports follow the ACK convention so the sender resolves the QP the same way
	// it does for an ACK or NACK, without treating the notification as data.
	trimHeader.SetSport(ch.udp.dport);
	trimHeader.SetDport(ch.udp.sport);
	trimHeader.SetTrimPayloadSize(payloadSize);
	trimHeader.SetTrimFtd(false);
	trimHeader.SetTrimLastHop(lastHop);
	trimHeader.SetIntHeader(ch.udp.ih);

	Ptr<Packet> trimPacket = Create<Packet>(
		std::max(60 - 14 - 20 - static_cast<int>(trimHeader.GetSerializedSize()), 0));
	trimPacket->AddHeader(trimHeader);

	Ipv4Header ipHeader;
	ipHeader.SetSource(Ipv4Address(ch.dip));
	ipHeader.SetDestination(Ipv4Address(ch.sip));
	ipHeader.SetProtocol(kUecTrimNotificationProtocol);
	ipHeader.SetDscp(static_cast<Ipv4Header::DscpType>(
		lastHop ? kUetDscpTrimmedLastHop : kUetDscpTrimmed));
	ipHeader.SetPayloadSize(trimPacket->GetSize());
	ipHeader.SetTtl(64);
	ipHeader.SetIdentification(ch.ipid);
	trimPacket->AddHeader(ipHeader);
	PppHeader ppp;
	ppp.SetProtocol(0x0021);
	trimPacket->AddHeader(ppp);

	// The notification is admitted against the ingress port the trimmed data
	// arrived on, so ingress accounting stays attributable.
	FlowIdTag inDevTag;
	if (original->PeekPacketTag(inDevTag))
		trimPacket->AddPacketTag(inDevTag);

	CustomHeader trimCh(CustomHeader::L2_Header | CustomHeader::L3_Header |
		CustomHeader::L4_Header);
	trimCh.getInt = 1;
	trimPacket->PeekHeader(trimCh);
	m_traceTrim(trimPacket, static_cast<uint32_t>(trigger));
	SendToDev(trimPacket, trimCh);
	return true;
}

void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex){
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldPause(inDev, qIndex)){
		device->SendPfc(qIndex, 0);
		m_mmu->SetPause(inDev, qIndex);
	}
}
void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex){
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldResume(inDev, qIndex)){
		device->SendPfc(qIndex, 1);
		m_mmu->SetResume(inDev, qIndex);
	}
}

bool SwitchNode::SendToDev(Ptr<Packet>p, CustomHeader &ch){
	int idx = GetOutDev(p, ch);
	if (idx < 0){
		m_traceDrop(p, static_cast<uint32_t>(SwitchDropReason::Route));
		return false; // Drop
	}
	NS_ASSERT_MSG(m_devices[idx]->IsLinkUp(), "The routing table look up should return link that is up");

	const uint32_t qIndex = QueueIndexFor(ch);
	// A trimmed packet is never trimmed again; on overflow it follows the normal
	// queue-overflow procedure (UEC 1.0.3 section 4.1).
	const bool alreadyTrimmed = IsUetTrimmedDscp(ch.GetIpv4Dscp());
	const SwitchDropReason admissionDrop = alreadyTrimmed
		? SwitchDropReason::TrimmedQueue : SwitchDropReason::Admission;
	const SwitchDropReason egressDrop = alreadyTrimmed
		? SwitchDropReason::TrimmedQueue : SwitchDropReason::EgressQueue;

	// admission control
	FlowIdTag t;
	p->PeekPacketTag(t);
	uint32_t inDev = t.GetFlowId();
	if (qIndex != 0){ //not highest priority
		if (m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize()) && m_mmu->CheckEgressAdmission(idx, qIndex, p->GetSize())){			// Admission control
			m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize());
			m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize());
		}else{
			if (PacketTrimEnabledFor(ch) &&
				TrimAndForward(p, ch, idx, PacketTrimTrigger::Admission)){
				return false; // the original data packet was consumed by trimming
			}
			m_traceDrop(p, static_cast<uint32_t>(admissionDrop));
			return false; // Drop
		}
		CheckAndSendPfc(inDev, qIndex);
	}
	m_bytes[inDev][idx][qIndex] += p->GetSize();
	if (!m_devices[idx]->SwitchSend(qIndex, p, ch)){
		m_bytes[inDev][idx][qIndex] -= p->GetSize();
		if (qIndex != 0){
			m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize());
			m_mmu->RemoveFromEgressAdmission(idx, qIndex, p->GetSize());
			CheckAndSendResume(inDev, qIndex);
		}
		if (PacketTrimEnabledFor(ch) &&
			TrimAndForward(p, ch, idx, PacketTrimTrigger::EgressQueue)){
			return false;
		}
		m_traceDrop(p, static_cast<uint32_t>(egressDrop));
		return false;
	}
	return true;
}

uint32_t SwitchNode::EcmpHash(const uint8_t* key, size_t len, uint32_t seed) {
  uint32_t h = seed;
  if (len > 3) {
    const uint32_t* key_x4 = (const uint32_t*) key;
    size_t i = len >> 2;
    do {
      uint32_t k = *key_x4++;
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      h ^= k;
      h = (h << 13) | (h >> 19);
      h += (h << 2) + 0xe6546b64;
    } while (--i);
    key = (const uint8_t*) key_x4;
  }
  if (len & 3) {
    size_t i = len & 3;
    uint32_t k = 0;
    key = &key[i - 1];
    do {
      k <<= 8;
      k |= *key--;
    } while (--i);
    k *= 0xcc9e2d51;
    k = (k << 15) | (k >> 17);
    k *= 0x1b873593;
    h ^= k;
  }
  h ^= len;
  h ^= h >> 16;
  h *= 0x85ebca6b;
  h ^= h >> 13;
  h *= 0xc2b2ae35;
  h ^= h >> 16;
  return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed){
	m_ecmpSeed = seed;
}

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx){
	uint32_t dip = dstAddr.Get();
	m_rtTable[dip].push_back(intf_idx);
}

void SwitchNode::ClearTable(){
	m_rtTable.clear();
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch){
	SendToDev(packet, ch);
	return true;
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p){
	FlowIdTag t;
	p->PeekPacketTag(t);
	// UEC 1.0.3 section 4.1.1: "A switch SHOULD NOT perform ECN marking on
	// trimmed packets", so they keep the ECN bits of the original data packet.
	// Section 4.1.3 likewise forbids editing headers beyond the outer IP header,
	// so in-network telemetry is not pushed into a truncated payload either.
	bool isTrimmed = false;
	{
		uint8_t* buf = p->GetBuffer();
		isTrimmed = IsUetTrimmedDscp((buf[PppHeader::GetStaticSize() + 1] >> 2) & 0x3f);
	}
	if (qIndex != 0){
		uint32_t inDev = t.GetFlowId();
		m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize());
		m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize());
		m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
		if (m_ecnEnabled && !isTrimmed){
			bool egressCongested = m_mmu->ShouldSendCN(ifIndex, qIndex);
			if (egressCongested){
				PppHeader ppp;
				Ipv4Header h;
				p->RemoveHeader(ppp);
				p->RemoveHeader(h);
				h.SetEcn((Ipv4Header::EcnType)0x03);
				p->AddHeader(h);
				p->AddHeader(ppp);
			}
		}
		//CheckAndSendPfc(inDev, qIndex);
		CheckAndSendResume(inDev, qIndex);
	}
	if (!isTrimmed){
		uint8_t* buf = p->GetBuffer();
		if (buf[PppHeader::GetStaticSize() + 9] == 0x11){ // udp packet
			IntHeader *ih = (IntHeader*)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (m_ccMode == 3){ // HPCC
				ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
			}else if (m_ccMode == 10){ // HPCC-PINT
				uint64_t t = Simulator::Now().GetTimeStep();
				uint64_t dt = t - m_lastPktTs[ifIndex];
				if (dt > m_maxRtt)
					dt = m_maxRtt;
				uint64_t B = dev->GetDataRate().GetBitRate() / 8; //Bps
				uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
				double newU;

				/**************************
				 * approximate calc
				 *************************/
				int b = 20, m = 16, l = 20; // see log2apprx's paremeters
				int sft = logres_shift(b,l);
				double fct = 1<<sft; // (multiplication factor corresponding to sft)
				double log_T = log2(m_maxRtt)*fct; // log2(T)*fct
				double log_B = log2(B)*fct; // log2(B)*fct
				double log_1e9 = log2(1e9)*fct; // log2(1e9)*fct
				double qterm = 0;
				double byteTerm = 0;
				double uTerm = 0;
				if ((qlen >> 8) > 0){
					int log_dt = log2apprx(dt, b, m, l); // ~log2(dt)*fct
					int log_qlen = log2apprx(qlen >> 8, b, m, l); // ~log2(qlen / 256)*fct
					qterm = pow(2, (
								log_dt + log_qlen + log_1e9 - log_B - 2*log_T
								)/fct
							) * 256;
					// 2^((log2(dt)*fct+log2(qlen/256)*fct+log2(1e9)*fct-log2(B)*fct-2*log2(T)*fct)/fct)*256 ~= dt*qlen*1e9/(B*T^2)
				}
				if (m_lastPktSize[ifIndex] > 0){
					int byte = m_lastPktSize[ifIndex];
					int log_byte = log2apprx(byte, b, m, l);
					byteTerm = pow(2, (
								log_byte + log_1e9 - log_B - log_T
								)/fct
							);
					// 2^((log2(byte)*fct+log2(1e9)*fct-log2(B)*fct-log2(T)*fct)/fct) ~= byte*1e9 / (B*T)
				}
				if (m_maxRtt > dt && m_u[ifIndex] > 0){
					int log_T_dt = log2apprx(m_maxRtt - dt, b, m, l); // ~log2(T-dt)*fct
					int log_u = log2apprx(int(round(m_u[ifIndex] * 8192)), b, m, l); // ~log2(u*512)*fct
					uTerm = pow(2, (
								log_T_dt + log_u - log_T
								)/fct
							) / 8192;
					// 2^((log2(T-dt)*fct+log2(u*512)*fct-log2(T)*fct)/fct)/512 = (T-dt)*u/T
				}
				newU = qterm+byteTerm+uTerm;

				#if 0
				/**************************
				 * accurate calc
				 *************************/
				double weight_ewma = double(dt) / m_maxRtt;
				double u;
				if (m_lastPktSize[ifIndex] == 0)
					u = 0;
				else{
					double txRate = m_lastPktSize[ifIndex] / double(dt); // B/ns
					u = (qlen / m_maxRtt + txRate) * 1e9 / B;
				}
				newU = m_u[ifIndex] * (1 - weight_ewma) + u * weight_ewma;
				printf(" %lf\n", newU);
				#endif

				/************************
				 * update PINT header
				 ***********************/
				uint16_t power = Pint::encode_u(newU);
				if (power > ih->GetPower())
					ih->SetPower(power);

				m_u[ifIndex] = newU;
			}
		}
	}
	m_txBytes[ifIndex] += p->GetSize();
	m_lastPktSize[ifIndex] = p->GetSize();
	m_lastPktTs[ifIndex] = Simulator::Now().GetTimeStep();
}

int SwitchNode::logres_shift(int b, int l){
	static int data[] = {0,0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5};
	return l - data[b];
}

int SwitchNode::log2apprx(int x, int b, int m, int l){
	int x0 = x;
	int msb = int(log2(x)) + 1;
	if (msb > m){
		x = (x >> (msb - m) << (msb - m));
		#if 0
		x += + (1 << (msb - m - 1));
		#else
		int mask = (1 << (msb-m)) - 1;
		if ((x0 & mask) > (rand() & mask))
			x += 1<<(msb-m);
		#endif
	}
	return int(log2(x) * (1<<logres_shift(b, l)));
}

} /* namespace ns3 */
