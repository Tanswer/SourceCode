// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_TIMERQUEUE_H
#define MUDUO_NET_TIMERQUEUE_H

#include <set>
#include <vector>

#include <boost/noncopyable.hpp>

#include <muduo/base/Mutex.h>
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>
#include <muduo/net/Channel.h>

namespace muduo
{
namespace net
{

class EventLoop;
class Timer;
class TimerId;

///
/// A best efforts timer queue.
/// No guarantee that the callback will be on time.
///
/* 定时器队列 */
class TimerQueue : boost::noncopyable
{
 public:
  explicit TimerQueue(EventLoop* loop);
  ~TimerQueue();

  ///
  /// Schedules the callback to be run at given time,
  /// repeats if @c interval > 0.0.
  ///
  /// Must be thread safe. Usually be called from other threads.
  /* 添加一个定时器 */
  TimerId addTimer(const TimerCallback& cb,
                   Timestamp when,
                   double interval);
#ifdef __GXX_EXPERIMENTAL_CXX0X__
  TimerId addTimer(TimerCallback&& cb,
                   Timestamp when,
                   double interval);
#endif

  /* 注销一个定时器 */
  void cancel(TimerId timerId);

 private:

  // FIXME: use unique_ptr<Timer> instead of raw pointers.
  // This requires heterogeneous comparison lookup (N3465) from C++14
  // so that we can find an T* in a set<unique_ptr<T>>.
  typedef std::pair<Timestamp, Timer*> Entry;   //对应一个定时任务
  typedef std::set<Entry> TimerList;            //定时任务集合，采用set,有key无value，且有序
  typedef std::pair<Timer*, int64_t> ActiveTimer; //下面有解释  
  typedef std::set<ActiveTimer> ActiveTimerSet;

  void addTimerInLoop(Timer* timer);    //添加一个定时任务
  void cancelInLoop(TimerId timerId);   //注销一个定时器
  // called when timerfd alarms
  void handleRead();                    //timerfd 可读 的回调
  // move out all expired timers
  std::vector<Entry> getExpired(Timestamp now); //获取所有超时的定时器
  /* 重置超时的定时器 */
  void reset(const std::vector<Entry>& expired, Timestamp now);

  bool insert(Timer* timer);    //把定时器插到TimerList中

  EventLoop* loop_;             //TimerQueue 所属的 EventLoop
  const int timerfd_;           // 内部的 timerfd 
  Channel timerfdChannel_;      //timerfd 对应的Channel，借此来观察timerfd_ 上的readable事件
  // Timer list sorted by expiration
  TimerList timers_;            //所有的定时任务

  // for cancel()
  // timers_ 与 activeTimers_ 都保存了相同的Timer 地址
  // timers_ 是按超时时间排序，activeTimers_ 是按定时器地址排序
  ActiveTimerSet activeTimers_;
  bool callingExpiredTimers_; /* atomic *///是否处于 处理定时器超时回调中
  ActiveTimerSet cancelingTimers_; //保存被注销的定时器
};

}
}
#endif  // MUDUO_NET_TIMERQUEUE_H
