// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_SOCKET_H
#define MUDUO_NET_SOCKET_H

#include <boost/noncopyable.hpp>

// struct tcp_info is in <netinet/tcp.h>
struct tcp_info;

namespace muduo
{
///
/// TCP networking.
///
namespace net
{

class InetAddress;

/// It closes the sockfd when desctructs.
/* sock 描述符 fd的封装，实现了 绑定了地址，开始监听 并接受连接 */
class Socket : boost::noncopyable
{
 public:
  explicit Socket(int sockfd)
    : sockfd_(sockfd)
  { }

  /* close(sockfd_) */
  ~Socket();

  /* 返回 这个 socket 的 fd*/
  int fd() const { return sockfd_; }

  /* 获取与这个套接字相关联的选项，成功返回 true */
  bool getTcpInfo(struct tcp_info*) const;
  bool getTcpInfoString(char* buf, int len) const;

  /* 调用bind，绑定sockaddr */
  void bindAddress(const InetAddress& localaddr);
  /* 调用listen ，开始监听*/
  void listen();

  /// On success, returns a non-negative integer that is
  /// a descriptor for the accepted socket, which has been
  /// set to non-blocking and close-on-exec. *peeraddr is assigned.
  /// On error, -1 is returned, and *peeraddr is untouched.
  /* 调用accept，成功返回non-blocking和close-on-exec属性的 connfd */
  int accept(InetAddress* peeraddr);

  /* 关闭"写"方向的连接 */
  void shutdownWrite();

  
  /* 是否使用 Nagle 算法 */
  void setTcpNoDelay(bool on);

  /* 是否重用本地地址 */
  void setReuseAddr(bool on);

  /* 是否重用本地端口 */
  void setReusePort(bool on);

  /* 是否定期检测连接数否存在 */
  void setKeepAlive(bool on);

 private:
  const int sockfd_; // sockket fd
};

}
}
#endif  // MUDUO_NET_SOCKET_H
