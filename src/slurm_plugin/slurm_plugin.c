/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>

#include <slurm/spank.h>

#include "spindle_launch.h"
#include "plugin_utils.h"

#include "config.h"

#define SPINDLE_USE_SESSION "SPINDLE_USE_SESSION"
#define SPANK_SPINDLE_USE_SESSION "SPANK_" SPINDLE_USE_SESSION

SPINDLE_EXPORT extern const char plugin_name[];
SPINDLE_EXPORT extern const char plugin_type[];
SPINDLE_EXPORT extern const unsigned int plugin_version;
SPINDLE_EXPORT extern struct spank_option spank_options[];
SPINDLE_EXPORT int slurm_spank_task_init(spank_t spank, int ac, char *argv[]);
SPINDLE_EXPORT int slurm_spank_task_exit(spank_t spank, int ac, char *argv[]);
SPINDLE_EXPORT int slurm_spank_init(spank_t spank, int ac, char *argv[]);
SPINDLE_EXPORT int slurm_spank_init_post_opt(spank_t spank, int ac, char *argv[]);
SPINDLE_EXPORT int slurm_spank_local_user_init(spank_t spank, int ac, char *argv[]);
SPINDLE_EXPORT int slurm_spank_exit(spank_t spank, int site_argc, char *site_argv[]);


SPINDLE_EXPORT int slurm_spank_job_prolog(spank_t spank, int ac, char *argv[]);
SPINDLE_EXPORT int slurm_spank_job_epilog(spank_t spank, int ac, char *argv[]);

/* Fail the job, not the node, if the plugin fails. */
SPINDLE_EXPORT int slurm_spank_init_failure_mode = ESPANK_JOB_FAILURE;

SPANK_PLUGIN(spindle, 1)

typedef struct {
   spank_t spank;
   int site_argc;
   char **site_argv;
} start_params_t;

typedef struct {
   spank_t spank;
   int site_argc;
   char **site_argv;
} exit_params_t;

#if defined(SPLIT_CALLBACKS_MODE)
static int set_spindle_args(spank_t spank, spindle_args_t *params, int argc, char **argv);
static int get_spindle_args(spank_t spank, spindle_args_t *params);
#endif

static int should_use_session(spank_t spank);
static int forward_environment(spank_t spank);
static int handle_forwarded_environment(void);
static int launchFE(char **hostlist, spindle_args_t *params);
static int launchBE(spank_t spank, spindle_args_t *params);
static int prepApp(spank_t spank, spindle_args_t *params);
static int launch_spindle(spank_t spank, spindle_args_t *params);
static unique_id_t getUniqueID(spank_t spank, int session_enabled);
static int spindle_options(int val, const char *optarg, int remote);
static int spindle_session_options(int val, const char *optarg, int remote);
static int fillInArgs(spank_t spank, spindle_args_t *args, int argc, char **argv, unique_id_t unique_id, int session_enabled);
static int process_spindle_args(spank_t spank, int site_argc, char *site_argv[], spindle_args_t *params, int *out_argc, char ***out_argv, int session_enabled);
static int handleExit(void *params, char **output_str);
static int handleStart(void *params, char **output_str);
static int get_num_hosts(spank_t spank);
static char **get_hostlist(spank_t spank, unsigned int num_hosts);
   
static pid_t pidBE = 0;
static pid_t pidFE = 0;
static __thread spank_t current_spank;
static const char *user_options = NULL;
static int enable_spindle = 0;
static int start_session = 0;

extern char **environ;
extern char *parse_location(char *loc, number_t number);

struct spank_option spank_options[] =
{
   { "spindle", "[spindle options]",
     "Accelerate library loading with spindle", 2, 0,
     (spank_opt_cb_f) spindle_options
   },
   SPANK_OPTIONS_TABLE_END
};

struct spank_option session_option =
{
   "spindle-session", NULL, 
   "Start a Spindle session for this allocation", 0, 0,
   (spank_opt_cb_f) spindle_session_options
};

static int should_use_session(spank_t spank) {
   spank_context_t context;
   spank_err_t err;
   char * session_env;

   context = spank_context();

   /* The technique for checking the session flag is different in
      different contexts. In local, we're running where the args
      were processed so we can check the flag directly . */
   if (context == S_CTX_LOCAL) {
      session_env = getenv(SPANK_SPINDLE_USE_SESSION);
      return (start_session || session_env);
   }
   /* In remote, we're running in a different process, so we check
      the env var set earlier in the local context. */
   if (context == S_CTX_REMOTE) {
      session_env = readSpankEnv(spank, SPANK_SPINDLE_USE_SESSION);
      if(session_env) {
         free(session_env);
         return 1;
      }
   }
   /* In the job script, due to a bug in SPANK, env vars are propagated
      to the prolog but not the epilog, so in the epilog instead we use
      spank_option_getopt (but this doesn't work in remote context
      which is why we need cases for both remote and job script). */
   if (context == S_CTX_JOB_SCRIPT) {
      session_env = getenv(SPANK_SPINDLE_USE_SESSION);
      if (session_env) 
         return 1;
      err = spank_option_getopt(spank, &session_option, NULL);
      return (err == ESPANK_SUCCESS); 
   }

   return 0;
}

