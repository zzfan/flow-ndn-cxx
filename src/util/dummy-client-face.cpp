/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (c) 2013-2015 Regents of the University of California.
 *
 * This file is part of ndn-cxx library (NDN C++ library with eXperimental eXtensions).
 *
 * ndn-cxx library is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later version.
 *
 * ndn-cxx library is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more details.
 *
 * You should have received copies of the GNU General Public License and GNU Lesser
 * General Public License along with ndn-cxx, e.g., in COPYING.md file.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * See AUTHORS.md for complete list of ndn-cxx authors and contributors.
 */

#include "dummy-client-face.hpp"
#include "../transport/transport.hpp"
#include "../management/nfd-controller.hpp"
#include "../management/nfd-control-response.hpp"

namespace ndn {
namespace util {

const DummyClientFace::Options DummyClientFace::DEFAULT_OPTIONS { true, false };

class DummyClientFace::Transport : public ndn::Transport
{
public:
  void
  receive(Block block)
  {
    block.encode();
    if (static_cast<bool>(m_receiveCallback)) {
      m_receiveCallback(block);
    }
  }

  virtual void
  close()
  {
  }

  virtual void
  pause()
  {
  }

  virtual void
  resume()
  {
  }

  virtual void
  send(const Block& wire)
  {
    onSendBlock(wire);
  }

  virtual void
  send(const Block& header, const Block& payload)
  {
    EncodingBuffer encoder(header.size() + payload.size(), header.size() + payload.size());
    encoder.appendByteArray(header.wire(), header.size());
    encoder.appendByteArray(payload.wire(), payload.size());

    this->send(encoder.block());
  }

