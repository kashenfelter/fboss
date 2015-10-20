/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "DHCPv4Handler.h"
#include <arpa/inet.h>
#include <string>
#include <folly/io/IOBuf.h>
#include <folly/io/Cursor.h>
#include <folly/IPAddress.h>
#include "FbossError.h"
#include "fboss/agent/packet/DHCPv4Packet.h"
#include "fboss/agent/packet/IPv4Hdr.h"
#include "fboss/agent/packet/IPProto.h"
#include "fboss/agent/packet/EthHdr.h"
#include "fboss/agent/packet/Ethertype.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/state/Vlan.h"
#include "fboss/agent/state/VlanMap.h"
#include "Platform.h"
#include "RxPacket.h"
#include "SwSwitch.h"
#include "SwitchStats.h"
#include "TxPacket.h"
#include "UDPHeader.h"

using std::string;
using std::unique_ptr;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::MacAddress;
using folly::io::Cursor;
using folly::IOBuf;
using folly::io::RWPrivateCursor;
using namespace facebook::fboss;

typedef EthHdr::VlanTags_t VlanTags_t;

namespace {
IPv4Hdr makeIpv4Header(IPAddressV4 srcIp, IPAddressV4 dstIp, uint8_t ttl,
    uint16_t length) {
  // Prepare IPv4 header
  IPv4Hdr ipHdr(4, // version
      IPv4Hdr::minSize() / 4, // ihl in 32 bit chunks
      0, // dscp
      0, // ecn
      length, // length
      0, // id
      false, // Don't fragment
      false, // More fragments
      0, // Fragment offset
      ttl, // TTL
      IP_PROTO::IP_PROTO_UDP, // Protocol
      0, // Checksum
      srcIp, // Source
      dstIp // Destination IP
      );
  ipHdr.computeChecksum();
  return ipHdr;
}

EthHdr makeEthHdr(MacAddress srcMac, MacAddress dstMac, VlanID vlan) {
  VlanTags_t vlanTags;
  vlanTags.push_back(VlanTag(vlan, ETHERTYPE_VLAN));
  return EthHdr(dstMac, srcMac, vlanTags, ETHERTYPE_IPV4);
}

void sendDHCPPacket(SwSwitch* sw, const EthHdr& ethHdr, const IPv4Hdr& ipHdr,
    const UDPHeader& udpHdr, const DHCPv4Packet& dhcpPacket) {
  // Allocate packet
  auto txPacket = sw->allocatePacket(
      18 + // ethernet header
      ipHdr.size() +
      udpHdr.size() +
      dhcpPacket.size());
  const auto& vlanTags = ethHdr.getVlanTags();
  CHECK(!vlanTags.empty());

  RWPrivateCursor rwCursor(txPacket->buf());
  // Write data to packet buffer
  txPacket->writeEthHeader(&rwCursor, ethHdr.getDstMac(), ethHdr.getSrcMac(),
                      VlanID(vlanTags[0].vid()), ethHdr.getEtherType());
  ipHdr.write(&rwCursor);
  rwCursor.writeBE<uint16_t>(udpHdr.srcPort);
  rwCursor.writeBE<uint16_t>(udpHdr.dstPort);
  rwCursor.writeBE<uint16_t>(udpHdr.length);
  folly::io::RWPrivateCursor csumCursor(rwCursor);
  rwCursor.skip(2);
  folly::io::Cursor payloadStart(rwCursor);

  dhcpPacket.write(&rwCursor);
  uint16_t csum = udpHdr.computeChecksum(ipHdr, payloadStart);
  csumCursor.writeBE<uint16_t>(csum);

  VLOG (4) << " Sent dhcp packet :"
    << " Eth header : " << ethHdr
    << " IPv4 Header : "<< ipHdr
    << " UDP Header : " << udpHdr;
  // Send packet
  sw->sendPacketSwitched(std::move(txPacket));
}

int processOption(const DHCPv4Packet::Options& optionsIn, int optIndex,
    DHCPv4Packet& dhcpPacketOut, bool toAppend) {

  uint8_t op = optionsIn[optIndex];
  uint8_t optLen = DHCPv4Packet::isOptionWithoutLength(op) ?
      0 : optionsIn[optIndex + 1];
  const uint8_t* optBytes = optLen == 0 ? nullptr :
    &optionsIn[optIndex + 2];
  if (toAppend) {
    dhcpPacketOut.appendOption(op, optLen, optBytes);
  }
  return optLen == 0 ?
      1 :          // option byte
      2 + optLen;  // Option byte + len + option data

}

}

