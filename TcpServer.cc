// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include <muduo/net/TcpServer.h>

#include <muduo/base/Logging.h>
#include <muduo/net/Acceptor.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/EventLoopThreadPool.h>
#include <muduo/net/SocketsOps.h>

#include <boost/bind.hpp>

#include <stdio.h>  // snprintf

using namespace muduo;
using namespace muduo::net;

TcpServer::TcpServer(EventLoop* loop,
                     const InetAddress& listenAddr,
                     const string& nameArg,
                     Option option)
  : loop_(CHECK_NOTNULL(loop)),
    /* 由InetAddress拿到ip和port */
    ipPort_(listenAddr.toIpPort()),
    /* server 的 name */
    name_(nameArg),   
    /* 用传入的listenAddr构造Acceptor */
    acceptor_(new Acceptor(loop, listenAddr, option == kReusePort)),

    threadPool_(new EventLoopThreadPool(loop, name_)),
    /* 用默认的处理连接和消息的回调函数 初始化 */
    connectionCallback_(defaultConnectionCallback),
    messageCallback_(defaultMessageCallback),
    /* id 从1 开始 */
    nextConnId_(1)
{
    /* 将newConnection传给acceptor_,acceptor_执行完accept后会调用这个函数 */
  acceptor_->setNewConnectionCallback(
      boost::bind(&TcpServer::newConnection, this, _1, _2));
}

TcpServer::~TcpServer()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "TcpServer::~TcpServer [" << name_ << "] destructing";

  for (ConnectionMap::iterator it(connections_.begin());
      it != connections_.end(); ++it)
  {
    TcpConnectionPtr conn(it->second);
    it->second.reset();
    conn->getLoop()->runInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));
  }
}

/* 设置线程数目，这一步就可以决定采用的是多线程还是单线程 */
void TcpServer::setThreadNum(int numThreads)
{
  assert(0 <= numThreads);
  threadPool_->setThreadNum(numThreads);
}

/*
 * TcpServer 的启动流程
 * 1. 启动线程池的线程
 * 2. 开始listen
 * 3. 注册listenfd的读事件
 */
void TcpServer::start()
{
  if (started_.getAndSet(1) == 0)
  {
      /* 启动一个IO线程 */
    threadPool_->start(threadInitCallback_);

    /* 断言没有在监听 */
    assert(!acceptor_->listenning());
    /* 开始listen */
    loop_->runInLoop(
        boost::bind(&Acceptor::listen, get_pointer(acceptor_)));
  }
}

/* 
 * Acceptor接受连接后 调用这个回调函数 
 * 为<connfd,peerAddr> 创建一个TcpConnection对象conn来管理该连接
 * 把它加入 ConnectionMap
 * 设置好 callback
 * 再调用conn->connectEstablished()
 * 注册connfd的可读事件并回调用户提供的ConnectionCallback
 */
void TcpServer::newConnection(int sockfd, const InetAddress& peerAddr)
{
  loop_->assertInLoopThread();
  /* 从线程池取一个loop 线程 */
  EventLoop* ioLoop = threadPool_->getNextLoop();
  char buf[64];
  /* 构造tcp连接的名称 每个TcpConnection 对象有一个名字 */
  snprintf(buf, sizeof buf, "-%s#%d", ipPort_.c_str(), nextConnId_);
  /* connid++ */
  ++nextConnId_;
  /*连接名字格式:servername + server.ip+server.port + connid */
  string connName = name_ + buf;

  LOG_INFO << "TcpServer::newConnection [" << name_
           << "] - new connection [" << connName
           << "] from " << peerAddr.toIpPort();
  InetAddress localAddr(sockets::getLocalAddr(sockfd));
  // FIXME poll with zero timeout to double confirm the new connection
  // FIXME use make_shared if necessary
  /* 新建TcpConnection 对象 conn */
  TcpConnectionPtr conn(new TcpConnection(ioLoop,
                                          connName,
                                          sockfd,
                                          localAddr,
                                          peerAddr));
  
  /* 
   * 把它加入 ConnectionMap 
   * key 是连接的name，value 为指向这个对象的 shared_ptr 
   */
  connections_[connName] = conn;

  /* 设置好 callback */
  /* TcpConnection 建立时调用 */
  conn->setConnectionCallback(connectionCallback_);
  /* 消息到来时调用 */
  conn->setMessageCallback(messageCallback_);
  /* 成功将所有数据写入对方内核缓冲区时调用 */
  conn->setWriteCompleteCallback(writeCompleteCallback_);
  
  /* TCP连接关闭时的回调函数，内部使用，不能用户指定  */
  conn->setCloseCallback(
      boost::bind(&TcpServer::removeConnection, this, _1)); // FIXME: unsafe
  /* 
   * 在loop线程中执行建立tcp连接的流程
   * 主要是设置tcp状态，注册读事件
   * 以及执行tcp 建立的回调函数connectionCallback_()
   */
  ioLoop->runInLoop(boost::bind(&TcpConnection::connectEstablished, conn));
}

/* 把 conn 从 ConnectionMap 中移除 */
void TcpServer::removeConnection(const TcpConnectionPtr& conn)
{
  // FIXME: unsafe
  // 
  loop_->runInLoop(boost::bind(&TcpServer::removeConnectionInLoop, this, conn));
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn)
{
  loop_->assertInLoopThread();
  LOG_INFO << "TcpServer::removeConnectionInLoop [" << name_
           << "] - connection " << conn->name();
  /* 从 TcpServer 中删除这个 TcpConnection */
  size_t n = connections_.erase(conn->name());
  (void)n;
  assert(n == 1);
  EventLoop* ioLoop = conn->getLoop();
  /* 在 ioLoop 中执行 TcpConnection::connectDestroyed() */
  ioLoop->queueInLoop(
      boost::bind(&TcpConnection::connectDestroyed, conn));
}