const char * contextToStr(spank_context_t ctx) { // TODO remove debug logging ~nchaimov
   switch(ctx) {
      case S_CTX_ERROR:      return "S_CTX_ERROR";
      case S_CTX_LOCAL:      return "S_CTX_LOCAL";
      case S_CTX_REMOTE:     return "S_CTX_REMOTE";
      case S_CTX_ALLOCATOR:  return "S_CTX_ALLOCATOR";
      case S_CTX_SLURMD:     return "S_CTX_SLURMD";
      case S_CTX_JOB_SCRIPT: return "S_CTX_JOB_SCRIPT";
   }
   return "<UNKNOWN CONTEXT>";
}


int slurm_spank_init(spank_t spank, int ac, char *argv[]) {
   spank_context_t context;
   context = spank_context();
   const char * ctxName = contextToStr(context); // TODO remove debug logging ~nchaimov
   slurm_spank_log("slurm_spank_init in ctx %s", ctxName);
   if (context == S_CTX_ALLOCATOR) {
      spank_option_register(spank, &session_option);
   }
   return 0;
}

int slurm_spank_init_post_opt(spank_t spank, int ac, char *argv[]) { 
   spank_context_t context;
   unique_id_t session_unique_id;
   
   context = spank_context();
   const char * ctxName = contextToStr(context);
   slurm_spank_log("slurm_spank_init_post_opt in ctx %s", ctxName); // TODO remove debug logging ~nchaimov

   /* In sbatch and salloc, forward session status in env var to make it
      available in in other contexts. */
   if (context == S_CTX_ALLOCATOR) {
      if (enable_spindle) {
         setenv("SPANK_SPINDLE", "1", 1);
      }
      if (start_session) {
         setenv(SPANK_SPINDLE_USE_SESSION, "1", 1);
      }
   }
   return 0;
}

static int forward_environment(spank_t spank) 
{
   spank_err_t err;
   char * envVal;

   if (should_use_session(spank)) {
      slurm_spank_log("slurm_spank_local_user_init: should use session");
      /* Forward session status from srun to job prolog. */
      err = spank_job_control_setenv(spank, SPINDLE_USE_SESSION, "1", 1);
      if (err != ESPANK_SUCCESS) return -1;
   }
   envVal = getenv("SPINDLE_DEBUG");
   if (envVal) {
       err = spank_job_control_setenv(spank, "SPINDLE_DEBUG", envVal, 1);
       if (err != ESPANK_SUCCESS) return -1;
   }
   envVal = getenv("SPINDLE_TEST");
   if (envVal) {
       err = spank_job_control_setenv(spank, "SPINDLE_TEST", envVal, 1);
       if (err != ESPANK_SUCCESS) return -1;
   }
   envVal = getenv("TMPDIR");
   if (envVal) {
       err = spank_job_control_setenv(spank, "TMPDIR", envVal, 1);
       if (err != ESPANK_SUCCESS) return -1;
   } else {
       err = spank_job_control_setenv(spank, "TMPDIR", "/tmp", 1);
       if (err != ESPANK_SUCCESS) return -1;
   }
   return 0;
}    

static int handle_forwarded_environment(void) {
   char * envVal;
   envVal = getenv("SPANK_SPINDLE_DEBUG");
   if (envVal) {
      setenv("SPINDLE_DEBUG", envVal, 1);
   }
   envVal = getenv("SPANK_SPINDLE_TEST");
   if (envVal) {
      setenv("SPINDLE_TEST", envVal, 1);
   }
   envVal = getenv("SPANK_TMPDIR");
   if (envVal) {
      setenv("TMPDIR", envVal, 1);
   }
   return 0;
}

int slurm_spank_local_user_init(spank_t spank, int ac, char *argv[]) {
   spank_context_t context;
   int result;

   context = spank_context();
   const char * ctxName = contextToStr(context);
   slurm_spank_log("slurm_spank_local_user_init in ctx %s", ctxName);
   
   result = forward_environment(spank);
   return result;
}

extern char **environ;

