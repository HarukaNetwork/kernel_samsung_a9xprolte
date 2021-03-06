/*
 * Queue spinlock
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * (C) Copyright 2013-2014 Hewlett-Packard Development Company, L.P.
 *
 * Authors: Waiman Long <waiman.long@hp.com>
 *          Peter Zijlstra <pzijlstr@redhat.com>
 */
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/mutex.h>
#include <asm/byteorder.h>
#include <asm/qspinlock.h>

/*
 * The basic principle of a queue-based spinlock can best be understood
 * by studying a classic queue-based spinlock implementation called the
 * MCS lock. The paper below provides a good description for this kind
 * of lock.
 *
 * http://www.cise.ufl.edu/tr/DOC/REP-1992-71.pdf
 *
 * This queue spinlock implementation is based on the MCS lock, however to make
 * it fit the 4 bytes we assume spinlock_t to be, and preserve its existing
 * API, we must modify it somehow.
 *
 * In particular; where the traditional MCS lock consists of a tail pointer
 * (8 bytes) and needs the next pointer (another 8 bytes) of its own node to
 * unlock the next pending (next->locked), we compress both these: {tail,
 * next->locked} into a single u32 value.
 *
 * Since a spinlock disables recursion of its own context and there is a limit
 * to the contexts that can nest; namely: task, softirq, hardirq, nmi. As there
 * are at most 4 nesting levels, it can be encoded by a 2-bit number. Now
 * we can encode the tail by combining the 2-bit nesting level with the cpu
 * number. With one byte for the lock value and 3 bytes for the tail, only a
 * 32-bit word is now needed. Even though we only need 1 bit for the lock,
 * we extend it to a full byte to achieve better performance for architectures
 * that support atomic byte write.
 *
 * We also change the first spinner to spin on the lock bit instead of its
 * node; whereby avoiding the need to carry a node from lock to unlock, and
 * preserving existing lock API. This also makes the unlock code simpler and
 * faster.
 *
 * N.B. The current implementation only supports architectures that allow
 *      atomic operations on smaller 8-bit and 16-bit data types.
 *
 */

#include "mcs_spinlock.h"

/*
 * Per-CPU queue node structures; we can never have more than 4 nested
 * contexts: task, softirq, hardirq, nmi.
 *
 * Exactly fits one 64-byte cacheline on a 64-bit architecture.
 */
static DEFINE_PER_CPU_ALIGNED(struct mcs_spinlock, mcs_nodes[4]);

/*
 * We must be able to distinguish between no-tail and the tail at 0:0,
 * therefore increment the cpu number by one.
 * BluesMan: The idx = atomic counter (nesting depth)
 */

static inline u32 encode_tail(int cpu, int idx)
{
	u32 tail;

#ifdef CONFIG_DEBUG_SPINLOCK
	BUG_ON(idx > 3);
#endif
	tail  = (cpu + 1) << _Q_TAIL_CPU_OFFSET;
	tail |= idx << _Q_TAIL_IDX_OFFSET; /* assume < 4 */

	return tail;
}

