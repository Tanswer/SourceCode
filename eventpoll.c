```
/*
 *  fs/eventpoll.c (Efficient event retrieval implementation)
 *  Copyright (C) 2001,...,2009	 Davide Libenzi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/hash.h>
#include <linux/spinlock.h>
#include <linux/syscalls.h>
#include <linux/rbtree.h>
#include <linux/wait.h>
#include <linux/eventpoll.h>
#include <linux/mount.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/anon_inodes.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/mman.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/compat.h>
#include <linux/rculist.h>
#include <net/busy_poll.h>


struct epoll_filefd {
	struct file *file;
	int fd;
} __packed;

/*
 * Structure used to track possible nested calls, for too deep recursions
 * and loop cycles.
 */
struct nested_call_node {
	struct list_head llink;
	void *cookie;
	void *ctx;
};

/*
 * This structure is used as collector for nested calls, to check for
 * maximum recursion dept and loop cycles.
 */
struct nested_calls {
	struct list_head tasks_call_list;
	spinlock_t lock;
};

/*
 * Each file descriptor added to the eventpoll interface will
 * have an entry of this type linked to the "rbr" RB tree.
 * Avoid increasing the size of this struct, there can be many thousands
 * of these on a server and we do not want this to take another cache line.
 */
/*
 * 表示一个被监听的 fd
 */
struct epitem {
	union {
        /*
         * 当使用epoll_ctl 将一批fds加入到某个epollfd时，
         * 内核会分配一批epitem与这些fds相对应，
         * 这些epitem以红黑树的形式组织起来，tree的root保存在struct eventpoll中
         * 这也就是挂载到eventpoll中的红黑树节点
         */
		/* RB tree node links this structure to the eventpoll RB tree */
		struct rb_node rbn;
		/* Used to free the struct epitem */
		struct rcu_head rcu;
	};

    /* 所有ready 的epitem都会被链到struct eventpoll 的rdlist中 */
	/* List header used to link this structure to the eventpoll ready list */
	struct list_head rdllink;

	/*
	 * Works together "struct eventpoll"->ovflist in keeping the
	 * single linked chain of items.
	 */
    /* 连接到 ovflist 的指针 */
	struct epitem *next;

    /* 里面包含了 fd 和struct file 结构，表示epitem 对应的 fd 和 struct file */
	/* The file descriptor information this item refers to */
	struct epoll_filefd ffd;

	/* Number of active wait queue attached to poll operations */
	int nwait;

	/* List containing poll wait queues */
	struct list_head pwqlist;

	/* The "container" of this item */
    /* 当前epitem 属于哪个 eventpoll */
	struct eventpoll *ep;

	/* List header used to link this item to the "struct file" items list */
	struct list_head fllink;

	/* wakeup_source used when EPOLLWAKEUP is set */
	struct wakeup_source __rcu *ws;

    /* 当前的epitem关系了哪些events，调用epoll_ctl从用户态传递过来 */
	/* The structure that describe the interested events and the source fd */
	struct epoll_event event;
};



/*
 * This structure is stored inside the "private_data" member of the file
 * structure and represents the main data structure for the eventpoll
 * interface.
 */
/*
 * 每创建一个 epollfd，内核就会分配一个eventpoll与之对应
 * 可以说是内核态的epollfd
 */
struct eventpoll {
	/* Protect the access to this structure */
	spinlock_t lock;

	/*
	 * This mutex is used to ensure that files are not removed
	 * while epoll is using them. This is held during the event
	 * collection loop, the file cleanup path, the epoll file exit
	 * code and the ctl operations.
	 */
    /*
     * 添加、修改或删除监听fd时，以及epoll
     */
	struct mutex mtx;

    /* 
     * epoll_wait 就是睡在这个等待队列上 
     * 如果没有就绪事件，则当前进程将自己阻塞在一个等待队列上
     */
	/* Wait queue used by sys_epoll_wait() */
	wait_queue_head_t wq;

    /* 这个用于epollfd 本身被poll的时候, 就是嵌套epoll*/
	/* Wait queue used by file->poll() */
	wait_queue_head_t poll_wait;

    /* 所有已经ready 的epitem 都在这个链表里面 */
	/* List ready file descriptors */
	struct list_head rdllist;

    /* 所有要监听的epitem都在这里 */
	/* RB tree root used to store monitored fd structs */
	struct rb_root rbr;

	/*
	 * This is a single linked list that chains all the "struct epitem" that
	 * happened while transferring ready events to userspace w/out
	 * holding ->lock.
	 */
    /* 这是一个单链表，链接着所有的epitem，当转到用户空间时 */
	struct epitem *ovflist;

	/* wakeup_source used when ep_scan_ready_list is running */
	struct wakeup_source *ws;


	/* The user that created the eventpoll descriptor */
	struct user_struct *user;

	struct file *file;

	/* used to optimize loop detection check */
	int visited;
	struct list_head visited_list_link;

#ifdef CONFIG_NET_RX_BUSY_POLL
	/* used to track busy poll napi_id */
	unsigned int napi_id;
#endif
};

/* poll 用到的钩子 ,主要完成epitem和epitem事件发生时的callback之间的关联 */
/* Wait structure used by the poll hooks */
struct eppoll_entry {
	/* List header used to link this structure to the "struct epitem" */
	struct list_head llink;

	/* The "base" pointer is set to the container "struct epitem" */
	struct epitem *base;    //所属epitem

	/*
	 * Wait queue item that will be linked to the target file wait
	 * queue head.
	 */
	wait_queue_t wait;  //等待队列结点,挂入被监听fd的wait队列中 

	/* The wait queue head that linked the "wait" wait queue item */
	wait_queue_head_t *whead;   //指向fd的等待队列
    /* 被监听fd的等待队列，如果fd为socket，那么whead 为 sock->sk_sleep */
};

/* Wrapper struct used by poll queueing */
struct ep_pqueue {
	poll_table pt;
	struct epitem *epi;
};


/* Used by the ep_send_events() function as callback private data */
struct ep_send_events_data {
	int maxevents;
	struct epoll_event __user *events;
};


/* 
 * epoll_create 基本啥都没干直接调用epoll_create1
 * size 参数其实是没有任何用处的,但是必须大于0
 */
SYSCALL_DEFINE1(epoll_create, int, size)
{
	if (size <= 0)
		return -EINVAL;

	return sys_epoll_create1(0);
}


/*
 * Open an eventpoll file descriptor.
 */
/*
 * 我们知道真正创建epollfd的是这个函数
 * 所以有时候直接用epoll_create1(flag)
 */
SYSCALL_DEFINE1(epoll_create1, int, flags)
{
	int error, fd;
	struct eventpoll *ep = NULL;    
	struct file *file;

	/* Check the EPOLL_* constant for consistency.  */
	BUILD_BUG_ON(EPOLL_CLOEXEC != O_CLOEXEC);

    /* epoll 目前唯一有效的 FLAG 就是CLOEXEC */
	if (flags & ~EPOLL_CLOEXEC)
		return -EINVAL;
	/*
	 * Create the internal data structure ("struct eventpoll").
	 */
    /* 分配一个 struct eventpoll ,函数里面包含初始化 */
	error = ep_alloc(&ep);
	if (error < 0)
		return error;
	/*
	 * Creates all the items needed to setup an eventpoll file. That is,
	 * a file structure and a free file descriptor.
	 */
    /* 创建一个匿名fd
     * epollfd 本身并不存在一个真正的文件与之对应，所以内核需要创建一个虚拟的文件，
     * 并为之分配真正的struct file 结构，而且有真正的fd
     */
    /* 这是拿到 fd */
	fd = get_unused_fd_flags(O_RDWR | (flags & O_CLOEXEC));
	if (fd < 0) {
		error = fd;
		goto out_free_ep;
	}
    /*
     * 这是分配 struct file 结构
     * eventpoll_fops : file operations,
     * 当对这个文件进行操作时fops里面的函数指针指向真正的操作实现
     * epoll 只实现了 poll 和release操作，其他文件系统操作都由VFS全权处理了
     * 
     * static const struct file_operations eventpoll_fops = {
     *          .release= ep_eventpoll_release,
     *          .poll   = ep_eventpoll_poll
     *     }; 
     * 
     * ep:就是struct eventpoll，它会作为一个私有数据保存在struct file 的private指针里面
     * 就是为了能通过 fd 找到 struct file，通过 struct file 再找到 eventpoll 结构
     */
	file = anon_inode_getfile("[eventpoll]", &eventpoll_fops, ep,
				 O_RDWR | (flags & O_CLOEXEC));
	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		goto out_free_fd;
	}
	ep->file = file;
	fd_install(fd, file);
	return fd;  //将创建好的epollfd 返回

out_free_fd:
	put_unused_fd(fd);
out_free_ep:
	ep_free(ep);
	return error;
}

/* 分配一个 eventpoll 结构 */
static int ep_alloc(struct eventpoll **pep)
{
	int error;
	struct user_struct *user;
	struct eventpoll *ep;

    /* 获取当前用户的的一些信息，比如是不是root，最大监听 fd 数目 */
	user = get_current_user();
	error = -ENOMEM;
    /* ep 即为开辟的 eventpoll 结构 */
	ep = kzalloc(sizeof(*ep), GFP_KERNEL);
	if (unlikely(!ep))
		goto free_uid;

    /* 下面都是初始化 */
	spin_lock_init(&ep->lock);
	mutex_init(&ep->mtx);   
	init_waitqueue_head(&ep->wq);           // 初始化等待队列，sys_epoll_wait使用，用于阻塞该进程 
	init_waitqueue_head(&ep->poll_wait);    // 初始化等待队列，file->poll使用，用于嵌套 epoll
	INIT_LIST_HEAD(&ep->rdllist);           // 初始化就绪链表
	ep->rbr = RB_ROOT;
	ep->ovflist = EP_UNACTIVE_PTR;
	ep->user = user;

	*pep = ep;

	return 0;

free_uid:
	free_uid(user);
	return error;
}


/*
 * The following function implements the controller interface for
 * the eventpoll file that enables the insertion/removal/change of
 * file descriptors inside the interest set.
 */
/*
 * 创建好之后就可以往里面添加、删除、修改等一系列操作了
 * epfd：就是创建好的epollfd
 * op：执行什么操作 ADD MOD DEL
 * fd：需要监听的fd
 * event：我们关心的事件
 */
SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd,
		struct epoll_event __user *, event)
{
	int error;
	int full_check = 0;
	struct fd f, tf;
	struct eventpoll *ep;
	struct epitem *epi;
	struct epoll_event epds;
	struct eventpoll *tep = NULL;

	error = -EFAULT;
    /* 
     * 判断op类型是否为EPOLL_CTL_DEL类型，如果要是的话就不用对参数epoll_event 进行拷贝了
     * 否则 从用户空间将epoll_event结构拷贝到内核空间 
     */
	if (ep_op_has_event(op) &&
	    copy_from_user(&epds, event, sizeof(struct epoll_event)))
		goto error_return;

	error = -EBADF;
    /* 取得 epfd 对应的 struct file 结构，这个结构就是epoll_create创建分配的 */
	f = fdget(epfd);
	if (!f.file)
		goto error_return;

	/* Get the "struct file *" for the target file */
    /* 取得要监听的fd对应的 struct file */
	tf = fdget(fd);
	if (!tf.file)
		goto error_fput;

	/* The target file descriptor must support poll */
    /* 确保要监听的文件支持 poll函数，那什么情况下文件会不支持 poll 呢？ */
	error = -EPERM;
	if (!tf.file->f_op->poll)
		goto error_tgt_fput;

	/* Check if EPOLLWAKEUP is allowed */
	if (ep_op_has_event(op))
		ep_take_care_of_epollwakeup(&epds);

	/*
	 * We have to check that the file structure underneath the file descriptor
	 * the user passed to us _is_ an eventpoll file. And also we do not permit
	 * adding an epoll file descriptor inside itself.
	 */
    /* epoll 不能自己监听自己 */
	error = -EINVAL;
	if (f.file == tf.file || !is_file_epoll(f.file))
		goto error_tgt_fput;

	/*
	 * epoll adds to the wakeup queue at EPOLL_CTL_ADD time only,
	 * so EPOLLEXCLUSIVE is not allowed for a EPOLL_CTL_MOD operation.
	 * Also, we do not currently supported nested exclusive wakeups.
	 */
	if (ep_op_has_event(op) && (epds.events & EPOLLEXCLUSIVE)) {
		if (op == EPOLL_CTL_MOD)
			goto error_tgt_fput;
		if (op == EPOLL_CTL_ADD && (is_file_epoll(tf.file) ||
				(epds.events & ~EPOLLEXCLUSIVE_OK_BITS)))
			goto error_tgt_fput;
	}

	/*
	 * At this point it is safe to assume that the "private_data" contains
	 * our own data structure.
	 */
    /* 通过struct file 取得我们从 epoll_create中分配到的 eventpoll 结构 */
	ep = f.file->private_data;

	/*
	 * When we insert an epoll file descriptor, inside another epoll file
	 * descriptor, there is the change of creating closed loops, which are
	 * better be handled here, than in more critical paths. While we are
	 * checking for loops we also determine the list of files reachable
	 * and hang them on the tfile_check_list, so we can check that we
	 * haven't created too many possible wakeup paths.
	 *
	 * We do not need to take the global 'epumutex' on EPOLL_CTL_ADD when
	 * the epoll file descriptor is attaching directly to a wakeup source,
	 * unless the epoll file descriptor is nested. The purpose of taking the
	 * 'epmutex' on add is to prevent complex toplogies such as loops and
	 * deep wakeup paths from forming in parallel through multiple
	 * EPOLL_CTL_ADD operations.
	 */
	mutex_lock_nested(&ep->mtx, 0);
	if (op == EPOLL_CTL_ADD) {
		if (!list_empty(&f.file->f_ep_links) ||
						is_file_epoll(tf.file)) {
			full_check = 1;
			mutex_unlock(&ep->mtx);
			mutex_lock(&epmutex);
			if (is_file_epoll(tf.file)) {
				error = -ELOOP;
				if (ep_loop_check(ep, tf.file) != 0) {
					clear_tfile_check_list();
					goto error_tgt_fput;
				}
			} else
				list_add(&tf.file->f_tfile_llink,
							&tfile_check_list);
			mutex_lock_nested(&ep->mtx, 0);
			if (is_file_epoll(tf.file)) {
				tep = tf.file->private_data;
				mutex_lock_nested(&tep->mtx, 1);
			}
		}
	}

	/*
	 * Try to lookup the file inside our RB tree, Since we grabbed "mtx"
	 * above, we can be sure to be able to use the item looked up by
	 * ep_find() till we release the mutex.
	 */
    /*
     * 对于每一个监听fd，内核都为其分配一个epitem结构与之对应
     * epoll 是不允许重复添加 fd 的
     * 所以我们首先查找fd 是不是已经存在了
     * ep_find 就是红黑树的查找 时间复杂度是O(logn)
     */
	epi = ep_find(ep, tf.file, fd);
    

	error = -EINVAL;
	switch (op) {
	case EPOLL_CTL_ADD:     /* 首先是添加 */
		if (!epi) {     
            /* 
             * 没有找到，是第一次添加，接受
             * 这里可以看出 POLLERR 和 POLLHUP 事件内核总是关心的 
             * POLLERR 发生错误   POLLHUP 对方描述符挂起
             */
			epds.events |= POLLERR | POLLHUP;
            /* 插入红黑树 */
			error = ep_insert(ep, &epds, tf.file, fd, full_check);
		} else
            /* 找到了，重复添加 */
			error = -EEXIST;
		if (full_check)
			clear_tfile_check_list();
		break;
	case EPOLL_CTL_DEL:     /* 删除 */
        /* 红黑树中已经存在 直接删除 */
		if (epi)    
			error = ep_remove(ep, epi);
		else
			error = -ENOENT;
		break;
	case EPOLL_CTL_MOD:     /* 修改 */
		if (epi) {
			if (!(epi->event.events & EPOLLEXCLUSIVE)) {
				epds.events |= POLLERR | POLLHUP;
				error = ep_modify(ep, epi, &epds);
			}
		} else
			error = -ENOENT;
		break;
	}
	if (tep != NULL)
		mutex_unlock(&tep->mtx);
	mutex_unlock(&ep->mtx);

error_tgt_fput:
	if (full_check)
		mutex_unlock(&epmutex);

	fdput(tf);
error_fput:
	fdput(f);
error_return:

	return error;
}

/*
 * Must be called with "mtx" held.
 */
/*
 * 在往 epollfd 中里面添加一个监听 fd 时被调用
 * tfile 就是 fd 在内核态的 struct file 结构
 */
static int ep_insert(struct eventpoll *ep, struct epoll_event *event,
		     struct file *tfile, int fd, int full_check)
{
	int error, revents, pwake = 0;
	unsigned long flags;
	long user_watches;
	struct epitem *epi;
	struct ep_pqueue epq;

    /* 查看是否达到当前用户的最大监听数 */
	user_watches = atomic_long_read(&ep->user->epoll_watches);
	if (unlikely(user_watches >= max_user_watches))
		return -ENOSPC;
    /* 从 slab 中分配一个 epitem */
	if (!(epi = kmem_cache_alloc(epi_cache, GFP_KERNEL)))
		return -ENOMEM;

	/* Item initialization follow here ... */
    /* 初始化 epitem 的相关成员 */
	INIT_LIST_HEAD(&epi->rdllink);
	INIT_LIST_HEAD(&epi->fllink);
	INIT_LIST_HEAD(&epi->pwqlist);
	epi->ep = ep;
    /* 保存需要监听的文件 fd 和它的 file 结构 */
	ep_set_ffd(&epi->ffd, tfile, fd);
	epi->event = *event;
	epi->nwait = 0;
    /* 这个指针的初值不是 NULL */
	epi->next = EP_UNACTIVE_PTR;
	if (epi->event.events & EPOLLWAKEUP) {
		error = ep_create_wakeup_source(epi);
		if (error)
			goto error_create_wakeup_source;
	} else {
		RCU_INIT_POINTER(epi->ws, NULL);
	}

    /*
     * struct ep_pqueue{
     * poll_table pt;
     * struct epitem *epi;
     * }
     */
    
    /* 下面要进入到 poll 的正题了 */
	/* Initialize the poll table using the queue callback */
	epq.epi = epi;
	/*
     * 将函数地址ep_ptable_queue_proc 赋值给epq(struct ep_pqueue)中的函数指针，会在下面的f_op->poll中调用
     * 初始化一个 poll_table 
     * 指定调用 poll_wait (注意不是 epoll_wait!!!)时的回调函数和我们关心哪些 events 
     * ep_ptable_queue_proc 就是回调函数，初值是所有 event 都关心
     */
    init_poll_funcptr(&epq.pt, ep_ptable_queue_proc);
    
	/*
	 * Attach the item to the poll hooks and get current event bits.
	 * We can safely use the file* here because its usage count has
	 * been increased by the caller of this function. Note that after
	 * this operation completes, the poll callback can start hitting
	 * the new item.
	 */
    /* 
     * 这一步很关键，涉及到内核的 poll 机制
     * 这个函数只是一个封装，函数调用层次很深
     * 完成这一步后，epitem 就和这个 socket 关联起来了
     * 当它有事件状态变化时，就会通过ep_poll_callback来通知
     * 这个函数还会查询当前的fd是否已经有事件就绪，有的话就将 event 返回
     */
	revents = ep_item_poll(epi, &epq.pt);

	/*
	 * We have to check if something went wrong during the poll wait queue
	 * install process. Namely an allocation for a wait queue failed due
	 * high memory pressure.
	 */
	error = -ENOMEM;
	if (epi->nwait < 0)
		goto error_unregister;

	/* Add the current item to the list of active epoll hook for this file */
	spin_lock(&tfile->f_lock);
	list_add_tail_rcu(&epi->fllink, &tfile->f_ep_links);
	spin_unlock(&tfile->f_lock);

	/*
	 * Add the current item to the RB tree. All RB tree operations are
	 * protected by "mtx", and ep_insert() is called with "mtx" held.
	 */
    /* 将epitem 插入到对应的 eventpoll 中去 */
	ep_rbtree_insert(ep, epi);

	/* now check if we've created too many backpaths */
	error = -EINVAL;
	if (full_check && reverse_path_check())
        goto error_remove_epi;
    
	/* We have to drop the new item inside our item list to keep track of it */
	spin_lock_irqsave(&ep->lock, flags);

	/* record NAPI ID of new item if present */
	ep_set_busy_poll_napi_id(epi);

	/* If the file is already "ready" we drop it inside the ready list */
    /* 如果有事件已经就绪，并且还没有加入就绪队列 */
	if ((revents & event->events) && !ep_is_linked(&epi->rdllink)) {
        /* 就将它加入就绪队列 */
		list_add_tail(&epi->rdllink, &ep->rdllist);
		ep_pm_stay_awake(epi);

		/* Notify waiting tasks that events are available */
        /* 如果有进程正在等待文件就绪，也就是调用epoll_wait睡眠的进程正在等待就唤醒 */
		if (waitqueue_active(&ep->wq))
			wake_up_locked(&ep->wq);
        /* 
         * 如果有进程在等待epollfd本身的事件就绪，那么就增加临时变量pwake的值 
         * pwake的值不为0时，在释放lock后，会唤醒等待进程
         */
		if (waitqueue_active(&ep->poll_wait))
			pwake++;
	}

	spin_unlock_irqrestore(&ep->lock, flags);

	atomic_long_inc(&ep->user->epoll_watches);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);

	return 0;

error_remove_epi:
	spin_lock(&tfile->f_lock);
	list_del_rcu(&epi->fllink);
	spin_unlock(&tfile->f_lock);

	rb_erase(&epi->rbn, &ep->rbr);

error_unregister:
	ep_unregister_pollwait(ep, epi);

	/*
	 * We need to do this because an event could have been arrived on some
	 * allocated wait queue. Note that we don't care about the ep->ovflist
	 * list, since that is used/cleaned only inside a section bound by "mtx".
	 * And ep_insert() is called with "mtx" held.
	 */
	spin_lock_irqsave(&ep->lock, flags);
	if (ep_is_linked(&epi->rdllink))
		list_del_init(&epi->rdllink);
	spin_unlock_irqrestore(&ep->lock, flags);

	wakeup_source_unregister(ep_wakeup_source(epi));

error_create_wakeup_source:
	kmem_cache_free(epi_cache, epi);

	return error;
}


/*
 * This is the callback that is used to add our wait queue to the
 * target file wakeup lists.
 */
/*
 * 为监听套接字注册一个等待函数(ep_poll_callback)并将epitem添加到指定的等待队列
 * 就是将epitem与指定的fd 通过等待队列 关联起来,用到了struct eppoll_entry 结构
 * 这个结构很关键，主要完成epitem和epitem事件发生是的callback函数之间的关联
 */
static void ep_ptable_queue_proc(struct file *file, wait_queue_head_t *whead,
				 poll_table *pt)
{
	struct epitem *epi = ep_item_from_epqueue(pt); //获取struct ep_pqueue的epi字段
	struct eppoll_entry *pwq;

	if (epi->nwait >= 0 && (pwq = kmem_cache_alloc(pwq_cache, GFP_KERNEL))) {
        /* 初始化等待队列，指定 ep_poll_callback 为唤醒时的回调函数 
         * 当我们监听的 fd 发生状态改变时，也就是队列头被唤醒时
         * 指定的回调函数将会被调用
         */
		init_waitqueue_func_entry(&pwq->wait, ep_poll_callback);
		/*
         * 针对 eppoll_entry 的初始化
         */ 
        pwq->whead = whead;
		pwq->base = epi;

        /* 将刚分配的等待队列项加入头中，头是由fd持有的 */
		if (epi->event.events & EPOLLEXCLUSIVE)
			add_wait_queue_exclusive(whead, &pwq->wait);
		else
			add_wait_queue(whead, &pwq->wait);
        /* 将 poll 使用的 eppoll_entry 结构加到 epi 的pwqlist上 */
		list_add_tail(&pwq->llink, &epi->pwqlist);
        /* nwait 记录了当前epitem加入到了多少个等待队列中 */
		epi->nwait++;
	} else {
		/* We have to signal that an error occurred */
		epi->nwait = -1;
	}
    /*
     * 梳理下过程：
     * 首先将eppoll_entry结构的whead指向fd的设备等待队列
     * 然后初始化base变量指向epitem，
     * 最后通过add_wait_queue将eppoll_entry挂载到fd的设备等待队列上
     * 由于ep_ptable_queue_proc函数设置了等待队列的ep_poll_callback回调函数
     * 所以在设备数据到来时，硬件中断处理函数中会唤醒该等待队列上等待的进程时
     * 会调用唤醒函数ep_poll_callback
     */
}

/*
 * This is the callback that is passed to the wait queue wakeup
 * mechanism. It is called by the stored file descriptors when they
 * have events to report.
 */
/*
 * 这是关键的回调函数，当监听的fd状态发生改变时，它会被调用
 * 将文件对应的epitem添加到就绪链表中
 * key 携带的是events 
 */
static int ep_poll_callback(wait_queue_t *wait, unsigned mode, int sync, void *key)
{
	int pwake = 0;
	unsigned long flags;
    /* 
     * 通过wait_queue_t 结构可以得到包含该结构的eppoll_entry结构
     * 从 eppoll_entry 结构中可以得到 epitem
     * 先根据等待队列项 找到 epi ,再找到 eventpoll，知道哪个进程挂到这个设备 
     */ 
	struct epitem *epi = ep_item_from_wait(wait);
	struct eventpoll *ep = epi->ep;
    
	int ewake = 0;

	if ((unsigned long)key & POLLFREE) {
		ep_pwq_from_wait(wait)->whead = NULL;
		/*
		 * whead = NULL above can race with ep_remove_wait_queue()
		 * which can do another remove_wait_queue() after us, so we
		 * can't use __remove_wait_queue(). whead->lock is held by
		 * the caller.
		 */
		list_del_init(&wait->task_list);
	}

	spin_lock_irqsave(&ep->lock, flags);

	ep_set_busy_poll_napi_id(epi);

	/*
	 * If the event mask does not contain any poll(2) event, we consider the
	 * descriptor to be disabled. This condition is likely the effect of the
	 * EPOLLONESHOT bit that disables the descriptor when an event is received,
	 * until the next EPOLL_CTL_MOD will be issued.
	 */
	if (!(epi->event.events & ~EP_PRIVATE_BITS))
		goto out_unlock;

	/*
	 * Check the events coming with the callback. At this stage, not
	 * every device reports the events in the "key" parameter of the
	 * callback. We need to be able to handle both cases here, hence the
	 * test for "key" != NULL before the event match test.
	 */
    /* 没有我们关心的event */
	if (key && !((unsigned long) key & epi->event.events))
		goto out_unlock;

	/*
	 * If we are transferring events to userspace, we can hold no locks
	 * (because we're accessing user memory, and because of linux f_op->poll()
	 * semantics). All the events that happen during that period of time are
	 * chained in ep->ovflist and requeued later on.
	 */
    /*
     * 如果该callback在被调用的同时，epoll_wait 已经返回了
     * 也就是说，此刻应用进程有可能已经在循环获取events
     * 这种情况下，内核将此刻发生的event的epitem用一个单独的链表ovflist链起来
     * 不发给应用程序，也不丢弃，在下一次epoll_wait时返回给用户
     */
	if (unlikely(ep->ovflist != EP_UNACTIVE_PTR)) {
		if (epi->next == EP_UNACTIVE_PTR) {
			epi->next = ep->ovflist;
			ep->ovflist = epi;
			if (epi->ws) {
				/*
				 * Activate ep->ws since epi->ws may get
				 * deactivated at any time.
				 */
				__pm_stay_awake(ep->ws);
			}

		}
		goto out_unlock;
	}

	/* If this file is already in the ready list we exit soon */
    /* 如果当前的epitem不在rdllist上，将当前的 epitem 放入readylist */
	if (!ep_is_linked(&epi->rdllink)) {
		list_add_tail(&epi->rdllink, &ep->rdllist);
		ep_pm_stay_awake_rcu(epi);
	}

	/*
	 * Wake up ( if active ) both the eventpoll wait list and the ->poll()
	 * wait list.
	 */
    /* epoll_wait时，会在ep->wq上挂载一个wait进程，有数据到来时就唤醒epoll_wait */
	if (waitqueue_active(&ep->wq)) {
		if ((epi->event.events & EPOLLEXCLUSIVE) &&
					!((unsigned long)key & POLLFREE)) {
			switch ((unsigned long)key & EPOLLINOUT_BITS) {
			case POLLIN:
				if (epi->event.events & POLLIN)
					ewake = 1;
				break;
			case POLLOUT:
				if (epi->event.events & POLLOUT)
					ewake = 1;
				break;
			case 0:
				ewake = 1;
				break;
			}
		}
		wake_up_locked(&ep->wq);
	}
    /* 如果epollfd也在被poll，那就唤醒等待队列里面的所有成员 */
	if (waitqueue_active(&ep->poll_wait))
		pwake++;

out_unlock:
	spin_unlock_irqrestore(&ep->lock, flags);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);

	if (epi->event.events & EPOLLEXCLUSIVE)
		return ewake;

	return 1;
}



/*
 * Implement the event wait interface for the eventpoll file. It is the kernel
 * part of the user space epoll_wait(2).
 */
/* 通过epfd -> file -> eventpoll 结构 ，再调用ep_poll 真正的让进程睡觉 */
SYSCALL_DEFINE4(epoll_wait, int, epfd, struct epoll_event __user *, events,
		int, maxevents, int, timeout)
{
	int error;
	struct fd f;
	struct eventpoll *ep;

	/* The maximum number of event must be greater than zero */
	if (maxevents <= 0 || maxevents > EP_MAX_EVENTS)
		return -EINVAL;

	/* Verify that the area passed by the user is writeable */
	/* 
     * 内核跟应用程序之间的数据大都是copy 不允许指针引用 
     * epoll_wait 需要内核返回数据给用户空间
     * 内核会用一些手段来验证这一段内存空间是不是有效的
     */
    if (!access_ok(VERIFY_WRITE, events, maxevents * sizeof(struct epoll_event)))
		return -EFAULT;

	/* Get the "struct file *" for the eventpoll file */
	/* 获取 epollfd 对应的 struct file */
    f = fdget(epfd);
	if (!f.file)
		return -EBADF;

	/*
	 * We have to check that the file structure underneath the fd
	 * the user passed to us _is_ an eventpoll file.
	 */
    /* 检查下是不是真正的 epollfd */
	error = -EINVAL;
	if (!is_file_epoll(f.file))
		goto error_fput;

	/*
	 * At this point it is safe to assume that the "private_data" contains
	 * our own data structure.
	 */
    /* 从struct file 中得到 eventpoll 结构 */
	ep = f.file->private_data;

	/* Time to fish for events ... */
    /* 调用ep_poll 让进程睡觉，等待事件到来 */
	error = ep_poll(ep, events, maxevents, timeout);

error_fput:
	fdput(f);
	return error;
}

static int ep_poll(struct eventpoll *ep, struct epoll_event __user *events,
		   int maxevents, long timeout)
{
	int res = 0, eavail, timed_out = 0;
	unsigned long flags;
	u64 slack = 0;
	wait_queue_t wait;          // 等待队列项
	ktime_t expires, *to = NULL;

	if (timeout > 0) {
		struct timespec64 end_time = ep_set_mstimeout(timeout);

		slack = select_estimate_accuracy(&end_time);
		to = &expires;
		*to = timespec64_to_ktime(end_time);
	} else if (timeout == 0) {
		/*
		 * Avoid the unnecessary trip to the wait queue loop, if the
		 * caller specified a non blocking operation.
		 */
		timed_out = 1;
		spin_lock_irqsave(&ep->lock, flags);
		goto check_events;
	}

fetch_events:

	if (!ep_events_available(ep))
		ep_busy_loop(ep, timed_out);

	spin_lock_irqsave(&ep->lock, flags);

    /* 如果ready list 不为空 就不睡了  直接干活 */
	if (!ep_events_available(ep)) {
		/*
		 * Busy poll timed out.  Drop NAPI ID for now, we can add
		 * it back in when we have moved a socket with a valid NAPI
		 * ID onto the ready list.
		 */
		ep_reset_busy_poll_napi_id(ep);

		/*
		 * We don't have any available event to return to the caller.
		 * We need to sleep here, and we will be wake up by
		 * ep_poll_callback() when events will become available.
		 */
        /* 
         * 初始化一个等待队列项，准备将自己挂起 
         * current 是一个宏，代表当前进程
         * 将当前进程对应的等待队列结点 wait 添加到ep结构的等待队列wp 的末尾
         */
		init_waitqueue_entry(&wait, current);
		__add_wait_queue_exclusive(&ep->wq, &wait);

		for (;;) {
			/*
			 * We don't want to sleep if the ep_poll_callback() sends us
			 * a wakeup in between. That's why we set the task state
			 * to TASK_INTERRUPTIBLE before doing the checks.
			 */
            /* 将当前进程的状态设置成 TASK_INTERRUPTIBLE 可以被信号唤醒 */
			set_current_state(TASK_INTERRUPTIBLE);
            /* 如果这个时候 ready list 里面有成员了 或者 睡眠时间过了 就不睡了 */
			if (ep_events_available(ep) || timed_out)
				break;
            /* 如果有信号产生 也不睡了 */
			if (signal_pending(current)) {
				res = -EINTR;
				break;
			}
            /* 解锁 此时开始 睡觉 */
			spin_unlock_irqrestore(&ep->lock, flags);
            /* 
             * timed_out 这个时间后，会被唤醒
             * ep_poll_callback 如果此时被调用，那么就不用等时间了，直接被唤醒
             * ep_poll_callback的调用时机是由被监听的fd的具体实现，
             * 比如socket或者某个设备驱动来决定的
             * 因为等待队列头是它们持有的，epoll和当前进程只是单纯的等待
             */
			if (!schedule_hrtimeout_range(to, slack, HRTIMER_MODE_ABS))
				timed_out = 1;

			spin_lock_irqsave(&ep->lock, flags);
		}
        /* 醒来了 */
		__remove_wait_queue(&ep->wq, &wait);
		__set_current_state(TASK_RUNNING);
	}
check_events:
	/* Is it worth to try to dig for events ? */
	eavail = ep_events_available(ep);

	spin_unlock_irqrestore(&ep->lock, flags);

	/*
	 * Try to transfer events to user space. In case we get 0 events and
	 * there's still timeout left over, we go trying again in search of
	 * more luck.
	 */
    /* 一切正常，有event 发生，开始准备数据copy 到用户空间 */
	if (!res && eavail &&
	    !(res = ep_send_events(ep, events, maxevents)) && !timed_out)
		goto fetch_events;

	return res;
}

static int ep_send_events(struct eventpoll *ep,
			  struct epoll_event __user *events, int maxevents)
{
	struct ep_send_events_data esed;

	esed.maxevents = maxevents;
	esed.events = events;

	return ep_scan_ready_list(ep, ep_send_events_proc, &esed, 0, false);
}

static int ep_scan_ready_list(struct eventpoll *ep,
			      int (*sproc)(struct eventpoll *,
					   struct list_head *, void *),
			      void *priv, int depth, bool ep_locked)
{
	int error, pwake = 0;
	unsigned long flags;
	struct epitem *epi, *nepi;
	LIST_HEAD(txlist);

	/*
	 * We need to lock this because we could be hit by
	 * eventpoll_release_file() and epoll_ctl().
	 */

	if (!ep_locked)
		mutex_lock_nested(&ep->mtx, depth);

	/*
	 * Steal the ready list, and re-init the original one to the
	 * empty list. Also, set ep->ovflist to NULL so that events
	 * happening while looping w/out locks, are not lost. We cannot
	 * have the poll callback to queue directly on ep->rdllist,
	 * because we want the "sproc" callback to be able to do it
	 * in a lockless way.
	 */
	spin_lock_irqsave(&ep->lock, flags);
    /*
     * 所有监听到events的epitem 都链到 rellist上了
     * 这一步将所有的epitem都转移到txlist上，而rdllist被清空
     */
	list_splice_init(&ep->rdllist, &txlist);
    /*
     * 将 ep->ovflist 由初始化的EP_UNACTIVE_PTR变为NULL
     * sproc函数调用中要将rdlist中的事件从内核态拷贝到用户态
     * 此时有新的event就加到这里面，而不是加入到ready list 
     */
	ep->ovflist = NULL;
	spin_unlock_irqrestore(&ep->lock, flags);

	/*
	 * Now call the callback function.
	 */
    /* 在这个回调函数中处理每个 epitem  sproc 就是 ep_send_events_proc 后面有 */
	error = (*sproc)(ep, &txlist, priv);

	spin_lock_irqsave(&ep->lock, flags);
	/*
	 * During the time we spent inside the "sproc" callback, some
	 * other events might have been queued by the poll callback.
	 * We re-insert them inside the main ready-list here.
	 */
    /* 在这里处理ovflist，这些epitem是我们在传递数据给用户空间时监听到了事件 */
	for (nepi = ep->ovflist; (epi = nepi) != NULL;
	     nepi = epi->next, epi->next = EP_UNACTIVE_PTR) {
		/*
		 * We need to check if the item is already in the list.
		 * During the "sproc" callback execution time, items are
		 * queued into ->ovflist but the "txlist" might already
		 * contain them, and the list_splice() below takes care of them.
		 */
        /* 将这些直接放入 readylist */
		if (!ep_is_linked(&epi->rdllink)) {
			list_add_tail(&epi->rdllink, &ep->rdllist);
			ep_pm_stay_awake(epi);
		}
	}
	/*
	 * We need to set back ep->ovflist to EP_UNACTIVE_PTR, so that after
	 * releasing the lock, events will be queued in the normal way inside
	 * ep->rdllist.
	 */
    /*将ovflist 置为 EP_UNACTIVE_PTR，后面就绪的events可以被正常的放到 readylist中 */
	ep->ovflist = EP_UNACTIVE_PTR;

	/*
	 * Quickly re-inject items left on "txlist".
	 */
	list_splice(&txlist, &ep->rdllist);
	__pm_relax(ep->ws);

	if (!list_empty(&ep->rdllist)) {
		/*
		 * Wake up (if active) both the eventpoll wait list and
		 * the ->poll() wait list (delayed after we release the lock).
		 */
		if (waitqueue_active(&ep->wq))
			wake_up_locked(&ep->wq);
		if (waitqueue_active(&ep->poll_wait))
			pwake++;
	}
	spin_unlock_irqrestore(&ep->lock, flags);

	if (!ep_locked)
		mutex_unlock(&ep->mtx);

	/* We have to call this outside the lock */
	if (pwake)
		ep_poll_safewake(&ep->poll_wait);

	return error;
}


static int ep_send_events_proc(struct eventpoll *ep, struct list_head *head,
			       void *priv)
{
	struct ep_send_events_data *esed = priv;
	int eventcnt;
	unsigned int revents;
	struct epitem *epi;
	struct epoll_event __user *uevent;
	struct wakeup_source *ws;
	poll_table pt;

	init_poll_funcptr(&pt, NULL);

	/*
	 * We can loop without lock because we are passed a task private list.
	 * Items cannot vanish during the loop because ep_scan_ready_list() is
	 * holding "mtx" during this call.
	 */
	for (eventcnt = 0, uevent = esed->events;
	     !list_empty(head) && eventcnt < esed->maxevents;) {
		epi = list_first_entry(head, struct epitem, rdllink);

		/*
		 * Activate ep->ws before deactivating epi->ws to prevent
		 * triggering auto-suspend here (in case we reactive epi->ws
		 * below).
		 *
		 * This could be rearranged to delay the deactivation of epi->ws
		 * instead, but then epi->ws would temporarily be out of sync
		 * with ep_is_linked().
		 */
		ws = ep_wakeup_source(epi);
		if (ws) {
			if (ws->active)
				__pm_stay_awake(ep->ws);
			__pm_relax(ws);
		}

		list_del_init(&epi->rdllink);

		revents = ep_item_poll(epi, &pt);

		/*
		 * If the event mask intersect the caller-requested one,
		 * deliver the event to userspace. Again, ep_scan_ready_list()
		 * is holding "mtx", so no operations coming from userspace
		 * can change the item.
		 */
		if (revents) {
            /* 如果从内核态向用户态拷贝失败，则将剥离下来的epi事件重新加入到txlist中 */
			if (__put_user(revents, &uevent->events) ||
			    __put_user(epi->event.data, &uevent->data)) {
				list_add(&epi->rdllink, head);
				ep_pm_stay_awake(epi);
				return eventcnt ? eventcnt : -EFAULT;
			}
			eventcnt++;
			uevent++;
            /* 当监听的event事件类型中加入了EPOLLONESHOT 内核就不会进行第二次触发 */
			if (epi->event.events & EPOLLONESHOT)
				epi->event.events &= EP_PRIVATE_BITS;
			else if (!(epi->event.events & EPOLLET)) {
				/*
				 * If this file has been added with Level
				 * Trigger mode, we need to insert back inside
				 * the ready list, so that the next call to
				 * epoll_wait() will check again the events
				 * availability. At this point, no one can insert
				 * into ep->rdllist besides us. The epoll_ctl()
				 * callers are locked out by
				 * ep_scan_ready_list() holding "mtx" and the
				 * poll callback will queue them in ep->ovflist.
				 */
                /*
                 * LT 和 ET的区别
                 * 如果是ET,epitem不会再进入到readylist
                 * 除非fd状态再次改变，ep_poll_callback被调用
                 * 如果不是ET,不管有没有有效的事件或者数据都会被重新插入readylist
                 * 再下一次epoll_wait会立即返回，并通知用户空间
                 * 如果这些被监听的fds确实没事件也没数据了，epoll_wait会返回一个0
                 * 空转一次
                 */
				list_add_tail(&epi->rdllink, &ep->rdllist);
				ep_pm_stay_awake(epi);
			}
		}
	}

	return eventcnt;
}

```