int slurm_spank_job_prolog(spank_t spank, int ac, char *argv[]) {
   spank_context_t context;
   uid_t userid;
   start_params_t start_params;
   spank_err_t err;
   int result;
   char *result_str, *work_dir;
   
   context = spank_context();
   const char * ctxName = contextToStr(context);
   int use_session = should_use_session(spank);
   slurm_spank_log("slurm_spank_job_prolog in ctx %s, use_session = %d", ctxName, use_session); // TODO remove debug logging ~nchaimov
   slurm_spank_log("slurm_spank_job_prolog: ac=%d", ac);
   for(int i = 0; i < ac; ++i) {
      slurm_spank_log("slurm_spank_job_prolog: argv[%d] = %s", i, argv[i]);
   }

    for (char **env = environ; *env != NULL; env++) {
        slurm_spank_log("in prolog, env entry: %s", *env);
    }

   handle_forwarded_environment();

   if (!use_session) {
      return 0;
   }

   // The prolog starts in the user's home directory.
   // Change to $SLURM_JOB_WORK_DIR so logs go to right place.
   work_dir = getenv("SLURM_JOB_WORK_DIR");
   if (work_dir) {
      chdir(work_dir);
   }

   err = spank_get_item(spank, S_JOB_UID, &userid);
   if (err != ESPANK_SUCCESS) {
      slurm_error("ERROR: Spindle plugin error.  Could not get uid in exit\n");
      return -1;
   }

   start_params.spank = spank;
   start_params.site_argc = ac;
   start_params.site_argv = argv;

   result = dropPrivilegeAndRun(handleStart, userid, &start_params, &result_str);

   if (result == -1) {
      slurm_error("Failed to start Spindle for session\n");
      return -1;
   }

   return 0;
}

int slurm_spank_job_epilog(spank_t spank, int ac, char *argv[]) {
   int result, use_session;
   char *result_str;
   spank_context_t context;
   spank_err_t err;
   uid_t userid;
   exit_params_t exit_params;

   context = spank_context();
   const char * ctxName = contextToStr(context);

   use_session = should_use_session(spank);
   slurm_spank_log("slurm_spank_job_epilog in ctx %s, use_session = %d", ctxName, use_session); // TODO remove debug logging ~nchaimov
   slurm_spank_log("slurm_spank_job_epilog: ac=%d", ac);
   for(int i = 0; i < ac; ++i) {
      slurm_spank_log("slurm_spank_job_epilog: argv[%d] = %s", i, argv[i]);
   }

   for (char **env = environ; *env != NULL; env++) {
      slurm_spank_log("in epilog, env entry: %s", *env);
   }
   if (!use_session)
      return 0;

   // If session, shutdown BE and FE here.
   err = spank_get_item(spank, S_JOB_UID, &userid);
   if (err != ESPANK_SUCCESS) {
      slurm_error("ERROR: Spindle plugin error.  Could not get uid in epilog exit\n");
      return -1;
   }

   exit_params.spank = spank;
   exit_params.site_argc = ac;
   exit_params.site_argv = argv;

   result = dropPrivilegeAndRun(handleExit, userid, &exit_params, &result_str);
   
   if (result == -1) {
      slurm_error("Failed to run handleExit.  Spindle may not shutdown properly\n");
      return -1;
   }
   
   // TODO last-chance opportunity to stop Spindle processes
   return 0;
}

int slurm_spank_exit(spank_t spank, int ac, char *argv[]) {
   spank_context_t context;
   context = spank_context();
   const char * ctxName = contextToStr(context);
   slurm_spank_log("slurm_spank_exit in ctx %s", ctxName);
   if (context == S_CTX_ALLOCATOR) {
       if (start_session) {
           // or should this happen in epilog? ~nchaimov
           slurm_spank_log("should stop session");
       }
   }
   return 0;
}