static inline struct mcs_spinlock *decode_tail(u32 tail)
{
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	int idx = (tail &  _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;

	return per_cpu_ptr(&mcs_nodes[idx], cpu);
}

#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
/*
 * BluesMan: These functions are highly verbose and may cause
 * system lock up or undefined behaviour if used in hot-paths
 * You've been warned.
 * Keep these disabled unless we need to debug something runtime.
 *
 */
#if 0
static void trace_dump_this_mcs_node(struct mcs_spinlock *node)
{
	if(NULL == node) {
		trace_printk("This Node is NULL, nothing to go on\n");
		return;
	}
	trace_printk("node = %pS\n",node);
	trace_printk("node->next = %pS, node->locked = %d,\n",
		     node->next, node->locked);
	trace_printk("node->count(nesting) = %d\n, node->tail = %d\n",
		     node->count, node->tail);
}
static void trace_dump_per_cpu_mcs_nodes(void)
{
	unsigned int cpu, j;
	struct mcs_spinlock *node, *tmp;
	/* iterate through all cpus, regardless of their state */
	for(cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
		for(j = 0; j < 4; j++) {
			if(!cpu_online(cpu))
				trace_printk("cpu %d is down\n", cpu);
			node = per_cpu_ptr(&mcs_nodes[j], cpu);
			trace_dump_this_mcs_node(node);
			if(node->next != NULL) {
				/* We have contenders */
				tmp = node->next;
				trace_printk("node[%d] has children\n",j);
				do {
					trace_dump_this_mcs_node(tmp);
				}while((tmp = tmp->next) != NULL);
			}
		}
	}
}
#endif
#endif

#define _Q_LOCKED_PENDING_MASK (_Q_LOCKED_MASK | _Q_PENDING_MASK)

/*
 * By using the whole 2nd least significant byte for the pending bit, we
 * can allow better optimization of the lock acquisition for the pending
 * bit holder.
 *
 * This internal structure is also used by the set_locked function which
 * is not restricted to _Q_PENDING_BITS == 8.
 */
struct __qspinlock {
	union {
		atomic_t val;
#ifdef __LITTLE_ENDIAN
		u8	 locked;
		struct {
			u16	locked_pending;
			u16	tail;
		};
#else
		struct {
			u16	tail;
			u16	locked_pending;
		};
		struct {
			u8	reserved[3];
			u8	locked;
		};
#endif
	};
};

#if _Q_PENDING_BITS == 8
/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queue spinlock structure
 * @val : Current value of the queue spinlock 32-bit word
 *
 * *,1,0 -> *,0,1
 *
 * Lock stealing is not allowed if this function is used.
 */
static __always_inline void
clear_pending_set_locked(struct qspinlock *lock, u32 val)
{
	struct __qspinlock *l = (void *)lock;

	ASSIGN_ONCE(_Q_LOCKED_VAL,l->locked_pending);
}

/*
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queue spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail)
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
static __always_inline u32 xchg_tail(struct qspinlock *lock, u32 tail)
{
	struct __qspinlock *l = (void *)lock;

	/* BluesMan: Debug print. */
	/* pr_emerg("xchg_tail: sizeof l->tail = 0x%x, l->tail = 0x%x, tail = 0x%x\n",
	       sizeof(l->tail), l->tail, tail); */

	return (u32)xchg(&l->tail, tail >> _Q_TAIL_OFFSET) << _Q_TAIL_OFFSET;
}

#else /* _Q_PENDING_BITS == 8 */

/**
 * clear_pending_set_locked - take ownership and clear the pending bit.
 * @lock: Pointer to queue spinlock structure
 * @val : Current value of the queue spinlock 32-bit word
 *
 * *,1,0 -> *,0,1
 */
static __always_inline void
clear_pending_set_locked(struct qspinlock *lock, u32 val)
{
	u32 new, old;

	for (;;) {
		new = (val & ~_Q_PENDING_MASK) | _Q_LOCKED_VAL;

		old = atomic_cmpxchg(&lock->val, val, new);
		if (old == val)
			break;

		val = old;
	}
}

/**
 * xchg_tail - Put in the new queue tail code word & retrieve previous one
 * @lock : Pointer to queue spinlock structure
 * @tail : The new queue tail code word
 * Return: The previous queue tail code word
 *
 * xchg(lock, tail)
 *
 * p,*,* -> n,*,* ; prev = xchg(lock, node)
 */
static __always_inline u32 xchg_tail(struct qspinlock *lock, u32 tail)
{
	u32 old, new, val = atomic_read(&lock->val);

	for (;;) {
		new = (val & _Q_LOCKED_PENDING_MASK) | tail;
		old = atomic_cmpxchg(&lock->val, val, new);
		if (old == val)
			break;

		val = old;
	}
	return old;
}
#endif /* _Q_PENDING_BITS == 8 */

/**
 * set_locked - Set the lock bit and own the lock
 * @lock: Pointer to queue spinlock structure
 *
 * *,*,0 -> *,0,1
 */
static __always_inline void set_locked(struct qspinlock *lock)
{
	struct __qspinlock *l = (void *)lock;

	ASSIGN_ONCE(_Q_LOCKED_VAL, l->locked);
}

