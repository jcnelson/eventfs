/*
   eventfs: a self-cleaning filesystem for event queues.
   Copyright (C) 2015  Jude Nelson

   This program is dual-licensed: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3 or later as 
   published by the Free Software Foundation. For the terms of this 
   license, see LICENSE.LGPLv3+ or <http://www.gnu.org/licenses/>.

   You are free to use this program under the terms of the GNU General
   Public License, but WITHOUT ANY WARRANTY; without even the implied 
   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU General Public License for more details.

   Alternatively, you are free to use this program under the terms of the 
   Internet Software Consortium License, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   For the terms of this license, see LICENSE.ISC or 
   <http://www.isc.org/downloads/software-support-policy/isc-license/>.
*/

#ifndef _EVENTFS_QUOTA_H_
#define _EVENTFS_QUOTA_H_

#include "os.h"
#include "sglib.h"

// quota for a user or group
struct eventfs_quota_entry {
    
    int64_t user_or_group;
    uint64_t max_files;
    uint64_t max_dirs;
    uint64_t max_files_per_dir;
    uint64_t max_bytes;
    
    // rb tree...
    int color;
    struct eventfs_quota_entry* left;
    struct eventfs_quota_entry* right;
};

typedef struct eventfs_quota_entry eventfs_quota;

#define EVENTFS_QUOTA_ENTRY_CMP( qe1, qe2 ) (((qe1)->user_or_group) - ((qe2)->user_or_group))

SGLIB_DEFINE_RBTREE_PROTOTYPES( eventfs_quota, left, right, color, EVENTFS_QUOTA_ENTRY_CMP );


// counts of each resource for a user 
struct eventfs_usage_entry {
    
    int64_t user_or_group;
    uint64_t num_files;
    uint64_t num_dirs;
    uint64_t num_bytes;
    
    // rb tree 
    int color;
    struct eventfs_usage_entry* left;
    struct eventfs_usage_entry* right;
};

typedef struct eventfs_usage_entry eventfs_usage;

#define EVENTFS_USAGE_ENTRY_CMP( qu1, qu2 ) (((qu1)->user_or_group) - ((qu2)->user_or_group))

SGLIB_DEFINE_RBTREE_PROTOTYPES( eventfs_usage, left, right, color, EVENTFS_USAGE_ENTRY_CMP );
    

int eventfs_quota_init( eventfs_quota* q, int64_t user_or_group, uint64_t max_files, uint64_t max_dirs, uint64_t max_files_per_dir, uint64_t max_bytes );
int eventfs_quota_put( eventfs_quota** q, eventfs_quota* new_quota );
int eventfs_quota_set( eventfs_quota** q, int64_t user_or_group, uint64_t max_files, uint64_t max_dirs, uint64_t max_files_per_dir, uint64_t max_bytes );
int eventfs_quota_clear( eventfs_quota** q, int64_t user_or_group );

eventfs_quota* eventfs_quota_lookup( eventfs_quota* q, int64_t user_or_group );

uint64_t eventfs_quota_get_max_files( eventfs_quota* q, int64_t user_or_group );
uint64_t eventfs_quota_get_max_dirs( eventfs_quota* q, int64_t user_or_group );
uint64_t eventfs_quota_get_max_files_per_dir( eventfs_quota* q, int64_t user_or_group );
uint64_t eventfs_quota_get_max_bytes( eventfs_quota* q, int64_t user_or_group );

int eventfs_usage_init( eventfs_usage* u, int64_t user_or_group, uint64_t num_files, uint64_t num_dirs, uint64_t num_bytes );
int eventfs_usage_put( eventfs_usage** u, eventfs_usage* new_usage );

eventfs_usage* eventfs_usage_lookup( eventfs_usage* u, int64_t user_or_group );

uint64_t eventfs_usage_get_num_files( eventfs_usage* u, int64_t user_or_group );
uint64_t eventfs_usage_get_num_dirs( eventfs_usage* u, int64_t user_or_group );
uint64_t eventfs_usage_get_num_bytes( eventfs_usage* u, int64_t user_or_group );

void eventfs_usage_change_num_files( eventfs_usage* u, int64_t user_or_group, int change );
void eventfs_usage_change_num_dirs( eventfs_usage* u, int64_t user_or_group, int change );
void eventfs_usage_change_num_bytes( eventfs_usage* u, int64_t user_or_group, int change );

void eventfs_quota_free( eventfs_quota* q );
void eventfs_usage_free( eventfs_usage* u );

#endif 