// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_TCPSERVER_H
#define MUDUO_NET_TCPSERVER_H

#include <muduo/base/Atomic.h>
#include <muduo/base/Types.h>
#include <muduo/net/TcpConnection.h>

#include <map>
#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/shared_ptr.hpp>

namespace muduo
{
namespace net
{

class Acceptor;
class EventLoop;
class EventLoopThreadPool;

///
/// TCP server, supports single-threaded and thread-pool models.
///
/// This is an interface class, so don't expose too much details.
/* */
class TcpServer : boost::noncopyable
{
 public:
  typedef boost::function<void(EventLoop*)> ThreadInitCallback;
  enum Option
  {
    kNoReusePort,
    kReusePort,
  };

  //TcpServer(EventLoop* loop, const InetAddress& listenAddr);
  /* 构造时接受一个ip和port组成的InetAddress参数，用来构造 Acceptor(listenfd) */
  TcpServer(EventLoop* loop,
            const InetAddress& listenAddr,
            const string& nameArg,
            Option option = kNoReusePort);
  ~TcpServer();  // force out-line dtor, for scoped_ptr members.

  const string& ipPort() const { return ipPort_; }
  const string& name() const { return name_; }
  EventLoop* getLoop() const { return loop_; }

  /// Set the number of threads for handling input.
  ///
  /// Always accepts new connection in loop's thread.
  /// Must be called before @c start
  /// @param numThreads
  /// - 0 means all I/O in loop's thread, no thread will created.
  ///   this is the default value.
  /// - 1 means all I/O in another thread.
  /// - N means a thread pool with N threads, new connections
  ///   are assigned on a round-robin basis.
  /* 设置线程数目 */
  void setThreadNum(int numThreads);
  void setThreadInitCallback(const ThreadInitCallback& cb)
  { threadInitCallback_ = cb; }
  /// valid after calling start()
  boost::shared_ptr<EventLoopThreadPool> threadPool()
  { return threadPool_; }

  /// Starts the server if it's not listenning.
  ///
  /// It's harmless to call it multiple times.
  /// Thread safe.
  void start();

  /// Set connection callback.
  /// Not thread safe.
  void setConnectionCallback(const ConnectionCallback& cb)
  { connectionCallback_ = cb; }

  /// Set message callback.
  /// Not thread safe.
  void setMessageCallback(const MessageCallback& cb)
  { messageCallback_ = cb; }

  /// Set write complete callback.
  /// Not thread safe.
  void setWriteCompleteCallback(const WriteCompleteCallback& cb)
  { writeCompleteCallback_ = cb; }

 private:
  /// Not thread safe, but in loop
  /* 新连接到达后，Acceptor会回调 newConnection */
  void newConnection(int sockfd, const InetAddress& peerAddr);
  /// Thread safe.
  void removeConnection(const TcpConnectionPtr& conn);
  /// Not thread safe, but in loop
  void removeConnectionInLoop(const TcpConnectionPtr& conn);

  /* TcpConnection对象的名字到指向它的share_ptr，TcpServer用map来管理所有的连接 */
  typedef std::map<string, TcpConnectionPtr> ConnectionMap;

  /* 负责接受tcp连接的EventLoop,如果threadnum为1,那么它是唯一的IO线程 */
  EventLoop* loop_;     // the acceptor loop
  const string ipPort_; //ip port
  const string name_;   //server 的名字
  /* 内部通过 Acceptor 负责 listenfd 的建立和 accept 连接 */
  boost::scoped_ptr<Acceptor> acceptor_;    // avoid revealing Acceptor 避免暴露给用户
  boost::shared_ptr<EventLoopThreadPool> threadPool_;//线程池，每个线程运行一个EventLoop

  ConnectionCallback connectionCallback_;   //连接建立和关闭时的callback
  MessageCallback messageCallback_;         //消息到来时的callback
  WriteCompleteCallback writeCompleteCallback_; //消息写入对方缓冲区时的callback
  ThreadInitCallback threadInitCallback_;   //EventLoop线程初始化时的回调函数
  
  AtomicInt32 started_;
  // always in loop thread
  int nextConnId_;              //下一个连接的id，用于给tcp连接构造名字
  ConnectionMap connections_;   //使用这个map管理所有的连接 
};

}
}

#endif  // MUDUO_NET_TCPSERVER_H
