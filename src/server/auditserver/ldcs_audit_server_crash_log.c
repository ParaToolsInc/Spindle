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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "ldcs_api.h"
#include "ldcs_audit_server_process.h"
#include "ldcs_audit_server_md.h"
#include "ldcs_audit_server_crash_handler.h"
#include "spindle_launch.h"
#include "msgbundle.h"

#define CRASH_LOG_RANK_WIRE_SIZE (3 * sizeof(int32_t) + sizeof(int64_t))

typedef struct {
   const char *site;
   int32_t site_len;
   int32_t exemplar;
   int32_t nranks;
   const char *ranks;
   size_t ranks_len;
} crash_log_entry_t;

/* Appends a (display rank, hostname, pid, timestamp) row to a site's crash-log rank list. */
void crash_log_append_rank(crash_site_entry_t *e, int32_t rank, int32_t pid,
                           int64_t timestamp, const char *hostname, size_t host_len)
{
   if (e->log_ranks_count >= e->log_ranks_cap) {
      int new_cap = e->log_ranks_cap ? e->log_ranks_cap * 2 : 8;
      e->log_ranks = realloc(e->log_ranks, new_cap * sizeof(*e->log_ranks));
      e->log_ranks_cap = new_cap;
   }
   crash_log_rank_t *row = &e->log_ranks[e->log_ranks_count];
   row->rank = rank;
   row->pid = pid;
   row->timestamp = timestamp;
   row->hostname = malloc(host_len + 1);
   memcpy(row->hostname, hostname, host_len);
   row->hostname[host_len] = '\0';
   e->log_ranks_count++;
}

void crash_log_free_ranks(crash_site_entry_t *e)
{
   int j;
   for (j = 0; j < e->log_ranks_count; ++j)
      free(e->log_ranks[j].hostname);
   free(e->log_ranks);
   e->log_ranks = NULL;
   e->log_ranks_count = 0;
   e->log_ranks_cap = 0;
}

static size_t crash_log_row_size(crash_log_rank_t *row)
{
   return CRASH_LOG_RANK_WIRE_SIZE + strlen(row->hostname);
}

static size_t crash_log_entry_size(crash_site_entry_t *e)
{
   size_t total = 3 * sizeof(int32_t) + e->site_len;
   int j;
   for (j = 0; j < e->log_ranks_count; ++j)
      total += crash_log_row_size(&e->log_ranks[j]);
   return total;
}

static size_t crash_log_entry_pack(char *buf, crash_site_entry_t *e)
{
   size_t pos = 0;
   int32_t site_len32 = (int32_t) e->site_len;
   int32_t exemplar32 = (int32_t) e->exemplar_rank;
   int32_t nranks32 = (int32_t) e->log_ranks_count;
   memcpy(buf + pos, &site_len32, sizeof(int32_t));
   pos += sizeof(int32_t);
   memcpy(buf + pos, e->site, e->site_len);
   pos += e->site_len;
   memcpy(buf + pos, &exemplar32, sizeof(int32_t));
   pos += sizeof(int32_t);
   memcpy(buf + pos, &nranks32, sizeof(int32_t));
   pos += sizeof(int32_t);
   for (int j = 0; j < e->log_ranks_count; ++j) {
      crash_log_rank_t *row = &e->log_ranks[j];
      int32_t host_len32 = (int32_t) strlen(row->hostname);
      memcpy(buf + pos, &row->rank, sizeof(int32_t));
      pos += sizeof(int32_t);
      memcpy(buf + pos, &row->pid, sizeof(int32_t));
      pos += sizeof(int32_t);
      memcpy(buf + pos, &row->timestamp, sizeof(int64_t));
      pos += sizeof(int64_t);
      memcpy(buf + pos, &host_len32, sizeof(int32_t));
      pos += sizeof(int32_t);
      memcpy(buf + pos, row->hostname, (size_t) host_len32);
      pos += (size_t) host_len32;
   }
   return pos;
}

static int crash_log_parse_row(const char *data, size_t len, size_t *pos,
                               int32_t *rank, int32_t *pid, int64_t *timestamp,
                               const char **hostname, int32_t *host_len)
{
   size_t p = *pos;
   if (p + CRASH_LOG_RANK_WIRE_SIZE > len)
      return -1;
   memcpy(rank, data + p, sizeof(int32_t));
   p += sizeof(int32_t);
   memcpy(pid, data + p, sizeof(int32_t));
   p += sizeof(int32_t);
   memcpy(timestamp, data + p, sizeof(int64_t));
   p += sizeof(int64_t);
   memcpy(host_len, data + p, sizeof(int32_t));
   p += sizeof(int32_t);
   if (*host_len < 0 || p + (size_t) *host_len > len)
      return -1;
   *hostname = data + p;
   p += (size_t) *host_len;
   *pos = p;
   return 0;
}

