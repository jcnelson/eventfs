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

#ifndef _EVENTFS_H_
#define _EVENTFS_H_

#include <fskit/fskit.h>
#include <fskit/fuse/fskit_fuse.h>

#include "config.h"
#include "deferred.h"
#include "inode.h"
#include "os.h"
#include "util.h"
#include "wq.h"
#include "quota.h"

struct eventfs_state {
    
    struct fskit_core* core;
    struct fskit_fuse_state* fuse_state;
    struct eventfs_config config;
    struct eventfs_wq* deferred_wq;
    
    pthread_rwlock_t quota_lock;
    eventfs_quota* user_quotas;
    eventfs_quota* group_quotas;
    
    eventfs_usage* user_usages;
    eventfs_usage* group_usages;
    
    char* mountpoint;
};

int eventfs_quota_rlock( struct eventfs_state* eventfs );
int eventfs_quota_wlock( struct eventfs_state* eventfs );
int eventfs_quota_unlock( struct eventfs_state* eventfs );

#endif