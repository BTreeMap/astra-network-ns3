#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"

namespace ns3 {

class Packet;

enum class SwitchDropReason : uint32_t {
	Route = 1,
	Admission = 2,
	EgressQueue = 3,
	// UEC 1.0.3 section 4.1: a trimmed packet "MUST obey buffer admission rules
	// for the queue associated with DSCP_TRIMMED"; on failure "the normal
	// procedure for queue overflow should be followed", i.e. it is dropped.
	TrimmedQueue = 4,
};

enum class PacketTrimMode : uint32_t {
	Disabled = 0,
	// UEC 1.0.3 section 4.1: the trimmed packet is forwarded to the destination.
	ForwardToDestination,
	// Back-to-sender notification. UEC 1.0.3 section 4.1 explicitly excludes
	// this: "Sending a trimmed packet back to the source ... is not part of this
	// specification". Retained as a non-UET research mode only.
	BackToSender,
};

enum class PacketTrimTrigger : uint32_t {
	Admission = 1,
	EgressQueue,
	// DSCP_TRIMMED_LAST_HOP variants (UEC 1.0.3 section 4.1.4.1).
	AdmissionLastHop,
	EgressQueueLastHop,
};

class SwitchNode : public Node{
	static const uint32_t pCnt = 1025;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes

	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
	bool m_ecnEnabled;
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK
	uint32_t m_packetTrimMode;
	uint32_t m_trimmedQueueIndex; // TC_med egress queue for DSCP_TRIMMED
	uint32_t m_minTrimSize;       // MIN_TRIM_SIZE, in IP payload bytes
	bool m_lastHopTrimCodepoint;  // emit DSCP_TRIMMED_LAST_HOP on TOR downlinks
	bool m_pfcEnabled;            // generate PFC on ingress pressure

private:
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	bool SendToDev(Ptr<Packet>p, CustomHeader &ch);
	bool TrimAndForward(Ptr<Packet> p, CustomHeader &ch, int outDev,
		PacketTrimTrigger trigger);
	bool TrimInPlace(Ptr<Packet> p, const CustomHeader &ch, bool lastHop);
	bool SendTrimNotification(Ptr<const Packet> original, const CustomHeader &ch,
		uint32_t payloadSize, bool lastHop, PacketTrimTrigger trigger);
	bool IsLastHopTo(uint32_t outDev) const;
	bool PacketTrimEnabledFor(const CustomHeader &ch) const;
	uint32_t QueueIndexFor(const CustomHeader &ch) const;
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
public:
	Ptr<SwitchMmu> m_mmu;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDrop;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceTrim;

	static TypeId GetTypeId (void);
	SwitchNode();
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
