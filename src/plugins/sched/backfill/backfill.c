/*****************************************************************************\
 *  backfill.c - simple backfill scheduler plugin.
 *
 *  If a partition is does not have root only access and nodes are not shared
 *  then raise the priority of pending jobs if doing so does not adversely
 *  effect the expected initiation of any higher priority job. We do not alter
 *  a job's required or excluded node list, so this is a conservative
 *  algorithm.
 *
 *  For example, consider a cluster "lx[01-08]" with one job executing on
 *  nodes "lx[01-04]". The highest priority pending job requires five nodes
 *  including "lx05". The next highest priority pending job requires any
 *  three nodes. Without explicitly forcing the second job to use nodes
 *  "lx[06-08]", we can't start it without possibly delaying the higher
 *  priority job.
 *****************************************************************************
 *  Copyright (C) 2003-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2010 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris Jette <jette1@llnl.gov>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "slurm/slurm.h"
#include "slurm/slurm_errno.h"

#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/node_select.h"
#include "src/common/parse_time.h"
#include "src/common/read_config.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#include "src/slurmctld/acct_policy.h"
#include "src/slurmctld/front_end.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/licenses.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/node_scheduler.h"
#include "src/slurmctld/preempt.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/srun_comm.h"
#include "backfill.h"
#include "src/unittests_lib/tools.h"

#ifdef SLURM_SIMULATOR
#include <fcntl.h>           /* For O_* constants */
#include <sys/stat.h>        /* For mode constants */
#include <semaphore.h>
#include <pthread.h>
#include "src/common/slurm_sim.h"
#endif

#ifndef BACKFILL_INTERVAL
#  define BACKFILL_INTERVAL	30
#endif

#ifndef BACKFILL_RESOLUTION
#  define BACKFILL_RESOLUTION	60
#endif

/* Do not build job/resource/time record for more than this
 * far in the future, in seconds, currently one day */
#ifndef BACKFILL_WINDOW
#define   BACKFILL_WINDOW		(24 * 60 * 60)
#endif

/* Length of uid/njobs arrays used for limiting the number of jobs
 * per user considered in each backfill iteration */
#ifndef BF_MAX_USERS
#  define BF_MAX_USERS	1000
#endif

#define SLURMCTLD_THREAD_LIMIT	5

typedef struct node_space_map {
	time_t begin_time;
	time_t end_time;
	bitstr_t *avail_bitmap;
	int next;	/* next record, by time, zero termination */
} node_space_map_t;

/* Diag statistics */
extern diag_stats_t slurmctld_diag_stats;
int bf_last_yields = 0;

/*********************** local variables *********************/
static bool stop_backfill = false;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t term_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  term_cond = PTHREAD_COND_INITIALIZER;
static bool config_flag = false;
static uint32_t debug_flags = 0;
static int backfill_interval = BACKFILL_INTERVAL;
static int backfill_resolution = BACKFILL_RESOLUTION;
static int backfill_window = BACKFILL_WINDOW;
static int max_backfill_job_cnt = 100;
static int max_backfill_job_per_part = 0;
static int max_backfill_job_per_user = 0;
static int max_backfill_jobs_start = 0;
static bool backfill_continue = false;
static int defer_rpc_cnt = 0;

#ifdef SLURM_SIMULATOR
char SEM_NAME[] 	= "serversem";
sem_t* mutexserver	= SEM_FAILED;
#endif
/*********************** local functions *********************/
static void _add_reservation(uint32_t start_time, uint32_t end_reserve,
			     bitstr_t *res_bitmap,
			     node_space_map_t *node_space,
			     int *node_space_recs);
static int  _attempt_backfill(void);
static bool _job_is_completing(void);
static void _load_config(void);
static bool _many_pending_rpcs(void);
static bool _more_work(time_t last_backfill_time);
static void _my_sleep(int secs);
static int  _num_feature_count(struct job_record *job_ptr);
static void _reset_job_time_limit(struct job_record *job_ptr, time_t now,
				  node_space_map_t *node_space);
static int  _start_job(struct job_record *job_ptr, bitstr_t *avail_bitmap);
static bool _test_resv_overlap(node_space_map_t *node_space,
			       bitstr_t *use_bitmap, uint32_t start_time,
			       uint32_t end_reserve);
static int  _try_sched(struct job_record *job_ptr, bitstr_t **avail_bitmap,
		       uint32_t min_nodes, uint32_t max_nodes,
		       uint32_t req_nodes, bitstr_t *exc_core_bitmap);

/* Log recousrces to be allocated to a pending job */
static void _dump_job_sched(struct job_record *job_ptr, time_t end_time,
			    bitstr_t *avail_bitmap)
{
	char begin_buf[32], end_buf[32], *node_list;

	slurm_make_time_str(&job_ptr->start_time, begin_buf, sizeof(begin_buf));
	slurm_make_time_str(&end_time, end_buf, sizeof(end_buf));
	node_list = bitmap2node_name(avail_bitmap);
	info("Job %u to start at %s, end at %s on %s",
	     job_ptr->job_id, begin_buf, end_buf, node_list);
	xfree(node_list);
}

static void _dump_job_test(struct job_record *job_ptr, bitstr_t *avail_bitmap,
			   time_t start_time)
{
	char begin_buf[32], *node_list;

	if (start_time == 0)
		strcpy(begin_buf, "NOW");
	else
		slurm_make_time_str(&start_time, begin_buf, sizeof(begin_buf));
	node_list = bitmap2node_name(avail_bitmap);
	info("Test job %u at %s on %s", job_ptr->job_id, begin_buf, node_list);
	xfree(node_list);
}

/* Log resource allocate table */
static void _dump_node_space_table(node_space_map_t *node_space_ptr)
{
	int i = 0;
	char begin_buf[32], end_buf[32], *node_list;

	info("=========================================");
	while (1) {
		slurm_make_time_str(&node_space_ptr[i].begin_time,
				    begin_buf, sizeof(begin_buf));
		slurm_make_time_str(&node_space_ptr[i].end_time,
				    end_buf, sizeof(end_buf));
		node_list = bitmap2node_name(node_space_ptr[i].avail_bitmap);
		info("Begin:%s End:%s Nodes:%s",
		     begin_buf, end_buf, node_list);
		xfree(node_list);
		if ((i = node_space_ptr[i].next) == 0)
			break;
	}
	info("=========================================");
}

/*
 * _job_is_completing - Determine if jobs are in the process of completing.
 *	This is a variant of job_is_completing in slurmctld/job_scheduler.c.
 *	It always gives completing jobs at least 5 secs to complete.
 * RET - True if any job is in the process of completing
 */
static bool _job_is_completing(void)
{
	bool completing = false;
	ListIterator job_iterator;
	struct job_record *job_ptr = NULL;
	uint16_t complete_wait = slurm_get_complete_wait();
	time_t recent;

	if (job_list == NULL)
		return completing;

	recent = time(NULL) - MAX(complete_wait, 5);
	job_iterator = list_iterator_create(job_list);
	while ((job_ptr = (struct job_record *) list_next(job_iterator))) {
		if (IS_JOB_COMPLETING(job_ptr) &&
		    (job_ptr->end_time >= recent)) {
			completing = true;
			break;
		}
	}
	list_iterator_destroy(job_iterator);

	return completing;
}

