#ifndef BESS_MODULES_INVISV_L4_PROXY_H_
#define BESS_MODULES_INVISV_L4_PROXY_H_

#include "../module.h"
#include "../pb/module_msg.pb.h"

#include <rte_config.h>
#include <rte_hash_crc.h>

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "../utils/cuckoo_map.h"
#include "../utils/endian.h"
#include "../utils/ip.h"
#include "../utils/random.h"

using IpProto = bess::utils::Ipv4::Proto;

// Theory of operation:
//
// Definitions:
// Endpoint = <IPv4 address, L4 port>
// This proxy = <proxy ingress IPv4 address, proxy ingress L4 port>
// Next-hop endpoint = <next-hop IPv4 address, next-hop L4 port>
//
// Packet transformation in the forward direction:
// [Endpoint -> This proxy] --> [This proxy -> Next-hop endpoint]
// Packet transformation in the reverse direction:
// [Next-hop endpoint -> This proxy] --> [This proxy -> Endpoint]
//
// There is a single hash table |map_| of Endpoint -> (Endpoint, timestamp),
// which contains both forward and reverse mapping. They have the same
// lifespan (e.g., if one entry is deleted, its peer is also deleted).
//
// INVISV L4 proxy module always assumes a next-hop endpoint C:c:
// Suppose the table is empty, and we see a packet A:a -> B:b.
// INVISV L4 proxy is to represent Endpoint A:a to communicate with C:c.
// To do so, we find a free L4 port number b' from the pool (TCP port for
// a TCP connection; UDP port for a UDP connection) and create two entries:
// - entry 1  A:a -> B:b'
// - entry 2  B:b' -> A:a
// Then, the packet is updated as:
// before: [A:a, B:b] --> after: [B:b', C:c] (with entry 1).
// When a return packet [C:c, B:b'] comes in, the packet is updated as:
// before: [C:c, B:b'] --> after: [B:b, A:a] (with entry 2).

using bess::utils::be16_t;
using bess::utils::be32_t;

struct alignas(8) Endpoint {
  be32_t addr;
  be16_t port;
  // L4 protocol (IPPROTO_*). Note that this is a 1-byte field in the IP header,
  // but we store the value in a 2-byte field so that the struct be 8-byte long
  // without a hole, without needing to initialize it explicitly.
  uint16_t protocol;

  bool operator<(const Endpoint &other) const {
    const Endpoint &me = *this;
    const union {
        Endpoint endpoint;
        uint64_t u64;
    } &left = {.endpoint = me}, &right = {.endpoint = other};

    return left.u64 < right.u64;
  }

  struct Hash {
    std::size_t operator()(const Endpoint &e) const {
#if __x86_64
      return crc32c_sse42_u64(
          (static_cast<uint64_t>(e.addr.raw_value()) << 32) |
              (static_cast<uint64_t>(e.port.raw_value()) << 16) |
              static_cast<uint64_t>(e.protocol),
          0);
#else
      return rte_hash_crc(&e, sizeof(uint64_t), 0);
#endif
    }
  };

  struct EqualTo {
    bool operator()(const Endpoint &lhs, const Endpoint &rhs) const {
      const union {
        Endpoint endpoint;
        uint64_t u64;
      } &left = {.endpoint = lhs}, &right = {.endpoint = rhs};

      return left.u64 == right.u64;
    }
  };
};

static_assert(sizeof(Endpoint) == sizeof(uint64_t), "Incorrect Endpoint");

struct ProxyEntry {
  Endpoint endpoint;

  // last_refresh is only updated for forward-direction (outbound) packets.
  // Reverse entries will have an garbage value.
  // We do lazy reclaim of expired: Proxy mapping entry will NOT expire unless
  // the module runs out of free transport ports in the port pool.
  uint64_t last_refresh;  // in nanoseconds (ctx.current_ns)
};

