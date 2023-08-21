// SPDX-License-Identifier: GPL-2.0
/*
 * Non-trivial C macros cannot be used in Rust. Similarly, inlined C functions
 * cannot be called either. This file explicitly creates functions ("helpers")
 * that wrap those so that they can be called from Rust.
 *
 * Even though Rust kernel modules should never use directly the bindings, some
 * of these helpers need to be exported because Rust generics and inlined
 * functions may not get their code generated in the crate where they are
 * defined. Other helpers, called from non-inline functions, may not be
 * exported, in principle. However, in general, the Rust compiler does not
 * guarantee codegen will be performed for a non-inline function either.
 * Therefore, this file exports all the helpers. In the future, this may be
 * revisited to reduce the number of exports after the compiler is informed
 * about the places codegen is required.
 *
 * All symbols are exported as GPL-only to guarantee no GPL-only feature is
 * accidentally exposed.
 */

#include <linux/bug.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errname.h>
#include <linux/instruction_pointer.h>
#include <linux/lockdep.h>
#include <linux/refcount.h>
#include <linux/mutex.h>
#include <linux/siphash.h>
#include <linux/spinlock.h>
#include <linux/sched/signal.h>
#include <linux/timekeeping.h>
#include <linux/wait.h>
#include <linux/xarray.h>

__noreturn void rust_helper_BUG(void)
{
	BUG();
}
EXPORT_SYMBOL_GPL(rust_helper_BUG);

void rust_helper_mutex_lock(struct mutex *lock)
{
	mutex_lock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_mutex_lock);

void rust_helper___spin_lock_init(spinlock_t *lock, const char *name,
				  struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_SPINLOCK
# ifndef CONFIG_PREEMPT_RT
	__raw_spin_lock_init(spinlock_check(lock), name, key, LD_WAIT_CONFIG);
# else
	rt_mutex_base_init(&lock->lock);
	__rt_spin_lock_init(lock, name, key, false);
# endif
#else
	spin_lock_init(lock);
#endif
}
EXPORT_SYMBOL_GPL(rust_helper___spin_lock_init);

void rust_helper_spin_lock(spinlock_t *lock)
{
	spin_lock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_spin_lock);

void rust_helper_spin_unlock(spinlock_t *lock)
{
	spin_unlock(lock);
}
EXPORT_SYMBOL_GPL(rust_helper_spin_unlock);

void rust_helper_init_wait(struct wait_queue_entry *wq_entry)
{
	init_wait(wq_entry);
}
EXPORT_SYMBOL_GPL(rust_helper_init_wait);

int rust_helper_signal_pending(struct task_struct *t)
{
	return signal_pending(t);
}
EXPORT_SYMBOL_GPL(rust_helper_signal_pending);

refcount_t rust_helper_REFCOUNT_INIT(int n)
{
	return (refcount_t)REFCOUNT_INIT(n);
}
EXPORT_SYMBOL_GPL(rust_helper_REFCOUNT_INIT);

void rust_helper_refcount_inc(refcount_t *r)
{
	refcount_inc(r);
}
EXPORT_SYMBOL_GPL(rust_helper_refcount_inc);

bool rust_helper_refcount_dec_and_test(refcount_t *r)
{
	return refcount_dec_and_test(r);
}
EXPORT_SYMBOL_GPL(rust_helper_refcount_dec_and_test);