/*
 * _many_pending_rpcs - Determine if slurmctld is busy with many active RPCs
 * RET - True if slurmctld currently has more than SLURMCTLD_THREAD_LIMIT
 *	 active RPCs
 */
static bool _many_pending_rpcs(void)
{
	//info("thread_count = %u", slurmctld_config.server_thread_count);
	if ((defer_rpc_cnt > 0) &&
	    (slurmctld_config.server_thread_count >= defer_rpc_cnt))
		return true;
	return false;
}

/* test if job has feature count specification */
static int _num_feature_count(struct job_record *job_ptr)
{
	struct job_details *detail_ptr = job_ptr->details;
	int rc = 0;
	ListIterator feat_iter;
	struct feature_record *feat_ptr;

	if (detail_ptr->feature_list == NULL)	/* no constraints */
		return rc;

	feat_iter = list_iterator_create(detail_ptr->feature_list);
	while ((feat_ptr = (struct feature_record *) list_next(feat_iter))) {
		if (feat_ptr->count)
			rc++;
	}
	list_iterator_destroy(feat_iter);

	return rc;
}

/* Attempt to schedule a specific job on specific available nodes
 * IN job_ptr - job to schedule
 * IN/OUT avail_bitmap - nodes available/selected to use
 * IN exc_core_bitmap - cores which can not be used
 * RET SLURM_SUCCESS on success, otherwise an error code
 */
static int  _try_sched(struct job_record *job_ptr, bitstr_t **avail_bitmap,
		       uint32_t min_nodes, uint32_t max_nodes,
		       uint32_t req_nodes, bitstr_t *exc_core_bitmap)
{
	bitstr_t *tmp_bitmap;
	int rc = SLURM_SUCCESS;
	int feat_cnt = _num_feature_count(job_ptr);
	List preemptee_candidates = NULL;
	List preemptee_job_list = NULL;

	if (feat_cnt) {
		/* Ideally schedule the job feature by feature,
		 * but I don't want to add that complexity here
		 * right now, so clear the feature counts and try
		 * to schedule. This will work if there is only
		 * one feature count. It should work fairly well
		 * in cases where there are multiple feature
		 * counts. */
		struct job_details *detail_ptr = job_ptr->details;
		ListIterator feat_iter;
		struct feature_record *feat_ptr;
		int i = 0, list_size;
		uint16_t *feat_cnt_orig = NULL, high_cnt = 0;

		/* Clear the feature counts */
		list_size = list_count(detail_ptr->feature_list);
		feat_cnt_orig = xmalloc(sizeof(uint16_t) * list_size);
		feat_iter = list_iterator_create(detail_ptr->feature_list);
		while ((feat_ptr =
			(struct feature_record *) list_next(feat_iter))) {
			high_cnt = MAX(high_cnt, feat_ptr->count);
			feat_cnt_orig[i++] = feat_ptr->count;
			feat_ptr->count = 0;
		}
		list_iterator_destroy(feat_iter);

		if ((job_req_node_filter(job_ptr, *avail_bitmap) !=
		     SLURM_SUCCESS) ||
		    (bit_set_count(*avail_bitmap) < high_cnt)) {
			rc = ESLURM_NODES_BUSY;
		} else {
			preemptee_candidates =
					slurm_find_preemptable_jobs(job_ptr);
			rc = select_g_job_test(job_ptr, *avail_bitmap,
					       high_cnt, max_nodes, req_nodes,
					       SELECT_MODE_WILL_RUN,
					       preemptee_candidates,
					       &preemptee_job_list,
					       exc_core_bitmap);
			if (preemptee_job_list) {
				list_destroy(preemptee_job_list);
				preemptee_job_list = NULL;
			}
		}

		/* Restore the feature counts */
		i = 0;
		feat_iter = list_iterator_create(detail_ptr->feature_list);
		while ((feat_ptr =
			(struct feature_record *) list_next(feat_iter))) {
			feat_ptr->count = feat_cnt_orig[i++];
		}
		list_iterator_destroy(feat_iter);
		xfree(feat_cnt_orig);
	} else {
		/* Try to schedule the job. First on dedicated nodes
		 * then on shared nodes (if so configured). */
		uint16_t orig_shared;
		time_t now = time(NULL);
		char str[100];

		preemptee_candidates = slurm_find_preemptable_jobs(job_ptr);
		orig_shared = job_ptr->details->share_res;
		job_ptr->details->share_res = 0;
		tmp_bitmap = bit_copy(*avail_bitmap);

		if (exc_core_bitmap) {
			bit_fmt(str, (sizeof(str) - 1), exc_core_bitmap);
			debug2(" _try_sched with exclude core bitmap: %s",str);
		}

		rc = select_g_job_test(job_ptr, *avail_bitmap, min_nodes,
				       max_nodes, req_nodes,
				       SELECT_MODE_WILL_RUN,
				       preemptee_candidates,
				       &preemptee_job_list,
				       exc_core_bitmap);
		if (preemptee_job_list) {
			list_destroy(preemptee_job_list);
			preemptee_job_list = NULL;
		}

		job_ptr->details->share_res = orig_shared;

		if (((rc != SLURM_SUCCESS) || (job_ptr->start_time > now)) &&
		    (orig_shared != 0)) {
			FREE_NULL_BITMAP(*avail_bitmap);
			*avail_bitmap = tmp_bitmap;
			rc = select_g_job_test(job_ptr, *avail_bitmap,
					       min_nodes, max_nodes, req_nodes,
					       SELECT_MODE_WILL_RUN,
					       preemptee_candidates,
					       &preemptee_job_list,
					       exc_core_bitmap);
			if (preemptee_job_list) {
				list_destroy(preemptee_job_list);
				preemptee_job_list = NULL;
			}
		} else
			FREE_NULL_BITMAP(tmp_bitmap);
	}

	if (preemptee_candidates)
		list_destroy(preemptee_candidates);
	return rc;

}

/* Terminate backfill_agent */
extern void stop_backfill_agent(void)
{
	pthread_mutex_lock(&term_lock);
	stop_backfill = true;
	pthread_cond_signal(&term_cond);
	pthread_mutex_unlock(&term_lock);
}

static void _my_sleep(int secs)
{
#ifndef SLURM_SIMULATOR
	struct timespec ts = {0, 0};

	ts.tv_sec = time(NULL) + secs;
	pthread_mutex_lock(&term_lock);
	if (!stop_backfill)
		pthread_cond_timedwait(&term_cond, &term_lock, &ts);
	pthread_mutex_unlock(&term_lock);

       /* For simulation purposes such a polite termintarion process is not necessary although it could be
          implemented as sleep wrapper does. By now just using a simple call to sleep here. */
#else
	/* Since the backfill and time controling loops are synced, we cannot make
	 * the sleep depend on "faked time", because it does not change while the
	 * backfilling is running... and _my_sleep is called form in there.
	 */
	usleep(10);
#endif
}

