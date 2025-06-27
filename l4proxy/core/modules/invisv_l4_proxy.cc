#include "invisv_l4_proxy.h"

#include <algorithm>
#include <numeric>
#include <string>

#include "../utils/checksum.h"
#include "../utils/common.h"
#include "../utils/ether.h"
#include "../utils/format.h"
#include "../utils/icmp.h"
#include "../utils/ip.h"
#include "../utils/tcp.h"
#include "../utils/udp.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::Tcp;
using bess::utils::Icmp;
using bess::utils::ChecksumIncrement16;
using bess::utils::ChecksumIncrement32;
using bess::utils::UpdateChecksumWithIncrement;
using bess::utils::UpdateChecksum16;

const Commands INVISVL4Proxy::cmds = {
    {"get_initial_arg", "EmptyArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::GetInitialArg), Command::THREAD_SAFE},
    {"get_runtime_config", "EmptyArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::GetRuntimeConfig), Command::THREAD_SAFE},
    {"set_runtime_config", "EmptyArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::SetRuntimeConfig), Command::THREAD_SAFE},
    {"set_proxy", "INVISVL4ProxySetProxyEndpointArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::SetL4Proxy), Command::THREAD_SAFE},
    {"set_next_hop_endpoint", "INVISVL4ProxySetNextHopEndpointArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::SetNextHopEndpoint),
     Command::THREAD_SAFE},
    {"get_proxy", "EmptyArg", MODULE_CMD_FUNC(&INVISVL4Proxy::GetL4Proxy),
     Command::THREAD_SAFE},
    {"get_next_hop_endpoint", "EmptyArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::GetNextHopEndpoint),
     Command::THREAD_SAFE},
    {"set_client_allowlist", "INVISVL4ProxySetClientAllowlistArg",
     MODULE_CMD_FUNC(&INVISVL4Proxy::SetL4ProxyClientAllowlist),
     Command::THREAD_SAFE}};

namespace {
static inline std::tuple<bool, Endpoint, Endpoint> ExtractEndpoint(
                                              const Ipv4 *ip, const void *l4) {
  IpProto proto = static_cast<IpProto>(ip->protocol);

  if (proto == IpProto::kUdp || proto == IpProto::kTcp) {
    // UDP and TCP share the same layout for port numbers
    const Udp *udp = static_cast<const Udp *>(l4);
    Endpoint src = {.addr = ip->src, .port = udp->src_port, .protocol = proto};
    Endpoint dst = {.addr = ip->dst, .port = udp->dst_port, .protocol = proto};

    return std::make_tuple(true, src, dst);
  }

  return std::make_tuple(false,
      Endpoint{.addr = ip->src, .port = be16_t(0), .protocol = 0},
      Endpoint{.addr = ip->dst, .port = be16_t(0), .protocol = 0});
}

static inline void Stamp(Ipv4 *ip, void *l4,
                  const Endpoint &old_src, const Endpoint &old_dst,
                  const Endpoint &new_src, const Endpoint &new_dst) {
  IpProto proto = static_cast<IpProto>(ip->protocol);
  DCHECK_EQ(old_src.protocol, new_src.protocol);
  DCHECK_EQ(old_src.protocol, proto);

  ip->src = new_src.addr;
  ip->dst = new_dst.addr;

  uint32_t l3_increment =
    ChecksumIncrement32(old_src.addr.raw_value(), new_src.addr.raw_value()) +
    ChecksumIncrement32(old_dst.addr.raw_value(), new_dst.addr.raw_value());
  ip->checksum = UpdateChecksumWithIncrement(ip->checksum, l3_increment);

  uint32_t l4_increment = l3_increment +
    ChecksumIncrement16(old_src.port.raw_value(), new_src.port.raw_value()) +
    ChecksumIncrement16(old_dst.port.raw_value(), new_dst.port.raw_value());

  Udp *udp = static_cast<Udp *>(l4);
  udp->src_port = new_src.port;
  udp->dst_port = new_dst.port;

  if (proto == IpProto::kTcp) {
    Tcp *tcp = static_cast<Tcp *>(l4);
    tcp->checksum = UpdateChecksumWithIncrement(tcp->checksum, l4_increment);
  } else {
    // NOTE: UDP checksum is tricky in two ways:
    // 1. if the old checksum field was 0 (not set), no need to update
    // 2. if the updated value is 0, use 0xffff (rfc768)
    if (udp->checksum != 0) {
      udp->checksum =
          UpdateChecksumWithIncrement(udp->checksum, l4_increment) ?: 0xffff;
    }
  }
}
} // namespace

CommandResponse INVISVL4Proxy::Init(const bess::pb::INVISVL4ProxyArg &arg) {
  // Check before committing any changes.
  uint32_t num_port_ranges = arg.l4_port_ranges().size();
  for (uint32_t i = 0; i < num_port_ranges; i++) {
    auto &range = arg.l4_port_ranges().Get(i);
    if (range.begin() >= range.end() || range.begin() > UINT16_MAX ||
        range.end() > UINT16_MAX) {
      return CommandFailure(EINVAL, "Port range %d is malformed", i);
    }
  }

  // Update the port range list. If no input, then all ports are available.
  for (const auto &range : arg.l4_port_ranges()) {
    l4_port_ranges_.emplace_back(PortRange{
        .begin = (uint16_t)range.begin(),
        .end = (uint16_t)range.end(),
        // Control plane gets to decide if the port range can be used.
        .suspended = range.suspended()});
  }
  if (l4_port_ranges_.size() == 0) {
    l4_port_ranges_.emplace_back(PortRange{
        .begin = 0u, .end = 65535u, .suspended = false,
    });
  }

  // Configure the proxy's connection timeout limit.
  if (arg.l4_port_timeout_ms() > 0) {
    connection_timeout_ns_ = (arg.l4_port_timeout_ms()) * 1000 * 1000;
  }

  // Configure this proxy's IP and port.
  curr_proxy_tcp_endpoint_.protocol = IpProto::kTcp;
  curr_proxy_udp_endpoint_.protocol = IpProto::kUdp;

  if (arg.proxy_addr().size() > 0) {
    be32_t addr;
    bool ret = bess::utils::ParseIpv4Address(arg.proxy_addr(), &addr);
    if (!ret) {
      return CommandFailure(EINVAL,
          "invalid proxy IP address %s", arg.proxy_addr().c_str());
    }
    curr_proxy_tcp_endpoint_.addr = addr;
    curr_proxy_tcp_endpoint_.port = be16_t(arg.proxy_tcp_port());
    curr_proxy_udp_endpoint_.addr = addr;
    curr_proxy_udp_endpoint_.port = be16_t(arg.proxy_udp_port());
  } else {
    curr_proxy_tcp_endpoint_.addr = be32_t(0);
    curr_proxy_tcp_endpoint_.port = be16_t(0);
    curr_proxy_udp_endpoint_.addr = be32_t(0);
    curr_proxy_udp_endpoint_.port = be16_t(0);
  }

  // Configure the next-hop endpoint's IP and port.
  next_hop_tcp_endpoint_.protocol = IpProto::kTcp;
  next_hop_udp_endpoint_.protocol = IpProto::kUdp;

  if (arg.next_hop_addr().size() > 0) {
    be32_t addr;
    bool ret = bess::utils::ParseIpv4Address(arg.next_hop_addr(), &addr);
    if (!ret) {
      return CommandFailure(EINVAL,
          "invalid proxy IP address %s", arg.next_hop_addr().c_str());
    }
    next_hop_tcp_endpoint_.addr = addr;
    next_hop_tcp_endpoint_.port = be16_t(arg.next_hop_tcp_port());
    next_hop_udp_endpoint_.addr = addr;
    next_hop_udp_endpoint_.port = be16_t(arg.next_hop_udp_port());
  } else {
    next_hop_tcp_endpoint_.addr = be32_t(0);
    next_hop_tcp_endpoint_.port = be16_t(0);
    next_hop_udp_endpoint_.addr = be32_t(0);
    next_hop_udp_endpoint_.port = be16_t(0);
  }

  use_ip_tun_ = false;
  if (arg.use_ip_tun()) {
    use_ip_tun_ = true;
  }

  return CommandSuccess();
}

CommandResponse INVISVL4Proxy::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::INVISVL4ProxyArg resp;
  for (const auto &range : l4_port_ranges_) {
    auto erange = resp.add_l4_port_ranges();
    erange->set_begin((uint32_t)range.begin);
    erange->set_end((uint32_t)range.end);
    erange->set_suspended(range.suspended);
  }
  resp.set_proxy_addr(ToIpv4Address(curr_proxy_tcp_endpoint_.addr));
  resp.set_proxy_tcp_port(curr_proxy_tcp_endpoint_.port.value());
  resp.set_proxy_udp_port(curr_proxy_udp_endpoint_.port.value());
  resp.set_next_hop_addr(ToIpv4Address(next_hop_tcp_endpoint_.addr));
  resp.set_next_hop_tcp_port(next_hop_tcp_endpoint_.port.value());
  resp.set_next_hop_udp_port(next_hop_udp_endpoint_.port.value());
  resp.set_l4_port_timeout_ms(connection_timeout_ns_ / 1000 / 1000);
  return CommandSuccess(resp);
}

CommandResponse INVISVL4Proxy::GetRuntimeConfig(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

CommandResponse INVISVL4Proxy::SetRuntimeConfig(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

CommandResponse INVISVL4Proxy::SetL4Proxy(
      const bess::pb::INVISVL4ProxySetProxyEndpointArg &arg) {
  if (arg.proxy_addr().size() > 0) {
    be32_t addr;
    bool ret = bess::utils::ParseIpv4Address(arg.proxy_addr(), &addr);
    if (!ret) {
      return CommandFailure(EINVAL,
          "invalid proxy IP address %s", arg.proxy_addr().c_str());
    }
    curr_proxy_tcp_endpoint_.addr = addr;
    curr_proxy_tcp_endpoint_.port = be16_t(arg.proxy_tcp_port());
    curr_proxy_udp_endpoint_.addr = addr;
    curr_proxy_udp_endpoint_.port = be16_t(arg.proxy_udp_port());
    return CommandSuccess();
  }
  return CommandFailure(EINVAL, "Incorrect proxy endpoint");
}

CommandResponse INVISVL4Proxy::SetNextHopEndpoint(
      const bess::pb::INVISVL4ProxySetNextHopEndpointArg &arg) {
  if (arg.next_hop_addr().size() > 0) {
    be32_t addr;
    bool ret = bess::utils::ParseIpv4Address(arg.next_hop_addr(), &addr);
    if (!ret) {
      return CommandFailure(EINVAL,
          "invalid next-hop proxy IP address %s", arg.next_hop_addr().c_str());
    }
    next_hop_tcp_endpoint_.addr = addr;
    next_hop_tcp_endpoint_.port = be16_t(arg.next_hop_tcp_port());
    next_hop_udp_endpoint_.addr = addr;
    next_hop_udp_endpoint_.port = be16_t(arg.next_hop_udp_port());
    return CommandSuccess();
  }
  return CommandFailure(EINVAL, "Incorrect next-hop proxy endpoint");
}

CommandResponse INVISVL4Proxy::GetL4Proxy(const bess::pb::EmptyArg &) {
  bess::pb::INVISVL4ProxyGetProxyEndpointArg r;
  r.set_proxy_addr(ToIpv4Address(curr_proxy_tcp_endpoint_.addr));
  r.set_proxy_tcp_port(curr_proxy_tcp_endpoint_.port.value());
  r.set_proxy_udp_port(curr_proxy_udp_endpoint_.port.value());
  return CommandSuccess(r);
}

CommandResponse INVISVL4Proxy::GetNextHopEndpoint(const bess::pb::EmptyArg &) {
  bess::pb::INVISVL4ProxyGetNextHopEndpointArg r;
  r.set_next_hop_addr(ToIpv4Address(next_hop_tcp_endpoint_.addr));
  r.set_next_hop_tcp_port(next_hop_tcp_endpoint_.port.value());
  r.set_next_hop_udp_port(next_hop_udp_endpoint_.port.value());
  return CommandSuccess(r);
}

CommandResponse INVISVL4Proxy::SetL4ProxyClientAllowlist(
              const bess::pb::INVISVL4ProxySetClientAllowlistArg &arg) {
  if (arg.client_addr().size() > 0) {
    be32_t client_addr;
    bool ret = bess::utils::ParseIpv4Address(arg.client_addr(), &client_addr);
    if (!ret) {
      return CommandFailure(EINVAL,
          "invalid client IP address %s", arg.client_addr().c_str());
    }

    std::lock_guard<std::mutex> guard(client_lock_);
    auto client_it = proxy_client_allowlist_.find(client_addr);
    if (client_it != proxy_client_allowlist_.end()) {
      proxy_client_allowlist_.erase(client_it);
    } else { // new
      if (arg.add()) {
        proxy_client_allowlist_.emplace(std::piecewise_construct,
            std::make_tuple(client_addr), std::make_tuple(true));
      }
    }
    return CommandSuccess();
  }
  return CommandFailure(EINVAL, "Incorrect client endpoint");
}

// Not necessary to inline this function, since it is less frequently called
INVISVL4Proxy::HashTable::Entry *INVISVL4Proxy::CreateNewEntry(
                                           const Endpoint &client,
                                           uint64_t now) {
  Endpoint src_external;

  // This proxy sends packets as |src_external|. To represent the |client|,
  // the proxy finds an un-used transport port, and rewrites the packet source
  // to use the current proxy's IP and the randomly selected port.
  src_external.addr = curr_proxy_tcp_endpoint_.addr;
  src_external.protocol = client.protocol;

  // Note: |port_ranges_| has at least one element.
  for (const auto &port_range : l4_port_ranges_) {
    uint16_t min;
    uint16_t range;  // consider [min, min + range) port range
    // Avoid allocation from an unusable range. We do this even when a range is
    // already in use since we might want to reclaim it once flows die out.
    if (port_range.suspended) {
      continue;
    }

    if (client.port == be16_t(0)) {
      // ignore port number 0
      return nullptr;
    } else if (client.port & ~be16_t(1023)) {
      if (port_range.end <= 1024u) {
        continue;
      }
      min = std::max((uint16_t)1024, port_range.begin);
      range = port_range.end - min + 1;
    } else {
      // Privileged ports are mapped to privileged ports (rfc4787 REQ-5-a)
      if (port_range.begin >= 1023u) {
        continue;
      }
      min = port_range.begin;
      range = std::min((uint16_t)1023, port_range.end) - min;
    }

    // Start from a random port, then do linear probing
    uint16_t start_port = min + rng_.GetRange(range);
    uint16_t port = start_port;
    int trials = 0;

    do {
      src_external.port = be16_t(port);
      auto *hash_reverse = map_.Find(src_external);
      if (src_external.port != curr_proxy_tcp_endpoint_.port &&
          src_external.port != curr_proxy_udp_endpoint_.port &&
          hash_reverse == nullptr) {
      found:
        // Found a valid client <-> src_external mapping
        ProxyEntry forward_entry;
        ProxyEntry reverse_entry;

        reverse_entry.endpoint = client;
        map_.Insert(src_external, reverse_entry);

        forward_entry.endpoint = src_external;
        return map_.Insert(client, forward_entry);
      } else {
        // A':a' is not free, but it might have been expired.
        // Check with the forward hash entry since timestamp refreshes only for
        // forward direction.
        auto *hash_forward = map_.Find(hash_reverse->second.endpoint);

        // Forward and reverse entries must share the same lifespan.
        DCHECK(hash_forward != nullptr);

        if (connection_timeout_ns_ > 0 &&
            now - hash_forward->second.last_refresh > connection_timeout_ns_) {
          // Found an expired mapping. Remove A':a' <-> A'':a''...
          map_.Remove(hash_forward->first);
          map_.Remove(hash_reverse->first);
          goto found;  // and go install A:a <-> A':a'
        }
      }

      port++;
      trials++;

      // Out of range? Also check if zero due to uint16_t overflow
      if (port == 0 || port >= min + range) {
        port = min;
      }
      // FIXME: Should not try for kMaxTrials.
    } while (port != start_port && trials < kMaxTrials);
  }
  return nullptr;
}

void INVISVL4Proxy::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  uint64_t now = ctx->current_ns;

  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    bess::Packet *pkt = batch->pkts()[i];

    Ipv4 *ip = nullptr;
    if (use_ip_tun_) {
      ip = pkt->head_data<Ipv4 *>();
    } else {
      Ethernet *eth = pkt->head_data<Ethernet *>();
      ip = reinterpret_cast<Ipv4 *>(eth + 1);
    }
    size_t ip_bytes = (ip->header_length) << 2;
    void *l4 = reinterpret_cast<uint8_t *>(ip) + ip_bytes;

    bool valid_protocol;
    Endpoint src, dst;
    std::tie(valid_protocol, src, dst) = ExtractEndpoint(ip, l4);

    if (!valid_protocol) {
      DropPacket(ctx, pkt);
      continue;
    }

    Direction dir = kReverse;
    if (!IsReverseTraffic(src, dst)) {
      if (!IsForwardTraffic(src, dst)) {
        DropPacket(ctx, pkt);
        continue;
      }
      dir = kForward;
    }

    Endpoint hash_key = dir == kForward ? src : dst;

    std::lock_guard<std::mutex> guard(map_lock_);
    auto *hash_item = map_.Find(hash_key);
    if (hash_item == nullptr) {
      if (dir != kForward || !(hash_item = CreateNewEntry(src, now))) {
        DropPacket(ctx, pkt);
        continue;
      }
    }

    if (dir == kForward) {
      // Only refresh for outbound packets
      hash_item->second.last_refresh = now;
      if (src.protocol == IpProto::kTcp) {
        Stamp(ip, l4, src, dst,
              hash_item->second.endpoint, next_hop_tcp_endpoint_);
      } else {
        Stamp(ip, l4, src, dst,
              hash_item->second.endpoint, next_hop_udp_endpoint_);
      }
    } else {
      if (src.protocol == IpProto::kTcp) {
        Stamp(ip, l4, src, dst,
              curr_proxy_tcp_endpoint_, hash_item->second.endpoint);
      } else {
        Stamp(ip, l4, src, dst,
              curr_proxy_udp_endpoint_, hash_item->second.endpoint);
      }
    }

    EmitPacket(ctx, pkt, ctx->current_igate);
  }
}

bool INVISVL4Proxy::IsForwardTraffic(Endpoint &src, Endpoint &dst) {
  // Client's protocol can be either TCP or UDP.
  Endpoint::EqualTo eq;
  if (!(eq(dst, curr_proxy_tcp_endpoint_) ||
      eq(dst, curr_proxy_udp_endpoint_))) { // Not for me
    return false;
  }

  if (!kAllowAllClientIPs) {
    std::lock_guard<std::mutex> guard(client_lock_);
    const auto it = proxy_client_allowlist_.find(src.addr);
    if (it == proxy_client_allowlist_.end()) { // Unknown client
      return false;
    }
  }
  return true;
}

bool INVISVL4Proxy::IsReverseTraffic(Endpoint &src, Endpoint &dst) const {
  Endpoint::EqualTo eq;
  if (!(eq(src, next_hop_tcp_endpoint_) ||
      eq(src, next_hop_udp_endpoint_))) { // Not from the next-hop endpoint
    return false;
  }
  if (dst.addr != curr_proxy_tcp_endpoint_.addr) { // Not for the current proxy
    return false;
  }
  return true;
}

std::string INVISVL4Proxy::GetDesc() const {
  // Divide by 2 since the table has both forward and reverse entries
  return bess::utils::Format("%zu entries", map_.Count() / 2);
}

ADD_MODULE(INVISVL4Proxy, "l4_proxy",
           "Dynamic network address/port translator for TCP/UDP traffic")
