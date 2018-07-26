/*
 * Unix SMB/CIFS implementation.
 * thread pool implementation
 * Copyright (C) Volker Lendecke 2009
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

#include "replace.h"
#include "system/time.h"
#include "system/wait.h"
#include "system/threads.h"
#include "system/filesys.h"
#include "pthreadpool.h"
#include "lib/util/dlinklist.h"
#include "lib/util/blocking.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>

struct pthreadpool_job {
	int id;
	void (*fn)(void *private_data);
	void *private_data;
};

struct pthreadpool {
	/*
	 * List pthreadpools for fork safety
	 */
	struct pthreadpool *prev, *next;

	/*
	 * Control access to this struct
	 */
	pthread_mutex_t mutex;

	/*
	 * Threads waiting for work do so here
	 */
	pthread_cond_t condvar;

	int check_pipefd[2];

	/*
	 * Array of jobs
	 */
	size_t jobs_array_len;
	struct pthreadpool_job *jobs;

	size_t head;
	size_t num_jobs;

	/*
	 * Indicate job completion
	 */
	int (*signal_fn)(int jobid,
			 void (*job_fn)(void *private_data),
			 void *job_fn_private_data,
			 void *private_data);
	void *signal_fn_private_data;

	/*
	 * indicator to worker threads to stop processing further jobs
	 * and exit.
	 */
	bool stopped;

	/*
	 * indicator to the last worker thread to free the pool
	 * resources.
	 */
	bool destroyed;

	/*
	 * maximum number of threads
	 * 0 means no real thread, only strict sync processing.
	 */
	unsigned max_threads;

	/*
	 * Number of threads
	 */
	unsigned num_threads;

	/*
	 * Number of idle threads
	 */
	unsigned num_idle;

	/*
	 * Condition variable indicating that helper threads should
	 * quickly go away making way for fork() without anybody
	 * waiting on pool->condvar.
	 */
	pthread_cond_t *prefork_cond;

	/*
	 * Waiting position for helper threads while fork is
	 * running. The forking thread will have locked it, and all
	 * idle helper threads will sit here until after the fork,
	 * where the forking thread will unlock it again.
	 */
	pthread_mutex_t fork_mutex;

	bool per_thread_cwd;
};

static pthread_mutex_t pthreadpools_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct pthreadpool *pthreadpools = NULL;
static bool pthreadpool_support_thread_cwd = false;
static pthread_once_t pthreadpool_atfork_initialized = PTHREAD_ONCE_INIT;

static void pthreadpool_prep_atfork(void);

/*
 * Initialize a thread pool
 */

int pthreadpool_init(unsigned max_threads, struct pthreadpool **presult,
		     int (*signal_fn)(int jobid,
				      void (*job_fn)(void *private_data),
				      void *job_fn_private_data,
				      void *private_data),
		     void *signal_fn_private_data)
{
	struct pthreadpool *pool;
	int ret;
	bool ok;

	pool = (struct pthreadpool *)malloc(sizeof(struct pthreadpool));
	if (pool == NULL) {
		return ENOMEM;
	}
	pool->signal_fn = signal_fn;
	pool->signal_fn_private_data = signal_fn_private_data;

	pool->jobs_array_len = 4;
	pool->jobs = calloc(
		pool->jobs_array_len, sizeof(struct pthreadpool_job));

	if (pool->jobs == NULL) {
		free(pool);
		return ENOMEM;
	}

	ret = pipe(pool->check_pipefd);
	if (ret != 0) {
		free(pool->jobs);
		free(pool);
		return ENOMEM;
	}

	ok = smb_set_close_on_exec(pool->check_pipefd[0]);
	if (!ok) {
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return EINVAL;
	}
	ok = smb_set_close_on_exec(pool->check_pipefd[1]);
	if (!ok) {
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return EINVAL;
	}
	ret = set_blocking(pool->check_pipefd[0], true);
	if (ret == -1) {
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return EINVAL;
	}
	ret = set_blocking(pool->check_pipefd[1], false);
	if (ret == -1) {
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return EINVAL;
	}

	pool->head = pool->num_jobs = 0;

	ret = pthread_mutex_init(&pool->mutex, NULL);
	if (ret != 0) {
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return ret;
	}

	ret = pthread_cond_init(&pool->condvar, NULL);
	if (ret != 0) {
		pthread_mutex_destroy(&pool->mutex);
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return ret;
	}

	ret = pthread_mutex_init(&pool->fork_mutex, NULL);
	if (ret != 0) {
		pthread_cond_destroy(&pool->condvar);
		pthread_mutex_destroy(&pool->mutex);
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return ret;
	}

	pool->stopped = false;
	pool->destroyed = false;
	pool->num_threads = 0;
	pool->max_threads = max_threads;
	pool->num_idle = 0;
	pool->prefork_cond = NULL;
	if (max_threads != 0) {
		pool->per_thread_cwd = pthreadpool_support_thread_cwd;
	} else {
		pool->per_thread_cwd = false;
	}

	ret = pthread_mutex_lock(&pthreadpools_mutex);
	if (ret != 0) {
		pthread_mutex_destroy(&pool->fork_mutex);
		pthread_cond_destroy(&pool->condvar);
		pthread_mutex_destroy(&pool->mutex);
		close(pool->check_pipefd[0]);
		close(pool->check_pipefd[1]);
		free(pool->jobs);
		free(pool);
		return ret;
	}
	DLIST_ADD(pthreadpools, pool);

	ret = pthread_mutex_unlock(&pthreadpools_mutex);
	assert(ret == 0);

	pthread_once(&pthreadpool_atfork_initialized, pthreadpool_prep_atfork);

	*presult = pool;

	return 0;
}