__force void *rust_helper_ERR_PTR(long err)
{
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(rust_helper_ERR_PTR);

bool rust_helper_IS_ERR(__force const void *ptr)
{
	return IS_ERR(ptr);
}
EXPORT_SYMBOL_GPL(rust_helper_IS_ERR);

long rust_helper_PTR_ERR(__force const void *ptr)
{
	return PTR_ERR(ptr);
}
EXPORT_SYMBOL_GPL(rust_helper_PTR_ERR);

const char *rust_helper_errname(int err)
{
	return errname(err);
}
EXPORT_SYMBOL_GPL(rust_helper_errname);

struct task_struct *rust_helper_get_current(void)
{
	return current;
}
EXPORT_SYMBOL_GPL(rust_helper_get_current);

void rust_helper_get_task_struct(struct task_struct *t)
{
	get_task_struct(t);
}
EXPORT_SYMBOL_GPL(rust_helper_get_task_struct);

void rust_helper_put_task_struct(struct task_struct *t)
{
	put_task_struct(t);
}
EXPORT_SYMBOL_GPL(rust_helper_put_task_struct);

u64 rust_helper_siphash(const void *data, size_t len,
			const siphash_key_t *key)
{
	return siphash(data, len, key);
}
EXPORT_SYMBOL_GPL(rust_helper_siphash);

void rust_helper_lock_acquire_ret(struct lockdep_map *lock, unsigned int subclass,
				  int trylock, int read, int check,
				  struct lockdep_map *nest_lock)
{
	lock_acquire(lock, subclass, trylock, read, check, nest_lock, _RET_IP_);
}
EXPORT_SYMBOL_GPL(rust_helper_lock_acquire_ret);

void rust_helper_lock_release_ret(struct lockdep_map *lock)
{
	lock_release(lock, _RET_IP_);
}
EXPORT_SYMBOL_GPL(rust_helper_lock_release_ret);

ktime_t rust_helper_ktime_get_real(void) {
	return ktime_get_real();
}
EXPORT_SYMBOL_GPL(rust_helper_ktime_get_real);

ktime_t rust_helper_ktime_get_boottime(void) {
	return ktime_get_boottime();
}
EXPORT_SYMBOL_GPL(rust_helper_ktime_get_boottime);

ktime_t rust_helper_ktime_get_clocktai(void) {
	return ktime_get_clocktai();
}
EXPORT_SYMBOL_GPL(rust_helper_ktime_get_clocktai);

void rust_helper_xa_init_flags(struct xarray *xa, gfp_t flags)
{
	xa_init_flags(xa, flags);
}
EXPORT_SYMBOL_GPL(rust_helper_xa_init_flags);

bool rust_helper_xa_empty(struct xarray *xa)
{
	return xa_empty(xa);
}
EXPORT_SYMBOL_GPL(rust_helper_xa_empty);

int rust_helper_xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit, gfp_t gfp)
{
	return xa_alloc(xa, id, entry, limit, gfp);
}
EXPORT_SYMBOL_GPL(rust_helper_xa_alloc);

void rust_helper_xa_lock(struct xarray *xa)
{
	xa_lock(xa);
}
EXPORT_SYMBOL_GPL(rust_helper_xa_lock);

void rust_helper_xa_unlock(struct xarray *xa)
{
	xa_unlock(xa);
}
EXPORT_SYMBOL_GPL(rust_helper_xa_unlock);

int rust_helper_xa_err(void *entry)
{
	return xa_err(entry);
}
EXPORT_SYMBOL_GPL(rust_helper_xa_err);

void *rust_helper_dev_get_drvdata(struct device *dev)
{
	return dev_get_drvdata(dev);
}
EXPORT_SYMBOL_GPL(rust_helper_dev_get_drvdata);

const char *rust_helper_dev_name(const struct device *dev)
{
	return dev_name(dev);
}
EXPORT_SYMBOL_GPL(rust_helper_dev_name);

unsigned long rust_helper_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	return copy_from_user(to, from, n);
}
EXPORT_SYMBOL_GPL(rust_helper_copy_from_user);

unsigned long rust_helper_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	return copy_to_user(to, from, n);
}
EXPORT_SYMBOL_GPL(rust_helper_copy_to_user);

unsigned long rust_helper_clear_user(void __user *to, unsigned long n)
{
	return clear_user(to, n);
}
EXPORT_SYMBOL_GPL(rust_helper_clear_user);

/*
 * We use `bindgen`'s `--size_t-is-usize` option to bind the C `size_t` type
 * as the Rust `usize` type, so we can use it in contexts where Rust
 * expects a `usize` like slice (array) indices. `usize` is defined to be
 * the same as C's `uintptr_t` type (can hold any pointer) but not
 * necessarily the same as `size_t` (can hold the size of any single
 * object). Most modern platforms use the same concrete integer type for
 * both of them, but in case we find ourselves on a platform where
 * that's not true, fail early instead of risking ABI or
 * integer-overflow issues.
 *
 * If your platform fails this assertion, it means that you are in
 * danger of integer-overflow bugs (even if you attempt to remove
 * `--size_t-is-usize`). It may be easiest to change the kernel ABI on
 * your platform such that `size_t` matches `uintptr_t` (i.e., to increase
 * `size_t`, because `uintptr_t` has to be at least as big as `size_t`).
 */
static_assert(
	sizeof(size_t) == sizeof(uintptr_t) &&
	__alignof__(size_t) == __alignof__(uintptr_t),
	"Rust code expects C `size_t` to match Rust `usize`"
);
