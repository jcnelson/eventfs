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

#include "quota.h"
#include "util.h"
#include "eventfs.h"

SGLIB_DEFINE_RBTREE_FUNCTIONS( eventfs_quota, left, right, color, EVENTFS_QUOTA_ENTRY_CMP );
SGLIB_DEFINE_RBTREE_FUNCTIONS( eventfs_usage, left, right, color, EVENTFS_QUOTA_ENTRY_CMP );


// set up a new quota. 
// always succeeds
int eventfs_quota_init( eventfs_quota* q, int64_t user_or_group, uint64_t max_files, uint64_t max_dirs, uint64_t max_files_per_dir, uint64_t max_bytes ) {
    
    memset( q, 0, sizeof(eventfs_quota) );
    
    q->user_or_group = user_or_group;
    q->max_files = max_files;
    q->max_dirs = max_dirs;
    q->max_files_per_dir = max_files_per_dir;
    q->max_bytes = max_bytes;
    return 0;
}

// put a new quota into a quota set 
// always succeeds
// NOTE: new_quota should be malloc'ed
// NOTE: no checks for existence will be performed; the caller must do so itself
int eventfs_quota_put( eventfs_quota** q, eventfs_quota* new_quota ) {
    
    sglib_eventfs_quota_add( q, new_quota );
    return 0;
}
    

// set a quota for a user or group
// return 0 on success 
// return -ENOMEM on OOM 
int eventfs_quota_set( eventfs_quota** q, int64_t user_or_group, uint64_t max_files, uint64_t max_dirs, uint64_t max_files_per_dir, uint64_t max_bytes ) {
    
    int rc = 0;
    eventfs_quota* member = NULL;
    eventfs_quota lookup;
    
    memset( &lookup, 0, sizeof(eventfs_quota) );
    lookup.user_or_group = user_or_group;
    
    member = sglib_eventfs_quota_find_member( *q, &lookup );
    if( member == NULL ) {
        
        // no quota defined. set it
        member = EVENTFS_CALLOC( eventfs_quota, 1 );
        if( member == NULL ) {
            return -ENOMEM;
        }
        
        eventfs_quota_init( member, user_or_group, max_files, max_dirs, max_files_per_dir, max_bytes );
        eventfs_quota_put( q, member );
    }
    else {
        
        // quota is defined.  reset it.
        eventfs_quota_init( member, user_or_group, max_files, max_dirs, max_files_per_dir, max_bytes );
    }
    
    return 0;
}


// remove a quota for a user or group
// return 0 on success
int eventfs_quota_clear( eventfs_quota** q, int64_t user_or_group ) {
    
    int rc = 0;
    eventfs_quota* member = NULL;
    eventfs_quota lookup;
    
    memset( &lookup, 0, sizeof(eventfs_quota) );
    lookup.user_or_group = user_or_group;
    
    sglib_eventfs_quota_delete_if_member( q, &lookup, &member );
    if( member != NULL ) {
        
        eventfs_safe_free( member );
    }
    return 0;
}


// look up the quota entry for a user or group
// return a pointer to the entry on success
// return NULL if not found 
eventfs_quota* eventfs_quota_lookup( eventfs_quota* q, int64_t user_or_group ) {
    
    eventfs_quota* member = NULL;
    eventfs_quota lookup;
    
    memset( &lookup, 0, sizeof(eventfs_quota) );
    lookup.user_or_group = user_or_group;
    member = sglib_eventfs_quota_find_member( q, &lookup );
    
    return member;
}


// get the maximum number of files this user may create
// return the number on success
// return 0 if not found
uint64_t eventfs_quota_get_max_files( eventfs_quota* q, int64_t user_or_group ) {
    
    int rc = 0;
    eventfs_quota* member = NULL;
    
    member = eventfs_quota_lookup( q, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no files
        return 0;
    }
    else {
        
        return member->max_files;
    }
}


// get the maximum number of directories this user may create 
// return the number on success 
// return 0 if not found
uint64_t eventfs_quota_get_max_dirs( eventfs_quota* q, int64_t user_or_group ) {
    
    int rc = 0;
    eventfs_quota* member = NULL;
    
    member = eventfs_quota_lookup( q, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no dirs
        return 0;
    }
    else {
        
        return member->max_dirs;
    }
}


// get the maximum number of files per directory this user may create
// return the number on success
// return 0 if not found 
uint64_t eventfs_quota_get_max_files_per_dir( eventfs_quota* q, int64_t user_or_group ) {
    
    int rc = 0;
    eventfs_quota* member = NULL;
    
    member = eventfs_quota_lookup( q, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no files
        return 0;
    }
    else {
        
        return member->max_files_per_dir;
    }
}

