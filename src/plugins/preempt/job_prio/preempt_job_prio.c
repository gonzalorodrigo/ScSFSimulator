/*****************************************************************************\
 *  preempt_job_prio.c
 *
 *  DESCRIPTION: This plugin enables the selection of preemptable jobs based
 *  upon their priority, the amount resources used under an account
 *  (optionally), the runtime of the job and its account (i.e. accounts not
 *  finishing with _p can be preempted...)
 *
 *  OPTIONS: The following constants can be set to modify the plugin's behavior:
 *
 *  CHECK_FOR_PREEMPTOR_OVERALLOC: If set to 1, overallocation of the
 *  preemptor's account will prevent preemption for the benefit of that job.
 *  E.g. if running this jobs will create an overallocation of an account, the
 *  preemptees creating this situation will be removed for the preemption
 *  candidates.
 *
 *  CHECK_FOR_ACCOUNT_UNDERALLOC: If set to 1, underallocation of a preemptee's
 *  account will prevents its preemption. E.g. if preempting a job reduces the
 *  usage of its account below its allocated share, it will be removed from the
 *  candidates.
 *****************************************************************************
 *  Copyright (C) 2009-2010 Lawrence Livermore National Security.
 *  Portions Copyright (C) 2010 SchedMD <http://www.schedmd.com>.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Morris jette <jette1@llnl.gov>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://www.schedmd.com/slurmdocs/>.
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
#include <math.h>
#include <stdio.h>

#include "slurm/slurm_errno.h"

#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/plugin.h"
#include "src/common/slurm_accounting_storage.h"
#include "src/common/slurm_priority.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"


/* If the #define options listed below for CHECK_FOR_PREEMPTOR_OVERALLOC and
 * CHECK_FOR_ACCOUNT_UNDERALLOC are commented out, this plugin will work as a
 * simple job priority based preemption plugin. */
#define CHECK_FOR_PREEMPTOR_OVERALLOC 0
#define CHECK_FOR_ACCOUNT_UNDERALLOC  0

const char  plugin_name[]   = "Preempt by Job Priority and Runtime";
const char  plugin_type[]   = "preempt/job_prio";
const uint32_t  plugin_version  = 100;

/* The acct_usage_element data structure holds informaiton about
 * an association's current usage and current CPU count*/
typedef struct
{
	uint32_t *id;
	double *current_usage;
	uint32_t *current_cpu_count;
} acct_usage_element;


/*****End of plugin specific declarations**********************************/

/* Destroy a acct_usage_element data structure element. */
static void _destroy_acct_usage_element(void *object)
{
	acct_usage_element *tmp = (acct_usage_element *)object;
	xfree(tmp->id);
	xfree(tmp->current_usage);
	xfree(tmp->current_cpu_count);
	xfree(tmp);
}

/* Find the matching association ID in usage_acct_list List. */
static int _find_acct_usage_list_entry(void *x, void *key)
{
	acct_usage_element *element_ptr = (acct_usage_element *) x;
	uint32_t *keyid = (uint32_t*)key;

	if (*(element_ptr->id) == *keyid)
		return 1;
	return 0;
}

/* Code taken from job_info.c calculate cummulative run time for a job */
static time_t _get_job_runtime(struct job_record *job_ptr)
{
	time_t end_time, run_time;

	if (IS_JOB_PENDING(job_ptr))
		run_time = 0;
	else if (IS_JOB_SUSPENDED(job_ptr))
		run_time = job_ptr->pre_sus_time;
	else {
		if (IS_JOB_RUNNING(job_ptr) || (job_ptr->end_time == 0))
			end_time = time(NULL);
		else
			end_time = job_ptr->end_time;
		if (job_ptr->suspend_time) {
			run_time = (time_t)
				   (difftime(end_time, job_ptr->suspend_time)
				    + job_ptr->pre_sus_time);
		} else {
			run_time = (time_t)
				   difftime(end_time, job_ptr->start_time);
		}
	}

	return run_time;
}

/* Return true of the cummulative run time of job1 is greater than job 2 */
static bool _is_job_runtime_greater(struct job_record *job_ptr1,
				    struct job_record *job_ptr2)
{
	time_t runtime_job1, runtime_job2;
	double timediff_job1_job2 = 0.0;

	runtime_job1 = _get_job_runtime(job_ptr1);
	runtime_job2 = _get_job_runtime(job_ptr2);
	timediff_job1_job2 = difftime(runtime_job1, runtime_job2);

