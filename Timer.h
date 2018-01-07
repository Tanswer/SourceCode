// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)
//
// This is an internal header file, you should not include this.

#ifndef MUDUO_NET_TIMER_H
#define MUDUO_NET_TIMER_H

#include <boost/noncopyable.hpp>

#include <muduo/base/Atomic.h>
#include <muduo/base/Timestamp.h>
#include <muduo/net/Callbacks.h>

namespace muduo
{
namespace net
{
///
/// Internal class for timer event.
///
/* 定时器 */
class Timer : boost::noncopyable
{
 public:
  Timer(const TimerCallback& cb, Timestamp when, double interval)
    : callback_(cb),
      expiration_(when),
      interval_(interval),
      repeat_(interval > 0.0),
      sequence_(s_numCreated_.incrementAndGet())
  { }

#ifdef __GXX_EXPERIMENTAL_CXX0X__
  Timer(TimerCallback&& cb, Timestamp when, double interval)
    : callback_(std::move(cb)),
      expiration_(when),
      interval_(interval),
      repeat_(interval > 0.0),
      sequence_(s_numCreated_.incrementAndGet())
  { }
#endif

  void run() const
  {
    callback_();    //执行定时器回调函数
  }

  /* 返回定时器的超时时间戳 */
  Timestamp expiration() const  { return expiration_; }
  /* 是否周期性定时 */
  bool repeat() const { return repeat_; }
  /* 返回本定时任务的序列号 */
  int64_t sequence() const { return sequence_; }

  /* 重置定时器 */
  void restart(Timestamp now);

  static int64_t numCreated() { return s_numCreated_.get(); }

 private:
  const TimerCallback callback_;    //超时回调函数
  Timestamp expiration_;            //超时时间戳
  const double interval_;           //时间间隔，如果是一次性定时器，该值为0
  const bool repeat_;               //是否重复执行
  const int64_t sequence_;          //本定时任务的序号

  static AtomicInt64 s_numCreated_; //定时器计数，当前已经创建的定时器数量
};
}
}
#endif  // MUDUO_NET_TIMER_H
