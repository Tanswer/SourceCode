// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_CHANNEL_H
#define MUDUO_NET_CHANNEL_H

#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>

#include <muduo/base/Timestamp.h>

namespace muduo
{
namespace net
{

class EventLoop;

///
/// A selectable I/O channel.
///
/// This class doesn't own the file descriptor.
/// The file descriptor could be a socket,
/// an eventfd, a timerfd, or a signalfd
class Channel : boost::noncopyable
{
 public:
  /* 事件回调函数 */
  typedef boost::function<void()> EventCallback;
  /* 读操作回调函数，需要传入时间 */
  typedef boost::function<void(Timestamp)> ReadEventCallback;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  /* 处理事件，一般由Poller通过EventLoop来调用 */
  void handleEvent(Timestamp receiveTime);
  /* 设置四种回调函数 */
  void setReadCallback(const ReadEventCallback& cb)
  { readCallback_ = cb; }
  void setWriteCallback(const EventCallback& cb)
  { writeCallback_ = cb; }
  void setCloseCallback(const EventCallback& cb)
  { closeCallback_ = cb; }
  void setErrorCallback(const EventCallback& cb)
  { errorCallback_ = cb; }
#ifdef __GXX_EXPERIMENTAL_CXX0X__
  /* C++11版本 右值语义 */
  void setReadCallback(ReadEventCallback&& cb)
  { readCallback_ = std::move(cb); }
  void setWriteCallback(EventCallback&& cb)
  { writeCallback_ = std::move(cb); }
  void setCloseCallback(EventCallback&& cb)
  { closeCallback_ = std::move(cb); }
  void setErrorCallback(EventCallback&& cb)
  { errorCallback_ = std::move(cb); }
#endif

  /// Tie this channel to the owner object managed by shared_ptr,
  /// prevent the owner object being destroyed in handleEvent.
  void tie(const boost::shared_ptr<void>&);

  /* 返回该Channel对应的fd*/
  int fd() const { return fd_; }
  /* 返回该Channel 正在监听的事件 */
  int events() const { return events_; }
  /* 进行poll或者epoll_wait后，根据fd的返回事件调用此函数 */
  void set_revents(int revt) { revents_ = revt; } // used by pollers
  // int revents() const { return revents_; }
  /* 判断Channel是不是 没有 事件处理 */
  bool isNoneEvent() const { return events_ == kNoneEvent; }

  /* update 通过eventloop 去更新epoll中fd的监听事件 */
  /* 设置可读事件 */
  void enableReading() { events_ |= kReadEvent; update(); }
  /* 销毁读事件 */
  void disableReading() { events_ &= ~kReadEvent; update(); }
  /* 设置可写事件 */
  void enableWriting() { events_ |= kWriteEvent; update(); }
  /* 销毁写事件 */
  void disableWriting() { events_ &= ~kWriteEvent; update(); }
  /* 停止监听所有事件 */
  void disableAll() { events_ = kNoneEvent; update(); }
  /* 是否注册读写事件 */
  bool isWriting() const { return events_ & kWriteEvent; }
  bool isReading() const { return events_ & kReadEvent; }

  // for Poller
  int index() { return index_; }
  void set_index(int idx) { index_ = idx; }

  // for debug
  string reventsToString() const;
  string eventsToString() const;

  void doNotLogHup() { logHup_ = false; }

  /* 返回持有本Channel的EventLoop 指针 */
  EventLoop* ownerLoop() { return loop_; }
  /* 将Channel 从EventLoop中移除 */
  void remove();

 private:
  static string eventsToString(int fd, int ev);

  /* 通过调用loop_->updateChannel()来改变本fd在epoll中监听的事件 */
  void update();
  void handleEventWithGuard(Timestamp receiveTime);

  static const int kNoneEvent;  //无事件
  static const int kReadEvent;  //可读事件
  static const int kWriteEvent; //可写事件

  EventLoop* loop_;             //本Channel所属的EventLoop
  const int  fd_;               //Channel负责的文件描述符
  int        events_;           //注册/正在监听的事件
  int        revents_;          //就绪的事件 由EventLoop/Poller设置
  int        index_;            //被Poller使用的下标 used by Poller.
  bool       logHup_;           //是否生成某些日志

  boost::weak_ptr<void> tie_;
  bool tied_;
  bool eventHandling_;          //是否正在处理事件
  bool addedToLoop_;
  /* 四种回调函数，使用boost提供的function模板*/
  ReadEventCallback readCallback_;  //读事件回调函数
  EventCallback writeCallback_;     //写事件回调函数
  EventCallback closeCallback_;     //关闭事件回调函数
  EventCallback errorCallback_;     //错误事件回调函数
};

}
}
#endif  // MUDUO_NET_CHANNEL_H
