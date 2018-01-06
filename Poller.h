// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_POLLER_H
#define MUDUO_NET_POLLER_H

#include <map>
#include <vector>
#include <boost/noncopyable.hpp>

#include <muduo/base/Timestamp.h>
#include <muduo/net/EventLoop.h>

namespace muduo
{
namespace net
{

class Channel;

///
/// Base class for IO Multiplexing
///
/// This class doesn't own the Channel objects.
class Poller : boost::noncopyable //不拥有Channel
{
 public:
  typedef std::vector<Channel*> ChannelList;
  /* 用于返回就绪事件集合 */
  Poller(EventLoop* loop);
  virtual ~Poller();

  /// Polls the I/O events.
  /// Must be called in the loop thread.
  /* Poller的核心功能，将就绪事件加入到 activeChannels 中 */
  virtual Timestamp poll(int timeoutMs, ChannelList* activeChannels) = 0;

  /// Changes the interested I/O events.
  /// Must be called in the loop thread.
  /* 更新 fd 的监听事件
   * Channel::update()->EventLoop::updateChannel(Channel* channel)->Poller::updateChannel(Channel* channel)
   */
  virtual void updateChannel(Channel* channel) = 0;

  /// Remove the channel, when it destructs.
  /// Must be called in the loop thread.
  /* 从poll/epoll 中移除fd 停止监听此fd 
   * EventLoop::removeChannel(Channel*)->Poller::removeChannel(Channel*)
   */
  virtual void removeChannel(Channel* channel) = 0;
  /* 判断该poll//epoll 模型是否监听了Channel对应的fd */
  virtual bool hasChannel(Channel* channel) const;

  /* */
  static Poller* newDefaultPoller(EventLoop* loop);

  /* 断言 确保没有跨线程 */
  void assertInLoopThread() const
  {
    ownerLoop_->assertInLoopThread();
  }

 protected:
  /*
   * 记录fd到Channel的对应关系 
   * 底层的epoll每次监听完fd，要根据这个映射关系去寻找对应的Channel
   */
  typedef std::map<int, Channel*> ChannelMap;
  ChannelMap channels_;//保存epoll监听的fd,及其对应的Channel指针

 private:
  /* 这个Poller对象所属的 EventLoop */
  EventLoop* ownerLoop_;
};

}
}
#endif  // MUDUO_NET_POLLER_H
