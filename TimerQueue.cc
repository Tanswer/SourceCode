// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif

#include <muduo/net/TimerQueue.h>

#include <muduo/base/Logging.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Timer.h>
#include <muduo/net/TimerId.h>

#include <boost/bind.hpp>

#include <sys/timerfd.h>
#include <unistd.h>

namespace muduo
{
namespace net
{
namespace detail
{
/* 创建 timerfd */
int createTimerfd()
{
  int timerfd = ::timerfd_create(CLOCK_MONOTONIC,
                                 TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerfd < 0)
  {
    LOG_SYSFATAL << "Failed in timerfd_create";
  }
  return timerfd;
}

/* 计算超时时间与当前时间的时间差,并将参数转换为 api 接受的类型  */
struct timespec howMuchTimeFromNow(Timestamp when)
{
    /* 微秒数 = 超时时刻微秒数 - 当前时刻微秒数 */
  int64_t microseconds = when.microSecondsSinceEpoch()
                         - Timestamp::now().microSecondsSinceEpoch();
  if (microseconds < 100)
  {
    microseconds = 100;
  }
  struct timespec ts;   //转换成 struct timespec 结构返回
  ts.tv_sec = static_cast<time_t>(
      microseconds / Timestamp::kMicroSecondsPerSecond);
  ts.tv_nsec = static_cast<long>(
      (microseconds % Timestamp::kMicroSecondsPerSecond) * 1000);
  return ts;
}

/* 读timerfd，避免定时器事件一直触发 */
void readTimerfd(int timerfd, Timestamp now)
{
  uint64_t howmany;
  ssize_t n = ::read(timerfd, &howmany, sizeof howmany);
  LOG_TRACE << "TimerQueue::handleRead() " << howmany << " at " << now.toString();
  if (n != sizeof howmany)
  {
    LOG_ERROR << "TimerQueue::handleRead() reads " << n << " bytes instead of 8";
  }
}

/* 重置定时器超时时间 */
void resetTimerfd(int timerfd, Timestamp expiration)
{
  // wake up loop by timerfd_settime()
  struct itimerspec newValue;
  struct itimerspec oldValue;
  bzero(&newValue, sizeof newValue);
  bzero(&oldValue, sizeof oldValue);
  newValue.it_value = howMuchTimeFromNow(expiration);
  int ret = ::timerfd_settime(timerfd, 0, &newValue, &oldValue); //到这个时间后，会产生一个定时事件
  if (ret)
  {
    LOG_SYSERR << "timerfd_settime()";
  }
}

}
}
}

using namespace muduo;
using namespace muduo::net;
using namespace muduo::net::detail;

TimerQueue::TimerQueue(EventLoop* loop)
  : loop_(loop),
    timerfd_(createTimerfd()),
    timerfdChannel_(loop, timerfd_),
    timers_(),
    callingExpiredTimers_(false)
{
  timerfdChannel_.setReadCallback(
      boost::bind(&TimerQueue::handleRead, this));//设置timerfd可读事件回调函数为handleRead
  // we are always reading the timerfd, we disarm it with timerfd_settime.
  timerfdChannel_.enableReading();  //timerfd 注册可读事件
}

TimerQueue::~TimerQueue()
{
  timerfdChannel_.disableAll();
  timerfdChannel_.remove();
  ::close(timerfd_);
  // do not remove channel, since we're in EventLoop::dtor();
  for (TimerList::iterator it = timers_.begin();
      it != timers_.end(); ++it)
  {
    delete it->second; //手动释放Timer*
  }
}

/* 添加定时任务，返回此定时器对应的唯一标识 */
TimerId TimerQueue::addTimer(const TimerCallback& cb,
                             Timestamp when,
                             double interval)
{
    /* new 一个定时器对象 interval 大于0 ，就是需要重复的定时器 */
  Timer* timer = new Timer(cb, when, interval);
  /* 
   * runInLoop 的意思是 如果本IO线程想要添加定时器则直接由 addTimerInLoop 添加
   * 如果是其他线程向IO线程添加定时器则需要间接通过 queueInLoop添加
   */
  loop_->runInLoop(
      boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}

#ifdef __GXX_EXPERIMENTAL_CXX0X__
TimerId TimerQueue::addTimer(TimerCallback&& cb,
                             Timestamp when,
                             double interval)
{
    // 右值语义
  Timer* timer = new Timer(std::move(cb), when, interval);
  loop_->runInLoop(
      boost::bind(&TimerQueue::addTimerInLoop, this, timer));
  return TimerId(timer, timer->sequence());
}
#endif
/* 注销一个定时器，被EventLoop::cancel(TimerId timerId)调用 */
void TimerQueue::cancel(TimerId timerId)
{
  loop_->runInLoop(
      boost::bind(&TimerQueue::cancelInLoop, this, timerId));
}

/* IO线程向自己添加定时器 */
void TimerQueue::addTimerInLoop(Timer* timer)
{
  loop_->assertInLoopThread();
  bool earliestChanged = insert(timer); //如果当前插入的定时器 比队列中的定时器都早 则返回真

  if (earliestChanged)      //最早的超时时间改变了，就需要重置timerfd_的超时时间
  {
    resetTimerfd(timerfd_, timer->expiration()); //timerfd_ 重新设置超时时间
  }
}

void TimerQueue::cancelInLoop(TimerId timerId)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  ActiveTimer timer(timerId.timer_, timerId.sequence_);
  ActiveTimerSet::iterator it = activeTimers_.find(timer);  //查找该定时器
  if (it != activeTimers_.end()) // 找到了
  {
      /* 从 timers_ 和 activeTimers_ 中删掉*/
    size_t n = timers_.erase(Entry(it->first->expiration(), it->first));
    assert(n == 1); (void)n;
    delete it->first; // FIXME: no delete please    //手动 delete 
    activeTimers_.erase(it);
  }
  else if (callingExpiredTimers_) //可能正在处理
  {
      /* 那就先 插入要被注销的定时器 */
    cancelingTimers_.insert(timer);
  }
  assert(timers_.size() == activeTimers_.size());
}

