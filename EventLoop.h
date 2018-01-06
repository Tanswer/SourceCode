// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is a public header file, it must only include public header files.

#ifndef MUDUO_NET_EVENTLOOP_H
#define MUDUO_NET_EVENTLOOP_H

#include <vector>

#include <boost/any.hpp>
#include <boost/function.hpp>
#include <boost/noncopyable.hpp>
#include <boost/scoped_ptr.hpp>

#include <muduo/base/Mutex.h>
#include <muduo/base/CurrentThread.h>
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/TimerId.h>

namespace muduo
{
namespace net
{

class Channel;  //前向声明，事件分发器主要用于事件注册与回调
class Poller;   //IO复用类，监听事件集合,即 epoll /poll 的功能
class TimerQueue;

///
/// Reactor, at most one per thread.
///
/// This is an interface class, so don't expose too much details.
/* EventLoop 是不可拷贝的 muduo中的大多数class都是不可拷贝的 */
class EventLoop : boost::noncopyable
{
 public:
  typedef boost::function<void()> Functor;  //回调函数

  EventLoop();
  ~EventLoop();  // force out-line dtor, for scoped_ptr members.

  ///
  /// Loops forever.
  ///
  /// Must be called in the same thread as creation of the object.
  ///
  /* 
   * IO线程创建了EventLoop对象，是这个类的核心接口
   * 用来启动事件循环 
   * EventLoop::loop()->Poller::Poll()获得就绪的事件集合
   * 再通过Channel::handleEvent()执行就绪事件回调
   */
  void loop();

  /// Quits loop.
  ///
  /// This is not 100% thread safe, if you call through a raw pointer,
  /// better to call through shared_ptr<EventLoop> for 100% safety.
  //终止事件循环
  void quit();

  ///
  /// Time when poll returns, usually means data arrival.
  ///
  Timestamp pollReturnTime() const { return pollReturnTime_; }

  int64_t iteration() const { return iteration_; }

  /// Runs callback immediately in the loop thread.
  /// It wakes up the loop, and run the cb.
  /// If in the same loop thread, cb is run within the function.
  /// Safe to call from other threads.
  void runInLoop(const Functor& cb);
  /// Queues callback in the loop thread.
  /// Runs after finish pooling.
  /// Safe to call from other threads.
  void queueInLoop(const Functor& cb);

  size_t queueSize() const;

#ifdef __GXX_EXPERIMENTAL_CXX0X__
  void runInLoop(Functor&& cb);
  void queueInLoop(Functor&& cb);
#endif

  // timers

  ///
  /// Runs callback at 'time'.
  /// Safe to call from other threads.
  ///
  //在某个绝对时间点执行定时回调
  TimerId runAt(const Timestamp& time, const TimerCallback& cb);
  ///
  /// Runs callback after @c delay seconds.
  /// Safe to call from other threads.
  ///
  //相对时间 执行定时回调
  TimerId runAfter(double delay, const TimerCallback& cb);
  ///
  /// Runs callback every @c interval seconds.
  /// Safe to call from other threads.
  ///
  //每隔interval执行定时回调
  TimerId runEvery(double interval, const TimerCallback& cb);
  ///
  /// Cancels the timer.
  /// Safe to call from other threads.
  ///
  //删除某个定时器
  void cancel(TimerId timerId);

#ifdef __GXX_EXPERIMENTAL_CXX0X__
  TimerId runAt(const Timestamp& time, TimerCallback&& cb);
  TimerId runAfter(double delay, TimerCallback&& cb);
  TimerId runEvery(double interval, TimerCallback&& cb);
#endif

  // internal usage
  // 唤醒IO线程
  void wakeup();
  // 更新某个事件分发器
  // 调用poller->updateChannel(channel)完成 fd 向事件集合注册事件及事件回调函数
  void updateChannel(Channel* channel);
  // 删除某个事件分发器
  void removeChannel(Channel* channel);
  bool hasChannel(Channel* channel);

  // pid_t threadId() const { return threadId_; }
  void assertInLoopThread()
  {
    if (!isInLoopThread())  //若运行线程不拥有EventLoop则退出，保证one loop per thread
    {
      abortNotInLoopThread();
    }
  }
  /* 判断当前线程是否为拥有此 EventLoop 的线程 */
  bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }
  // bool callingPendingFunctors() const { return callingPendingFunctors_; }
  bool eventHandling() const { return eventHandling_; }

  void setContext(const boost::any& context)
  { context_ = context; }

  const boost::any& getContext() const
  { return context_; }

  boost::any* getMutableContext()
  { return &context_; }

  /* 返回此线程的EventLoop对象 */
  static EventLoop* getEventLoopOfCurrentThread();

 private:
  /* 在不拥有EventLoop 线程中终止 */
  void abortNotInLoopThread();
  /* timerfd 上可读事件回调 */
  void handleRead();  // waked up
  /* 执行队列pendingFunctors 中的用户任务回调 */
  void doPendingFunctors();

  void printActiveChannels() const; // DEBUG

  typedef std::vector<Channel*> ChannelList;

  bool looping_; /* atomic */ //运行标志
  bool quit_; /* atomic and shared between threads, okay on x86, I guess. */ //退出循环标志
  bool eventHandling_; /* atomic */
  bool callingPendingFunctors_; /* atomic *///是否有用户任务回调标志
  int64_t iteration_;
  const pid_t threadId_;    //EventLoop 的附属线程ID
  Timestamp pollReturnTime_;
  boost::scoped_ptr<Poller> poller_;        //多路复用类Poller
  boost::scoped_ptr<TimerQueue> timerQueue_;//定时器队列用于存放定时器
  int wakeupFd_;    //调用eventfd返回的eventfd,用于唤醒EventLoop所在的线程
  // unlike in TimerQueue, which is an internal class,
  // we don't expose Channel to client.
  // 通过wakeuoChannel_观察wakeupFd_上的可读事件
  // 当可读表明需要唤醒EventLoop所在线程执行用户回调
  boost::scoped_ptr<Channel> wakeupChannel_;
  boost::any context_;

  // scratch variables
  ChannelList activeChannels_;      //活跃的事件集合,类似epoll的就绪事件集合
  Channel* currentActiveChannel_;   //当前活跃的事件

  mutable MutexLock mutex_;
  std::vector<Functor> pendingFunctors_; // @GuardedBy mutex_ //用户任务回调队列
};

}
}
#endif  // MUDUO_NET_EVENTLOOP_H
