/*
 * Copyright (C) 2026 The pgmoneta community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <pgmoneta.h>
#include <mctf.h>
#include <utils.h>
#include <walfile/rmgr.h>
#include <walfile/rm_xact.h>
#include <walfile/wal_reader.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Build a COMMIT or ABORT main data area with one dropped stats item followed
 * by either one catcache invalidation (id 7) or a two-phase xid (777).
 * PostgreSQL 15 to 17 write a 12 byte stats item, 18 and later 16 bytes.
 */
static uint32_t
build_xact_record(char* data, int version, uint32_t xinfo)
{
   uint32_t off = 0;
   int32_t one = 1;
   uint32_t item[4] = {2, 5, 16384, 0}; /* kind, dboid, objid (lo, hi) */
   union shared_invalidation_message msg;
   transaction_id xid = 777;

   memset(data, 0, 128);
   off += sizeof(timestamp_tz);
   memcpy(data + off, &xinfo, sizeof(xinfo));
   off += sizeof(xinfo);

   memcpy(data + off, &one, sizeof(one));
   off += sizeof(one);
   memcpy(data + off, item, version >= 18 ? 16 : 12);
   off += version >= 18 ? 16 : 12;

   if (xinfo & XACT_XINFO_HAS_INVALS)
   {
      memset(&msg, 0, sizeof(msg));
      msg.id = 7;
      memcpy(data + off, &one, sizeof(one));
      off += sizeof(one);
      memcpy(data + off, &msg, sizeof(msg));
      off += sizeof(msg);
   }

   if (xinfo & XACT_XINFO_HAS_TWOPHASE)
   {
      memcpy(data + off, &xid, sizeof(xid));
      off += sizeof(xid);
   }

   return off;
}

static char*
describe_xact(int version, uint8_t info, uint32_t xinfo)
{
   struct server srv;
   struct server* saved = server_config;
   struct decoded_xlog_record record;
   uint64_t data[16];
   char* description = NULL;

   memset(&srv, 0, sizeof(srv));
   srv.version = version;
   server_config = &srv;

   memset(&record, 0, sizeof(record));
   record.header.xl_rmid = RM_XACT_ID;
   record.header.xl_info = info | XLOG_XACT_HAS_INFO;
   record.main_data = (char*)data;
   record.main_data_len = build_xact_record((char*)data, version, xinfo);

   description = pgmoneta_wal_xact_desc(pgmoneta_append(NULL, ""), &record);

   server_config = saved;
   return description;
}

MCTF_TEST(test_xact_commit_dropped_stats_invals)
{
   int versions[] = {17, 18};
   char* description = NULL;

   for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
   {
      description = describe_xact(versions[i], XLOG_XACT_COMMIT,
                                  XACT_XINFO_HAS_DROPPED_STATS | XACT_XINFO_HAS_INVALS);
      MCTF_ASSERT_PTR_NONNULL(description, cleanup, "commit was not described");
      MCTF_ASSERT(strstr(description, "; inval msgs: catcache 7") != NULL, cleanup,
                  "invalidations after dropped stats are read from the wrong offset");
      free(description);
      description = NULL;
   }

cleanup:
   free(description);
   MCTF_FINISH();
}

MCTF_TEST(test_xact_abort_prepared_dropped_stats_twophase)
{
   int versions[] = {17, 18};
   char* description = NULL;

   for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
   {
      description = describe_xact(versions[i], XLOG_XACT_ABORT_PREPARED,
                                  XACT_XINFO_HAS_DROPPED_STATS | XACT_XINFO_HAS_TWOPHASE);
      MCTF_ASSERT_PTR_NONNULL(description, cleanup, "abort was not described");
      MCTF_ASSERT(strncmp(description, "777: ", 5) == 0, cleanup,
                  "two-phase xid after dropped stats is read from the wrong offset");
      free(description);
      description = NULL;
   }

cleanup:
   free(description);
   MCTF_FINISH();
}