static void _load_config(void)
{
	char *sched_params, *tmp_ptr;

	sched_params = slurm_get_sched_params();
	debug_flags  = slurm_get_debug_flags();

	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_interval=")))
		backfill_interval = atoi(tmp_ptr + 12);
	if (backfill_interval < 1) {
		error("Invalid SchedulerParameters bf_interval: %d",
		      backfill_interval);
		backfill_interval = BACKFILL_INTERVAL;
	}

	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_window=")))
		backfill_window = atoi(tmp_ptr + 10) * 60;  /* mins to secs */
	if (backfill_window < 1) {
		error("Invalid SchedulerParameters bf_window: %d",
		      backfill_window);
		backfill_window = BACKFILL_WINDOW;
	}

	/* "max_job_bf" replaced by "bf_max_job_test" in version 14.03 and
	 * can be removed later. Only "bf_max_job_test" is documented. */
	if (sched_params && (tmp_ptr=strstr(sched_params, "max_job_bf=")))
		max_backfill_job_cnt = atoi(tmp_ptr + 11);
	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_max_job_test=")))
		max_backfill_job_cnt = atoi(tmp_ptr + 16);
	if (max_backfill_job_cnt < 1) {
		error("Invalid SchedulerParameters bf_max_job_test: %d",
		      max_backfill_job_cnt);
		max_backfill_job_cnt = 50;
	}
	/* "bf_res=" is vestigial from version 2.3 and can be removed later.
	 * Only "bf_resolution=" is documented. */
	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_res=")))
		backfill_resolution = atoi(tmp_ptr + 7);
	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_resolution=")))
		backfill_resolution = atoi(tmp_ptr + 14);
	if (backfill_resolution < 1) {
		error("Invalid SchedulerParameters bf_resolution: %d",
		      backfill_resolution);
		backfill_resolution = BACKFILL_RESOLUTION;
	}

	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_max_job_part=")))
		max_backfill_job_per_part = atoi(tmp_ptr + 16);
	if (max_backfill_job_per_part < 0) {
		error("Invalid SchedulerParameters bf_max_job_part: %d",
		      max_backfill_job_per_part);
		max_backfill_job_per_part = 0;
	}

	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_max_job_start=")))
		max_backfill_jobs_start = atoi(tmp_ptr + 17);
	if (max_backfill_jobs_start < 0) {
		error("Invalid SchedulerParameters bf_max_job_start: %d",
		      max_backfill_jobs_start);
		max_backfill_jobs_start = 0;
	}

	if (sched_params && (tmp_ptr=strstr(sched_params, "bf_max_job_user=")))
		max_backfill_job_per_user = atoi(tmp_ptr + 16);
	if (max_backfill_job_per_user < 0) {
		error("Invalid SchedulerParameters bf_max_job_user: %d",
		      max_backfill_job_per_user);
		max_backfill_job_per_user = 0;
	}

	/* bf_continue makes backfill continue where it was if interrupted
	 */
	if (sched_params && (strstr(sched_params, "bf_continue"))) {
		backfill_continue = true;
	}

	if (sched_params && (tmp_ptr=strstr(sched_params, "max_rpc_cnt=")))
		defer_rpc_cnt = atoi(tmp_ptr + 12);
	if (defer_rpc_cnt < 0) {
		error("Invalid SchedulerParameters max_rpc_cnt: %d",
		      defer_rpc_cnt);
		defer_rpc_cnt = 0;
	}

	xfree(sched_params);
}

/* Note that slurm.conf has changed */
extern void backfill_reconfig(void)
{
	config_flag = true;
}

static void _do_diag_stats(struct timeval *tv1, struct timeval *tv2,
			   int yield_sleep)
{
	uint32_t yield_sleep_usecs = yield_sleep * 1000000;
	uint32_t delta_t, real_time;

	delta_t  = (tv2->tv_sec  - tv1->tv_sec) * 1000000;
	delta_t +=  tv2->tv_usec - tv1->tv_usec;

	real_time = (delta_t - (bf_last_yields * yield_sleep_usecs));

	slurmctld_diag_stats.bf_cycle_counter++;
	slurmctld_diag_stats.bf_cycle_sum += real_time;
	slurmctld_diag_stats.bf_cycle_last = real_time;

	slurmctld_diag_stats.bf_depth_sum += slurmctld_diag_stats.bf_last_depth;
	slurmctld_diag_stats.bf_depth_try_sum +=
		slurmctld_diag_stats.bf_last_depth_try;
	if (slurmctld_diag_stats.bf_cycle_last >
	    slurmctld_diag_stats.bf_cycle_max) {
		slurmctld_diag_stats.bf_cycle_max = slurmctld_diag_stats.
						    bf_cycle_last;
	}

	slurmctld_diag_stats.bf_active = 0;
}

#ifdef SLURM_SIMULATOR
int
open_global_sync_sem() {
	int iter = 0;
	while(mutexserver == SEM_FAILED && iter < 10) {
		mutexserver = sem_open(SEM_NAME, 0, 0644, 0);
		if(mutexserver == SEM_FAILED) sleep(1);
		++iter;
	}

	if(mutexserver == SEM_FAILED)
		return -1;
	else
		return 0;
}

void
perform_global_sync() {
	while(*global_sync_flag < 2 || *global_sync_flag > 4) {
		usleep(100000);
	}
	sem_wait(mutexserver);
	*global_sync_flag += 1;
	if(*global_sync_flag > 4) {*global_sync_flag = 1;}
	sem_post(mutexserver);
}

void
close_global_sync_sem() {
	if(mutexserver != SEM_FAILED) sem_close(mutexserver);
}
#endif



#ifdef SLURM_SIMULATOR

char BF_SEM_NAME[] = "bf_sem";
char BF_DONE_SEM_NAME[] = "bf_done_sem";
sem_t* mutex_bf_pg=NULL;
sem_t* mutex_bf_done_pg=NULL;

int open_BF_sync_semaphore_pg() {
	mutex_bf_pg = sem_open(BF_SEM_NAME, O_CREAT, 0644, 0);
	if(mutex_bf_pg == SEM_FAILED) {
		error("unable to create backfill semaphore");
		sem_unlink(BF_SEM_NAME);
		return -1;
	}

	mutex_bf_done_pg = sem_open(BF_DONE_SEM_NAME, O_CREAT, 0644, 0);
	if(mutex_bf_done_pg == SEM_FAILED) {
		error("unable to create backfill done semaphore");
		sem_unlink(BF_DONE_SEM_NAME);
		return -1;
	}

	return 0;
}

void close_BF_sync_semaphore() {
	if(mutex_bf_pg != SEM_FAILED) sem_close(mutex_bf_pg);
	if(mutex_bf_done_pg != SEM_FAILED) sem_close(mutex_bf_done_pg);
}
#endif