int slurm_spank_task_init(spank_t spank, int site_argc, char *site_argv[])
{
   spank_context_t context;
   int result, func_result = -1;
   saved_env_t *env = NULL;
   static int initialized = 0;
   spindle_args_t params = {0};
   int use_session;
   uid_t userid;
   start_params_t start_params;
   saved_env_t *saved_env;
   char *result_str;
   spank_err_t err;

   context = spank_context();
   const char * ctxName = contextToStr(context);
   use_session = should_use_session(spank);
   unique_id_t my_id = getUniqueID(spank, use_session);
   slurm_spank_log("slurm_spank_task_init in ctx %s, use_session = %d, unique_id = %llu", ctxName, use_session, (unsigned long long)my_id);
   slurm_spank_log("slurm_spank_task_init: ac=%d", site_argc);
   for(int i = 0; i < site_argc; ++i) {
      slurm_spank_log("slurm_spank_task_init: argv[%d] = %s", i, site_argv[i]);
   }
   if (!enable_spindle)
      return 0;

   if (getuid() == 0) {
      return 0; //No spindle calls as root
   }   

   if (context == S_CTX_ERROR) {
      slurm_error("ERROR: spank_context returned an error in spindle task_init plugin.\n");
      return -1;
   }
   if (context != S_CTX_REMOTE) {
      return 0;
   }
   if (initialized) {
      return 0;
   }
   
   push_env(spank, &env);
   sdprintf(1, "Beginning spindle plugin\n");

   result = process_spindle_args(spank, site_argc, site_argv, &params, NULL, NULL, use_session);
   if (result == -1) {
      sdprintf(1, "Error processesing spindle arguments.  Aborting spindle\n");
      goto done;
   }

   if (params.opts & OPT_OFF) {
     return 0;
   }

   /* When using a session, handle start in job prolog, not here. */
   if (!use_session) {
      start_params.spank = spank;
      start_params.site_argc = site_argc;
      start_params.site_argv = site_argv;

      push_env(spank, &saved_env);   
      result = handleStart(&start_params, &result_str);
      pop_env(saved_env);
   }

   result = prepApp(spank, &params);
   if (result == -1) {
      sdprintf(1, "Error launching spindle.  Aborting spindle\n");
      goto done;
   }
   
   func_result = 0;
  done:
   sdprintf(1, "Finishing spindle plugin. Returning %d to slurm\n", func_result); 
   if (env)
      pop_env(env);
   initialized = 1;

   return func_result;
}

static int handleStart(void *params, char **output_str) 
{
   start_params_t *start_params;
   spank_t spank;
   int site_argc, result, use_session;
   char **site_argv;
   spindle_args_t args = {0};

   start_params = (start_params_t *) params;
   spank = start_params->spank;
   site_argc = start_params->site_argc;
   site_argv = start_params->site_argv;

   sdprintf(1, "In handleStart\n");
   use_session = should_use_session(spank);
   result = process_spindle_args(spank, site_argc, site_argv, &args, NULL, NULL, use_session);
   if (result == -1) {
      sdprintf(1, "ERROR: Could not process spindle args in handlestart\n");
      return -1;
   }
   
   if (args.opts & OPT_OFF) {
      return 0;
   }
   
   slurm_spank_log("Hello from handleStart, use_session = %d", use_session); 
   result = launch_spindle(spank, &args);
   if (result == -1) {
      sdprintf(1, "Error launching spindle.  Aborting spindle\n");
      slurm_spank_log("Error launchig spindle in handleStart");
      return -1;
   }
   return 0;
}

int slurm_spank_task_exit(spank_t spank, int site_argc, char *site_argv[])
{
   spank_context_t context;
   char *result_str;
   int result, use_session;
   uid_t userid;
   spank_err_t err;
   saved_env_t *saved_env;
   exit_params_t exit_params;

   context = spank_context();
   const char * ctxName = contextToStr(context);
   slurm_spank_log("slurm_spank_task_exit in context %s", ctxName);
   if (!enable_spindle) {
      return 0;
   }

   if (context == S_CTX_ERROR) {
      slurm_error("ERROR: spank_context returned an error in spindle exit plugin.\n"); 
      return -1;
   }
   
   if (context != S_CTX_REMOTE) {
      return 0;
   }

   use_session = should_use_session(spank);
   if (use_session) {
      /* When using a session, leave Spindle running between steps. */
      return 0;
   }

   err = spank_get_item(spank, S_JOB_UID, &userid);
   if (err != ESPANK_SUCCESS) {
      slurm_error("ERROR: Spindle plugin error.  Could not get uid in exit\n");
      return -1;
   }

   exit_params.spank = spank;
   exit_params.site_argc = site_argc;
   exit_params.site_argv = site_argv;

   push_env(spank, &saved_env);   
   result = dropPrivilegeAndRun(handleExit, userid, &exit_params, &result_str);
   pop_env(saved_env);
   
   if (result == -1) {
      slurm_error("Failed to run handleExit.  Spindle may not shutdown properly\n");
      return -1;
   }
   return 0;
}

