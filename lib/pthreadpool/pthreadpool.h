/*
 * Unix SMB/CIFS implementation.
 * threadpool implementation based on pthreads
 * Copyright (C) Volker Lendecke 2009,2011
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __PTHREADPOOL_H__
#define __PTHREADPOOL_H__

struct pthreadpool;

/**
 * @defgroup pthreadpool The pthreadpool API
 *
 * This API provides a way to run threadsafe functions in a helper
 * thread. It is initially intended to run getaddrinfo asynchronously.
 */


/**
 * @brief Create a pthreadpool
 *
 * A struct pthreadpool is the basis for for running threads in the
 * background.
 *
 * @param[in]	max_threads	Maximum parallelism in this pool
 * @param[out]	presult		Pointer to the threadpool returned
 * @return			success: 0, failure: errno
 *
 * max_threads=0 means unlimited parallelism. The caller has to take
 * care to not overload the system.
 */
int pthreadpool_init(unsigned max_threads, struct pthreadpool **presult,
		     int (*signal_fn)(int jobid,
				      void (*job_fn)(void *private_data),
				      void *job_fn_private_data,
				      void *private_data),
		     void *signal_fn_private_data);

/**
 * @brief Get the max threads value of pthreadpool
 *
 * @note This can be 0 for strict sync processing.
 *
 * @param[in]	pool		The pool
 * @return			number of possible threads
 */
size_t pthreadpool_max_threads(struct pthreadpool *pool);

/**
 * @brief The number of queued jobs of pthreadpool
 *
 * This is the number of jobs added by pthreadpool_add_job(),
 * which are not yet processed by a thread.
 *
 * @param[in]	pool		The pool
 * @return			The number of jobs
 */
size_t pthreadpool_queued_jobs(struct pthreadpool *pool);

/**
 * @brief Check for per thread current working directory support of pthreadpool
 *
 * Since Linux kernel 2.6.16, unshare(CLONE_FS) is supported,
 * which provides a per thread current working directory
 * and allows [f]chdir() within the worker threads.
 *
 * Note that this doesn't work on some contraint container setups,
 * the complete unshare() syscall might be rejected.
 * pthreadpool_per_thread_cwd() returns what is available
 * at runtime, so the callers should really check this!
 *
 * @param[in]	pool		The pool to run the job on
 * @return			supported: true, otherwise: false
 */
bool pthreadpool_per_thread_cwd(struct pthreadpool *pool);

/**
 * @brief Stop a pthreadpool
 *
 * Stop a pthreadpool. If jobs are submitted, but not yet active in
 * a thread, they won't get executed. If a job has already been
 * submitted to a thread, the job function will continue running, and
 * the signal function might still be called.
 *
 * This allows a multi step shutdown using pthreadpool_stop(),
 * pthreadpool_cancel_job() and pthreadpool_destroy().
 *
 * @param[in]	pool		The pool to stop
 * @return			success: 0, failure: errno
 *
 * @see pthreadpool_cancel_job()
 * @see pthreadpool_destroy()
 */
int pthreadpool_stop(struct pthreadpool *pool);

/**
 * @brief Destroy a pthreadpool
 *
 * This basically implies pthreadpool_stop() if the pool
 * isn't already stopped.
 *
 * Destroy a pthreadpool. If jobs are submitted, but not yet active in
 * a thread, they won't get executed. If a job has already been
 * submitted to a thread, the job function will continue running, and
 * the signal function might still be called. The caller of
 * pthreadpool_init must make sure the required resources are still
 * around when the pool is destroyed with pending jobs.  The last
 * thread to exit will finally free() the pool memory.
 *
 * @param[in]	pool		The pool to destroy
 * @return			success: 0, failure: errno
 *
 * @see pthreadpool_stop()
 */
int pthreadpool_destroy(struct pthreadpool *pool);

/**
 * @brief Add a job to a pthreadpool
 *
 * This adds a job to a pthreadpool. The job can be identified by
 * job_id. This integer will be passed to signal_fn() when the
 * job is completed.
 *
 * @param[in]	pool		The pool to run the job on
 * @param[in]	job_id		A custom identifier
 * @param[in]	fn		The function to run asynchronously
 * @param[in]	private_data	Pointer passed to fn
 * @return			success: 0, failure: errno
 */
int pthreadpool_add_job(struct pthreadpool *pool, int job_id,
			void (*fn)(void *private_data), void *private_data);

/**
 * @brief Check if the pthreadpool needs a restart.
 *
 * This checks if there are enough threads to run the already
 * queued jobs. This should be called only the callers signal_fn
 * (passed to pthreadpool_init()) returned an error, so
 * that the job's worker thread exited.
 *
 * Typically this is called once the file destriptor
 * returned by pthreadpool_restart_check_monitor_fd()
 * became readable and pthreadpool_restart_check_monitor_drain()
 * returned success.
 *
 * This function tries to restart the missing threads.
 *
 * @param[in]	pool		The pool to run the job on
 * @return			success: 0, failure: errno
 *
 * @see pthreadpool_restart_check_monitor_fd
 * @see pthreadpool_restart_check_monitor_drain
 */
int pthreadpool_restart_check(struct pthreadpool *pool);

/**
 * @brief Return a file destriptor that monitors the pool.
 *
 * If the file destrictor becomes readable,
 * the event handler should call pthreadpool_restart_check_monitor_drain().
 *
 * pthreadpool_restart_check() should also be called once the
 * state is drained.
 *
 * This function returns a fresh fd using dup() each time.
 *
 * If the pool doesn't require restarts, this function
 * returns -1 and sets errno = ENOSYS. The caller
 * may ignore that situation.
 *
 * @param[in]	pool		The pool to run the job on
 * @return			success: 0, failure: -1 (set errno)
 *
 * @see pthreadpool_restart_check_monitor_fd
 * @see pthreadpool_restart_check_monitor_drain
 */
int pthreadpool_restart_check_monitor_fd(struct pthreadpool *pool);

/**
 * @brief Drain the monitor file destriptor of the pool.
 *
 * If the file destrictor returned by pthreadpool_restart_check_monitor_fd()
 * becomes readable, pthreadpool_restart_check_monitor_drain() should be
 * called before pthreadpool_restart_check().
 *
 * If this function returns an error the caller should close
 * the file destriptor it got from pthreadpool_restart_check_monitor_fd().
 *
 * @param[in]	pool		The pool to run the job on
 * @return			success: 0, failure: errno
 *
 * @see pthreadpool_restart_check_monitor_fd
 * @see pthreadpool_restart_check
 */
int pthreadpool_restart_check_monitor_drain(struct pthreadpool *pool);

/**
 * @brief Try to cancel a job in a pthreadpool
 *
 * This tries to cancel a job in a pthreadpool. The same
 * arguments, which were given to pthreadpool_add_job()
 * needs to be passed.
 *
 * The combination of id, fn, private_data might not be unique.
 * So the function tries to cancel as much matching jobs as possible.
 * Note once a job is scheduled in a thread it's to late to
 * cancel it.
 *
 * Canceled jobs that weren't started yet won't be reported via a
 * pool's signal_fn.
 *
 * @param[in]	pool		The pool to run the job on
 * @param[in]	job_id		A custom identifier
 * @param[in]	fn		The function to run asynchronously
 * @param[in]	private_data	Pointer passed to fn
 * @return			The number of canceled jobs
 *
 * @see pthreadpool_add_job()
 * @see pthreadpool_stop()
 * @see pthreadpool_destroy()
 */
size_t pthreadpool_cancel_job(struct pthreadpool *pool, int job_id,
			      void (*fn)(void *private_data), void *private_data);

#endif