/**
 * queue_spin_lock_slowpath - acquire the queue spinlock
 * @lock: Pointer to queue spinlock structure
 * @val: Current value of the queue spinlock 32-bit word
 *
 * (queue tail, pending bit, lock value)
 *
 *              fast     :    slow                                  :    unlock
 *                       :                                          :
 * uncontended  (0,0,0) -:--> (0,0,1) ------------------------------:--> (*,*,0)
 *                       :       | ^--------.------.             /  :
 *                       :       v           \      \            |  :
 * pending               :    (0,1,1) +--> (0,1,0)   \           |  :
 *                       :       | ^--'              |           |  :
 *                       :       v                   |           |  :
 * uncontended           :    (n,x,y) +--> (n,0,0) --'           |  :
 *   queue               :       | ^--'                          |  :
 *                       :       v                               |  :
 * contended             :    (*,x,y) +--> (*,0,0) ---> (*,0,1) -'  :
 *   queue               :         ^--'                             :
 */
void queue_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	struct mcs_spinlock *prev, *next, *node;
	u32 new, old, tail;
	int idx;

	BUILD_BUG_ON(CONFIG_NR_CPUS >= (1U << _Q_TAIL_CPU_BITS));

	/*
	 * wait for in-progress pending->locked hand-overs
	 *
	 * 0,1,0 -> 0,0,1
	 * BluesMan: _Q_PENDING_VAL = 1 << 8 = 256
	 */
	if (val == _Q_PENDING_VAL) { /* check for the 8th bit set */
		while ((val = atomic_read(&lock->val)) == _Q_PENDING_VAL) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
			trace_printk("Waiting for pending->locked handover\n");
#endif
			cpu_relax();
		}
	}

	/*
	 * trylock || pending
	 *
	 * 0,0,0 -> 0,0,1 ; trylock
	 * 0,0,1 -> 0,1,1 ; pending
	 */
	for (;;) {
		/*
		 * If we observe any contention; queue.
		 *
		 * This iS
		 * 1111 1111 1111 1111 1111 1111 0000 0000 state:
		 *
		 * bit fields are:
		 * 0  -  7  :: locked byte
		 * 8  -  15 :: pending bit (now, extended to a byte)
		 * 16 - 17  :: tail index
		 * 18 - 31  :: tail cpu (+1)
		 *
		 * If we have a cpu waiting + pending bit set already,
		 * just queue.
		 *
		 */
		if (val & ~_Q_LOCKED_MASK) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
			trace_printk("contention detected, will make MCS Q\n");
#endif
			goto queue;
		}

		/*
		 * BluesMan: Here at this state, there is no contention for the
		 * lock but it is unavailable. So, we will be the first ones to
		 * get it once it is released. Hence, instead of building the
		 * queue, we will set the pending bit.
		 */

		new = _Q_LOCKED_VAL;
		if (val == new) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
			trace_printk("we're 1st waiter, setting pending bit\n");
#endif
			new |= _Q_PENDING_VAL;
		}

		/*
		 * BluesMan: Test code to dump
		 * if we're having 16 bits
		 */
		/* if(sizeof(&lock->val) == 2)
			WARN_ON(1); */

		/*
		 * BluesMan:
		 * actual setting of locked + pending bit to val
		 */
		old = atomic_cmpxchg(&lock->val, val, new);
		if (old == val)
			break;

		val = old;
	}

	/*
	 * we won the trylock
	 */
	if (new == _Q_LOCKED_VAL) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
		trace_printk("we won the trylock, returning from slowpath\n");
#endif
		return;
	}

	/*
	 * we're pending, wait for the owner to go away.
	 *
	 * *,1,1 -> *,1,0
	 *
	 * this wait loop must be a load-acquire such that we match the
	 * store-release that clears the locked bit and create lock
	 * sequentiality; this is because not all clear_pending_set_locked()
	 * implementations imply full barriers.
	 *
	 * BluesMan: Here the first waiter spins on the main lock!
	 */
	while ((val = smp_load_acquire(&lock->val.counter)) &
			_Q_LOCKED_MASK)
		cpu_relax();

	/*
	 * take ownership and clear the pending bit.
	 *
	 * *,1,0 -> *,0,1
	 *
	 * BluesMan: We were the first waiter and we've got the lock,
	 * rejoice!
	 *
	 */
	clear_pending_set_locked(lock, val);
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
	trace_printk("pending bit cleared, locked, returning from slowpath\n");