static unique_id_t getUniqueID(spank_t spank, int session_enabled)
{
   char *slurm_job_id_s, *slurm_step_id_s;
   spank_err_t err;
   uint32_t jobid, stepid;
   uint64_t combined;
   
   slurm_job_id_s = getenv("SLURM_JOB_ID");
   if (slurm_job_id_s) {
      jobid = (uint32_t) atol(slurm_job_id_s);
   }
   else {
      err = spank_get_item(spank, S_JOB_ID, &jobid);
      if (err != ESPANK_SUCCESS) {
         slurm_error("Could not setup spindle:  Could not get SLURM_JOB_ID");
         return 0;
      }
   }

   if (!session_enabled) {
      slurm_step_id_s = getenv("SLURM_STEP_ID");
      if (slurm_step_id_s) {
         stepid = (uint32_t) atol(slurm_step_id_s);
      }
      else {
         err = spank_get_item(spank, S_JOB_STEPID, &stepid);
         if (err != ESPANK_SUCCESS) {
            slurm_error("Could not setup spindle: Could not get SLURM_STEP_ID");
            return 0;
         }
      }

      combined = stepid;
      combined <<= 32;
      combined |= jobid;
   } else {
      combined = jobid;
   }
   sdprintf(2, "Computed unique_id for session as %llu, session_enabled = %d\n", (unsigned long long) combined, session_enabled);
   return combined;
}

static int fillInArgs(spank_t spank, spindle_args_t *args, int argc, char **argv, unique_id_t unique_id, int session_enabled)
{
   int result;
   char *oldlocation;
   char *err_string;

   args->unique_id = unique_id;
   args->number = (number_t) args->unique_id;
   result = fillInSpindleArgsCmdlineFE(args, SPINDLE_FILLARGS_NOUNIQUEID | SPINDLE_FILLARGS_NONUMBER,
                                       argc, argv, &err_string);
   if (result == -1) {
      if (err_string)
         slurm_error("Spindle Options Error: %s", err_string);
      else
         slurm_error("Spindle Options Error: Error processesing spindle options\n");
      sdprintf(1, "Error processesing spindle options: %s\n", err_string ? err_string : "UNKNOWN");
      return -1;
   }
   args->opts |= OPT_BEEXIT;
   args->use_launcher = slurm_plugin_launcher;
   args->startup_type = startup_external;

   if (session_enabled) {
      args->opts |= OPT_PERSIST;
      args->opts |= OPT_SESSION;
   }

   oldlocation = args->location;
   current_spank = spank;
   args->location = parse_location(oldlocation, args->number);
   free(oldlocation);

   return 0;
}

static int process_spindle_args(spank_t spank, int site_argc, char *site_argv[], spindle_args_t *params, int *out_argc, char ***out_argv, int session_enabled)
{
   char *site_options = NULL, *combined_options = NULL;
   size_t combined_options_size, site_options_size, user_options_size;
   int combined_argc, result = -1, post_opt_result = -1, i;
   char **combined_argv = NULL;
   char *spindle_config = NULL;
   unique_id_t unique_id;

   unique_id = getUniqueID(spank, session_enabled);
   if (!unique_id)
      return -1;
      
   sdprintf(2, "Setting up options.  User options '%s'.  Site options \"%s %s %s...\"\n",
            user_options ? user_options : "NULL",
            site_argc >= 1 ? site_argv[0] : "", site_argc >= 2 ? site_argv[1] : "", site_argc >= 3 ? site_argv[2] : "");
   
   site_options = encodeCmdArgs(site_argc, site_argv);
   if (!site_options)
      site_options = strdup("");
   if (!user_options)
      user_options = "";

   site_options_size = strlen(site_options);
   user_options_size = strlen(user_options);
   combined_options_size = site_options_size + user_options_size + 3;
   combined_options = (char *) malloc(combined_options_size);
   snprintf(combined_options, combined_options_size, "%s%s%s ",
            site_options,
            (site_options_size && user_options_size) ? " " : "",
            user_options);
   
   sdprintf(1, "Combined site and user options are \"%s\"\n", combined_options);

   decodeCmdArgs(combined_options, &combined_argc, &combined_argv);
   result = fillInArgs(spank, params, combined_argc, combined_argv, unique_id, session_enabled);
   if (result == -1)
      goto done;

   if (out_argc)
      *out_argc = combined_argc;
   if (out_argv)
      *out_argv = combined_argv;
   post_opt_result = 0;

  done:
   if (combined_options)
      free(combined_options);
   if (site_options)
      free(site_options);
   if (spindle_config)
      free(spindle_config);
         
   if (!out_argv && combined_argv) {
      for (i = 0; i < combined_argc; i++) {
         if (combined_argv[i])
            free(combined_argv[i]);
      }
      free(combined_argv);
   }
   
   enable_spindle = (post_opt_result == 0);
   return post_opt_result;
}