namespace facebook { namespace fboss {


constexpr uint16_t DHCPv4Handler::kBootPSPort;
constexpr uint16_t DHCPv4Handler::kBootPCPort;

bool DHCPv4Handler::isDHCPv4Packet(const UDPHeader& udpHdr) {
  auto srcPort = udpHdr.srcPort;
  auto dstPort = udpHdr.dstPort;
  return (srcPort == kBootPCPort || srcPort == kBootPSPort) ||
         (dstPort == kBootPCPort || dstPort == kBootPSPort);
}

void DHCPv4Handler::handlePacket(SwSwitch* sw, std::unique_ptr<RxPacket> pkt,
    MacAddress srcMac, MacAddress dstMac, const IPv4Hdr& ipHdr,
    const UDPHeader& udpHdr, Cursor cursor) {
  sw->stats()->port(pkt->getSrcPort())->dhcpV4Pkt();
  if (ipHdr.ttl <= 1) {
    sw->stats()->port(pkt->getSrcPort())->dhcpV4BadPkt();
    VLOG(4) << "Dropped DHCP packet with TTL of " << ipHdr.ttl;
    return;
  }

  // Parse dhcp packet
  DHCPv4Packet dhcpPkt;
  try {
    dhcpPkt.parse(&cursor);
  } catch (const FbossError& err) {
     sw->stats()->port(pkt->getSrcPort())->dhcpV4BadPkt();
     throw; // Rethrow
  }
  if (dhcpPkt.hasDhcpCookie()) {
    switch(dhcpPkt.op) {
      case BOOTREQUEST:
        VLOG(4) << " Got boot request ";
        processRequest(sw, std::move(pkt), srcMac, ipHdr, dhcpPkt);
        break;
      case BOOTREPLY:
        VLOG(4) << " Got boot reply";
        processReply(sw, std::move(pkt), ipHdr, dhcpPkt);
        break;
      default:
        VLOG(4)<<" Unknown DHCP Packet type "<<(uint)dhcpPkt.op;
        sw->stats()->port(pkt->getSrcPort())->dhcpV4BadPkt();
        break;
    }
  } else {
    // Got a bootp packet do nothing. Should never
    // really happen since DHCP obsoleted BOOT protocol
    // and we run only DHCP. Drop the packet
    VLOG (4) << " Dropped bootp packet ";
  }

}


void DHCPv4Handler::processRequest(SwSwitch* sw, std::unique_ptr<RxPacket> pkt,
    MacAddress srcMac, const IPv4Hdr& origIPHdr,
    const DHCPv4Packet& dhcpPacket) {
  auto dhcpPacketOut(dhcpPacket);
  auto vlan = sw->getState()->getVlans()->getVlanIf(pkt->getSrcVlan());
  if (!vlan) {
    sw->stats()->dhcpV4DropPkt();
    VLOG(4) << " VLAN  "<< pkt->getSrcVlan() << " is no longer present "
      << " dropped dhcp packet received on a port in this VLAN";
    return;
  }
  auto dhcpServer = vlan->getDhcpV4Relay();

  VLOG(4) << "srcMac: " << srcMac.toString();
  // look in the override map, and use relevant destination
  auto dhcpOverrideMap = vlan->getDhcpV4RelayOverrides();
  if (dhcpOverrideMap.find(srcMac) != dhcpOverrideMap.end()) {
    dhcpServer = dhcpOverrideMap[srcMac];
    VLOG(4) << "dhcpServer: " << dhcpServer;
  }

  if (dhcpServer.isZero()) {
    sw->stats()->dhcpV4DropPkt();
    VLOG(4) << " No relay configured for VLAN : "<< vlan->getID()
      << " dropped dhcp packet ";
    return;
  }

  IPAddressV4 switchIp;
  auto vlanInterface = sw->getState()->getInterfaces()->getInterfaceInVlanIf(
      pkt->getSrcVlan());
  auto& addresses = vlanInterface->getAddresses();
  for (auto address: addresses) {
    if (address.first.isV4()) {
      switchIp = address.first.asV4();
      break;
    }
  }

  if (switchIp.isZero()) {
    sw->stats()->dhcpV4DropPkt();
    LOG(ERROR) << "Could not find a SVI interface on vlan : "
      << pkt->getSrcVlan()<< "DHCP packet dropped ";
    return;
  }

  VLOG(4) << " Got switch ip : " << switchIp;
  // Prepare DHCP packet to relay
  if (!addAgentOptions(sw, pkt->getSrcPort(), switchIp, dhcpPacket,
        dhcpPacketOut)) {
    sw->stats()->port(pkt->getSrcPort())->dhcpV4BadPkt();
    VLOG(4) << "Bad DHCP packet, error adding agent options."
      << " DHCP packet dropped";
    return;
  }
  // Incrementing hops is optional for relay agent forwarding,
  // however it seems safer in case of loops. Also seen cases
  // where not incrementing this on the DHCP request causes
  // the server to drop our request.
  const int kMaxHops = 255;
  if (dhcpPacketOut.hops < kMaxHops) {
    dhcpPacketOut.hops++;
  } else {
    VLOG(4) << "Max hops exceeded for dhcp packet";
    sw->stats()->port(pkt->getSrcPort())->dhcpV4BadPkt();
    return;
  }
  dhcpPacketOut.giaddr = switchIp;
  // Look up cpu mac from platform
  MacAddress cpuMac = sw->getPlatform()->getLocalMac();

  // Prepare the packet to be sent out
  EthHdr ethHdr = makeEthHdr(cpuMac, cpuMac, pkt->getSrcVlan());
  auto ipHdr = makeIpv4Header(switchIp, dhcpServer, origIPHdr.ttl - 1,
      IPv4Hdr::minSize() + UDPHeader::size() + dhcpPacketOut.size());
  UDPHeader udpHdr(kBootPSPort, kBootPSPort,
      UDPHeader::size() + dhcpPacketOut.size());
  // Send packet
  sendDHCPPacket(sw, ethHdr, ipHdr, udpHdr, dhcpPacketOut);
}

void DHCPv4Handler::processReply(SwSwitch* sw, std::unique_ptr<RxPacket> pkt,
      const IPv4Hdr& origIPHdr, const DHCPv4Packet& dhcpPacket) {
  auto dhcpPacketOut(dhcpPacket);
  if (!stripAgentOptions(sw, pkt->getSrcPort(), dhcpPacket, dhcpPacketOut)) {
    sw->stats()->port(pkt->getSrcPort())->dhcpV4BadPkt();
    VLOG(4) << "Bad DHCP packet, error stripping agent options."
      << " DHCP packet dropped";
    return;
  }
  IPAddressV4 clientIP = IPAddressV4::fromLong(INADDR_BROADCAST);
  if (!(dhcpPacket.flags & DHCPv4Packet::kFlagBroadcast)) {
    clientIP = dhcpPacket.yiaddr;
  }
  auto switchIp = origIPHdr.dstAddr;
  MacAddress cpuMac = sw->getPlatform()->getLocalMac();
  // Extract client MAC address from dhcp reply
  uint8_t chaddr[MacAddress::SIZE];
  memcpy(chaddr, dhcpPacketOut.chaddr.data(), MacAddress::SIZE);
  MacAddress dstMac = MacAddress::fromBinary(
    folly::ByteRange(chaddr, MacAddress::SIZE));

  // Clear out the relay address field
  dhcpPacketOut.giaddr = IPAddressV4();

  // TODO we should add router id information to the packet
  // to get the VRF of the interface that this packet came
  // in on. Assuming 0 for now since we have only one VRF
  auto intf = sw->getState()->getInterfaces()->getInterface(RouterID(0),
      IPAddress(switchIp));
  if (!intf) {
    sw->stats()->port(pkt->getSrcPort())->dhcpV4DropPkt();
    LOG (INFO) << "Could not lookup interface for : " << switchIp
      << "DHCP packet dropped ";
    return;
  }

  // Prepare the packet to be sent out
  EthHdr ethHdr = makeEthHdr(cpuMac, dstMac, intf->getVlanID());
  auto ipHdr = makeIpv4Header(switchIp, clientIP, origIPHdr.ttl - 1,
      IPv4Hdr::minSize() + UDPHeader::size() + dhcpPacketOut.size());
  UDPHeader udpHdr(kBootPSPort, kBootPCPort,
      UDPHeader::size() + dhcpPacketOut.size());

  sendDHCPPacket(sw, ethHdr, ipHdr, udpHdr, dhcpPacketOut);
}


bool DHCPv4Handler::addAgentOptions(SwSwitch* sw, PortID port,
    IPAddressV4 relayAddr, const DHCPv4Packet& dhcpPacketIn,
    DHCPv4Packet& dhcpPacketOut) {
  dhcpPacketOut.clearOptions();
  const auto& optionsIn = dhcpPacketIn.options;
  auto optIndex = 0;
  bool isDHCP = false;
  bool done = false;
  uint16_t maxMsgSize = 0;
  while (optIndex < optionsIn.size() && !done) {
    bool appendOption = true;
    switch(optionsIn[optIndex]) {
      case DHCP_MESSAGE_TYPE:
        isDHCP = true;
        break;
      case DHCP_MAX_MESSAGE_SIZE:
        maxMsgSize = ntohs(optionsIn[optIndex + 2]);
        break;
      case DHCP_AGENT_OPTIONS:
        if (isDHCP) {
          // FIXME We should really forward this along unchanged.
          // see t3862629 for details.
          LOG (INFO) <<" Agent options already present dropping DHCP packet";
          return false; //Options already present discard packet
        }
        break;
      case END:
        done = true;
        appendOption = false;
        break;
    }
    optIndex += processOption(optionsIn, optIndex, dhcpPacketOut,
        appendOption);
  }
  if (isDHCP) {
    //Append agent option
    uint8_t optlen = 2 + relayAddr.byteCount();
    uint8_t subOptArray[optlen];
    subOptArray[0] = AGENT_CIRCUIT_ID;
    subOptArray[1] = relayAddr.byteCount();
    memcpy(subOptArray + 2, relayAddr.bytes(), relayAddr.byteCount());
    dhcpPacketOut.appendOption(DHCP_AGENT_OPTIONS, optlen, subOptArray);
  }
  dhcpPacketOut.appendOption(END, 0, nullptr);
  dhcpPacketOut.padToMinLength();
  if (isDHCP && maxMsgSize && dhcpPacketOut.size() > maxMsgSize) {
    return false;
  }

  return isDHCP;
}

bool DHCPv4Handler::stripAgentOptions(SwSwitch* sw, PortID port,
    const DHCPv4Packet& dhcpPacketIn, DHCPv4Packet& dhcpPacketOut) {
  dhcpPacketOut.clearOptions();
  const auto& optionsIn = dhcpPacketIn.options;
  auto optIndex = 0;
  bool isDHCP = false;
  bool done = false;
  while (optIndex < optionsIn.size() && !done) {
    bool appendOption = true;
    switch(optionsIn[optIndex]) {
      case DHCP_MESSAGE_TYPE:
        isDHCP = true;
        break;
      case DHCP_AGENT_OPTIONS:
        if (isDHCP) {
          appendOption = false;
        }
        break;
      case END:
        done = true;
        appendOption = true;
        break;
    }
    optIndex += processOption(optionsIn, optIndex, dhcpPacketOut,
        appendOption);
  }
  dhcpPacketOut.padToMinLength();
  return isDHCP;
}

}} //facebook::fboss