	if (timediff_job1_job2 > 0) {
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
    			info("%s: Runtime of JobId %u > JobId %u (%u > %u)",
			     plugin_type, job_ptr1->job_id, job_ptr2->job_id,
			     (uint32_t) runtime_job1, (uint32_t) runtime_job2);
		}
		return true;
	} else {
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
    			info("%s: Runtime of JobId %u <= JobId %u (%u <= %u)",
			     plugin_type, job_ptr1->job_id, job_ptr2->job_id,
			     (uint32_t) runtime_job1, (uint32_t) runtime_job2);
		}
		return false;
	}
}

/* This _get_nb_cpus function is greatly inspired from the Job_Size calculation
 * in job_manager.c, but reused here to find out the requested resources. As
 * stated in the comment of the Job_Size calculation, the first scheduling run
 * may not have the actual total_cpus so we start by using the amount requested.
 * Then the actual required cpus will be filled in. This function estimates
 * the future value of total_cpus if it is not set.
 */
static int _get_nb_cpus(struct job_record *job_ptr)
{
	uint32_t cpu_cnt = 0;
	uint32_t min_nodes = 0;
	uint32_t max_nodes = 0;
	uint32_t req_nodes = 0;
	uint32_t cpus_per_node;

	cpus_per_node = (uint32_t) job_ptr->part_ptr->total_cpus /
			job_ptr->part_ptr->total_nodes;
	min_nodes = MAX(job_ptr->details->min_nodes,
			job_ptr->part_ptr->min_nodes);

	if (job_ptr->details->max_nodes == 0) {
		max_nodes = job_ptr->part_ptr->max_nodes;
	} else {
		max_nodes = MIN(job_ptr->details->max_nodes,
				job_ptr->part_ptr->max_nodes);
	}
	max_nodes = MIN(max_nodes, 500000);	/* prevent overflows */

	if (!job_ptr->limit_set_max_nodes && job_ptr->details->max_nodes)
		req_nodes = max_nodes;
	else
		req_nodes = min_nodes;


	if (job_ptr->total_cpus) {
		/* This indicates that nodes have been allocated already, but
		 * the job might have been requeued afterward. */
		cpu_cnt = job_ptr->total_cpus;
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
			info("%s: JobId=%u (%s) total_cpus=%u",
			     plugin_type, job_ptr->job_id, job_ptr->name,
			     cpu_cnt);
		}
	} else {
		cpu_cnt = req_nodes * cpus_per_node;
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
			info("%s: JobId=%u (%s) req_cpus=%u",
			     plugin_type, job_ptr->job_id, job_ptr->name,
			     cpu_cnt);
		}
	}

	return cpu_cnt;
}

/* Test if preemptor request will overallocate the account */
static int _overalloc_test(struct job_record *preemptor,
			   struct job_record *preemptee)
{
	uint32_t cpu_cnt_preemptee, cpu_cnt_preemptor;
	slurmdb_association_rec_t *assoc_preemptee, *assoc_preemptor;
	double shares_preemptee, shares_preemptor;
	uint32_t new_usage_preemptee, new_usage_preemptor;
	double allotment_preemptee, allotment_preemptor;
	double new_fairshare_preemptee, new_fairshare_preemptor;
	double new_fairshare_diff;
	char *relation = "equal";
	int rc = 0;

	cpu_cnt_preemptee = _get_nb_cpus(preemptee);
	cpu_cnt_preemptor = _get_nb_cpus(preemptor);

	assoc_preemptee = (slurmdb_association_rec_t *)preemptee->assoc_ptr;
	assoc_preemptor = (slurmdb_association_rec_t *)preemptor->assoc_ptr;

	shares_preemptee = assoc_preemptee->usage->shares_norm;
	shares_preemptor = assoc_preemptor->usage->shares_norm;
	new_usage_preemptee = assoc_preemptee->usage->grp_used_cpus;
	new_usage_preemptor = assoc_preemptor->usage->grp_used_cpus +
			      cpu_cnt_preemptor;

	allotment_preemptee = shares_preemptee * preemptee->part_ptr->total_cpus;
	allotment_preemptor = shares_preemptor * preemptor->part_ptr->total_cpus;

	/* Fairshare will be less than 1 if running the job will not overrun
	 * the share allocation */
	new_fairshare_preemptee = (double)new_usage_preemptee /
				  allotment_preemptee;
	new_fairshare_preemptor = (double)new_usage_preemptor /
			 	  allotment_preemptor;
	new_fairshare_diff = new_fairshare_preemptee - new_fairshare_preemptor;