static int get_num_hosts(spank_t spank)
{
   char *num_hosts_str;
   uint32_t num_hosts;
   int result;
   spank_err_t err;
   
   num_hosts_str = getenv("SLURM_STEP_NUM_NODES");
   if (num_hosts_str) {
      result = atoi(num_hosts_str);
      if (result > 0)
         return (int) result;
   }
   num_hosts_str = getenv("SLURM_JOB_NUM_NODES");
   if (num_hosts_str) {
      result = atoi(num_hosts_str);
      if (result > 0)
         return (int) result;
   }
   num_hosts_str = getenv("SLURM_NNODES");
   if (num_hosts_str) {
      result = atoi(num_hosts_str);
      if (result > 0)
         return (int) result;
   }
   err = spank_get_item(spank, S_JOB_NNODES, &num_hosts);
   if (err != ESPANK_SUCCESS)
      return -1;
   return num_hosts;

}

static char **get_hostlist(spank_t spank, unsigned int num_hosts)
{
   char *short_hosts, **hostlist;;

   short_hosts = readSpankEnv(spank, "SLURM_STEP_NODELIST");
   if (!short_hosts)
      short_hosts = readSpankEnv(spank, "SLURM_JOB_NODELIST");
   if (!short_hosts)
      short_hosts = readSpankEnv(spank, "SLURM_NODELIST");
   if (!short_hosts) {
      sdprintf(1, "ERROR: SLURM_JOB_NODELIST not set.\n");
      return NULL;
   }   
   slurm_spank_log("get_hostlist: short_hosts = %s", short_hosts);

#if defined(SCONTROL_BIN)
   slurm_spank_log("get_hostlist: calling getHostsScontrol(%u, %s)", num_hosts, short_hosts);
   hostlist = getHostsScontrol(num_hosts, short_hosts);
   slurm_spank_log("get_hostlist: back from getHostsScontrol");
#else
   hostlist = getHostsParse(num_hosts, short_hosts);
#endif
   free(short_hosts);
   if (!hostlist) {
      sdprintf(1, "ERROR: Could not get list of hosts in job.  Aborting spindle\n");
      return NULL;
   }
   return hostlist;
}

#if defined(SPLIT_CALLBACKS_MODE)
static int set_spindle_args(spank_t spank, spindle_args_t *params, int argc, char **argv)
{
   char *spindle_config;
   spank_err_t err;
   
   spindle_config = encodeSpindleConfig(params->port, params->num_ports, params->unique_id, OPT_GET_SEC(params->opts),
                                        argc, argv);
   if (!spindle_config) {
      sdprintf(1, "ERROR: Failed to set spindle_config\n");
      return -1;
   }
   sdprintf(2, "Set spindle_config to \"%s\"\n", spindle_config);

   err = spank_setenv(spank, "SPINDLE_CONFIG", spindle_config, 1);
   if (err != ESPANK_SUCCESS) {
      slurm_error("Could not set SPINDLE_CONFIG environment variable\n");
      free(spindle_config);
      return -1;
   }   
   setenv("SPANK_SPINDLE_CONFIG", spindle_config, 1);
   free(spindle_config);   
   return 0;
}

static int get_spindle_args(spank_t spank, spindle_args_t *params)
{
   char *spindle_config;
   unsigned int port, num_ports;
   uint64_t unique_id;
   uint32_t security_type;
   int spindle_argc = 0;
   char **spindle_argv = NULL;
   int result, get_args_result = -1, i;
   
   spindle_config = readSpankEnv(spank, "SPANK_SPINDLE_CONFIG");
   if (!spindle_config) {
      sdprintf(1, "ERROR: SPANK_SPINDLE_CONFIG not set\n");
      goto done;
   }
   unsetenv("SPANK_SPINDLE_CONFIG");

   result = decodeSpindleConfig(spindle_config, &port, &num_ports, &unique_id, &security_type,
                                &spindle_argc, &spindle_argv);
   if (result == -1) {
      sdprintf(1, "ERROR: Could not decode spindle config \"%s\"\n", spindle_config);
      goto done;
   }

   result = fillInArgs(spank, params, spindle_argc, spindle_argv, unique_id);
   if (result == -1)
      goto done;
   
   get_args_result = 0;
  done:
   if (spindle_config)
      free(spindle_config);
   if (spindle_argv) {
      for (i = 0; i < spindle_argc; i++) {
         if (spindle_argv[i])
            free(spindle_argv[i]);
      }
      free(spindle_argv);
   }
   return get_args_result;   
}
#endif

