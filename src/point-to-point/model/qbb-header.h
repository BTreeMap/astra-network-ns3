//yibo

#ifndef QBB_HEADER_H
#define QBB_HEADER_H

#include <stdint.h>
#include "ns3/header.h"
#include "ns3/buffer.h"
#include "ns3/int-header.h"

namespace ns3 {

/**
 * \ingroup Pause
 * \brief Header for the Congestion Notification Message
 *
 * This class has two fields: The five-tuple flow id and the quantized
 * congestion level. This can be serialized to or deserialzed from a byte
 * buffer.
 */
 
class qbbHeader : public Header
{
public:
 
  enum {
    FLAG_CNP = 0,
    FLAG_TRIM_LASTHOP = 1,
    // The receiver reports that the budget entry this range belongs to has no
    // allowance left. It is the only thing a sender cannot work out for
    // itself, and the only thing that ends its congestion exemption.
    FLAG_ALLOWANCE_EXHAUSTED = 2,
    // The receiver reports that this flow is one it may forgive on this step.
    // An acknowledgement carrying it with FLAG_ALLOWANCE_EXHAUSTED clear is
    // the grant: from it the sender withholds congestion signals. The sender
    // cannot know either fact, because eligibility and the step's phase are
    // the receiver's, so the grant is the receiver's to give.
    FLAG_FORGIVENESS_ELIGIBLE = 3
  };
  qbbHeader (uint16_t pg);
  qbbHeader ();
  virtual ~qbbHeader ();

//Setters
  /**
   * \param pg The PG
   */
  void SetPG (uint16_t pg);
  void SetSeq(uint32_t seq);
  void SetSport(uint32_t _sport);
  void SetDport(uint32_t _dport);
  void SetTs(uint64_t ts);
  void SetCnp();
  void SetTrimPayloadSize(uint32_t payloadSize);
  void SetTrimLastHop(bool lastHop);
  void SetAllowanceExhausted(bool spent);
  void SetForgivenessEligible(bool eligible);
  void SetIntHeader(const IntHeader &_ih);

//Getters
  /**
   * \return The pg
   */
  uint16_t GetPG () const;
  uint32_t GetSeq() const;
  uint16_t GetPort() const;
  uint16_t GetSport() const;
  uint16_t GetDport() const;
  uint64_t GetTs() const;
  uint8_t GetCnp() const;
  uint32_t GetTrimPayloadSize() const;
  bool IsTrimLastHop() const;
  bool IsAllowanceExhausted() const;

  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual void Print (std::ostream &os) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  static uint32_t GetBaseSize(); // size without INT

private:
  uint16_t sport, dport;
  uint16_t flags;
  uint16_t m_pg;
  uint32_t m_seq; // the qbb sequence number.
  uint32_t m_trimPayloadSize;
  IntHeader ih;
  
};

}; // namespace ns3

#endif /* QBB_HEADER */