// Port ranges are used to scale out the Proxy.
struct PortRange {
  // Start of port range.
  uint16_t begin;
  // End of port range (exclusive).
  uint16_t end;
  // Is range usable, i.e., can we safely give out ports.
  bool suspended;
};

class INVISVL4Proxy final : public Module {
 public:
  enum Direction {
    kForward = 0,  // client -> next-hop endpoint
    kReverse = 1,  // next-hop endpoint -> client
  };

  static const bool kAllowAllClientIPs = true;
  static const gate_idx_t kNumOGates = 1;
  static const gate_idx_t kNumIGates = 1;

  static const Commands cmds;

  INVISVL4Proxy() 
      : Module(),
      connection_timeout_ns_(kDefaultTimeOutNs) {
    // Enable multi-core.
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::INVISVL4ProxyArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &arg);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetL4Proxy(
              const bess::pb::INVISVL4ProxySetProxyEndpointArg &arg);
  CommandResponse SetNextHopEndpoint(
              const bess::pb::INVISVL4ProxySetNextHopEndpointArg &arg);
  CommandResponse GetL4Proxy(const bess::pb::EmptyArg &);
  CommandResponse GetNextHopEndpoint(const bess::pb::EmptyArg &);
  CommandResponse SetL4ProxyClientAllowlist(
              const bess::pb::INVISVL4ProxySetClientAllowlistArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // returns the number of active proxy connection entries (UDP port mappings)
  std::string GetDesc() const override;

 private:
  using HashTable = bess::utils::CuckooMap<Endpoint, ProxyEntry, Endpoint::Hash,
                                           Endpoint::EqualTo>;

  bool use_ip_tun_ = false;

  // The max time duration (in ms) that is allowed for keeping a pair of
  // connection entry in |map_| with no new packet arrivals from a client.
  // If |connection_timeout_ns_| is 0, then no timeout limit.
  uint64_t connection_timeout_ns_;

  // By default, no connection timeout.
  static const uint64_t kDefaultTimeOutNs = 0;

  // how many times shall we try to find a free port number?
  static const int kMaxTrials = 128;

  // Try to create a new connection entry for |client|. This function should be
  // called after the proxy has verified that |client| is a valid client that
  // can use the RaaS service.
  // This function selects a port from |l4_port_ranges_|, makes sure that it
  // is un-used or has expired. Then, it creates two entries in |map_| for
  // traffic of both directions, resets the mapping timeout.
  HashTable::Entry *CreateNewEntry(const Endpoint &client, uint64_t now);

  // If |dst| is |curr_l4_proxy_| and |src| is allowed: |src| is in
  // in |proxy_client_allowlist_|, return true. Otherwise, return false.
  bool IsForwardTraffic(Endpoint &src, Endpoint &dst);

  // If |src| is |next_hop_| and |dst|'s IP matches |curr_l4_proxy_| IP,
  // return true. Otherwise, return false.
  bool IsReverseTraffic(Endpoint &src, Endpoint &dst) const;

  // The endpoint info [IP:Port:Protocol] for this proxy service.
  Endpoint curr_proxy_tcp_endpoint_;
  Endpoint curr_proxy_udp_endpoint_;

  // The endpoint info [IP:Port:Protocol] for the next-hop endpoint to which
  // |this| L4 proxy should send packets to.
  Endpoint next_hop_tcp_endpoint_;
  Endpoint next_hop_udp_endpoint_;

  // Available TCP/UDP port ranges that can be used by this proxy instance.
  std::vector<PortRange> l4_port_ranges_;

  // |map_| contains all [client <-> this proxy <-> next-hop] connections.
  HashTable map_;
  std::mutex map_lock_;

  // |proxy_client_allowlist_| maintains the service status for clients. For
  // each client, map key is IP; value indicates whether the module should
  // process traffic for this client or not.
  std::map<be32_t, bool> proxy_client_allowlist_;
  std::mutex client_lock_;

  Random rng_;
};

#endif  // BESS_MODULES_INVISV_L4_PROXY_H_
