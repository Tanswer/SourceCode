// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_ACCEPTOR_H
#define MUDUO_NET_ACCEPTOR_H

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>

#include <muduo/net/Channel.h>
#include <muduo/net/Socket.h>

namespace muduo
{
namespace net
{

class EventLoop;
class InetAddress;

///
/// Acceptor of incoming TCP connections.
///
// accept一个TCP连接
class Acceptor : boost::noncopyable
{
 public:
     /* 新建立连接之后的回调 */
  typedef boost::function<void (int sockfd,
                                const InetAddress&)> NewConnectionCallback;

  Acceptor(EventLoop* loop, const InetAddress& listenAddr, bool reuseport);
  ~Acceptor();

  /* 设置用户任务回调 */
  void setNewConnectionCallback(const NewConnectionCallback& cb)
  { newConnectionCallback_ = cb; }

  bool listenning() const { return listenning_; }
  void listen();            // 开始监听  

 private:
  void handleRead();        // listenfd -> Channel上的可读事件回调 

  EventLoop* loop_;         // 所在的 EventLoop
  Socket acceptSocket_;     // listenfd 
  Channel acceptChannel_;   // listenfd 对应的 Channel
  NewConnectionCallback newConnectionCallback_;     // 处理新连接的回调函数,accept 后调用
  bool listenning_;         // 是否正在 listen 
  int idleFd_;              //占位fd，用于fd满的情况
};

}
}

#endif  // MUDUO_NET_ACCEPTOR_H
