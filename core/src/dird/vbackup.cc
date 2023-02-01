/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2008-2012 Free Software Foundation Europe e.V.
   Copyright (C) 2011-2016 Planets Communications B.V.
   Copyright (C) 2013-2023 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
// Kern Sibbald, July MMVIII
/* @file
 * responsible for doing virtual backup jobs or
 *                    in other words, consolidation or synthetic backups.
 *
 * Basic tasks done here:
 *   * Open DB and create records for this job.
 *   * Figure out what Jobs to copy.
 *   * Open Message Channel with Storage daemon to tell him a job will be
 * starting.
 *   * Open connection with File daemon and pass him commands to do the backup.
 *   * When the File daemon finishes the job, update the DB.
 */

#include "include/bareos.h"
#include "dird.h"
#include "dird/dird_globals.h"
#include "dird/backup.h"
#include "dird/bsr.h"
#include "dird/jcr_private.h"
#include "dird/job.h"
#include "dird/migration.h"
#include "dird/msgchan.h"
#include "dird/sd_cmds.h"
#include "dird/storage.h"
#include "dird/ua_server.h"
#include "dird/ua_purge.h"
#include "dird/vbackup.h"
#include "lib/edit.h"
#include "lib/util.h"
#include "include/make_unique.h"
#include "cats/sql.h"

#include <algorithm>
#include <vector>
#include <string>

namespace directordaemon {

static const int dbglevel = 10;

static bool CreateBootstrapFile(JobControlRecord& jcr,
                                const std::string& jobids);

class JobConsistencyChecker {
  enum
  {
    col_JobId = 0,
    col_Type = 1,
    col_ClientId = 2,
    col_FilesetId = 3,
    col_PurgedFiles = 4
  };

 public:
  std::vector<std::string> JobList;
  std::vector<std::string> JobsWithPurgedFiles;