size_t pthreadpool_max_threads(struct pthreadpool *pool)
{
	if (pool->stopped) {
		return 0;
	}

	return pool->max_threads;
}

size_t pthreadpool_queued_jobs(struct pthreadpool *pool)
{
	int res;
	int unlock_res;
	size_t ret;

	if (pool->stopped) {
		return 0;
	}

	res = pthread_mutex_lock(&pool->mutex);
	if (res != 0) {
		return res;
	}

	if (pool->stopped) {
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	ret = pool->num_jobs;

	unlock_res = pthread_mutex_unlock(&pool->mutex);
	assert(unlock_res == 0);
	return ret;
}

bool pthreadpool_per_thread_cwd(struct pthreadpool *pool)
{
	if (pool->stopped) {
		return false;
	}

	return pool->per_thread_cwd;
}

static void pthreadpool_prepare_pool(struct pthreadpool *pool)
{
	int ret;

	ret = pthread_mutex_lock(&pool->fork_mutex);
	assert(ret == 0);

	ret = pthread_mutex_lock(&pool->mutex);
	assert(ret == 0);

	while (pool->num_idle != 0) {
		unsigned num_idle = pool->num_idle;
		pthread_cond_t prefork_cond;

		ret = pthread_cond_init(&prefork_cond, NULL);
		assert(ret == 0);

		/*
		 * Push all idle threads off pool->condvar. In the
		 * child we can destroy the pool, which would result
		 * in undefined behaviour in the
		 * pthread_cond_destroy(pool->condvar). glibc just
		 * blocks here.
		 */
		pool->prefork_cond = &prefork_cond;

		ret = pthread_cond_signal(&pool->condvar);
		assert(ret == 0);

		while (pool->num_idle == num_idle) {
			ret = pthread_cond_wait(&prefork_cond, &pool->mutex);
			assert(ret == 0);
		}

		pool->prefork_cond = NULL;

		ret = pthread_cond_destroy(&prefork_cond);
		assert(ret == 0);
	}

	/*
	 * Probably it's well-defined somewhere: What happens to
	 * condvars after a fork? The rationale of pthread_atfork only
	 * writes about mutexes. So better be safe than sorry and
	 * destroy/reinit pool->condvar across a fork.
	 */

	ret = pthread_cond_destroy(&pool->condvar);
	assert(ret == 0);
}

static void pthreadpool_prepare(void)
{
	int ret;
	struct pthreadpool *pool;

	ret = pthread_mutex_lock(&pthreadpools_mutex);
	assert(ret == 0);

	pool = pthreadpools;

	while (pool != NULL) {
		pthreadpool_prepare_pool(pool);
		pool = pool->next;
	}
}

static void pthreadpool_parent(void)
{
	int ret;
	struct pthreadpool *pool;

	for (pool = DLIST_TAIL(pthreadpools);
	     pool != NULL;
	     pool = DLIST_PREV(pool)) {
		ret = pthread_cond_init(&pool->condvar, NULL);
		assert(ret == 0);
		ret = pthread_mutex_unlock(&pool->mutex);
		assert(ret == 0);
		ret = pthread_mutex_unlock(&pool->fork_mutex);
		assert(ret == 0);
	}

	ret = pthread_mutex_unlock(&pthreadpools_mutex);
	assert(ret == 0);
}

static void pthreadpool_child(void)
{
	int ret;
	struct pthreadpool *pool;

	for (pool = DLIST_TAIL(pthreadpools);
	     pool != NULL;
	     pool = DLIST_PREV(pool)) {

		pool->num_threads = 0;
		pool->num_idle = 0;
		pool->head = 0;
		pool->num_jobs = 0;
		pool->stopped = true;
		if (pool->check_pipefd[0] != -1) {
			close(pool->check_pipefd[0]);
			pool->check_pipefd[0] = -1;
		}
		if (pool->check_pipefd[1] != -1) {
			close(pool->check_pipefd[1]);
			pool->check_pipefd[1] = -1;
		}

		ret = pthread_cond_init(&pool->condvar, NULL);
		assert(ret == 0);

		ret = pthread_mutex_unlock(&pool->mutex);
		assert(ret == 0);

		ret = pthread_mutex_unlock(&pool->fork_mutex);
		assert(ret == 0);
	}

	ret = pthread_mutex_unlock(&pthreadpools_mutex);
	assert(ret == 0);
}

static void pthreadpool_prep_atfork(void)
{
#ifdef HAVE_UNSHARE_CLONE_FS
	int res;

	/* remember if unshare(CLONE_FS) works. */
	res = unshare(CLONE_FS);
	if (res == 0) {
		pthreadpool_support_thread_cwd = true;
	}
#endif

	pthread_atfork(pthreadpool_prepare, pthreadpool_parent,
		       pthreadpool_child);
}

static int pthreadpool_free(struct pthreadpool *pool)
{
	int ret, ret1, ret2;

	ret = pthread_mutex_lock(&pthreadpools_mutex);
	if (ret != 0) {
		return ret;
	}
	DLIST_REMOVE(pthreadpools, pool);
	ret = pthread_mutex_unlock(&pthreadpools_mutex);
	assert(ret == 0);

	ret = pthread_mutex_lock(&pool->mutex);
	assert(ret == 0);
	ret = pthread_mutex_unlock(&pool->mutex);
	assert(ret == 0);

	ret = pthread_mutex_destroy(&pool->mutex);
	ret1 = pthread_cond_destroy(&pool->condvar);
	ret2 = pthread_mutex_destroy(&pool->fork_mutex);

	if (ret != 0) {
		return ret;
	}
	if (ret1 != 0) {
		return ret1;
	}
	if (ret2 != 0) {
		return ret2;
	}

	if (pool->check_pipefd[0] != -1) {
		close(pool->check_pipefd[0]);
		pool->check_pipefd[0] = -1;
	}
	if (pool->check_pipefd[1] != -1) {
		close(pool->check_pipefd[1]);
		pool->check_pipefd[1] = -1;
	}
	free(pool->jobs);
	free(pool);

	return 0;
}

/*
 * Stop a thread pool. Wake up all idle threads for exit.
 */

static int pthreadpool_stop_locked(struct pthreadpool *pool)
{
	int ret;

	pool->stopped = true;

	if (pool->check_pipefd[0] != -1) {
		close(pool->check_pipefd[0]);
		pool->check_pipefd[0] = -1;
	}
	if (pool->check_pipefd[1] != -1) {
		close(pool->check_pipefd[1]);
		pool->check_pipefd[1] = -1;
	}

	if (pool->num_threads == 0) {
		return 0;
	}

	/*
	 * We have active threads, tell them to finish.
	 */

	ret = pthread_cond_broadcast(&pool->condvar);

	return ret;
}

/*
 * Stop a thread pool. Wake up all idle threads for exit.
 */

int pthreadpool_stop(struct pthreadpool *pool)
{
	int ret, ret1;

	ret = pthread_mutex_lock(&pool->mutex);
	if (ret != 0) {
		return ret;
	}

	if (!pool->stopped) {
		ret = pthreadpool_stop_locked(pool);
	}

	ret1 = pthread_mutex_unlock(&pool->mutex);
	assert(ret1 == 0);

	return ret;
}

/*
 * Destroy a thread pool. Wake up all idle threads for exit. The last
 * one will free the pool.
 */

int pthreadpool_destroy(struct pthreadpool *pool)
{
	int ret, ret1;
	bool free_it;

	assert(!pool->destroyed);

	ret = pthread_mutex_lock(&pool->mutex);
	if (ret != 0) {
		return ret;
	}

	pool->destroyed = true;

	if (!pool->stopped) {
		ret = pthreadpool_stop_locked(pool);
	}

	free_it = (pool->num_threads == 0);

	ret1 = pthread_mutex_unlock(&pool->mutex);
	assert(ret1 == 0);

	if (free_it) {
		pthreadpool_free(pool);
	}

	return ret;
}
/*
 * Prepare for pthread_exit(), pool->mutex must be locked and will be
 * unlocked here. This is a bit of a layering violation, but here we
 * also take care of removing the pool if we're the last thread.
 */
static void pthreadpool_server_exit(struct pthreadpool *pool)
{
	int ret;
	bool free_it;

	pool->num_threads -= 1;

	free_it = (pool->destroyed && (pool->num_threads == 0));

	while (true) {
		uint8_t c = 0;
		ssize_t nwritten = 0;

		if (pool->check_pipefd[1] == -1) {
			break;
		}

		nwritten = write(pool->check_pipefd[1], &c, 1);
		if (nwritten == -1) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				break;
			}
#ifdef EWOULDBLOCK
			if (errno == EWOULDBLOCK) {
				break;
			}
#endif
			/* ignore ... */
		}

		break;
	}

	ret = pthread_mutex_unlock(&pool->mutex);
	assert(ret == 0);

	if (free_it) {
		pthreadpool_free(pool);
	}
}