/* backfill_agent - detached thread periodically attempts to backfill jobs */
extern void *backfill_agent(void *args)
{
	time_t now;
	double wait_time;
	static time_t last_backfill_time = 0;
	/* Read config and partitions; Write jobs and nodes */
	slurmctld_lock_t all_locks = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, READ_LOCK };

	open_global_sync_sem();
	_load_config();
	last_backfill_time = time(NULL);
#ifdef SLURM_SIMULATOR
	open_BF_sync_semaphore_pg();
	backfill_interval=2;
#endif
	while (!stop_backfill) {
#ifdef SLURM_SIMULATOR
		sem_wait(mutex_bf_pg);
#endif
#ifndef SLURM_SIMULATOR
		_my_sleep(backfill_interval);
#endif
		if (stop_backfill) {
			break;
		}
		if (config_flag) {
			config_flag = false;
			_load_config();
		}
		now = time(NULL);
		wait_time = difftime(now, last_backfill_time);
#ifndef SLURM_SIMULATOR
		if ((wait_time < backfill_interval) ||
		    _job_is_completing() || _many_pending_rpcs() ||
		    !avail_front_end(NULL) || !_more_work(last_backfill_time)) {
			continue;
		}
#endif
#ifdef SLURM_SIMULATOR
		if (!((wait_time < backfill_interval) ||
		    _job_is_completing() || _many_pending_rpcs() ||
		    !avail_front_end(NULL) || !_more_work(last_backfill_time))) {
			lock_slurmctld(all_locks);
			(void) _attempt_backfill();
			last_backfill_time = time(NULL);
			unlock_slurmctld(all_locks);
		}
		sem_post(mutex_bf_done_pg);
#endif

	}
#ifdef SLURM_SIMULATOR
	close_BF_sync_semaphore();
#endif
	perform_global_sync(); /* st on 20151020 */
	return NULL;
}

/* Return non-zero to break the backfill loop if change in job, node or
 * partition state or the backfill scheduler needs to be stopped. */
static int _yield_locks(int secs)
{
	slurmctld_lock_t all_locks = {
		READ_LOCK, WRITE_LOCK, WRITE_LOCK, READ_LOCK };
	time_t job_update, node_update, part_update;

	job_update  = last_job_update;
	node_update = last_node_update;
	part_update = last_part_update;

	unlock_slurmctld(all_locks);
	bf_last_yields++;
	_my_sleep(secs);
	lock_slurmctld(all_locks);

	if ((last_job_update  == job_update)  &&
	    (last_node_update == node_update) &&
	    (last_part_update == part_update) &&
	    (! stop_backfill) && (! config_flag))
		return 0;
	else
		return 1;
}