// get the maximum number of bytes this user can have
// return the number on success
// return 0 if not found 
uint64_t eventfs_quota_get_max_bytes( eventfs_quota* q, int64_t user_or_group ) {
    
    int rc = 0;
    eventfs_quota* member = NULL;
    
    member = eventfs_quota_lookup( q, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no files
        return 0;
    }
    else {
        
        return member->max_bytes;
    }
}


// make a usage instance 
int eventfs_usage_init( eventfs_usage* u, int64_t user_or_group, uint64_t num_files, uint64_t num_dirs, uint64_t num_bytes ) {
    
    memset( u, 0, sizeof(eventfs_usage) );
    
    u->user_or_group = user_or_group;
    u->num_files = num_files;
    u->num_dirs = num_dirs;
    u->num_bytes = num_bytes;
    
    return 0;
}

// put a usage 
int eventfs_usage_put( eventfs_usage** u, eventfs_usage* new_usage ) {
    
    sglib_eventfs_usage_add( u, new_usage );
    return 0;
}


// look up the usage entry for a user or group
// return a pointer to the entry on success
// return NULL if not found 
eventfs_usage* eventfs_usage_lookup( eventfs_usage* u, int64_t user_or_group ) {
    
    eventfs_usage* member = NULL;
    eventfs_usage lookup;
    
    memset( &lookup, 0, sizeof(eventfs_usage) );
    lookup.user_or_group = user_or_group;
    member = sglib_eventfs_usage_find_member( u, &lookup );
    
    return member;
}


// get the currently-used number of files this user created 
// return the number on success
// return 0 if not found 
uint64_t eventfs_usage_get_num_files( eventfs_usage* u, int64_t user_or_group ) {
    
    eventfs_usage* member = NULL;
    
    member = eventfs_usage_lookup( u, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no files
        return 0;
    }
    else {
        
        return member->num_files;
    }
}


// get the currently-used number of directories this user created 
// return the number on success 
// return 0 if not found 
uint64_t eventfs_usage_get_num_dirs( eventfs_usage* u, int64_t user_or_group ) {
    
    eventfs_usage* member = NULL;
    
    member = eventfs_usage_lookup( u, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no dirs
        return 0;
    }
    else {
        
        return member->num_dirs;
    }
}


// get the currently-used number of bytes this user wrote 
// return the number on success 
// return 0 if not found 
uint64_t eventfs_usage_get_num_bytes( eventfs_usage* u, int64_t user_or_group ) {
    
    eventfs_usage* member = NULL;
    
    member = eventfs_usage_lookup( u, user_or_group );
    if( member == NULL ) {
        
        // no quota set? no bytes
        return 0;
    }
    else {
        
        return member->num_bytes;
    }
}


// change the currently-used number of files this user owns 
void eventfs_usage_change_num_files( eventfs_usage* u, int64_t user_or_group, int change ) {
    
    eventfs_usage* member = NULL;
    
    member = eventfs_usage_lookup( u, user_or_group );
    if( member != NULL ) {
        
        __sync_fetch_and_add( &member->num_files, (uint64_t)change );
    }
}


// change the currently-used number of directories this user owns 
void eventfs_usage_change_num_dirs( eventfs_usage* u, int64_t user_or_group, int change ) {
    
    eventfs_usage* member = NULL;
    
    member = eventfs_usage_lookup( u, user_or_group );
    if( member != NULL ) {
        
        __sync_fetch_and_add( &member->num_dirs, (uint64_t)change );
    }
}


// change the currently-used number of bytes this user owns
void eventfs_usage_change_num_bytes( eventfs_usage* u, int64_t user_or_group, int change ) {
    
    eventfs_usage* member = NULL;
    
    member = eventfs_usage_lookup( u, user_or_group );
    if( member != NULL ) {
        
        __sync_fetch_and_add( &member->num_bytes, (uint64_t)change );
    }
}


// free a quota table 
void eventfs_quota_free( eventfs_quota* q ) {
   
   struct sglib_eventfs_quota_iterator itr;
   struct eventfs_quota_entry* dp = NULL;
   struct eventfs_quota_entry* old_dp = NULL;

   for( dp = sglib_eventfs_quota_it_init_inorder( &itr, q ); dp != NULL; ) {
      
      old_dp = dp;
      dp = sglib_eventfs_quota_it_next( &itr );
      
      eventfs_safe_free( old_dp );
   }
}


// free a usage table 
void eventfs_usage_free( eventfs_usage* u ) {
   
   struct sglib_eventfs_usage_iterator itr;
   struct eventfs_usage_entry* dp = NULL;
   struct eventfs_usage_entry* old_dp = NULL;

   for( dp = sglib_eventfs_usage_it_init_inorder( &itr, u ); dp != NULL; ) {
      
      old_dp = dp;
      dp = sglib_eventfs_usage_it_next( &itr );
      
      eventfs_safe_free( old_dp );
   }
}