static bool pthreadpool_get_job(struct pthreadpool *p,
				struct pthreadpool_job *job)
{
	if (p->stopped) {
		return false;
	}

	if (p->num_jobs == 0) {
		return false;
	}
	*job = p->jobs[p->head];
	p->head = (p->head+1) % p->jobs_array_len;
	p->num_jobs -= 1;
	return true;
}

static bool pthreadpool_put_job(struct pthreadpool *p,
				int id,
				void (*fn)(void *private_data),
				void *private_data)
{
	struct pthreadpool_job *job;

	if (p->num_jobs == p->jobs_array_len) {
		struct pthreadpool_job *tmp;
		size_t new_len = p->jobs_array_len * 2;

		tmp = realloc(
			p->jobs, sizeof(struct pthreadpool_job) * new_len);
		if (tmp == NULL) {
			return false;
		}
		p->jobs = tmp;

		/*
		 * We just doubled the jobs array. The array implements a FIFO
		 * queue with a modulo-based wraparound, so we have to memcpy
		 * the jobs that are logically at the queue end but physically
		 * before the queue head into the reallocated area. The new
		 * space starts at the current jobs_array_len, and we have to
		 * copy everything before the current head job into the new
		 * area.
		 */
		memcpy(&p->jobs[p->jobs_array_len], p->jobs,
		       sizeof(struct pthreadpool_job) * p->head);

		p->jobs_array_len = new_len;
	}

	job = &p->jobs[(p->head + p->num_jobs) % p->jobs_array_len];
	job->id = id;
	job->fn = fn;
	job->private_data = private_data;

	p->num_jobs += 1;

	return true;
}