#endif
	return;

	/*
	 * End of pending bit optimistic spinning and beginning of MCS
	 * queuing.
	 */
queue:
	node = this_cpu_ptr(&mcs_nodes[0]);
	/*
	 * BluesMan: increment nesting count
	 * However, idx still gets
	 * old count value though.
	 */
	idx = node->count++;
	tail = encode_tail(smp_processor_id(), idx);

	node += idx;
	node->locked = 0;
	node->next = NULL;

	/*
	 * We touched a (possibly) cold cacheline in the per-cpu queue node;
	 * attempt the trylock once more in the hope someone let go while we
	 * weren't watching.
	 */
	if (queue_spin_trylock(lock)) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
		trace_printk("before MCS, we won trylock; going to release\n");
#endif
		goto release;
	}

	/*
	 * We have already touched the queueing cacheline; don't bother with
	 * pending stuff.
	 *
	 * p,*,* -> n,*,*
	 */
	old = xchg_tail(lock, tail);

	/*
	 * if there was a previous node; link it and wait until reaching the
	 * head of the waitqueue.
	 */
	if (old & _Q_TAIL_MASK) {
		prev = decode_tail(old);
		ASSIGN_ONCE(node, prev->next);
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
		trace_printk("we've cpu %d ahead of us\n",
			     (old >> _Q_TAIL_CPU_OFFSET) - 1);
		/* trace_dump_this_mcs_node(prev); */
#endif
		/*
		 * BluesMan: Read atomically the locked variable while
		 * == 0 and wfe
		 */
		arch_mcs_spin_lock_contended(&node->locked);
	}


	/*
	 * we're at the head of the waitqueue, wait for the owner & pending to
	 * go away.
	 * Load-acquired is used here because the set_locked()
	 * function below may not be a full memory barrier.
	 *
	 * *,x,y -> *,0,0
	 */
	while ((val = smp_load_acquire(&lock->val.counter)) &
			_Q_LOCKED_PENDING_MASK)
		cpu_relax();

	/*
	 * claim the lock:
	 *
	 * n,0,0 -> 0,0,1 : lock, uncontended
	 * *,0,0 -> *,0,1 : lock, contended
	 *
	 * If the queue head is the only one in the queue (lock value == tail),
	 * clear the tail code and grab the lock. Otherwise, we only need
	 * to grab the lock.
	 */
	for (;;) {
		if (val != tail) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
			trace_printk("contended Q, owner has released\n");
			trace_printk("just grab the lock and wait for next\n");
#endif
			set_locked(lock);
			break;
		}
		/* val == tail */
		old = atomic_cmpxchg(&lock->val, val, _Q_LOCKED_VAL);
		if (old == val ) {
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
			trace_printk("uncontented Q, owner has released\n");
			trace_printk("grab the lock, clear the tail and goto release\n");
#endif
			goto release;	/* No contention */
		}

		val = old;
	}
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
	trace_printk("contented path\n");
	/* trace_dump_this_mcs_node(node->next); */
#endif
	/*
	 * contended path; wait for next, release.
	 */
	while (!(next = READ_ONCE(node->next)))
		cpu_relax();

	arch_mcs_spin_unlock_contended(&next->locked);

release:
	/*
	 * BluesMan: Strictly debug code
	 */
#ifdef CONFIG_DEBUG_QUEUE_SPINLOCK
	trace_printk("we're at release, will dec the percpu ptr\n");
	/* trace_dump_per_cpu_mcs_nodes(); */
#endif
	/*
	 * release the node
	 */
	this_cpu_dec(mcs_nodes[0].count);
}
EXPORT_SYMBOL(queue_spin_lock_slowpath);