  boost::asio::io_service&
  getIoService()
  {
    return *m_ioService;
  }

public:
  Signal<Transport, Block> onSendBlock;
};

DummyClientFace::DummyClientFace(const Options& options, shared_ptr<Transport> transport)
  : Face(transport)
  , m_transport(transport)
{
  this->construct(options);
}

DummyClientFace::DummyClientFace(const Options& options, shared_ptr<Transport> transport,
                                 boost::asio::io_service& ioService)
  : Face(transport, ioService)
  , m_transport(transport)
{
  this->construct(options);
}

void
DummyClientFace::construct(const Options& options)
{
  m_transport->onSendBlock.connect([this] (const Block& blockFromDaemon) {
    try {
      Block packet(blockFromDaemon);
      packet.encode();
      lp::Packet lpPacket(packet);

      Buffer::const_iterator begin, end;
      std::tie(begin, end) = lpPacket.get<lp::FragmentField>();
      Block block(&*begin, std::distance(begin, end));

      if (block.type() == tlv::Interest) {
        shared_ptr<Interest> interest = make_shared<Interest>(block);
        if (lpPacket.has<lp::NackField>()) {
          shared_ptr<lp::Nack> nack = make_shared<lp::Nack>(std::move(*interest));
          nack->setHeader(lpPacket.get<lp::NackField>());
          if (lpPacket.has<lp::NextHopFaceIdField>()) {
            nack->getLocalControlHeader().setNextHopFaceId(lpPacket.get<lp::NextHopFaceIdField>());
          }
          onSendNack(*nack);
        }
        else {
          if (lpPacket.has<lp::NextHopFaceIdField>()) {
            interest->getLocalControlHeader().
              setNextHopFaceId(lpPacket.get<lp::NextHopFaceIdField>());
          }
          onSendInterest(*interest);
        }
      }
      else if (block.type() == tlv::Data) {
        shared_ptr<Data> data = make_shared<Data>(block);

        if (lpPacket.has<lp::CachePolicyField>()) {
          if (lpPacket.get<lp::CachePolicyField>().getPolicy() == lp::CachePolicyType::NO_CACHE) {
            data->getLocalControlHeader().setCachingPolicy(nfd::LocalControlHeader::CachingPolicy::NO_CACHE);
          }
        }

        onSendData(*data);
      }
    }
    catch (tlv::Error& e) {
      throw tlv::Error("Error decoding NDNLPv2 packet");
    }
  });

  if (options.enablePacketLogging)
    this->enablePacketLogging();

  if (options.enableRegistrationReply)
    this->enableRegistrationReply();
}

void
DummyClientFace::enablePacketLogging()
{
  onSendInterest.connect([this] (const Interest& interest) {
    this->sentInterests.push_back(interest);
  });
  onSendData.connect([this] (const Data& data) {
    this->sentDatas.push_back(data);
  });
  onSendNack.connect([this] (const lp::Nack& nack) {
    this->sentNacks.push_back(nack);
  });
}

void
DummyClientFace::enableRegistrationReply()
{
  onSendInterest.connect([this] (const Interest& interest) {
    static const Name localhostRegistration("/localhost/nfd/rib");
    if (!localhostRegistration.isPrefixOf(interest.getName()))
      return;

    nfd::ControlParameters params(interest.getName().get(-5).blockFromValue());
    params.setFaceId(1);
    params.setOrigin(0);
    if (interest.getName().get(3) == name::Component("register")) {
      params.setCost(0);
    }

    nfd::ControlResponse resp;
    resp.setCode(200);
    resp.setBody(params.wireEncode());

    shared_ptr<Data> data = make_shared<Data>(interest.getName());
    data->setContent(resp.wireEncode());

    KeyChain keyChain;
    keyChain.sign(*data, security::SigningInfo(security::SigningInfo::SIGNER_TYPE_SHA256));

    this->getIoService().post([this, data] { this->receive(*data); });
  });
}

template<typename Packet>
void
DummyClientFace::receive(const Packet& packet)
{
  lp::Packet lpPacket(packet.wireEncode());

  nfd::LocalControlHeader localControlHeader = packet.getLocalControlHeader();

  if (localControlHeader.hasIncomingFaceId()) {
    lpPacket.add<lp::IncomingFaceIdField>(localControlHeader.getIncomingFaceId());
  }

  if (localControlHeader.hasNextHopFaceId()) {
    lpPacket.add<lp::NextHopFaceIdField>(localControlHeader.getNextHopFaceId());
  }

  m_transport->receive(lpPacket.wireEncode());
}

template void
DummyClientFace::receive<Interest>(const Interest& packet);

template void
DummyClientFace::receive<Data>(const Data& packet);

template<>
void
DummyClientFace::receive<lp::Nack>(const lp::Nack& nack)
{
  lp::Packet lpPacket;
  lpPacket.add<lp::NackField>(nack.getHeader());
  Block interest = nack.getInterest().wireEncode();
  lpPacket.add<lp::FragmentField>(make_pair(interest.begin(), interest.end()));

  nfd::LocalControlHeader localControlHeader = nack.getLocalControlHeader();

  if (localControlHeader.hasIncomingFaceId()) {
    lpPacket.add<lp::IncomingFaceIdField>(localControlHeader.getIncomingFaceId());
  }

  m_transport->receive(lpPacket.wireEncode());
}

shared_ptr<DummyClientFace>
makeDummyClientFace(const DummyClientFace::Options& options)
{
  // cannot use make_shared<DummyClientFace> because DummyClientFace constructor is private
  return shared_ptr<DummyClientFace>(
         new DummyClientFace(options, make_shared<DummyClientFace::Transport>()));
}

shared_ptr<DummyClientFace>
makeDummyClientFace(boost::asio::io_service& ioService,
                    const DummyClientFace::Options& options)
{
  // cannot use make_shared<DummyClientFace> because DummyClientFace constructor is private
  return shared_ptr<DummyClientFace>(
         new DummyClientFace(options, make_shared<DummyClientFace::Transport>(),
                             ref(ioService)));
}

} // namespace util
} // namespace ndn