static void pthreadpool_undo_put_job(struct pthreadpool *p)
{
	p->num_jobs -= 1;
}

static void *pthreadpool_server(void *arg)
{
	struct pthreadpool *pool = (struct pthreadpool *)arg;
	int res;

#ifdef HAVE_UNSHARE_CLONE_FS
	if (pool->per_thread_cwd) {
		res = unshare(CLONE_FS);
		assert(res == 0);
	}
#endif

	res = pthread_mutex_lock(&pool->mutex);
	if (res != 0) {
		return NULL;
	}

	while (1) {
		struct timespec ts;
		struct pthreadpool_job job;

		/*
		 * idle-wait at most 1 second. If nothing happens in that
		 * time, exit this thread.
		 */

		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 1;

		while ((pool->num_jobs == 0) && !pool->stopped) {

			pool->num_idle += 1;
			res = pthread_cond_timedwait(
				&pool->condvar, &pool->mutex, &ts);
			pool->num_idle -= 1;

			if (pool->prefork_cond != NULL) {
				/*
				 * Me must allow fork() to continue
				 * without anybody waiting on
				 * &pool->condvar. Tell
				 * pthreadpool_prepare_pool that we
				 * got that message.
				 */

				res = pthread_cond_signal(pool->prefork_cond);
				assert(res == 0);

				res = pthread_mutex_unlock(&pool->mutex);
				assert(res == 0);

				/*
				 * pthreadpool_prepare_pool has
				 * already locked this mutex across
				 * the fork. This makes us wait
				 * without sitting in a condvar.
				 */
				res = pthread_mutex_lock(&pool->fork_mutex);
				assert(res == 0);
				res = pthread_mutex_unlock(&pool->fork_mutex);
				assert(res == 0);

				res = pthread_mutex_lock(&pool->mutex);
				assert(res == 0);
			}

			if (res == ETIMEDOUT) {

				if (pool->num_jobs == 0) {
					/*
					 * we timed out and still no work for
					 * us. Exit.
					 */
					pthreadpool_server_exit(pool);
					return NULL;
				}

				break;
			}
			assert(res == 0);
		}

		if (pthreadpool_get_job(pool, &job)) {
			int ret;

			/*
			 * Do the work with the mutex unlocked
			 */

			res = pthread_mutex_unlock(&pool->mutex);
			assert(res == 0);

			job.fn(job.private_data);

			ret = pool->signal_fn(job.id,
					      job.fn, job.private_data,
					      pool->signal_fn_private_data);

			res = pthread_mutex_lock(&pool->mutex);
			assert(res == 0);

			if (ret != 0) {
				pthreadpool_server_exit(pool);
				return NULL;
			}
		}

		if (pool->stopped) {
			/*
			 * we're asked to stop processing jobs, so exit
			 */
			pthreadpool_server_exit(pool);
			return NULL;
		}
	}
}