static int _attempt_backfill(void)
{
	DEF_TIMERS;
	bool filter_root = false;
	List job_queue;
	job_queue_rec_t *job_queue_rec;
	slurmdb_qos_rec_t *qos_ptr = NULL;
	int i, j, node_space_recs;
	struct job_record *job_ptr;
	struct part_record *part_ptr, **bf_part_ptr = NULL;
	uint32_t end_time, end_reserve;
	uint32_t time_limit, comp_time_limit, orig_time_limit, part_time_limit;
	uint32_t min_nodes, max_nodes, req_nodes;
	bitstr_t *avail_bitmap = NULL, *resv_bitmap = NULL;
	bitstr_t *exc_core_bitmap = NULL, *non_cg_bitmap = NULL;
	time_t now, sched_start, later_start, start_res, resv_end, window_end;
	node_space_map_t *node_space;
	struct timeval bf_time1, bf_time2;
	int sched_timeout = 2, yield_sleep = 1;
	int rc = 0;
	int job_test_count = 0;
	uint32_t *uid = NULL, nuser = 0, bf_parts = 0, *bf_part_jobs = NULL;
	uint16_t *njobs = NULL;
	bool already_counted;
#ifdef SLURM_SIMULATOR
       int local_loops;
#endif
	uint32_t reject_array_job_id = 0;
	struct part_record *reject_array_part = NULL;
	uint32_t job_start_cnt = 0, start_time;
	time_t config_update = slurmctld_conf.last_update;
	time_t part_update = last_part_update;

	bf_last_yields = 0;
#ifdef HAVE_ALPS_CRAY
	/*
	 * Run a Basil Inventory immediately before setting up the schedule
	 * plan, to avoid race conditions caused by ALPS node state change.
	 * Needs to be done with the node-state lock taken.
	 */
	START_TIMER;
	if (select_g_reconfigure()) {
		debug4("backfill: not scheduling due to ALPS");
		return SLURM_SUCCESS;
	}
	END_TIMER;
	if (debug_flags & DEBUG_FLAG_BACKFILL)
		info("backfill: ALPS inventory completed, %s", TIME_STR);

	/* The Basil inventory can take a long time to complete. Process
	 * pending RPCs before starting the backfill scheduling logic */
	_yield_locks(1);
#endif

	START_TIMER;
	if (debug_flags & DEBUG_FLAG_BACKFILL)
		info("backfill: beginning");
	else
		debug("backfill: beginning");
	sched_start = now = time(NULL);

	if (slurm_get_root_filter())
		filter_root = true;

	job_queue = build_job_queue(true, true);
	if (list_count(job_queue) == 0) {
		if (debug_flags & DEBUG_FLAG_BACKFILL)
			info("backfill: no jobs to backfill");
		else
			debug("backfill: no jobs to backfill");
		list_destroy(job_queue);
		return 0;
	}

	gettimeofday(&bf_time1, NULL);

	non_cg_bitmap = bit_copy(cg_node_bitmap);
	bit_not(non_cg_bitmap);

	slurmctld_diag_stats.bf_queue_len = list_count(job_queue);
	slurmctld_diag_stats.bf_queue_len_sum += slurmctld_diag_stats.
						 bf_queue_len;
	slurmctld_diag_stats.bf_last_depth = 0;
	slurmctld_diag_stats.bf_last_depth_try = 0;
	slurmctld_diag_stats.bf_when_last_cycle = now;
	slurmctld_diag_stats.bf_active = 1;

	node_space = xmalloc(sizeof(node_space_map_t) *
			     (max_backfill_job_cnt * 2 + 1));
	node_space[0].begin_time = sched_start;
	window_end = sched_start + backfill_window;
	node_space[0].end_time = window_end;
	node_space[0].avail_bitmap = bit_copy(avail_node_bitmap);
	node_space[0].next = 0;
	node_space_recs = 1;
	if (debug_flags & DEBUG_FLAG_BACKFILL_MAP)
		_dump_node_space_table(node_space);

	if (max_backfill_job_per_part) {
		ListIterator part_iterator;
		struct part_record *part_ptr;
		bf_parts = list_count(part_list);
		bf_part_ptr  = xmalloc(sizeof(struct part_record *) * bf_parts);
		bf_part_jobs = xmalloc(sizeof(int) * bf_parts);
		part_iterator = list_iterator_create(part_list);
		i = 0;
		while ((part_ptr = (struct part_record *)
				   list_next(part_iterator))) {
			bf_part_ptr[i++] = part_ptr;
		}
		list_iterator_destroy(part_iterator);
	}
	if (max_backfill_job_per_user) {
		uid = xmalloc(BF_MAX_USERS * sizeof(uint32_t));
		njobs = xmalloc(BF_MAX_USERS * sizeof(uint16_t));
	}
	sort_job_queue(job_queue);
	while (1) {
		job_queue_rec = (job_queue_rec_t *) list_pop(job_queue);
		if (!job_queue_rec) {
			if (debug_flags & DEBUG_FLAG_BACKFILL)
				info("backfill: reached end of job queue");
			break;
		}
		if (slurmctld_config.shutdown_time)
			break;
		if (((defer_rpc_cnt > 0) &&
		     (slurmctld_config.server_thread_count >= defer_rpc_cnt)) ||
		    ((time(NULL) - sched_start) >= sched_timeout)) {
			if (debug_flags & DEBUG_FLAG_BACKFILL) {
				END_TIMER;
				info("backfill: completed yielding locks "
				     "after testing %u(%d) jobs, %s",
				     slurmctld_diag_stats.bf_last_depth,
				     job_test_count, TIME_STR);
			}
			if ((_yield_locks(yield_sleep) && !backfill_continue) ||
			    (slurmctld_conf.last_update != config_update) ||
			    (last_part_update != part_update)) {
				if (debug_flags & DEBUG_FLAG_BACKFILL) {
					info("backfill: system state changed, "
					     "breaking out after testing "
					     "%u(%d) jobs",
					     slurmctld_diag_stats.bf_last_depth,
					     job_test_count);
				}
				rc = 1;
				xfree(job_queue_rec);
				break;
			}
			/* cg_node_bitmap may be changed */
			bit_copybits(non_cg_bitmap, cg_node_bitmap);
			bit_not(non_cg_bitmap);
			/* Reset backfill scheduling timers, resume testing */
			sched_start = time(NULL);
			job_test_count = 0;
			START_TIMER;
		}
		job_ptr  = job_queue_rec->job_ptr;
		/* With bf_continue configured, the original job could have
		 * been cancelled and purged. Validate pointer here. */
		if ((job_ptr->magic  != JOB_MAGIC) ||
		    (job_ptr->job_id != job_queue_rec->job_id)) {
			xfree(job_queue_rec);
			continue;
		}
		orig_time_limit = job_ptr->time_limit;
		part_ptr = job_queue_rec->part_ptr;

		job_test_count++;
		slurmctld_diag_stats.bf_last_depth++;
		already_counted = false;

		xfree(job_queue_rec);
		if (!IS_JOB_PENDING(job_ptr))
			continue;	/* started in another partition */
		if (job_ptr->preempt_in_progress)
			continue; 	/* scheduled in another partition */
		if (!avail_front_end(job_ptr))
			continue;	/* No available frontend for this job */
		if (job_ptr->array_task_id != NO_VAL) {
			if ((reject_array_job_id == job_ptr->array_job_id) &&
			    (reject_array_part   == part_ptr))
				continue;  /* already rejected array element */
			/* assume reject whole array for now, clear if OK */
			reject_array_job_id = job_ptr->array_job_id;
			reject_array_part   = part_ptr;
		}
		job_ptr->part_ptr = part_ptr;

		if (debug_flags & DEBUG_FLAG_BACKFILL) {
			info("backfill test for JobID=%u Prio=%u Partition=%s",
			     job_ptr->job_id, job_ptr->priority,
			     job_ptr->part_ptr->name);
		}

		if (max_backfill_job_per_part) {
			bool skip_job = false;
			for (j = 0; j < bf_parts; j++) {
				if (bf_part_ptr[j] != job_ptr->part_ptr)
					continue;
				if (bf_part_jobs[j]++ >=
				    max_backfill_job_per_part)
					skip_job = true;
				break;
			}
			if (skip_job) {
				if (debug_flags & DEBUG_FLAG_BACKFILL)
					info("backfill: have already "
					     "checked %u jobs for "
					     "partition %s; skipping "
					     "job %u",
					     max_backfill_job_per_part,
					     job_ptr->part_ptr->name,
					     job_ptr->job_id);
				continue;
			}
		}
		if (max_backfill_job_per_user) {
			for (j = 0; j < nuser; j++) {
				if (job_ptr->user_id == uid[j]) {
					njobs[j]++;
					if (debug_flags & DEBUG_FLAG_BACKFILL)
						debug("backfill: user %u: "
						      "#jobs %u",
						      uid[j], njobs[j]);
					break;
				}
			}
			if (j == nuser) { /* user not found */
				static bool bf_max_user_msg = true;
				if (nuser < BF_MAX_USERS) {
					uid[j] = job_ptr->user_id;
					njobs[j] = 1;
					nuser++;
				} else if (bf_max_user_msg) {
					bf_max_user_msg = false;
					error("backfill: too many users in "
					      "queue. Consider increasing "
					      "BF_MAX_USERS");
				}
				if (debug_flags & DEBUG_FLAG_BACKFILL)
					debug2("backfill: found new user %u. "
					       "Total #users now %u",
					       job_ptr->user_id, nuser);
			} else {
				if (njobs[j] >= max_backfill_job_per_user) {
					/* skip job */
					if (debug_flags & DEBUG_FLAG_BACKFILL)
						info("backfill: have already "
						     "checked %u jobs for "
						     "user %u; skipping "
						     "job %u",
						     max_backfill_job_per_user,
						     job_ptr->user_id,
						     job_ptr->job_id);
					continue;
				}
			}
		}

		if (((part_ptr->state_up & PARTITION_SCHED) == 0) ||
		    (part_ptr->node_bitmap == NULL) ||
		    ((part_ptr->flags & PART_FLAG_ROOT_ONLY) && filter_root)) {
			if (debug_flags & DEBUG_FLAG_BACKFILL)
				info("backfill: partition %s not usable",
				     job_ptr->part_ptr->name);
			continue;
		}

		if ((!job_independent(job_ptr, 0)) ||
		    (license_job_test(job_ptr, time(NULL)) != SLURM_SUCCESS)) {
			if (debug_flags & DEBUG_FLAG_BACKFILL)
				info("backfill: job %u not runable now",
				     job_ptr->job_id);
			continue;
		}

		/* Determine minimum and maximum node counts */
		min_nodes = MAX(job_ptr->details->min_nodes,
				part_ptr->min_nodes);
		if (job_ptr->details->max_nodes == 0)
			max_nodes = part_ptr->max_nodes;
		else
			max_nodes = MIN(job_ptr->details->max_nodes,
					part_ptr->max_nodes);
		max_nodes = MIN(max_nodes, 500000);     /* prevent overflows */
		if (job_ptr->details->max_nodes)
			req_nodes = max_nodes;
		else
			req_nodes = min_nodes;
		if (min_nodes > max_nodes) {
			if (debug_flags & DEBUG_FLAG_BACKFILL)
				info("backfill: job %u node count too high",
				     job_ptr->job_id);
			continue;
		}

		/* Determine job's expected completion time */
		if (part_ptr->max_time == INFINITE)
			part_time_limit = 365 * 24 * 60; /* one year */
		else
			part_time_limit = part_ptr->max_time;
		if (job_ptr->time_limit == NO_VAL) {
			time_limit = part_time_limit;
		} else {
			if (part_ptr->max_time == INFINITE)
				time_limit = job_ptr->time_limit;
			else
				time_limit = MIN(job_ptr->time_limit,
						 part_time_limit);
		}
		comp_time_limit = time_limit;
		qos_ptr = job_ptr->qos_ptr;
		if (qos_ptr && (qos_ptr->flags & QOS_FLAG_NO_RESERVE) &&
		    slurm_get_preempt_mode())
			time_limit = job_ptr->time_limit = 1;
		else if (job_ptr->time_min && (job_ptr->time_min < time_limit))
			time_limit = job_ptr->time_limit = job_ptr->time_min;

		/* Determine impact of any resource reservations */
		later_start = now;
 TRY_LATER:
		if (slurmctld_config.shutdown_time)
			break;
#ifndef SLURM_SIMULATOR
		if (((defer_rpc_cnt > 0) &&
		     (slurmctld_config.server_thread_count >= defer_rpc_cnt)) ||
		    ((time(NULL) - sched_start) >= sched_timeout)) {
#else
			if (local_loops == 20) {
#endif
			uint32_t save_job_id = job_ptr->job_id;
			uint32_t save_time_limit = job_ptr->time_limit;
			job_ptr->time_limit = orig_time_limit;
			if (debug_flags & DEBUG_FLAG_BACKFILL) {
				END_TIMER;
				info("backfill: completed yielding locks "
				     "after testing %u(%d) jobs, %s",
				     slurmctld_diag_stats.bf_last_depth,
				     job_test_count, TIME_STR);
			}
			if ((_yield_locks(yield_sleep) && !backfill_continue) ||
			    (slurmctld_conf.last_update != config_update) ||
			    (last_part_update != part_update)) {
				if (debug_flags & DEBUG_FLAG_BACKFILL) {
					info("backfill: system state changed, "
					     "breaking out after testing "
					     "%u(%d) jobs",
					     slurmctld_diag_stats.bf_last_depth,
					     job_test_count);
				}
				rc = 1;
				break;
			}
#ifdef SLURM_SIMULATOR
                       local_loops = 0;
#endif
			/* cg_node_bitmap may be changed */
			bit_copybits(non_cg_bitmap, cg_node_bitmap);
			bit_not(non_cg_bitmap);

			/* With bf_continue configured, the original job could
			 * have been scheduled or cancelled and purged.
			 * Revalidate job the record here. */
			if ((job_ptr->magic  != JOB_MAGIC) ||
			    (job_ptr->job_id != save_job_id))
				continue;
			if (!IS_JOB_PENDING(job_ptr))
				continue;
			if (!avail_front_end(job_ptr))
				continue;	/* No available frontend */

			job_ptr->time_limit = save_time_limit;
			/* Reset backfill scheduling timers, resume testing */
			sched_start = time(NULL);
			job_test_count = 1;
			START_TIMER;
		}
#ifdef SLURM_SIMULATOR
               local_loops++;
#endif
		FREE_NULL_BITMAP(avail_bitmap);
		FREE_NULL_BITMAP(exc_core_bitmap);
		start_res   = later_start;
		later_start = 0;
		j = job_test_resv(job_ptr, &start_res, true, &avail_bitmap,
				  &exc_core_bitmap);
		if (j != SLURM_SUCCESS) {
			if (debug_flags & DEBUG_FLAG_BACKFILL)
				info("backfill: job %u reservation defer",
				     job_ptr->job_id);
			job_ptr->time_limit = orig_time_limit;
			continue;
		}
		if (start_res > now)
			end_time = (time_limit * 60) + start_res;
		else
			end_time = (time_limit * 60) + now;
		resv_end = find_resv_end(start_res);
		/* Identify usable nodes for this job */
		bit_and(avail_bitmap, part_ptr->node_bitmap);
		bit_and(avail_bitmap, up_node_bitmap);
		bit_and(avail_bitmap, non_cg_bitmap);
		for (j=0; ; ) {
			if ((node_space[j].end_time > start_res) &&
			     node_space[j].next && (later_start == 0))
				later_start = node_space[j].end_time;
			if (node_space[j].end_time <= start_res)
				;
			else if (node_space[j].begin_time <= end_time) {
				bit_and(avail_bitmap,
					node_space[j].avail_bitmap);
			} else
				break;
			if ((j = node_space[j].next) == 0)
				break;
		}
		if (resv_end && (++resv_end < window_end) &&
		    ((later_start == 0) || (resv_end < later_start))) {
			later_start = resv_end;
		}

		if (job_ptr->details->exc_node_bitmap) {
			bit_not(job_ptr->details->exc_node_bitmap);
			bit_and(avail_bitmap,
				job_ptr->details->exc_node_bitmap);
			bit_not(job_ptr->details->exc_node_bitmap);
		}

		/* Test if insufficient nodes remain OR
		 *	required nodes missing OR
		 *	nodes lack features OR
		 *	no change since previously tested nodes (only changes
		 *	in other partition nodes) */
		if ((bit_set_count(avail_bitmap) < min_nodes) ||
		    ((job_ptr->details->req_node_bitmap) &&
		     (!bit_super_set(job_ptr->details->req_node_bitmap,
				     avail_bitmap))) ||
		    (job_req_node_filter(job_ptr, avail_bitmap))) {
#ifndef SLURM_SIMULATOR
			if (later_start) {
				job_ptr->start_time = 0;
				goto TRY_LATER;
			}
#endif

			/* Job can not start until too far in the future */
			job_ptr->time_limit = orig_time_limit;
			job_ptr->start_time = sched_start + backfill_window;
			continue;
		}

		/* Identify nodes which are definitely off limits */
		FREE_NULL_BITMAP(resv_bitmap);
		resv_bitmap = bit_copy(avail_bitmap);
		bit_not(resv_bitmap);

		/* this is the time consuming operation */
		debug2("backfill: entering _try_sched for job %u.",
		       job_ptr->job_id);

		if (!already_counted) {
			slurmctld_diag_stats.bf_last_depth_try++;
			already_counted = true;
		}

		if (debug_flags & DEBUG_FLAG_BACKFILL_MAP)
			_dump_job_test(job_ptr, avail_bitmap, start_res);
		j = _try_sched(job_ptr, &avail_bitmap, min_nodes, max_nodes,
			       req_nodes, exc_core_bitmap);

		now = time(NULL);
		if (j != SLURM_SUCCESS) {
			job_ptr->time_limit = orig_time_limit;
			job_ptr->start_time = 0;
			continue;	/* not runable */
		}

		if (start_res > job_ptr->start_time) {
			job_ptr->start_time = start_res;
			last_job_update = now;
		}
		if (job_ptr->start_time <= now) {	/* Can start now */
			uint32_t save_time_limit = job_ptr->time_limit;
			uint32_t hard_limit;
			bool reset_time = false;
			int rc = _start_job(job_ptr, resv_bitmap);
			if (qos_ptr && (qos_ptr->flags & QOS_FLAG_NO_RESERVE)) {
				if (orig_time_limit == NO_VAL) {
					acct_policy_alter_job(
						job_ptr, comp_time_limit);
					job_ptr->time_limit = comp_time_limit;
				} else {
					acct_policy_alter_job(
						job_ptr, orig_time_limit);
					job_ptr->time_limit = orig_time_limit;
				}
			} else if ((rc == SLURM_SUCCESS) && job_ptr->time_min) {
				/* Set time limit as high as possible */
				acct_policy_alter_job(job_ptr, comp_time_limit);
				job_ptr->time_limit = comp_time_limit;
				reset_time = true;
			} else if (orig_time_limit == NO_VAL) {
				acct_policy_alter_job(job_ptr, comp_time_limit);
				job_ptr->time_limit = comp_time_limit;
			} else {
				acct_policy_alter_job(job_ptr, orig_time_limit);
				job_ptr->time_limit = orig_time_limit;

			}
			if (job_ptr->time_limit == INFINITE)
				hard_limit = 365 * 24 * 60;	/* one year */
			else
				hard_limit = job_ptr->time_limit;
			job_ptr->end_time = job_ptr->start_time +
					    (hard_limit * 60);
			if (reset_time) {
				_reset_job_time_limit(job_ptr, now,
						      node_space);
				time_limit = job_ptr->time_limit;
			}

			if (rc == ESLURM_ACCOUNTING_POLICY) {
				/* Unknown future start time, just skip job */
				job_ptr->start_time = 0;
				continue;
			} else if (rc != SLURM_SUCCESS) {
				if (debug_flags & DEBUG_FLAG_BACKFILL) {
					info("backfill: planned start of job %u"
					     " failed: %s", job_ptr->job_id,
					     slurm_strerror(rc));
				}
				/* Drop through and reserve these resources.
				 * Likely due to state changes during sleep.
				 * Make best-effort based upon original state */
				job_ptr->time_limit = orig_time_limit;
				later_start = 0;
			} else {
				/* Started this job, move to next one */
				reject_array_job_id = 0;
				reject_array_part   = NULL;

				/* Update the database if job time limit
				 * changed and move to next job */
				if (save_time_limit != job_ptr->time_limit)
					jobacct_storage_g_job_start(acct_db_conn,
								    job_ptr);
				job_start_cnt++;
				if (max_backfill_jobs_start &&
				    (job_start_cnt >= max_backfill_jobs_start)){
					if (debug_flags & DEBUG_FLAG_BACKFILL) {
						info("backfill: bf_max_job_start"
						     " limit of %d reached",
						     max_backfill_jobs_start);
					}
					break;
				}
				continue;
			}
		} else {
			job_ptr->time_limit = orig_time_limit;
		}

		start_time  = job_ptr->start_time;
		end_reserve = job_ptr->start_time + (time_limit * 60);
		start_time  = (start_time / backfill_resolution) *
			      backfill_resolution;
		end_reserve = (end_reserve / backfill_resolution) *
			      backfill_resolution;
#ifndef SLURM_SIMULATOR
		if (later_start && (start_time > later_start)) {
			/* Try later when some nodes currently reserved for
			 * pending jobs are free */
			job_ptr->start_time = 0;
			goto TRY_LATER;
		}
#endif

		if (job_ptr->start_time > (sched_start + backfill_window)) {
			/* Starts too far in the future to worry about */
			if (debug_flags & DEBUG_FLAG_BACKFILL)
				_dump_job_sched(job_ptr, end_reserve,
						avail_bitmap);
			continue;
		}

		if (node_space_recs >= max_backfill_job_cnt) {
			if (debug_flags & DEBUG_FLAG_BACKFILL) {
				info("backfill: table size limit of %u reached",
				     max_backfill_job_cnt);
			}
			break;
		}

#ifndef SLURM_SIMULATOR
		if ((job_ptr->start_time > now) &&
		    _test_resv_overlap(node_space, avail_bitmap,
				       start_time, end_reserve)) {
			/* This job overlaps with an existing reservation for
			 * job to be backfill scheduled, which the sched
			 * plugin does not know about. Try again later. */
			later_start = job_ptr->start_time;
			job_ptr->start_time = 0;
			goto TRY_LATER;
		}
#endif

		/*
		 * Add reservation to scheduling table if appropriate
		 */
		if (debug_flags & DEBUG_FLAG_BACKFILL)
			_dump_job_sched(job_ptr, end_reserve, avail_bitmap);
		if (qos_ptr && (qos_ptr->flags & QOS_FLAG_NO_RESERVE))
			continue;
		reject_array_job_id = 0;
		reject_array_part   = NULL;
		bit_not(avail_bitmap);
		_add_reservation(start_time, end_reserve,
				 avail_bitmap, node_space, &node_space_recs);
		if (debug_flags & DEBUG_FLAG_BACKFILL_MAP)
			_dump_node_space_table(node_space);
	}
	xfree(bf_part_jobs);
	xfree(bf_part_ptr);
	xfree(uid);
	xfree(njobs);
	FREE_NULL_BITMAP(avail_bitmap);
	FREE_NULL_BITMAP(exc_core_bitmap);
	FREE_NULL_BITMAP(resv_bitmap);
	FREE_NULL_BITMAP(non_cg_bitmap);

	for (i=0; ; ) {
		FREE_NULL_BITMAP(node_space[i].avail_bitmap);
		if ((i = node_space[i].next) == 0)
			break;
	}
	xfree(node_space);
	list_destroy(job_queue);
	gettimeofday(&bf_time2, NULL);
	_do_diag_stats(&bf_time1, &bf_time2, yield_sleep);
	if (debug_flags & DEBUG_FLAG_BACKFILL) {
		END_TIMER;
		info("backfill: completed testing %u(%d) jobs, %s",
		     slurmctld_diag_stats.bf_last_depth,
		     job_test_count, TIME_STR);
	}
	return rc;
}

/* Try to start the job on any non-reserved nodes */
static int _start_job(struct job_record *job_ptr, bitstr_t *resv_bitmap)
{
	int rc;
	bitstr_t *orig_exc_nodes = NULL;
	static uint32_t fail_jobid = 0;

	if (job_ptr->details->exc_node_bitmap) {
		orig_exc_nodes = bit_copy(job_ptr->details->exc_node_bitmap);
		bit_or(job_ptr->details->exc_node_bitmap, resv_bitmap);
	} else
		job_ptr->details->exc_node_bitmap = bit_copy(resv_bitmap);

	rc = select_nodes(job_ptr, false, NULL);
	if (job_ptr->details) { /* select_nodes() might cancel the job! */
		FREE_NULL_BITMAP(job_ptr->details->exc_node_bitmap);
		job_ptr->details->exc_node_bitmap = orig_exc_nodes;
	} else
		FREE_NULL_BITMAP(orig_exc_nodes);
	if (rc == SLURM_SUCCESS) {
		/* job initiated */
		last_job_update = time(NULL);
		info("backfill: Started JobId=%u on %s",
		     job_ptr->job_id, job_ptr->nodes);
		if (job_ptr->batch_flag == 0)
			srun_allocate(job_ptr->job_id);
		else if ((job_ptr->details == NULL) ||
			 (job_ptr->details->prolog_running == 0))
			launch_job(job_ptr);
		slurmctld_diag_stats.backfilled_jobs++;
		slurmctld_diag_stats.last_backfilled_jobs++;
		if (debug_flags & DEBUG_FLAG_BACKFILL) {
			info("backfill: Jobs backfilled since boot: %u",
			     slurmctld_diag_stats.backfilled_jobs);
		}
	} else if ((job_ptr->job_id != fail_jobid) &&
		   (rc != ESLURM_ACCOUNTING_POLICY)) {
		char *node_list;
		bit_not(resv_bitmap);
		node_list = bitmap2node_name(resv_bitmap);
		/* This happens when a job has sharing disabled and
		 * a selected node is still completing some job,
		 * which should be a temporary situation. */
		verbose("backfill: Failed to start JobId=%u on %s: %s",
			job_ptr->job_id, node_list, slurm_strerror(rc));
		xfree(node_list);
		fail_jobid = job_ptr->job_id;
	} else {
		debug3("backfill: Failed to start JobId=%u: %s",
		       job_ptr->job_id, slurm_strerror(rc));
	}

	return rc;
}

/* Reset a job's time limit (and end_time) as high as possible
 *	within the range job_ptr->time_min and job_ptr->time_limit.
 *	Avoid using resources reserved for pending jobs or in resource
 *	reservations */
static void _reset_job_time_limit(struct job_record *job_ptr, time_t now,
				  node_space_map_t *node_space)
{
	int32_t j, resv_delay;
	uint32_t orig_time_limit = job_ptr->time_limit;
	uint32_t new_time_limit;

	for (j=0; ; ) {
		if ((node_space[j].begin_time != now) &&
		    (node_space[j].begin_time < job_ptr->end_time) &&
		    (!bit_super_set(job_ptr->node_bitmap,
				    node_space[j].avail_bitmap))) {
			/* Job overlaps pending job's resource reservation */
			resv_delay = difftime(node_space[j].begin_time, now);
			resv_delay /= 60;	/* seconds to minutes */
			if (resv_delay < job_ptr->time_limit)
				job_ptr->time_limit = resv_delay;
		}
		if ((j = node_space[j].next) == 0)
			break;
	}
	new_time_limit = MAX(job_ptr->time_min, job_ptr->time_limit);
	acct_policy_alter_job(job_ptr, new_time_limit);
	job_ptr->time_limit = new_time_limit;
	job_ptr->end_time = job_ptr->start_time + (job_ptr->time_limit * 60);

	job_time_adj_resv(job_ptr);

	if (orig_time_limit != job_ptr->time_limit) {
		info("backfill: job %u time limit changed from %u to %u",
		     job_ptr->job_id, orig_time_limit, job_ptr->time_limit);
	}
}

/* Report if any changes occurred to job, node or partition information */
static bool _more_work (time_t last_backfill_time)
{
	bool rc = false;

	pthread_mutex_lock( &thread_flag_mutex );
	if ( (last_job_update  >= last_backfill_time ) ||
	     (last_node_update >= last_backfill_time ) ||
	     (last_part_update >= last_backfill_time ) ) {
		rc = true;
	}
	pthread_mutex_unlock( &thread_flag_mutex );
	return rc;
}

/* Create a reservation for a job in the future */
static void _add_reservation(uint32_t start_time, uint32_t end_reserve,
			     bitstr_t *res_bitmap,
			     node_space_map_t *node_space,
			     int *node_space_recs)
{
	bool placed = false;
	int i, j;

#if 0	
	info("add job start:%u end:%u", start_time, end_reserve);
	for (j = 0; ; ) {
		info("node start:%u end:%u",
		     (uint32_t) node_space[j].begin_time,
		     (uint32_t) node_space[j].end_time);
		if ((j = node_space[j].next) == 0)
			break;
	}
#endif

	start_time = MAX(start_time, node_space[0].begin_time);
	for (j = 0; ; ) {
		if (node_space[j].end_time > start_time) {
			/* insert start entry record */
			i = *node_space_recs;
			node_space[i].begin_time = start_time;
			node_space[i].end_time = node_space[j].end_time;
			node_space[j].end_time = start_time;
			node_space[i].avail_bitmap =
				bit_copy(node_space[j].avail_bitmap);
			node_space[i].next = node_space[j].next;
			node_space[j].next = i;
			(*node_space_recs)++;
			placed = true;
		}
		if (node_space[j].end_time == start_time) {
			/* no need to insert new start entry record */
			placed = true;
		}
		if (placed == true) {
			while ((j = node_space[j].next)) {
				if (end_reserve < node_space[j].end_time) {
					/* insert end entry record */
					i = *node_space_recs;
					node_space[i].begin_time = end_reserve;
					node_space[i].end_time = node_space[j].
								 end_time;
					node_space[j].end_time = end_reserve;
					node_space[i].avail_bitmap =
						bit_copy(node_space[j].
							 avail_bitmap);
					node_space[i].next = node_space[j].next;
					node_space[j].next = i;
					(*node_space_recs)++;
					break;
				}
				if (end_reserve == node_space[j].end_time) {
					break;
				}
			}
			break;
		}
		if ((j = node_space[j].next) == 0)
			break;
	}

	for (j = 0; ; ) {
		if ((node_space[j].begin_time >= start_time) &&
		    (node_space[j].end_time <= end_reserve))
			bit_and(node_space[j].avail_bitmap, res_bitmap);
		if ((node_space[j].begin_time >= end_reserve) ||
		    ((j = node_space[j].next) == 0))
			break;
	}

	/* Drop records with identical bitmaps (up to one record).
	 * This can significantly improve performance of the backfill tests. */
	for (i = 0; ; ) {
		if ((j = node_space[i].next) == 0)
			break;
		if (!bit_equal(node_space[i].avail_bitmap,
			       node_space[j].avail_bitmap)) {
			i = j;
			continue;
		}
		node_space[i].end_time = node_space[j].end_time;
		node_space[i].next = node_space[j].next;
		FREE_NULL_BITMAP(node_space[j].avail_bitmap);
		break;
	}
}

/*
 * Determine if the resource specification for a new job overlaps with a
 *	reservation that the backfill scheduler has made for a job to be
 *	started in the future.
 * IN use_bitmap - nodes to be allocated
 * IN start_time - start time of job
 * IN end_reserve - end time of job
 */
static bool _test_resv_overlap(node_space_map_t *node_space,
			       bitstr_t *use_bitmap, uint32_t start_time,
			       uint32_t end_reserve)
{
	bool overlap = false;
	int j;

	for (j=0; ; ) {
		if ((node_space[j].end_time   > start_time) &&
		    (node_space[j].begin_time < end_reserve) &&
		    (!bit_super_set(use_bitmap, node_space[j].avail_bitmap))) {
			overlap = true;
			break;
		}
		if ((j = node_space[j].next) == 0)
			break;
	}
	return overlap;
}
