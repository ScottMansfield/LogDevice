/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <string>
#include <unordered_set>

#include <arpa/inet.h>
#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <folly/hash/Hash.h>
#include <netinet/in.h>

namespace facebook { namespace logdevice {

/**
 * @file A tiny wrapper around folly::SocketAddress which encapsulates an IPv4
 *       or IPv6 or unix domain socket And adds a few convenience methods.
 */

class Sockaddr {
 public:
  Sockaddr(){}; // creates an invalid sockaddr object

  explicit Sockaddr(folly::SocketAddress&& sa) : addr_(std::move(sa)) {}
  explicit Sockaddr(const folly::SocketAddress& sa) : addr_(sa) {}

  /**
   * Creates a new Sockaddr representing the host:port address.
   *
   * @param ip    an ipv4 ("127.0.0.1") or ipv6 ("[::1]") address
   * @param port  a numeric string containing a port number
   *
   * @throws ConstructorFailed if ip or port have invalid format (sets
   *         err to INVALID_PARAM), or if @param ip is unroutable
   *         (sets err to INVALID_IP) or if we encountered any other error (sets
   *         err to FAILED).
   */
  Sockaddr(const std::string& ip, in_port_t port);
  Sockaddr(const std::string& ip, const std::string& port);

  /**
   * Creates a new Sockaddr representing a local unix path.
   * @param path Path of the UNIX domain socket.
   * @throws ConstructorFailed if path has invalid format (sets
   *         err to INVALID_PARAM) or if we encountered any other error (sets
   *         err to FAILED).
   */
  explicit Sockaddr(const std::string& path);

  /**
   * Create a Sockaddr object from a struct sockaddr.
   * This method is not supported for AF_UNIX addresses.  For unix addresses,
   * the address length must be explicitly specified.
   */
  explicit Sockaddr(const struct sockaddr* sa);

  /**
   * Create a Sockaddr object from a struct sockaddr.
   * @param address  A struct sockaddr.
   * @param addrlen  The length of address data available.  This must be long
   *                 enough for the full address type required by
   *                 address->sa_family.
   */
  Sockaddr(const struct sockaddr* sa, int len);

  /**
   * @return true if this Sockaddr object is valid.
   */
  bool valid() const {
    return !addr_.empty();
  }

  /**
   * @return Address family of this Sockaddr object.
   */
  sa_family_t family() const {
    return addr_.getFamily();
  }

  /**
   * @return true if this Sockaddr object contains the path of a unix domain
   * socket.
   */
  bool isUnixAddress() const {
    return family() == AF_UNIX;
  }

  /**
   * Asserts if this is not an IPv4 or IPv6 address.
   *
   * @return the port component of this address, in host byte order
   */
  in_port_t port() const;

  /**
   * Asserts if this is not an IPv4 or IPv6 address.
   *
   * @param port in host byte order
   */
  void setPort(in_port_t port);

  /**
   * Converts to a struct sockaddr.
   * Asserts if this Sockaddr if not valid.
   *
   * @return length of the returned struct or -1 on failure
   */
  int toStructSockaddr(struct sockaddr_storage* sockaddr_out) const;

  /**
   * Asserts if this is not an IPv4 or IPv6 address.
   *
   * Returns a copy of the IPAddress instance.
   */
  folly::IPAddress getAddress() const;

  /**
   * Asserts if this is not an IPv4 or IPv6 address.
   *
   * Get the path name for a Unix domain socket.
   */
  std::string getPath() const;

  /**
   * @return a human-readable representation of this address.
   */
  std::string toString() const {
    return toStringImpl(true);
  }

  /**
   * Similar to toString() but omits the brackets for IPv6 addresses.
   */
  std::string toStringNoBrackets() const {
    return toStringImpl(false);
  }

  /**
   * Similar to toStringNoBrackets() but omits the port number
   */
  std::string toStringNoPort() const {
    return toStringImpl(false, false);
  }

  /**
   * Asserts if this is not an IPv4 or IPv6 address.
   *
   * @returns a copy of the Sockaddr with a different port.
   */
  Sockaddr withPort(in_port_t new_port) const;

  friend bool operator==(const Sockaddr& a1, const Sockaddr& a2);
  friend bool operator!=(const Sockaddr& a1, const Sockaddr& a2);

  static Sockaddr INVALID;

  struct Hash {
    size_t operator()(const Sockaddr& addr) const {
      return addr.addr_.hash();
    }
  };

  folly::SocketAddress getSocketAddress() const {
    return addr_;
  }
  folly::IPAddress getIPAddress() const {
    return addr_.getIPAddress();
  }

 private:
  folly::SocketAddress addr_;

  /**
   * @return a human-readable representation of this address
   * @param with_brackets If set to true, this will wrap an IPv6 address between
   *        brackets.
   * @param with_port If set to true, will include the port number if relevant
   */
  std::string toStringImpl(bool with_brackets, bool with_port = true) const;
};

struct SockaddrSet {
  std::unordered_set<Sockaddr, Sockaddr::Hash> elements;
  bool anonymous_unix_socket_present{false};

  // Returns `true` if:
  // - the supplied socket address is present in the set
  // - the supplied host with port 0 is present in the set (for addresses of
  //   AF_INET and AF_INET6 types)
  // - the supplied Sockaddr refers to a unix socket and a Sockaddr referring to
  //   an anonymous unix socket is present in the set. A separate check is
  //   needed for this because operator== always returns `false` for two
  //   folly::SocketAddress instances that refer to anonymous unix sockets
  bool isPresent(const Sockaddr& address) const {
    if (address.isUnixAddress() && anonymous_unix_socket_present) {
      return true;
    }
    if (elements.count(address) > 0) {
      return true;
    }
    // Peer address with port 0 == any port
    if (address.family() == AF_INET || address.family() == AF_INET6) {
      Sockaddr p0_addr = address.withPort(0);
      if (elements.count(p0_addr) != 0) {
        // passed the host filter with port 0
        return true;
      }
    }
    return false;
  }

  bool empty() const {
    return elements.empty() && !anonymous_unix_socket_present;
  }
};

}} // namespace facebook::logdevice