static int pthreadpool_create_thread(struct pthreadpool *pool)
{
	pthread_attr_t thread_attr;
	pthread_t thread_id;
	int res;
	sigset_t mask, omask;

	/*
	 * Create a new worker thread. It should not receive any signals.
	 */

	sigfillset(&mask);

	res = pthread_attr_init(&thread_attr);
	if (res != 0) {
		return res;
	}

	res = pthread_attr_setdetachstate(
		&thread_attr, PTHREAD_CREATE_DETACHED);
	if (res != 0) {
		pthread_attr_destroy(&thread_attr);
		return res;
	}

	res = pthread_sigmask(SIG_BLOCK, &mask, &omask);
	if (res != 0) {
		pthread_attr_destroy(&thread_attr);
		return res;
	}

	res = pthread_create(&thread_id, &thread_attr, pthreadpool_server,
			     (void *)pool);

	assert(pthread_sigmask(SIG_SETMASK, &omask, NULL) == 0);

	pthread_attr_destroy(&thread_attr);

	if (res == 0) {
		pool->num_threads += 1;
	}

	return res;
}

int pthreadpool_add_job(struct pthreadpool *pool, int job_id,
			void (*fn)(void *private_data), void *private_data)
{
	int res;
	int unlock_res;

	assert(!pool->destroyed);

	res = pthread_mutex_lock(&pool->mutex);
	if (res != 0) {
		return res;
	}

	if (pool->stopped) {
		/*
		 * Protect against the pool being shut down while
		 * trying to add a job
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return EINVAL;
	}

	if (pool->max_threads == 0) {
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);

		/*
		 * If no thread are allowed we do strict sync processing.
		 */
		fn(private_data);
		res = pool->signal_fn(job_id, fn, private_data,
				      pool->signal_fn_private_data);
		return res;
	}

	/*
	 * Add job to the end of the queue
	 */
	if (!pthreadpool_put_job(pool, job_id, fn, private_data)) {
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return ENOMEM;
	}

	if (pool->num_idle > 0) {
		/*
		 * We have idle threads, wake one.
		 */
		res = pthread_cond_signal(&pool->condvar);
		if (res != 0) {
			pthreadpool_undo_put_job(pool);
		}
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return res;
	}

	if (pool->num_threads >= pool->max_threads) {
		/*
		 * No more new threads, we just queue the request
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	res = pthreadpool_create_thread(pool);
	if (res == 0) {
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	if (pool->num_threads != 0) {
		/*
		 * At least one thread is still available, let
		 * that one run the queued job.
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	/*
	 * No thread could be created to run job, fallback to sync
	 * call.
	 */
	pthreadpool_undo_put_job(pool);

	unlock_res = pthread_mutex_unlock(&pool->mutex);
	assert(unlock_res == 0);

	return res;
}

int pthreadpool_restart_check(struct pthreadpool *pool)
{
	int res;
	int unlock_res;
	unsigned possible_threads = 0;
	unsigned missing_threads = 0;

	assert(!pool->destroyed);

	res = pthread_mutex_lock(&pool->mutex);
	if (res != 0) {
		return res;
	}

	if (pool->stopped) {
		/*
		 * Protect against the pool being shut down while
		 * trying to add a job
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return EINVAL;
	}

	if (pool->num_jobs == 0) {
		/*
		 * This also handles the pool->max_threads == 0 case as it never
		 * calls pthreadpool_put_job()
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	if (pool->num_idle > 0) {
		/*
		 * We have idle threads and pending jobs,
		 * this means we better let all threads
		 * start and check for pending jobs.
		 */
		res = pthread_cond_broadcast(&pool->condvar);
		assert(res == 0);
	}

	if (pool->num_threads < pool->max_threads) {
		possible_threads = pool->max_threads - pool->num_threads;
	}

	if (pool->num_idle < pool->num_jobs) {
		missing_threads = pool->num_jobs - pool->num_idle;
	}

	missing_threads = MIN(missing_threads, possible_threads);

	while (missing_threads > 0) {

		res = pthreadpool_create_thread(pool);
		if (res != 0) {
			break;
		}

		missing_threads--;
	}

	if (missing_threads == 0) {
		/*
		 * Ok, we recreated all thread we need.
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	if (pool->num_threads != 0) {
		/*
		 * At least one thread is still available, let
		 * that one run the queued jobs.
		 */
		unlock_res = pthread_mutex_unlock(&pool->mutex);
		assert(unlock_res == 0);
		return 0;
	}

	/*
	 * There's no thread available to run any pending jobs.
	 * The caller may want to cancel the jobs and destroy the pool.
	 * But that's up to the caller.
	 */
	unlock_res = pthread_mutex_unlock(&pool->mutex);
	assert(unlock_res == 0);

	return res;
}

int pthreadpool_restart_check_monitor_fd(struct pthreadpool *pool)
{
	int fd;
	int ret;
	bool ok;

	if (pool->stopped) {
		errno = EINVAL;
		return -1;
	}

	if (pool->check_pipefd[0] == -1) {
		errno = ENOSYS;
		return -1;
	}

	fd = dup(pool->check_pipefd[0]);
	if (fd == -1) {
		return -1;
	}

	ok = smb_set_close_on_exec(fd);
	if (!ok) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}

	ret = set_blocking(fd, false);
	if (ret == -1) {
		int saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}

	return fd;
}