static int launch_spindle(spank_t spank, spindle_args_t *params)
{
   char **hostlist = NULL, **hostaddrlist = NULL;
   int result;
   int is_fe_host = 0;
   int is_be_leader = 0;
   unsigned int i, num_hosts;
   int num_hosts_result;
   int launch_result = -1;

   num_hosts_result = get_num_hosts(spank);
   if (num_hosts_result == -1)
      goto done;
   num_hosts = (unsigned int) num_hosts_result;

   hostlist = get_hostlist(spank, num_hosts);
   if (!hostlist)
      goto done;
   
   is_be_leader = isBEProc(params, 0);
   if (is_be_leader == -1)
      goto done;
   
   is_fe_host = isFEHost(hostlist, num_hosts);
   if (is_fe_host == -1) 
      goto done;

   sdprintf(1, "is_fe_host = %d, is_be_leader = %d\n", (int) is_fe_host, (int) is_be_leader);
   
   if (is_be_leader) {
      result = launchBE(spank, params);
      if (result == -1)
         goto done;
   }

   if (is_fe_host && is_be_leader) {
#if defined(SINFO_BIN)
      hostaddrlist = getHostAddrSinfo(num_hosts, hostlist);
      if (!hostaddrlist)
	  goto done;
      result = launchFE(hostaddrlist, params);
#else
      result = launchFE(hostlist, params);
#endif
      if (result == -1)
         goto done;
   }

   launch_result = 0;
   
  done:
   if (hostlist) {
      for (i = 0; i < num_hosts; i++) free(hostlist[i]);
      free(hostlist);
   }
   if (hostaddrlist) {
      for (i = 0; i < num_hosts; i++) free(hostaddrlist[i]);
      free(hostaddrlist);
   }
   
   return launch_result;
}

static int spindle_options(int val, const char *optarg, int remote)
{
   enable_spindle = 1;
   user_options = optarg;
   
   return 0;
}

static int spindle_session_options(int val, const char *optarg, int remote)
{
   start_session = 1;
   slurm_spank_log("will start session, start_session = %d", start_session); // TODO remove debug logging ~nchaimov
   
   return 0;
}

char *custom_getenv(char *envname)
{
   return readSpankEnv(current_spank, envname);
}

static int launchFE(char **hostlist, spindle_args_t *params)
{
   int result;
   pidFE = grandchild_fork();
   if (pidFE == -1) {
      sdprintf(1, "ERROR: Could not fork FE process.  Aborting spindle\n");
      return -1;
   }
   if (pidFE) {
      sdprintf(2, "Forked FE as pid %d\n", pidFE);
      slurm_spank_log("Forked FE as pid %d", pidFE);
      return 0;
   }

   superclose();
   
   sdprintf(1, "Initializing FE on pid %d with unique_id %lu\n", (int) getpid(), params->unique_id);
   result = spindleInitFE((const char **) hostlist, params);
   if (result == -1) {
      sdprintf(1, "ERROR: Could not launch FE.  Spindle will likely hang.\n");
      exit(-1);
   }

   if (!params->opts & OPT_SESSION) {
      result = spindleWaitForCloseFE(params);
      if (result == -1) {
          sdprintf(1, "ERROR: Could not wait for FE close\n");      
      }
      
      sdprintf(1, "FE received exit signal.  Shutting down FE\n");
      result = spindleCloseFE(params);
      if (result == -1) {
          sdprintf(1, "ERROR: Could not clean up FE.\n");
          exit(-1);
      }
   }
   exit(0);
}

static int launchBE(spank_t spank, spindle_args_t *params)
{
   int result;

   if (pidBE) {
      sdprintf(3, "Spindle BE already running.  Not relaunching\n");
      return 0;
   }

   sdprintf(1, "Launching spindle BE process\n");

   //Run a grandchild fork so the app process doesn't see extra children
   pidBE = grandchild_fork();
   
   if (pidBE == -1) {
      sdprintf(1, "ERROR: Could not fork process for spindle server\n");
      return -1;
   }
   else if (pidBE) {
      sdprintf(2, "Launched spindle BE process as pid %d\n", (int) pidBE);
      slurm_spank_log("Launched spindle BE process as pid %d", (int) pidBE);
      return 0;
   }

   superclose();

   result = spindleRunBE(params->port, params->num_ports, params->unique_id, OPT_GET_SEC(params->opts), NULL);
   if (result == -1)
      sdprintf(1, "ERROR: spindleRunBE failed\n");
   else
      sdprintf(1, "spindleRunBE completed.  Session finishing.\n");

   if (unique_file) unlink(unique_file);
   free(unique_file);
   unique_file = NULL;

   exit(result);

   return 0;
}