	/* We don't always want to preempt based solely on priority.
	 * A fairshare value greater than 1 means share overallocation.
	 * 1) if both jobs will overallocate their account pocket -> use
	 *    priority value
	 * 2) if fairshare for preemptor is less than 1 but fairshare for
	 *    preemptee is greater than 1 -> Preemptor CAN preempt
	 * 3) if fairshare for preemptee is less than 1 but fairshare for
	 *    preemptor is greater than 1 -> Preemptor WILL NOT preempt
	 * 4) if fairshare for both jobs is less than 1 -> use priority value
	 * 5) if both jobs have equal fairshare OR are from the same account
	 *    then use priority value
	 */
	if (((new_fairshare_preemptee > 1.0 && new_fairshare_preemptor < 1.0) ||
	     (new_fairshare_preemptee < 1.0 && new_fairshare_preemptor > 1.0))&&
	    (new_fairshare_diff != 0.0) &&
	    (strcmp(assoc_preemptor->acct, assoc_preemptee->acct) != 0)) {
		if (new_fairshare_diff > 0.0) {
			relation = "lower (better)";
			rc = 1;	/* Preemptor can preempt */
		} else {
			relation = "higher (worse)";
			rc =  -1;	/* Preemptor not can preempt */
		}
	}

	if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
		info("%s: Preemptor(%u, %s) acccount %s have %s "
		     "fairshare than preemptee(%u, %s) account %s  %f vs. %f",
		     plugin_type, preemptor->job_id, preemptor->name,
		     assoc_preemptor->acct, relation, preemptee->job_id,
		     preemptee->name, assoc_preemptee->acct,
		     new_fairshare_preemptor, new_fairshare_preemptor);
		info(" 	CPU CNT: %u and %u  USED CPUS: %u and %u  "
		     "SHARES: %f and %f  TOT-CPUS: %u and %u",
		     cpu_cnt_preemptor, cpu_cnt_preemptee,
		     assoc_preemptor->usage->grp_used_cpus,
		     assoc_preemptee->usage->grp_used_cpus,
		     shares_preemptor, shares_preemptee,
		     preemptor->part_ptr->total_cpus,
		     preemptee->part_ptr->total_cpus);
	}

	return rc;
}

/*  Return true if the preemptor can preempt the preemptee, otherwise false */
static bool _job_prio_preemptable(struct job_record *preemptor,
				  struct job_record *preemptee)
{
	uint32_t job_prio1, job_prio2;
	int rc;

	if (CHECK_FOR_PREEMPTOR_OVERALLOC) {
		rc = _overalloc_test(preemptor, preemptee);
		if (rc > 0)
			return true;
		else if (rc < 0)
			return false;
	}

	job_prio1 = preemptor->priority;
	job_prio2 = preemptee->priority;

	if (job_prio1 > job_prio2) {
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
    			info("%s: Priority of JobId %u > JobId %u (%u > %u)",
			     plugin_type, preemptor->job_id, preemptee->job_id,
			     job_prio1, job_prio2);
		}
		return true;	/* Preemptor can preempt */
	} else {
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
    			info("%s: Priority of JobId %u <= JobId %u (%u <= %u)",
			     plugin_type, preemptor->job_id, preemptee->job_id,
			     job_prio1, job_prio2);
		}
		return false;	/* Preemptor can not preempt */
	}
}

/* Sort jobs by priority. Use runtime as secondary key */
static int _sort_by_job_prio(void *x, void *y)
{
	struct job_record *job_ptr1 = (struct job_record *) x;
	struct job_record *job_ptr2 = (struct job_record *) y;

	if      (job_ptr1->priority > job_ptr2->priority)
		return 1;
	else if (job_ptr1->priority < job_ptr2->priority)
		return -1;
	else if (_is_job_runtime_greater(job_ptr1, job_ptr2))
		return 1;
	return 0;
}

/**************************************************************************/
/*  TAG(                              init                              ) */
/**************************************************************************/
extern int init( void )
{
	int rc = SLURM_SUCCESS;
	char *prio_type = slurm_get_priority_type();

	if (strncasecmp(prio_type, "priority/multifactor", 20)) {
		error("The priority plugin (%s) is currently loaded. "
		      "This is NOT compatible with the %s plugin. "
		      "The priority/multifactor plugin must be used",
		      prio_type, plugin_type);
		rc = SLURM_FAILURE;
	}

	xfree(prio_type);
	verbose("%s loaded", plugin_type);
	return rc;
}