int pthreadpool_restart_check_monitor_drain(struct pthreadpool *pool)
{
	if (pool->stopped) {
		return EINVAL;
	}

	if (pool->check_pipefd[0] == -1) {
		return ENOSYS;
	}

	while (true) {
		uint8_t buf[128];
		ssize_t nread;

		nread = read(pool->check_pipefd[0], buf, sizeof(buf));
		if (nread == -1) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				return 0;
			}
#ifdef EWOULDBLOCK
			if (errno == EWOULDBLOCK) {
				return 0;
			}
#endif
			if (errno == 0) {
				errno = INT_MAX;
			}

			return errno;
		}

		if (nread < sizeof(buf)) {
			return 0;
		}
	}

	abort();
	return INT_MAX;
}

size_t pthreadpool_cancel_job(struct pthreadpool *pool, int job_id,
			      void (*fn)(void *private_data), void *private_data)
{
	int res;
	size_t i, j;
	size_t num = 0;

	assert(!pool->destroyed);

	res = pthread_mutex_lock(&pool->mutex);
	if (res != 0) {
		return res;
	}

	for (i = 0, j = 0; i < pool->num_jobs; i++) {
		size_t idx = (pool->head + i) % pool->jobs_array_len;
		size_t new_idx = (pool->head + j) % pool->jobs_array_len;
		struct pthreadpool_job *job = &pool->jobs[idx];

		if ((job->private_data == private_data) &&
		    (job->id == job_id) &&
		    (job->fn == fn))
		{
			/*
			 * Just skip the entry.
			 */
			num++;
			continue;
		}

		/*
		 * If we already removed one or more jobs (so j will be smaller
		 * then i), we need to fill possible gaps in the logical list.
		 */
		if (j < i) {
			pool->jobs[new_idx] = *job;
		}
		j++;
	}

	pool->num_jobs -= num;

	res = pthread_mutex_unlock(&pool->mutex);
	assert(res == 0);

	return num;
}