/* timerfd 可读事件的回调函数 */
void TimerQueue::handleRead()
{
  loop_->assertInLoopThread();
  Timestamp now(Timestamp::now());
  readTimerfd(timerfd_, now);

  /* 找出所有超时的事件 */
  std::vector<Entry> expired = getExpired(now);

  callingExpiredTimers_ = true;
  cancelingTimers_.clear();
  // safe to callback outside critical section
  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    it->second->run();  //执行超时定时器的回调
  }
  callingExpiredTimers_ = false;

  reset(expired, now); //重置定时器，如果不需要再次定时，就删掉，否则再次定时
}

/* 获取队列中超时的定时器 */
std::vector<TimerQueue::Entry> TimerQueue::getExpired(Timestamp now)
{
  assert(timers_.size() == activeTimers_.size());
  std::vector<Entry> expired;   //保存超时定时器的容器
  Entry sentry(now, reinterpret_cast<Timer*>(UINTPTR_MAX)); //哨兵值
  TimerList::iterator end = timers_.lower_bound(sentry);    //返回第一个未超时的Timer的迭代器
  assert(end == timers_.end() || now < end->first);         //均未超时或者找到了
  std::copy(timers_.begin(), end, back_inserter(expired));  //把超时的定时器拷贝到 expired 容器中
  timers_.erase(timers_.begin(), end);                      //将超时的定时器从timers_删掉

  for (std::vector<Entry>::iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    size_t n = activeTimers_.erase(timer);      // 将超时的定时器 从 activeTimers_ 删掉
    assert(n == 1); (void)n;
  }

  assert(timers_.size() == activeTimers_.size());   // 都删掉之后 size 应该相同
  return expired;   //返回超时的那部分定时器
}

/* 已经执行完超时回调的定时任务后，检查这些定时器是否需要重复 */
void TimerQueue::reset(const std::vector<Entry>& expired, Timestamp now)
{
  Timestamp nextExpire;

  for (std::vector<Entry>::const_iterator it = expired.begin();
      it != expired.end(); ++it)
  {
    ActiveTimer timer(it->second, it->second->sequence());
    if (it->second->repeat()    // 需要重复 而且 没有要被注销
        && cancelingTimers_.find(timer) == cancelingTimers_.end())
    {
        /* 将该定时器的超时时间改为下次超时的时间 */
      it->second->restart(now);
      insert(it->second);   //重新插入到定时器容器中
    }
    else
    {
      // FIXME move to a free list
      // 不需要重复就删除
      delete it->second; // FIXME: no delete please
    }
  }

  if (!timers_.empty())
  {
      /* 获取当前定时器集合中的最早定时器的时间戳，作为下次超时时间*/
    nextExpire = timers_.begin()->second->expiration();
  }

  if (nextExpire.valid())
  {
    resetTimerfd(timerfd_, nextExpire);     //重置 timerfd_ 的超时时间
  }
}

/* 向 set 中插入新的定时器 */
bool TimerQueue::insert(Timer* timer)
{
  loop_->assertInLoopThread();
  assert(timers_.size() == activeTimers_.size());
  bool earliestChanged = false;             // 最早的超时时间 是否被更改
  Timestamp when = timer->expiration();     //新插入 timer 的超时时间
  TimerList::iterator it = timers_.begin(); // 当前最早的定时任务
  if (it == timers_.end() || when < it->first)
  {
    earliestChanged = true; 
    //如果timers_为空，或者when 小于目前最早的定时任务，那么最早的超时时间，肯定需要被改变
  }
  {
      /* 向 timers_ 中插入定时任务 */
    std::pair<TimerList::iterator, bool> result
      = timers_.insert(Entry(when, timer));
    assert(result.second); (void)result;
  }
  {
      /* 向 activeTimers_ 中插入定时任务 */
    std::pair<ActiveTimerSet::iterator, bool> result
      = activeTimers_.insert(ActiveTimer(timer, timer->sequence()));
    assert(result.second); (void)result;
  }

  /* 插入完成后，两个容器元素数目应该相同 */
  assert(timers_.size() == activeTimers_.size());
  return earliestChanged;   //返回修改标志，表示最近的超时时间已经改变
}