static int crash_log_parse_entry(char *data, size_t len, size_t *pos,
                                 crash_log_entry_t *out)
{
   size_t p = *pos;
   if (p + sizeof(int32_t) > len)
      return -1;
   memcpy(&out->site_len, data + p, sizeof(int32_t));
   p += sizeof(int32_t);
   if (out->site_len <= 0 || p + (size_t) out->site_len > len)
      return -1;
   out->site = data + p;
   p += (size_t) out->site_len;
   if (p + 2 * sizeof(int32_t) > len)
      return -1;
   memcpy(&out->exemplar, data + p, sizeof(int32_t));
   p += sizeof(int32_t);
   memcpy(&out->nranks, data + p, sizeof(int32_t));
   p += sizeof(int32_t);
   if (out->nranks < 0)
      return -1;
   out->ranks = data + p;
   for (int32_t j = 0; j < out->nranks; ++j) {
      int32_t rank, pid, host_len;
      int64_t timestamp;
      const char *hostname;
      if (crash_log_parse_row(data, len, &p, &rank, &pid, &timestamp,
                              &hostname, &host_len) != 0)
         return -1;
   }
   out->ranks_len = p - (size_t) (out->ranks - data);
   *pos = p;
   return 0;
}

static void crash_log_clear_pending(ldcs_process_data_t *procdata)
{
   int i;
   for (i = 0; i < procdata->crash_sites_count; ++i)
      crash_log_free_ranks(&procdata->crash_sites[i]);
}

void crash_log_flush_to_parent(ldcs_process_data_t *procdata)
{
   int i;

   if (!(procdata->opts & OPT_CRASH_LOG))
      return;
   if (ldcs_audit_server_md_is_responsible(procdata, ""))
      return;

   int32_t site_count = 0;
   size_t total = sizeof(int32_t);
   for (i = 0; i < procdata->crash_sites_count; ++i) {
      crash_site_entry_t *e = &procdata->crash_sites[i];
      if (e->log_ranks_count == 0)
         continue;
      site_count++;
      total += crash_log_entry_size(e);
   }
   if (site_count == 0)
      return;

   char *buf = malloc(total);
   memcpy(buf, &site_count, sizeof(int32_t));
   size_t pos = sizeof(int32_t);
   for (i = 0; i < procdata->crash_sites_count; ++i) {
      crash_site_entry_t *e = &procdata->crash_sites[i];
      if (e->log_ranks_count == 0)
         continue;
      pos += crash_log_entry_pack(buf + pos, e);
   }

   ldcs_message_t msg;
   msg.header.type = LDCS_MSG_CRASH_LOG;
   msg.header.len = total;
   msg.data = buf;
   debug_printf2("flushing %d crash sites to parent (%lu bytes)\n",
                 (int) site_count, (unsigned long) total);
   int rc = spindle_forward_query(procdata, &msg);
   free(buf);
   if (rc == -1)
      return;

   crash_log_clear_pending(procdata);
}

/* Non-root servers push pending log data to the parent;
   the root defers the file write until teardown. */
void crash_log_updated(ldcs_process_data_t *procdata)
{
   if (ldcs_audit_server_md_is_responsible(procdata, "")) {
      if (procdata->crash_log_teardown)
         crash_log_root_write(procdata);
   }
   else {
      crash_log_flush_to_parent(procdata);
   }
}

static void crash_log_merge_entry(ldcs_process_data_t *procdata,
                                  crash_log_entry_t *ent)
{
   int j;
   crash_site_entry_t *e = crash_site_find(procdata, ent->site, (size_t) ent->site_len);
   if (!e) {
      e = crash_site_insert(procdata, ent->site, (size_t) ent->site_len);
      e->resolved = 1;
   }
   if (ent->exemplar != -1)
      e->exemplar_rank = (int) ent->exemplar;
   size_t pos = 0;
   for (j = 0; j < (int) ent->nranks; ++j) {
      int32_t r, pid, host_len;
      int64_t timestamp;
      const char *hostname;
      crash_log_parse_row(ent->ranks, ent->ranks_len, &pos, &r, &pid, &timestamp,
                          &hostname, &host_len);
      crash_log_append_rank(e, r, pid, timestamp, hostname, (size_t) host_len);
   }
   debug_printf2("crash log merged site '%s': now %d ranks, exemplar %d\n",
                 e->site, e->log_ranks_count, e->exemplar_rank);
}

int handle_crash_log_recv(ldcs_process_data_t *procdata,
                          node_peer_t peer, ldcs_message_t *msg)
{
   char *data = msg->data;
   size_t len = msg->header.len;
   size_t pos = 0;
   int32_t site_count;
   int i;

   if (ldcs_audit_server_md_is_parent(peer)) {
      err_printf("unexpectedly got CRASH_LOG from peer other than a child\n");
      return -1;
   }

   if (len < sizeof(int32_t))
      goto malformed;
   memcpy(&site_count, data, sizeof(int32_t));
   pos = sizeof(int32_t);
   if (site_count < 0)
      goto malformed;

   debug_printf2("crash log merging %d sites from child\n", (int) site_count);
   for (i = 0; i < site_count; ++i) {
      crash_log_entry_t ent;
      if (crash_log_parse_entry(data, len, &pos, &ent) != 0)
         goto malformed;
      crash_log_merge_entry(procdata, &ent);
   }

   crash_log_updated(procdata);
   return 0;

malformed:
   err_printf("malformed CRASH_LOG message from child\n");
   return -1;
}