static int prepApp(spank_t spank, spindle_args_t *params)
{
#if HAVE_DECL_SPANK_PREPEND_TASK_ARGV == 1
   int result, i;
#else
   int app_argc, result;
   char **app_argv;
   char *app_exe_name, *last_slash;
#endif
   spank_err_t err;
   int bootstrap_argc;
   char **bootstrap_argv;

   result = getApplicationArgsFE(params, &bootstrap_argc, &bootstrap_argv);
   if (result == -1) {
      sdprintf(1, "ERROR: Failure getting bootstrap arguments.  Aborting spindle\n");
      return -1;
   }
      
#if HAVE_DECL_SPANK_PREPEND_TASK_ARGV == 1
   sdprintf(2, "Prepping task process %d to run spindle using spank_prepend_task_argv method\n", getpid());
   sdprintf(2, "spank_prepend_task_argv will prepend %d args\n", bootstrap_argc);
   for(i = 0; i < bootstrap_argc; ++i) {
      sdprintf(2, "bootstrap_argv[%d] = %s\n", i, bootstrap_argv[i]);
   }

   const char **filter_argv = (const char **)bootstrap_argv;
   err = spank_prepend_task_argv(spank, bootstrap_argc, filter_argv);
   if (err != ESPANK_SUCCESS) {
      sdprintf(1, "WARNING: Could not prepend spindle filter.\n");
      result = -1;
   }
#else
   sdprintf(2, "Prepping app process %d to run spindle using spindleHookSpindleArgsIntoExecBE method\n", getpid());

   err = spank_get_item(spank, S_JOB_ARGV, &app_argc, &app_argv);
   if (err != ESPANK_SUCCESS) {
      sdprintf(1, "WARNING: Could not get job argv to filter spindle.  Slurm processes may be spindleized\n");
      app_exe_name = NULL;
   }
   else {
      last_slash = strrchr(app_argv[0], '/');
      app_exe_name = last_slash ? last_slash+1 : app_argv[0];
      sdprintf(2, "Filtering spindle to run on app %s\n", app_exe_name);
   }

   result = spindleHookSpindleArgsIntoExecBE(bootstrap_argc, bootstrap_argv, app_exe_name);
#endif
   if (result == -1) {
      sdprintf(1, "ERROR setting up app to run spindle.  Spindle won't work\n");
      return -1;
   }

   return 0;
}

static int handleExit(void *params, char **output_str)
{
   exit_params_t *exit_params;
   spank_t spank;
   int site_argc, result, is_be_leader, is_fe_host, use_session, num_hosts_result;
   unsigned int num_hosts;
   char **site_argv, **hostlist;
   spindle_args_t args = {0};
   int launch_result = -1;

   exit_params = (exit_params_t *) params;
   spank = exit_params->spank;
   site_argc = exit_params->site_argc;
   site_argv = exit_params->site_argv;

   sdprintf(1, "In handleExit\n");
   use_session = should_use_session(spank);
   slurm_spank_log("Hello from handleExit, use_session = %d", use_session); 
   result = process_spindle_args(spank, site_argc, site_argv, &args, NULL, NULL, use_session);
   if (result == -1) {
      sdprintf(1, "ERROR: Could not process spindle args in handleExit\n");
      return -1;
   }
   
   if (args.opts & OPT_OFF) {
      return 0;
   }

   if (!args.location) {
      sdprintf(2, "WARNING: spindleExitBE not called since location is NULL\n");
   } else {
      // The task_exit callback is run for _each proc_, so we use
      // isBEProc to pick only one proc per node to call spindleExitBE.
      is_be_leader = isBEProc(&args, 1);
      if (is_be_leader) {
         slurm_spank_log("I am the BE leader and will call spindleExitBE on loc %s", args.location);
         result = spindleExitBE(args.location);
         slurm_spank_log("did call spindleExitBE on loc %s, result=%d", args.location, result);
         if (result == -1) {
             sdprintf(1, "ERROR: spindleExitBE returned an error on location %s\n", args.location);
             return -1;
         }
         if (use_session) {
            slurm_spank_log("Exiting from a session, should close FE");
            num_hosts_result = get_num_hosts(spank);
            if (num_hosts_result == -1)
                return -1;
            num_hosts = (unsigned int) num_hosts_result;
            hostlist = get_hostlist(spank, num_hosts);
            if (!hostlist)
                return -1;
            is_fe_host = isFEHost(hostlist, num_hosts);
            slurm_spank_log("handleExit: is_fe_host=%d", is_fe_host);
            if (is_fe_host) {
               slurm_spank_log("will call spindleCloseFE");
               result = spindleCloseFE(&args);
               slurm_spank_log("did call spindleCloseFE, result = %d", result);
               if (result == -1) {
                  sdprintf(1, "ERROR: Could not clean up FE.\n");
                  return -1;
               }
            }
         }
      }
   }
   return 0;
}