/**************************************************************************/
/*  TAG(                              fini                              ) */
/**************************************************************************/
extern void fini(void)
{
	/* Empty. */
}

/**************************************************************************/
/* TAG(                 find_preemptable_jobs                           ) */
/**************************************************************************/
extern List find_preemptable_jobs(struct job_record *job_ptr)
{
	ListIterator preemptee_candidate_iterator;
	struct job_record *preemptee_job_ptr;
	struct job_record *preemptor_job_ptr = job_ptr;
	List preemptee_job_list = NULL;

	/* Validate the preemptor job */
	if (preemptor_job_ptr == NULL) {
		error("%s: preemptor_job_ptr is NULL", plugin_type);
		return preemptee_job_list;
	}
	if (!IS_JOB_PENDING(preemptor_job_ptr)) {
		error("%s: JobId %u not pending",
		      plugin_type, preemptor_job_ptr->job_id);
		return preemptee_job_list;
	}
	if (preemptor_job_ptr->part_ptr == NULL) {
		error("%s: JobId %u has NULL partition ptr",
		      plugin_type, preemptor_job_ptr->job_id);
		return preemptee_job_list;
	}
	if (preemptor_job_ptr->part_ptr->node_bitmap == NULL) {
		error("%s: partition %s node_bitmap==NULL",
		      plugin_type, preemptor_job_ptr->part_ptr->name);
		return preemptee_job_list;
	}

	if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
		info("%s: Looking for jobs to preempt for JobId %u",
		    plugin_type, preemptor_job_ptr->job_id);
	}

	/* Build an array of pointers to preemption candidates */
	preemptee_candidate_iterator = list_iterator_create(job_list);
	while ((preemptee_job_ptr = (struct job_record *)
				    list_next(preemptee_candidate_iterator))) {
		if (!IS_JOB_RUNNING(preemptee_job_ptr) &&
		    !IS_JOB_SUSPENDED(preemptee_job_ptr))
			continue;
		if (!_job_prio_preemptable(preemptor_job_ptr,preemptee_job_ptr))
			continue;
		if ((preemptee_job_ptr->node_bitmap == NULL) ||
		   (bit_overlap(preemptee_job_ptr->node_bitmap,
				preemptor_job_ptr->part_ptr->node_bitmap) == 0))
			continue;
		if (preemptor_job_ptr->details &&
		    (preemptor_job_ptr->details->expanding_jobid ==
		     preemptee_job_ptr->job_id))
			continue;

		/* This job is a valid preemption candidate and should be added
		 * to the list. Create the list as needed. */
		if (preemptee_job_list == NULL)
			preemptee_job_list = list_create(NULL);
		list_append(preemptee_job_list, preemptee_job_ptr);
	}
	list_iterator_destroy(preemptee_candidate_iterator);

	if ((preemptee_job_list == NULL) &&
	    (slurm_get_debug_flags() & DEBUG_FLAG_PRIO)) {
    		info("NULL preemptee list for job (%u) %s",
		     preemptor_job_ptr->job_id, preemptor_job_ptr->name);
	}

	return preemptee_job_list;
}

/**************************************************************************/
/* TAG(                 job_preempt_mode                                ) */
/**************************************************************************/
extern uint16_t job_preempt_mode(struct job_record *job_ptr)
{
	uint16_t mode;

	if (job_ptr->qos_ptr &&
	   ((slurmdb_qos_rec_t *)job_ptr->qos_ptr)->preempt_mode) {
		mode = ((slurmdb_qos_rec_t *)job_ptr->qos_ptr)->preempt_mode;
		if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
			info("%s: in job_preempt_mode return = %s",
			     plugin_type, preempt_mode_string(mode));
		}
		return mode;
	}

	mode = slurm_get_preempt_mode() & (~PREEMPT_MODE_GANG);
	if (slurm_get_debug_flags() & DEBUG_FLAG_PRIO) {
		info("%s: in job_preempt_mode return = %s",
		     plugin_type, preempt_mode_string(mode));
	}
	return mode;
}

/**************************************************************************/
/* TAG(                 preemption_enabled                              ) */
/**************************************************************************/
extern bool preemption_enabled(void)
{
	return (slurm_get_preempt_mode() != PREEMPT_MODE_OFF);
}

/***************************************************************************/
/* Return true if the preemptor can preempt the preemptee, otherwise false */
/***************************************************************************/
extern bool job_preempt_check(job_queue_rec_t *preemptor,
			      job_queue_rec_t *preemptee)
{
	return _job_prio_preemptable(preemptor->job_ptr, preemptee->job_ptr);
}