  bool operator()(int num_fields, char** row)
  {
    assert(num_fields == 5);
    JobList.push_back(row[col_JobId]);
    if (row[col_PurgedFiles][0] != '0') {
      JobsWithPurgedFiles.push_back(row[col_JobId]);
    }
    return true;
  }
  bool CheckNumJobs(size_t t_NumJobs) { return JobList.size() == t_NumJobs; }
  std::vector<std::string> JobListDiff(std::vector<std::string> FullJobList)
  {
    std::sort(FullJobList.begin(), FullJobList.end());
    std::sort(JobList.begin(), JobList.end());
    std::vector<std::string> diff;
    std::set_difference(FullJobList.begin(), FullJobList.end(), JobList.begin(),
                        JobList.end(), std::inserter(diff, diff.begin()));
    return diff;
  }
  bool CheckPurgedFiles() { return JobsWithPurgedFiles.empty(); }
};

std::string GetVfJobids(JobControlRecord& jcr)
{
  // See if we already got a list of jobids to use.
  if (jcr.impl->vf_jobids) {
    Dmsg1(10, "jobids=%s\n", jcr.impl->vf_jobids);
    return jcr.impl->vf_jobids;
  } else {
    db_list_ctx jobids_ctx;
    jcr.db->AccurateGetJobids(&jcr, &jcr.impl->jr, &jobids_ctx);
    Dmsg1(10, "consolidate candidates:  %s.\n",
          jobids_ctx.GetAsString().c_str());
    return jobids_ctx.GetAsString();
  }
}

// Called here before the job is run to do the job specific setup.
bool DoNativeVbackupInit(JobControlRecord* jcr)
{
  const char* storage_source;

  if (!GetOrCreateFilesetRecord(jcr)) {
    Dmsg1(dbglevel, "JobId=%d no FileSet\n", (int)jcr->JobId);
    return false;
  }

  ApplyPoolOverrides(jcr);

  if (!AllowDuplicateJob(jcr)) { return false; }

  jcr->impl->jr.PoolId
      = GetOrCreatePoolRecord(jcr, jcr->impl->res.pool->resource_name_);
  if (jcr->impl->jr.PoolId == 0) {
    Dmsg1(dbglevel, "JobId=%d no PoolId\n", (int)jcr->JobId);
    Jmsg(jcr, M_FATAL, 0, _("Could not get or create a Pool record.\n"));
    return false;
  }

  /* Note, at this point, pool is the pool for this job.
   * We transfer it to rpool (read pool), and a bit later,
   * pool will be changed to point to the write pool,
   * which comes from pool->NextPool.
   */
  jcr->impl->res.rpool = jcr->impl->res.pool; /* save read pool */
  PmStrcpy(jcr->impl->res.rpool_source, jcr->impl->res.pool_source);

  // If pool storage specified, use it for restore
  CopyRstorage(jcr, jcr->impl->res.pool->storage, _("Pool resource"));

  Dmsg2(dbglevel, "Read pool=%s (From %s)\n",
        jcr->impl->res.rpool->resource_name_, jcr->impl->res.rpool_source);

  jcr->start_time = time(NULL);
  jcr->impl->jr.StartTime = jcr->start_time;
  if (!jcr->db->UpdateJobStartRecord(jcr, &jcr->impl->jr)) {
    Jmsg(jcr, M_FATAL, 0, "%s", jcr->db->strerror());
  }

  // See if there is a next pool override.
  if (jcr->impl->res.run_next_pool_override) {
    PmStrcpy(jcr->impl->res.npool_source, _("Run NextPool override"));
    PmStrcpy(jcr->impl->res.pool_source, _("Run NextPool override"));
    storage_source = _("Storage from Run NextPool override");
  } else {
    // See if there is a next pool override in the Job definition.
    if (jcr->impl->res.job->next_pool) {
      jcr->impl->res.next_pool = jcr->impl->res.job->next_pool;
      PmStrcpy(jcr->impl->res.npool_source, _("Job's NextPool resource"));
      PmStrcpy(jcr->impl->res.pool_source, _("Job's NextPool resource"));
      storage_source = _("Storage from Job's NextPool resource");
    } else {
      // Fall back to the pool's NextPool definition.
      jcr->impl->res.next_pool = jcr->impl->res.pool->NextPool;
      PmStrcpy(jcr->impl->res.npool_source, _("Job Pool's NextPool resource"));
      PmStrcpy(jcr->impl->res.pool_source, _("Job Pool's NextPool resource"));
      storage_source = _("Storage from Pool's NextPool resource");
    }
  }

  /* If the original backup pool has a NextPool, make sure a
   * record exists in the database. Note, in this case, we
   * will be migrating from pool to pool->NextPool.
   */
  if (jcr->impl->res.next_pool) {
    jcr->impl->jr.PoolId
        = GetOrCreatePoolRecord(jcr, jcr->impl->res.next_pool->resource_name_);
    if (jcr->impl->jr.PoolId == 0) { return false; }
  }

  if (!SetMigrationWstorage(jcr, jcr->impl->res.pool, jcr->impl->res.next_pool,
                            storage_source)) {
    return false;
  }

  jcr->impl->res.pool = jcr->impl->res.next_pool;

  Dmsg2(dbglevel, "Write pool=%s read rpool=%s\n",
        jcr->impl->res.pool->resource_name_,
        jcr->impl->res.rpool->resource_name_);

  // CreateClones(jcr);

  return true;
}

/**
 * Do a virtual backup, which consolidates all previous backups into a sort of
 * synthetic Full.
 *
 * Returns:  false on failure
 *           true  on success
 */
bool DoNativeVbackup(JobControlRecord* jcr)
{
  if (!jcr->impl->res.read_storage_list) {
    Jmsg(jcr, M_FATAL, 0, _("No storage for reading given.\n"));
    return false;
  }

  if (!jcr->impl->res.write_storage_list) {
    Jmsg(jcr, M_FATAL, 0, _("No storage for writing given.\n"));
    return false;
  }

  Dmsg2(100, "read_storage_list=%p write_storage_list=%p\n",
        jcr->impl->res.read_storage_list, jcr->impl->res.write_storage_list);
  Dmsg2(100, "Read store=%s, write store=%s\n",
        ((StorageResource*)jcr->impl->res.read_storage_list->first())
            ->resource_name_,
        ((StorageResource*)jcr->impl->res.write_storage_list->first())
            ->resource_name_);

  Jmsg(jcr, M_INFO, 0, _("Start Virtual Backup JobId %lu, Job=%s\n"),
       jcr->JobId, jcr->Job);

  if (!jcr->accurate) {
    Jmsg(jcr, M_WARNING, 0,
         _("This Job is not an Accurate backup so is not equivalent to a Full "
           "backup.\n"));
  }

  std::string jobids = GetVfJobids(*jcr);
  std::vector<std::string> jobid_list = split_string(jobids, ',');
  if (jobid_list.empty()) {
    Jmsg(jcr, M_FATAL, 0, _("No previous Jobs found.\n"));
    return false;
  }

  using namespace std::string_literals;
  JobConsistencyChecker cc;
  std::string query = "SELECT JobId, Type, ClientId, FilesetId, PurgedFiles "s
                      + "FROM Job WHERE JobId IN ("s + jobids + ")"s;

  jcr->db->SqlQuery(query.c_str(), ObjectHandler<decltype(cc)>, &cc);

  if (!cc.CheckNumJobs(jobid_list.size())) {
    for (auto& missing_job : cc.JobListDiff(jobid_list)) {
      Jmsg(jcr, M_ERROR, 0, "JobId %s is not present in the catalog\n",
           missing_job.c_str());
    }
    Jmsg(jcr, M_FATAL, 0, "Jobs missing from catalog. Cannot continue.\n");
    return false;
  }
  if (!cc.CheckPurgedFiles()) {
    for (auto& purged_files_job : cc.JobsWithPurgedFiles) {
      Jmsg(jcr, M_ERROR, 0,
           "Files for JobId %s have been purged from the catalog\n",
           purged_files_job.c_str());
    }
    Jmsg(jcr, M_FATAL, 0,
         "At least one job's files were pruned from the catalog.\n");
    return false;
  }

  // Find first Jobid, get the db record and find its level
  JobDbRecord tmp_jr{};
  tmp_jr.JobId = str_to_int64(jobid_list.front().c_str());
  Dmsg1(10, "Previous JobId=%s\n", jobid_list.front().c_str());

  if (!jcr->db->GetJobRecord(jcr, &tmp_jr)) {
    Jmsg(jcr, M_FATAL, 0, _("Error getting Job record for first Job: ERR=%s\n"),
         jcr->db->strerror());
    return false;
  }

  int JobLevel_of_first_job = tmp_jr.JobLevel;
  Dmsg2(10, "Level of first consolidated job %d: %s\n", tmp_jr.JobId,
        job_level_to_str(JobLevel_of_first_job));

  /* Now we find the newest job that ran and store its info in
   * the previous_jr record. We will set our times to the
   * values from that job so that anything changed after that
   * time will be picked up on the next backup.
   */
  jcr->impl->previous_jr = JobDbRecord{};
  jcr->impl->previous_jr.JobId = str_to_int64(jobid_list.back().c_str());
  Dmsg1(10, "Previous JobId=%s\n", jobid_list.back().c_str());

  if (!jcr->db->GetJobRecord(jcr, &jcr->impl->previous_jr)) {
    Jmsg(jcr, M_FATAL, 0,
         _("Error getting Job record for previous Job: ERR=%s\n"),
         jcr->db->strerror());
    return false;
  }

  if (!CreateBootstrapFile(*jcr, jobids)) {
    Jmsg(jcr, M_FATAL, 0, _("Could not create bootstrap file\n"));
    return false;
  }

  Jmsg(jcr, M_INFO, 0, _("Consolidating JobIds %s containing %d files\n"),
       jobids.c_str(), jcr->impl->ExpectedFiles);

  /* Open a message channel connection with the Storage
   * daemon. */
  Dmsg0(110, "Open connection with storage daemon\n");
  jcr->setJobStatus(JS_WaitSD);

  // Start conversation with Storage daemon
  if (!ConnectToStorageDaemon(jcr, 10, me->SDConnectTimeout, true)) {
    return false;
  }

  // Now start a job with the Storage daemon
  if (!StartStorageDaemonJob(jcr, jcr->impl->res.read_storage_list,
                             jcr->impl->res.write_storage_list,
                             /* send_bsr */ true)) {
    return false;
  }
  Dmsg0(100, "Storage daemon connection OK\n");

  /* We re-update the job start record so that the start
   * time is set after the run before job.  This avoids
   * that any files created by the run before job will
   * be saved twice.  They will be backed up in the current
   * job, but not in the next one unless they are changed.
   * Without this, they will be backed up in this job and
   * in the next job run because in that case, their date
   * is after the start of this run. */
  jcr->start_time = time(NULL);
  jcr->impl->jr.StartTime = jcr->start_time;
  jcr->impl->jr.JobTDate = jcr->start_time;
  jcr->setJobStatus(JS_Running);

  // Update job start record
  if (!jcr->db->UpdateJobStartRecord(jcr, &jcr->impl->jr)) {
    Jmsg(jcr, M_FATAL, 0, "%s", jcr->db->strerror());
    return false;
  }

  // Declare the job started to start the MaxRunTime check
  jcr->setJobStarted();

  /* Start the job prior to starting the message thread below
   * to avoid two threads from using the BareosSocket structure at
   * the same time. */
  if (!jcr->store_bsock->fsend("run")) { return false; }

  // Now start a Storage daemon message thread
  if (!StartStorageDaemonMessageThread(jcr)) { return false; }

  jcr->setJobStatus(JS_Running);

  /* Pickup Job termination data
   * Note, the SD stores in jcr->JobFiles/ReadBytes/JobBytes/JobErrors */
  WaitForStorageDaemonTermination(jcr);
  jcr->setJobStatus(jcr->impl->SDJobStatus);
  jcr->db_batch->WriteBatchFileRecords(
      jcr); /* used by bulk batch file insert */
  if (!jcr->is_JobStatus(JS_Terminated)) { return false; }

  NativeVbackupCleanup(jcr, jcr->JobStatus, JobLevel_of_first_job);

  // Remove the successfully consolidated jobids from the database
  if (jcr->impl->res.job->AlwaysIncremental
      && jcr->impl->res.job->AlwaysIncrementalJobRetention) {
    UaContext* ua;
    ua = new_ua_context(jcr);
    PurgeJobsFromCatalog(ua, jobids.c_str());
    Jmsg(jcr, M_INFO, 0,
         _("purged JobIds %s as they were consolidated into Job %lu\n"),
         jobids.c_str(), jcr->JobId);
  }
  return true;
}

// Release resources allocated during backup.
void NativeVbackupCleanup(JobControlRecord* jcr, int TermCode, int JobLevel)
{
  char ec1[30], ec2[30];
  char term_code[100];
  const char* TermMsg;
  int msg_type = M_INFO;
  ClientDbRecord cr;
  PoolMem query(PM_MESSAGE);

  Dmsg2(100, "Enter backup_cleanup %d %c\n", TermCode, TermCode);

  switch (jcr->JobStatus) {
    case JS_Terminated:
    case JS_Warnings:
      jcr->impl->jr.JobLevel = JobLevel; /* We want this to appear as what the
                                      first consolidated job was */
      Jmsg(jcr, M_INFO, 0,
           _("Joblevel was set to joblevel of first consolidated job: %s\n"),
           job_level_to_str(JobLevel));
      break;
    default:
      break;
  }

  jcr->JobFiles = jcr->impl->SDJobFiles;
  jcr->JobBytes = jcr->impl->SDJobBytes;

  if (jcr->getJobStatus() == JS_Terminated
      && (jcr->JobErrors || jcr->impl->SDErrors)) {
    TermCode = JS_Warnings;
  }

  UpdateJobEnd(jcr, TermCode);

  // Update final items to set them to the previous job's values
  Mmsg(query,
       "UPDATE Job SET StartTime='%s',EndTime='%s',"
       "JobTDate=%s WHERE JobId=%s",
       jcr->impl->previous_jr.cStartTime, jcr->impl->previous_jr.cEndTime,
       edit_uint64(jcr->impl->previous_jr.JobTDate, ec1),
       edit_uint64(jcr->JobId, ec2));
  jcr->db->SqlQuery(query.c_str());

  // Get the fully updated job record
  if (!jcr->db->GetJobRecord(jcr, &jcr->impl->jr)) {
    Jmsg(jcr, M_WARNING, 0,
         _("Error getting Job record for Job report: ERR=%s\n"),
         jcr->db->strerror());
    jcr->setJobStatus(JS_ErrorTerminated);
  }

  if (jcr->impl->vf_jobids && jcr->impl->vf_jobids[0] != '\0') {
    using namespace std::string_literals;
    Jmsg(jcr, M_INFO, 0,
         "Replicating deleted files from jobids %s to jobid %d\n",
         jcr->impl->vf_jobids, jcr->JobId);
    PoolMem q1(PM_MESSAGE);
    jcr->db->FillQuery(
        q1, BareosDbQueryEnum::SQL_QUERY::select_recent_version_with_basejob,
        jcr->impl->vf_jobids, jcr->impl->vf_jobids, jcr->impl->vf_jobids,
        jcr->impl->vf_jobids);
    std::string query
        = "INSERT INTO File (FileIndex, JobId, PathId, LStat, MD5, Name) "s
          + "SELECT FileIndex, "s + std::to_string(jcr->JobId) + " AS JobId, "s
          + "PathId, LStat, MD5, Name FROM ("s + q1.c_str() + ") T "s
          + "WHERE FileIndex = 0"s;
    if (!jcr->db->SqlQuery(query.c_str())) {
      Jmsg(jcr, M_WARNING, 0, "Error replicating deleted files: ERR=%s\n",
           jcr->db->strerror());
    }
  }

  bstrncpy(cr.Name, jcr->impl->res.client->resource_name_, sizeof(cr.Name));
  if (!jcr->db->GetClientRecord(jcr, &cr)) {
    Jmsg(jcr, M_WARNING, 0,
         _("Error getting Client record for Job report: ERR=%s\n"),
         jcr->db->strerror());
  }

  UpdateBootstrapFile(jcr);

  switch (jcr->JobStatus) {
    case JS_Terminated:
      TermMsg = _("Backup OK");
      break;
    case JS_Warnings:
      TermMsg = _("Backup OK -- with warnings");
      break;
    case JS_FatalError:
    case JS_ErrorTerminated:
      TermMsg = _("*** Backup Error ***");
      msg_type = M_ERROR; /* Generate error message */
      if (jcr->store_bsock) {
        jcr->store_bsock->signal(BNET_TERMINATE);
        if (jcr->impl->SD_msg_chan_started) {
          pthread_cancel(jcr->impl->SD_msg_chan);
        }
      }
      break;
    case JS_Canceled:
      TermMsg = _("Backup Canceled");
      if (jcr->store_bsock) {
        jcr->store_bsock->signal(BNET_TERMINATE);
        if (jcr->impl->SD_msg_chan_started) {
          pthread_cancel(jcr->impl->SD_msg_chan);
        }
      }
      break;
    default:
      TermMsg = term_code;
      sprintf(term_code, _("Inappropriate term code: %c\n"), jcr->JobStatus);
      break;
  }

  GenerateBackupSummary(jcr, &cr, msg_type, TermMsg);

  Dmsg0(100, "Leave vbackup_cleanup()\n");
}

/**
 * This callback routine is responsible for inserting the
 *  items it gets into the bootstrap structure. For each JobId selected
 *  this routine is called once for each file. We do not allow
 *  duplicate filenames, but instead keep the info from the most
 *  recent file entered (i.e. the JobIds are assumed to be sorted)
 *
 *   See uar_sel_files in sql_cmds.c for query that calls us.
 *      row[0]=Path, row[1]=Filename, row[2]=FileIndex
 *      row[3]=JobId row[4]=LStat
 */
static int InsertBootstrapHandler(void* ctx, int num_fields, char** row)
{
  JobId_t JobId;
  int FileIndex;
  RestoreBootstrapRecord* bsr = (RestoreBootstrapRecord*)ctx;

  JobId = str_to_int64(row[3]);
  FileIndex = str_to_int64(row[2]);
  AddFindex(bsr, JobId, FileIndex);
  return 0;
}

static bool CreateBootstrapFile(JobControlRecord& jcr,
                                const std::string& jobids)
{
  if (!jcr.db->OpenBatchConnection(&jcr)) {
    Jmsg0(&jcr, M_FATAL, 0, "Can't get batch sql connexion");
    return false;
  }

  PoolMem jobids_pm{jobids};
  RestoreContext rx;
  rx.bsr = std::make_unique<RestoreBootstrapRecord>();
  rx.JobIds = jobids_pm.addr();

  if (!jcr.db_batch->GetFileList(&jcr, rx.JobIds, false /* don't use md5 */,
                                 true /* use delta */, InsertBootstrapHandler,
                                 (void*)rx.bsr.get())) {
    Jmsg(&jcr, M_ERROR, 0, "%s", jcr.db_batch->strerror());
  }

  UaContext* ua = new_ua_context(&jcr);
  AddVolumeInformationToBsr(ua, rx.bsr.get());
  jcr.impl->ExpectedFiles = WriteBsrFile(ua, rx);
  Dmsg1(10, "Found %d files to consolidate.\n", jcr.impl->ExpectedFiles);
  FreeUaContext(ua);
  rx.bsr.reset(nullptr);
  return jcr.impl->ExpectedFiles != 0;
}

} /* namespace directordaemon */