/*
 * Build a PREPARE main data area the way twophase.c writes it: the on-disk
 * xl_xact_prepare header, the GID, one delete-on-commit relation, two
 * dropped stats items and one catcache invalidation (id 7), each MAXALIGN'd.
 * With truncate set, the record ends before the invalidation.
 */
static uint32_t
build_prepare_record(char* data, int version, bool truncate)
{
   struct xl_xact_prepare_v15 hdr;
   struct rel_file_node rel = {1663, 5, 16390};
   uint32_t item[4] = {2, 5, 16384, 0}; /* kind, dboid, objid (lo, hi) */
   size_t item_size = version >= 18 ? 16 : 12;
   union shared_invalidation_message msg;
   uint32_t off = 0;

   memset(data, 0, 256);
   memset(&hdr, 0, sizeof(hdr));
   hdr.xid = 777;
   hdr.database = 5;
   hdr.ncommitrels = 1;
   hdr.ncommitstats = 2;
   hdr.ninvalmsgs = 1;
   hdr.gidlen = 3;
   memcpy(data, &hdr, sizeof(hdr));
   off += MAXALIGN(sizeof(hdr));

   memcpy(data + off, "g1", 3);
   off += MAXALIGN(3);

   memcpy(data + off, &rel, sizeof(rel));
   off += MAXALIGN(sizeof(rel));

   memcpy(data + off, item, item_size);
   memcpy(data + off + item_size, item, item_size);
   off += MAXALIGN(2 * item_size);

   if (truncate)
   {
      return off;
   }

   memset(&msg, 0, sizeof(msg));
   msg.id = 7;
   memcpy(data + off, &msg, sizeof(msg));
   off += MAXALIGN(sizeof(msg));

   return off;
}

static char*
describe_prepare(int version, bool truncate)
{
   struct server srv;
   struct server* saved = server_config;
   struct decoded_xlog_record record;
   uint64_t data[32];
   char* description = NULL;

   memset(&srv, 0, sizeof(srv));
   srv.version = version;
   server_config = &srv;

   memset(&record, 0, sizeof(record));
   record.header.xl_rmid = RM_XACT_ID;
   record.header.xl_info = XLOG_XACT_PREPARE;
   record.main_data = (char*)data;
   record.main_data_len = build_prepare_record((char*)data, version, truncate);

   description = pgmoneta_wal_xact_desc(pgmoneta_append(NULL, ""), &record);

   server_config = saved;
   return description;
}

MCTF_TEST(test_xact_prepare_gid_rels_stats_invals)
{
   int versions[] = {17, 18};
   char* description = NULL;

   for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++)
   {
      description = describe_prepare(versions[i], false);
      MCTF_ASSERT_PTR_NONNULL(description, cleanup, "prepare was not described");
      MCTF_ASSERT(strncmp(description, "gid g1: ", 8) == 0, cleanup,
                  "gid is not read from after the on-disk header");
      MCTF_ASSERT(strstr(description, "; rels(commit): base/5/16390") != NULL, cleanup,
                  "commit relations are read from the wrong offset");
      MCTF_ASSERT(strstr(description, "; inval msgs: catcache 7") != NULL, cleanup,
                  "invalidations after dropped stats are read from the wrong offset");
      free(description);
      description = NULL;
   }

cleanup:
   free(description);
   MCTF_FINISH();
}

MCTF_TEST(test_xact_prepare_truncated)
{
   char* description = NULL;

   description = describe_prepare(18, true);
   MCTF_ASSERT_PTR_NONNULL(description, cleanup, "prepare was not described");
   MCTF_ASSERT(strstr(description, "; rels(commit): base/5/16390") != NULL, cleanup,
               "commit relations inside the record are lost");
   MCTF_ASSERT(strstr(description, "inval msgs") == NULL, cleanup,
               "invalidations beyond the end of the record are read");

cleanup:
   free(description);
   MCTF_FINISH();
}
