//
// Created by consti10 on 21.04.22.
//

#ifndef WIFIBROADCAST_SOCKETHELPER_HPP
#define WIFIBROADCAST_SOCKETHELPER_HPP

#include "Helper.hpp"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <resolv.h>
#include <cstring>
#include <utime.h>
#include <unistd.h>
#include <getopt.h>
#include <endian.h>
#include <fcntl.h>
#include <ctime>
#include <sys/mman.h>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <cstdarg>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <netpacket/packet.h>
#include <termio.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <iostream>
#include <memory>
#include <cassert>
#include <functional>
#include <thread>
#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>

namespace SocketHelper {
struct UDPConfig{
  const std::string ip;
  const int port;
};
static const std::string ADDRESS_LOCALHOST = "127.0.0.1";
static const std::string ADDRESS_NULL="0.0.0.0";
// returns the current socket receive timeout
static std::chrono::nanoseconds getCurrentSocketReceiveTimeout(int socketFd) {
  timeval tv{};
  socklen_t len = sizeof(tv);
  auto res = getsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &tv, &len);
  assert(res == 0);
  assert(len == sizeof(tv));
  return GenericHelper::timevalToDuration(tv);
}
// set the receive timeout on the socket
// throws runtime exception if this step fails (should never fail on linux)
static void setSocketReceiveTimeout(int socketFd, const std::chrono::nanoseconds timeout) {
  const auto currentTimeout = getCurrentSocketReceiveTimeout(socketFd);
  if (currentTimeout != timeout) {
    //std::cout<<"Changing timeout\n";
    auto tv = GenericHelper::durationToTimeval(timeout);
    if (setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
      throw std::runtime_error(StringFormat::convert("Cannot set socket timeout %d", timeout.count()));
      //std::cout<<"Cannot set socket timeout "<<timeout.count()<<"\n";
    }
  }
}
// Set the reuse flag on the socket, so it doesn't care if there is a broken down process
// still on the socket or not.
static void setSocketReuse(int sockfd) {
  int enable = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
    //throw std::runtime_error(StringFormat::convert("Error setting reuse on socket %d: %s",port, strerror(errno)));
    // don't crash here
    std::cerr << "Cannot set socket reuse\n";
  }
}
// increase the receive size, needed for high bandwidth
static void increaseSocketRecvBuffer(int sockfd, const int wantedSize) {
  int recvBufferSize = 0;
  socklen_t len = sizeof(recvBufferSize);
  getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &recvBufferSize, &len);
  std::cout << "Default socket recv buffer is " << StringHelper::memorySizeReadable(recvBufferSize);
  if (wantedSize > recvBufferSize) {
    recvBufferSize = wantedSize;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &recvBufferSize, len)) {
      std::cout << "Cannot increase buffer size to " << StringHelper::memorySizeReadable(wantedSize);
    }
    getsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &recvBufferSize, &len);
    std::cout << "Wanted " << StringHelper::memorySizeReadable(wantedSize) << " Set "
              << StringHelper::memorySizeReadable(recvBufferSize);
  }
}
// Open the specified port for udp receiving
// sets SO_REUSEADDR to true if possible
// throws a runtime exception if opening the socket fails
static int openUdpSocketForReceiving(const std::string& address,const int port) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) throw std::runtime_error(StringFormat::convert("Error opening socket %d: %s", port, strerror(errno)));
  setSocketReuse(fd);
  struct sockaddr_in saddr{};
  bzero((char *) &saddr, sizeof(saddr));
  saddr.sin_family = AF_INET;
  //saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  inet_aton(address.c_str(), (in_addr *) &saddr.sin_addr.s_addr);
  saddr.sin_port = htons((unsigned short) port);
  if (bind(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
    throw std::runtime_error(StringFormat::convert("Bind error on socket %s:%d: %s",address.c_str(), port, strerror(errno)));
  }
  return fd;
}
// Wrapper around an UDP port you can send data to
// opens port on construction, closes port on destruction
class UDPForwarder {
 public:
  explicit UDPForwarder(std::string client_addr, int client_udp_port) :
      client_addr(std::move(client_addr)), client_udp_port(client_udp_port) {
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
      std::stringstream message;
      message << "Error opening socket:" << strerror(errno) << "\n";
      std::cerr << message.str();
      throw std::runtime_error(message.str());
    }
    //set up the destination
    bzero((char *) &saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    //saddr.sin_addr.s_addr = inet_addr(client_addr.c_str());
    inet_aton(client_addr.c_str(), (in_addr *) &saddr.sin_addr.s_addr);
    saddr.sin_port = htons((unsigned short) client_udp_port);
    std::cout << "UDPForwarder::configured for " << client_addr << " " << client_udp_port << "\n";
	//

  }
  UDPForwarder(const UDPForwarder &) = delete;
  UDPForwarder &operator=(const UDPForwarder &) = delete;
  ~UDPForwarder() {
    close(sockfd);
  }
  void forwardPacketViaUDP(const uint8_t *packet, const std::size_t packetSize) const {
    //std::cout<<"Send"<<packetSize<<"\n";
    //send(sockfd,packet,packetSize, MSG_DONTWAIT);
    const auto ret=sendto(sockfd, packet, packetSize, 0, (const struct sockaddr *) &saddr,
           sizeof(saddr));
	if(ret <0 || ret != packetSize){
	  std::cout<<"Error sending packet of size:"<<packetSize<<" to port:"<<saddr.sin_port<<" code:"<<ret<<"\n";
	}
  }
  void lula(std::string address,int port){
	setSocketReuse(sockfd);
	struct sockaddr_in saddr{};
	bzero((char *) &saddr, sizeof(saddr));
	saddr.sin_family = AF_INET;
	//saddr.sin_addr.s_addr = htonl(INADDR_ANY);
	inet_aton(address.c_str(), (in_addr *) &saddr.sin_addr.s_addr);
	saddr.sin_port = htons((unsigned short) port);
	if (bind(sockfd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
	  throw std::runtime_error(StringFormat::convert("Bind error on socket %s:%d: %s",address.c_str(), port, strerror(errno)));
	}
  }
 private:
  struct sockaddr_in saddr{};
  int sockfd;
 public:
  const std::string client_addr;
  const int client_udp_port;
};

/**
 * Similar to UDP forwarder, but allows forwarding the same data to 0 or more IP::Port tuples
 */
class UDPMultiForwarder {
 public:
  /**
  * Start forwarding data to another IP::Port tuple
  */
  void addForwarder(const std::string &client_addr, int client_udp_port) {
    std::lock_guard<std::mutex> guard(udpForwardersLock);
    // check if we already forward data to this IP::Port tuple
    for (const auto &udpForwarder: udpForwarders) {
      if (udpForwarder->client_addr == client_addr && udpForwarder->client_udp_port == client_udp_port) {
        std::cout << "UDPMultiForwarder: already forwarding to:" << client_addr << ":" << client_udp_port << "\n";
        return;
      }
    }
    std::cout << "UDPMultiForwarder: add forwarding to:" << client_addr << ":" << client_udp_port << "\n";
    udpForwarders.emplace_back(std::make_unique<SocketHelper::UDPForwarder>(client_addr, client_udp_port));
  }
  /**
  * Remove an already existing udp forwarding instance.
  * Do nothing if such an instance is not found.
  */
  void removeForwarder(const std::string &client_addr, int client_udp_port) {
    std::lock_guard<std::mutex> guard(udpForwardersLock);
    udpForwarders.erase(std::find_if(udpForwarders.begin(),
                                     udpForwarders.end(),
                                     [&client_addr, &client_udp_port](const auto &udpForwarder) {
                                       return udpForwarder->client_addr == client_addr
                                           && udpForwarder->client_udp_port == client_udp_port;
                                     }));
  }
  /**
   * Forward data to all added IP::Port tuples via UDP
   */
  void forwardPacketViaUDP(const uint8_t *packet, const std::size_t packetSize) {
    std::lock_guard<std::mutex> guard(udpForwardersLock);
    for (const auto &udpForwarder: udpForwarders) {
      udpForwarder->forwardPacketViaUDP(packet, packetSize);
    }
  }
  [[nodiscard]] const std::list<std::unique_ptr<SocketHelper::UDPForwarder>> &getForwarders() const {
    return udpForwarders;
  }
 private:
  // list of host::port tuples where we send the data to.
  std::list<std::unique_ptr<SocketHelper::UDPForwarder>> udpForwarders;
  // modifying the list of forwarders must be thread-safe
  std::mutex udpForwardersLock;
};

class UDPReceiver {
 public:
  typedef std::function<void(const uint8_t *payload, const std::size_t payloadSize)> OUTPUT_DATA_CALLBACK;
  static constexpr const size_t UDP_PACKET_MAX_SIZE = 65507;
  /**
   * Receive data from socket and forward it via callback until stopLooping() is called
   */
  explicit UDPReceiver(std::string client_addr, int client_udp_port, OUTPUT_DATA_CALLBACK cb) : mCb(std::move(cb)) {
    mSocket = SocketHelper::openUdpSocketForReceiving(client_addr,client_udp_port);
    //increaseSocketRecvBuffer(mSocket,1024*1024);
    std::cout << "UDPReceiver created with " << client_addr << ":" << client_udp_port << "\n";
  }
  void loopUntilError() {
    const auto buff = std::make_unique<std::array<uint8_t, UDP_PACKET_MAX_SIZE>>();
    //sockaddr_in source;
    //socklen_t sourceLen= sizeof(sockaddr_in);
    while (receiving) {
      //const ssize_t message_length = recvfrom(mSocket,buff->data(),UDP_PACKET_MAX_SIZE, MSG_WAITALL,(sockaddr*)&source,&sourceLen);
      const ssize_t message_length = recv(mSocket, buff->data(), buff->size(), MSG_WAITALL);
      if (message_length > 0) {
        mCb(buff->data(), (size_t) message_length);
      } else {
        // this can also come from the shutdown, in which case it is not an error.
        // But this way we break out of the loop.
        std::cout << "ERROR got message length of:" << message_length << "\n";
        receiving = false;
      }
    }
    std::cout << "UDP end\n";
  }
  // Now this one is kinda special - for mavsdk we need to send messages from the port we are listening on
  // to a specific IP::PORT tuple (such that the source address of the then received packet matches the address we are listening on).
  void forwardPacketViaUDP(const std::string& destIp,const int destPort,const uint8_t *packet, const std::size_t packetSize) const {
	//set up the destination
	struct sockaddr_in saddr{};
    bzero((char *) &saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    //saddr.sin_addr.s_addr = inet_addr(client_addr.c_str());
    inet_aton(destIp.c_str(), (in_addr *) &saddr.sin_addr.s_addr);
    saddr.sin_port = htons((unsigned short)  destPort);
    const auto ret=sendto(mSocket, packet, packetSize, 0, (const struct sockaddr *) &saddr,
           sizeof(saddr));
	if(ret <0 || ret != packetSize){
	  std::cout<<"Error sending packet of size:"<<packetSize<<" to port:"<<saddr.sin_port<<" code:"<<ret<<"\n";
	}
  }
  void stopLooping() {
    receiving = false;
    // from https://github.com/mavlink/MAVSDK/blob/main/src/mavsdk/core/udp_connection.cpp#L102
    shutdown(mSocket, SHUT_RDWR);
    close(mSocket);
  }
  void runInBackground() {
    if (receiverThread) {
      std::cerr << "Receiver thread is already running or has not been properly stopped\n";
      return;
    }
    receiverThread = std::make_unique<std::thread>(&UDPReceiver::loopUntilError, this);
  }
  void stopBackground() {
    stopLooping();
    if (receiverThread && receiverThread->joinable()) {
      receiverThread->join();
    }
    receiverThread = nullptr;
  }
 private:
  const OUTPUT_DATA_CALLBACK mCb;
  bool receiving = true;
  int mSocket;
  std::unique_ptr<std::thread> receiverThread = nullptr;
};
}

#endif //WIFIBROADCAST_SOCKETHELPER_HPP