static int rank_cmp(const void *a, const void *b)
{
   const crash_log_rank_t *ra = a;
   const crash_log_rank_t *rb = b;
   int c;
   if (ra->rank < rb->rank) return -1;
   if (ra->rank > rb->rank) return 1;
   c = strcmp(ra->hostname, rb->hostname);
   if (c != 0) return c;
   if (ra->pid < rb->pid) return -1;
   if (ra->pid > rb->pid) return 1;
   return 0;
}

#define CRASH_LOG_HEADER "rank,hostname,pid,timestamp,exe,site,exemplar,corepath"

static void format_timestamp(int64_t timestamp, char *buf, size_t buflen)
{
   time_t t = (time_t) timestamp;
   struct tm tm;
   if (!localtime_r(&t, &tm) || strftime(buf, buflen, "%Y-%m-%dT%H:%M:%S%z", &tm) == 0)
      snprintf(buf, buflen, "%lld", (long long) timestamp);
}

static void write_csv_field(FILE *f, const char *s, size_t len)
{
   size_t i;
   int quote = 0;

   /* We have to quote the field if it contains , or " */
   for (i = 0; i < len && !quote; i++)
      quote = (s[i] == ',' || s[i] == '"');
   if (quote)
      fputc('"', f);
   /* Handle CSV escapes */
   for (i = 0; i < len; i++) {
      switch (s[i]) {
         case '\\':
            fputs("\\\\", f);
            break;
         case '\n':
            fputs("\\n", f);
            break;
         case '"':
            fputs("\"\"", f);
            break;
         default:
            fputc(s[i], f);
      }
   }
   if (quote)
      fputc('"', f);
}

/* Write the crash log from the crash log accumulated at the root */
void crash_log_root_write(ldcs_process_data_t *procdata)
{
   int i, nsites = 0;
   char *tmppath;

   if (!(procdata->opts & OPT_CRASH_LOG))
      return;
   if (!ldcs_audit_server_md_is_responsible(procdata, ""))
      return;
   if (!procdata->crash_log || procdata->crash_log[0] == '\0') {
      err_printf("OPT_CRASH_LOG set but no crash-log path present\n");
      return;
   }

   for (i = 0; i < procdata->crash_sites_count; ++i) {
      if (procdata->crash_sites[i].log_ranks_count > 0)
         nsites++;
   }
   if (nsites == 0)
      return;

   tmppath = malloc(strlen(procdata->crash_log) + 5);
   sprintf(tmppath, "%s.tmp", procdata->crash_log);
   FILE *f = fopen(tmppath, "w");
   if (!f) {
      err_printf("Could not open crash log temp file %s for writing: %s\n",
                 tmppath, strerror(errno));
      free(tmppath);
      return;
   }

   fputs(CRASH_LOG_HEADER "\n", f);

   for (i = 0; i < procdata->crash_sites_count; ++i) {
      crash_site_entry_t *e = &procdata->crash_sites[i];
      int j;
      if (e->log_ranks_count == 0)
         continue;
      qsort(e->log_ranks, e->log_ranks_count, sizeof(*e->log_ranks), rank_cmp);
      const char *sep = strchr(e->site, '|');
      const char *exe = sep ? e->site : "";
      size_t exe_len = sep ? (size_t) (sep - e->site) : 0;
      const char *site = sep ? sep + 1 : e->site;
      size_t site_len = strlen(site);
      const char *corepath = e->exemplar_corepath ? e->exemplar_corepath : "";
      size_t corepath_len = strlen(corepath);
      for (j = 0; j < e->log_ranks_count; ++j) {
         crash_log_rank_t *row = &e->log_ranks[j];
         char tsbuf[64];
         format_timestamp(row->timestamp, tsbuf, sizeof(tsbuf));
         fprintf(f, "%d,", (int) row->rank);
         write_csv_field(f, row->hostname, strlen(row->hostname));
         fprintf(f, ",%d,%s,", (int) row->pid, tsbuf);
         write_csv_field(f, exe, exe_len);
         fputc(',', f);
         write_csv_field(f, site, site_len);
         fprintf(f, ",%d,", e->exemplar_rank);
         write_csv_field(f, corepath, corepath_len);
         fputc('\n', f);
      }
   }

   if (fclose(f) != 0) {
      err_printf("Error writing crash log %s: %s\n",
                 tmppath, strerror(errno));
      unlink(tmppath);
      free(tmppath);
      return;
   }
   if (rename(tmppath, procdata->crash_log) != 0) {
      err_printf("Could not rename crash log %s to %s: %s\n",
                 tmppath, procdata->crash_log, strerror(errno));
      unlink(tmppath);
      free(tmppath);
      return;
   }
   free(tmppath);
   debug_printf("crash log: wrote %d sites to %s\n", nsites, procdata->crash_log);
}
