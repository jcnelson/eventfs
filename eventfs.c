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

#include "eventfs.h"

// command-line options 
struct eventfs_opts {
   
    char* config_path;
};

// try to prune dead directories every REAP_FREQUENCY mkdir's
#define REAP_FREQUENCY 10
int g_mkdir_count = 0;


// locks on the quota sets 
int eventfs_quota_rlock( struct eventfs_state* eventfs ) {
    return pthread_rwlock_rdlock( &eventfs->quota_lock );
}

int eventfs_quota_wlock( struct eventfs_state* eventfs ) {
    return pthread_rwlock_wrlock( &eventfs->quota_lock );
}

int eventfs_quota_unlock( struct eventfs_state* eventfs ) {
    return pthread_rwlock_unlock( &eventfs->quota_lock );
}

// create a eventfs file 
// return 0 on success
// return -ENOMEM on OOM 
// return negative on failure to initialize the inode
int eventfs_create( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, mode_t mode, void** inode_data, void** handle_data ) {
   
   eventfs_debug("eventfs_create(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   int rc = 0;
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   
   // NOTE: parent will be write-locked
   struct fskit_entry* parent = fskit_route_metadata_get_parent( route_metadata );
   struct eventfs_dir_inode* parent_inode = NULL;
   struct eventfs_file_inode* inode = NULL;
   
   char name[FSKIT_FILESYSTEM_NAMEMAX+1];
   memset( name, 0, FSKIT_FILESYSTEM_NAMEMAX + 1 );
   
   pid_t calling_tid = fskit_fuse_get_pid();
   uid_t calling_uid = fskit_fuse_get_uid( eventfs->fuse_state );
   gid_t calling_gid = fskit_fuse_get_gid( eventfs->fuse_state );
   
   uint64_t file_quota_user = eventfs->config.default_file_quota;
   uint64_t dir_size_quota = eventfs->config.default_files_per_dir_quota;
   uint64_t file_quota_group = eventfs->config.default_file_quota;
   
   uint64_t num_files_user = 0;
   uint64_t num_files_group = 0;
   uint64_t num_dir_children = 0;
   uid_t parent_owner = 0;
   gid_t parent_group = 0;
   bool unknown_user = false;
   bool unknown_group = false;
   
   eventfs_usage* new_user_usage = NULL;
   eventfs_usage* new_group_usage = NULL;
   
   num_dir_children = fskit_entry_get_num_children( parent );
   parent_owner = fskit_entry_get_owner( parent );
   parent_group = fskit_entry_get_group( parent );
   
   // look up quotas
   eventfs_quota_rlock( eventfs );
   
   if( eventfs_quota_lookup( eventfs->user_quotas, parent_owner ) != NULL ) {
       
      dir_size_quota = eventfs_quota_get_max_files_per_dir( eventfs->user_quotas, parent_owner );
   }
   else if( eventfs_quota_lookup( eventfs->group_quotas, parent_group ) != NULL ) {
      
      dir_size_quota = eventfs_quota_get_max_files_per_dir( eventfs->group_quotas, parent_group );
   }
   
   if( eventfs_quota_lookup( eventfs->user_quotas, calling_uid ) != NULL ) {
      
      file_quota_user = eventfs_quota_get_max_files( eventfs->user_quotas, calling_uid );
   }
   if( eventfs_usage_lookup( eventfs->user_usages, calling_uid ) != NULL ) {
       
      num_files_user = eventfs_usage_get_num_files( eventfs->user_usages, calling_uid );
   }
   else {
       
      unknown_user = true;
   }
   
   if( eventfs_quota_lookup( eventfs->group_quotas, calling_gid ) != NULL ) {
       
      file_quota_group = eventfs_quota_get_max_files( eventfs->group_quotas, calling_gid );
   }
   if( eventfs_usage_lookup( eventfs->group_usages, calling_gid ) != NULL ) {
       
      num_files_group = eventfs_usage_get_num_files( eventfs->group_usages, calling_gid );
   }
   else {
       
      unknown_group = true;
   }
   
   eventfs_quota_unlock( eventfs );
   
   // check quotas 
   if( dir_size_quota + 2 <= num_dir_children ) {
        
       printf("User %d has per-directory quota of %d; using %d\n", calling_uid, (int)dir_size_quota, (int)(num_dir_children) );
       
       // directory has gotten too big
       return -EDQUOT;
   }
   
   if( file_quota_user <= num_files_user ) {
    
       printf("User %d has file quota of %d; using %d\n", calling_uid, (int)file_quota_user, (int)(num_files_user) );
       
       // user has too many files
       // BUT!  Can we reap some directories?
       rc = eventfs_deferred_reap( eventfs );
       if( rc != 0 ) {
        
           eventfs_error("eventfs_deferred_reap rc = %d\n", rc );
           rc = 0;
       }
       return -EDQUOT;
   }
   
   if( file_quota_group <= num_files_group ) {
       
       printf("Group %d has file quota of %d; using %d\n", calling_gid, (int)file_quota_group, (int)(num_files_group) );
       
       // group has too many files 
       // BUT!  Can we reap some directories?
       rc = eventfs_deferred_reap( eventfs );
       if( rc != 0 ) {
        
           eventfs_error("eventfs_deferred_reap rc = %d\n", rc );
           rc = 0;
       }
       return -EDQUOT;
   }
   
   // set up new usages, if we need to 
   if( unknown_user ) {
       
       new_user_usage = EVENTFS_CALLOC( eventfs_usage, 1 );
       if( new_user_usage == NULL ) {
           return -ENOMEM;
       }
   }
   
   if( unknown_group ) {
       
       new_group_usage = EVENTFS_CALLOC( eventfs_usage, 1 );
       if( new_group_usage == NULL ) {
           
           eventfs_safe_free( new_user_usage );
           return -ENOMEM;
       }
   }
   
   // set up inode
   inode = EVENTFS_CALLOC( struct eventfs_file_inode, 1 );
   if( inode == NULL ) {
       
      eventfs_safe_free( new_user_usage );
      eventfs_safe_free( new_group_usage );
      return -ENOMEM;
   }
   
   rc = eventfs_file_inode_init( inode );
   if( rc != 0 ) {
       
      // phantom process?
      eventfs_safe_free( inode );
      eventfs_safe_free( new_user_usage );
      eventfs_safe_free( new_group_usage );
      return rc;
   }
   
   // attach to parent (will already be write-locked)
   parent_inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );
   if( parent_inode == NULL ) {
       
       eventfs_error("BUG: parent %p has no inode data!\n", parent );
       eventfs_file_inode_free( inode );
       eventfs_safe_free( inode );
       eventfs_safe_free( new_user_usage );
       eventfs_safe_free( new_group_usage );
       return -EIO;
   }
   
   rc = eventfs_dir_inode_append( core, parent_inode, parent, fskit_route_metadata_get_name( route_metadata ) );
   if( rc != 0 ) {
       
       // failed 
       eventfs_file_inode_free( inode );
       eventfs_safe_free( inode );
       eventfs_safe_free( new_user_usage );
       eventfs_safe_free( new_group_usage );
       return rc;
   }
   
   *inode_data = (void*)inode;
   
   // update usages 
   if( !unknown_user ) {
      
      eventfs_usage_change_num_files( eventfs->user_usages, calling_uid, 1 );
   }
   else {
      
      eventfs_usage_init( new_user_usage, calling_uid, 1, 0, 0 );
      eventfs_usage_put( &eventfs->user_usages, new_user_usage );
   }
   
   if( !unknown_group ) {
      
      eventfs_usage_change_num_files( eventfs->group_usages, calling_gid, 1 );
   }
   else {
      
      eventfs_usage_init( new_group_usage, calling_gid, 1, 0, 0 );
      eventfs_usage_put( &eventfs->group_usages, new_group_usage );
   }
   
   return rc;
}


// create a directory 
// in eventfs, there can only be one "layer" of directories.
// return 0 on success, and set *inode_data
// return -ENOMEM on OOM 
// return negative on failure to initialize the inode
int eventfs_mkdir( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* dent, mode_t mode, void** inode_data ) {
   
   eventfs_debug("eventfs_mkdir(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   int rc = 0;
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   
   struct fskit_entry* head = NULL;
   struct fskit_entry* tail = NULL;
   
   char* path = fskit_route_metadata_get_path( route_metadata );
   
   if( fskit_depth( path ) > 1 ) {
       
       // not allowed 
       return -EPERM;
   }
   
   // NOTE: parent will be write-locked
   struct fskit_entry* parent = fskit_route_metadata_get_parent( route_metadata );
   
   struct eventfs_dir_inode* parent_inode = NULL;
   struct eventfs_dir_inode* inode = NULL;
   char name[FSKIT_FILESYSTEM_NAMEMAX+1];
   memset( name, 0, FSKIT_FILESYSTEM_NAMEMAX + 1 );
   
   pid_t calling_tid = fskit_fuse_get_pid();
   uid_t calling_uid = fskit_fuse_get_uid( eventfs->fuse_state );
   gid_t calling_gid = fskit_fuse_get_gid( eventfs->fuse_state );
   
   uint64_t dir_quota_user = eventfs->config.default_dir_quota;
   uint64_t dir_quota_group = eventfs->config.default_dir_quota;
   
   uint64_t num_dirs_user = 0;
   uint64_t num_dirs_group = 0;
   bool unknown_user = false;
   bool unknown_group = false;
   
   eventfs_usage* new_user_usage = NULL;
   eventfs_usage* new_group_usage = NULL;
   
   // look up quotas
   eventfs_quota_rlock( eventfs );
   
   if( eventfs_quota_lookup( eventfs->user_quotas, calling_uid ) != NULL ) {
      
       dir_quota_user = eventfs_quota_get_max_dirs( eventfs->user_quotas, calling_uid );
   }
   if( eventfs_usage_lookup( eventfs->user_usages, calling_uid ) != NULL ) {
       
       num_dirs_user = eventfs_usage_get_num_dirs( eventfs->user_usages, calling_uid );
   }
   else {
       
       unknown_user = true;
   }
   
   if( eventfs_quota_lookup( eventfs->group_quotas, calling_gid ) != NULL ) {
       
       dir_quota_group = eventfs_quota_get_max_dirs( eventfs->group_quotas, calling_gid );
   }
   else if( eventfs_usage_lookup( eventfs->group_usages, calling_gid ) != NULL ) {
       
       num_dirs_group = eventfs_usage_get_num_dirs( eventfs->group_usages, calling_gid );
   }
   else {
      
       unknown_group = true;
   }
   
   eventfs_quota_unlock( eventfs );
   
   // check quotas
   if( dir_quota_user <= num_dirs_user ) {
    
       // user has too many dirs
       // BUT!  Can we reap some of them?
       rc = eventfs_deferred_reap( eventfs );
       if( rc != 0 ) {
        
           eventfs_error("eventfs_deferred_reap rc = %d\n", rc );
           rc = 0;
       }
       return -EDQUOT;
   }
   
   if( dir_quota_group <= num_dirs_group ) {
       
       // group has too many dirs 
       // BUT!  Can we reap some of them?
       rc = eventfs_deferred_reap( eventfs );
       if( rc != 0 ) {
        
           eventfs_error("eventfs_deferred_reap rc = %d\n", rc );
           rc = 0;
       }
       return -EDQUOT;
   }
   
   // set up new usages, if we need to 
   if( unknown_user ) {
       
       new_user_usage = EVENTFS_CALLOC( eventfs_usage, 1 );
       if( new_user_usage == NULL ) {
           return -ENOMEM;
       }
   }
   
   if( unknown_group ) {
       
       new_group_usage = EVENTFS_CALLOC( eventfs_usage, 1 );
       if( new_group_usage == NULL ) {
           
           eventfs_safe_free( new_user_usage );
           return -ENOMEM;
       }
   }
   
   inode = EVENTFS_CALLOC( struct eventfs_dir_inode, 1 );
   if( inode == NULL ) {
       
       // OOM 
       eventfs_safe_free( new_user_usage );
       eventfs_safe_free( new_group_usage );
       return -ENOMEM;
   }
   
   // set up directory state
   rc = eventfs_dir_inode_init( inode, calling_tid, EVENTFS_VERIFY_DEFAULT );
   if( rc != 0 ) {
       
       // phantom process?
       eventfs_safe_free( new_user_usage );
       eventfs_safe_free( new_group_usage );
       eventfs_safe_free( inode );
       return rc;
   }
   
   *inode_data = (void*)inode;
   
   int cur_mkdir_count = __sync_add_and_fetch( &g_mkdir_count, 1 );
   
   if( cur_mkdir_count == REAP_FREQUENCY ) {
       
       cur_mkdir_count = __sync_sub_and_fetch( &g_mkdir_count, REAP_FREQUENCY );
       
       if( cur_mkdir_count < REAP_FREQUENCY ) {
          
          // Either the process that decremented g_mkdir_count gets this far, 
          // or it doesn't. If it doesn't--that is, g_mkdir_count gets incremented 
          // more than REAP_FREQUENCY times between this process's __sync_add_and_fetch
          // and its subsequent __sync_sub_and_fetch, then it means at least one 
          // other process will have observed cur_mkdir_count == REAP_FREQUENCY,
          // and exactly one of them will observe cur_mkdir_count < REAP_FREQUENCY.
          // The point is, a high volume of mkdirs should not starve the reaper thread.
          eventfs_debug("%s", "Reap dead directories\n" );
          
          rc = eventfs_deferred_reap( eventfs );
          if( rc != 0 ) {
            
              eventfs_error("eventfs_deferred_reap rc = %d\n", rc );
              rc = 0;
          }
       }
   }
   
   // update usages 
   if( new_user_usage == NULL ) {
      
      eventfs_usage_change_num_dirs( eventfs->user_usages, calling_uid, 1 );
   }
   else {
      
      eventfs_usage_init( new_user_usage, calling_uid, 0, 1, 0 );
      eventfs_usage_put( &eventfs->user_usages, new_user_usage );
   }
   
   if( new_group_usage == NULL ) {
      
      eventfs_usage_change_num_dirs( eventfs->group_usages, calling_gid, 1 );
   }
   else {
      
      eventfs_usage_init( new_group_usage, calling_gid, 0, 1, 0 );
      eventfs_usage_put( &eventfs->group_usages, new_group_usage );
   }
   
   // success!
   return rc;
}


// read a file 
// return the number of bytes read on success
// return 0 on EOF 
// return -ENOSYS if the inode is not initialize (should *never* happen)
int eventfs_read( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, char* buf, size_t buflen, off_t offset, void* handle_data ) {
   
   eventfs_debug("eventfs_read(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
   int num_read = 0;
   
   if( inode == NULL ) {
      return -ENOSYS;
   }
   
   if( offset >= inode->size ) {
      return 0;
   }
   
   // copy data out, if we have any 
   if( inode->contents != NULL ) {
      
      num_read = buflen;
      
      if( (unsigned)(offset + buflen) >= inode->size ) {
         
         num_read = inode->size - offset;
         
         if( num_read < 0 ) {
            num_read = 0;
         }
      }
   }
   
   if( num_read > 0 ) {
      
      memcpy( buf, inode->contents + offset, num_read );
   }
   
   return num_read;
}

// write to a file 
// return the number of bytes written, and expand the file in RAM if we write off the edge.
// return -ENOSYS if for some reason we don't have an inode (should *never* happen)
// return -ENOMEM on OOM
// NOTE: we use FSKIT_INODE_SEQUENTIAL, so fent will be write-locked
int eventfs_write( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, char* buf, size_t buflen, off_t offset, void* handle_data ) {
   
   eventfs_debug("eventfs_write(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
   size_t new_contents_len = inode->contents_len;
   
   if( inode == NULL ) {
      return -ENOSYS;
   }
   
   off_t cur_size = inode->size;
   int64_t add_to_usage = (cur_size >= offset + buflen ? 0 : (offset + buflen) - cur_size);
   
   pid_t calling_tid = fskit_fuse_get_pid();
   uid_t owner_uid = fskit_entry_get_owner( fent );
   gid_t owner_gid = fskit_entry_get_group( fent );
   
   uint64_t bytes_quota_user = eventfs->config.default_bytes_quota;
   uint64_t bytes_quota_group = eventfs->config.default_bytes_quota;
   
   uint64_t num_bytes_user = 0;
   uint64_t num_bytes_group = 0;
   bool unknown_user = false;
   bool unknown_group = false;
   
   // look up quotas
   eventfs_quota_rlock( eventfs );
   
   if( eventfs_quota_lookup( eventfs->user_quotas, owner_uid ) != NULL ) {
      
       bytes_quota_user = eventfs_quota_get_max_bytes( eventfs->user_quotas, owner_uid );
   }
   if( eventfs_usage_lookup( eventfs->user_usages, owner_uid ) != NULL ) {
       
       num_bytes_user = eventfs_usage_get_num_bytes( eventfs->user_usages, owner_uid );
   }
   else {
       
       unknown_user = true;
   }
   
   if( eventfs_quota_lookup( eventfs->group_quotas, owner_gid ) != NULL ) {
       
      bytes_quota_group = eventfs_quota_get_max_bytes( eventfs->group_quotas, owner_gid );
   }
   if( eventfs_usage_lookup( eventfs->group_usages, owner_gid ) != NULL ) {
       
      num_bytes_group = eventfs_usage_get_num_bytes( eventfs->group_usages, owner_gid );
   }
   else {
       
      unknown_group = true;
   }
   
   eventfs_quota_unlock( eventfs );
   
   if( unknown_user && unknown_group ) {
       
       // something weird is going on 
       return -EDQUOT;
   }
       
   // check quotas
   if( bytes_quota_user <= num_bytes_user + add_to_usage ) {
    
       printf("User %d has byte quota of %d; using %d (%d)\n", owner_uid, (int)bytes_quota_user, (int)(num_bytes_user + add_to_usage), (int)add_to_usage );
       // user has too many bytes
       return -EDQUOT;
   }
   
   if( bytes_quota_group <= num_bytes_group + add_to_usage ) {
       
       // group has too many bytes 
       printf("Group %d has byte quota of %d; using %d (%d)\n", owner_gid, (int)bytes_quota_user, (int)(num_bytes_user + add_to_usage), (int)add_to_usage );
       return -EDQUOT;
   }
   
   if( new_contents_len == 0 ) {
      new_contents_len = 1;
   }
   
   // expand contents?
   while( offset + buflen > new_contents_len ) {
      new_contents_len *= 2;
   }
   
   if( new_contents_len > inode->contents_len ) {
      
      // expand
      char* tmp = (char*)realloc( inode->contents, new_contents_len );
      
      if( tmp == NULL ) {
         return -ENOMEM;
      }
      
      inode->contents = tmp;
      
      memset( inode->contents + inode->contents_len, 0, new_contents_len - inode->contents_len );
      
      inode->contents_len = new_contents_len;
   }
   
   // write in 
   memcpy( inode->contents + offset, buf, buflen );
   
   // expand size?
   if( (unsigned)(offset + buflen) > inode->size ) {
      inode->size = offset + buflen;
   }
   
   // update usages 
   if( !unknown_user ) {
      
      eventfs_usage_change_num_bytes( eventfs->user_usages, owner_uid, add_to_usage );
   }
   
   if( !unknown_group ) {
      
      eventfs_usage_change_num_bytes( eventfs->group_usages, owner_gid, add_to_usage );
   }
   
   return buflen;
}


// truncate a file 
// return 0 on success, and reset the size and RAM buffer 
// return -ENOMEM on OOM 
// return -ENOSYS if for some reason we don't have an inode (should *never* happen)
// use under the FSKIT_INODE_SEQUENTIAL consistency discipline--the entry will be write-locked when we call this method.
int eventfs_truncate( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, off_t new_size, void* inode_data ) {
   
   eventfs_debug("eventfs_truncate(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
   size_t new_contents_len = inode->contents_len;
   
   if( inode == NULL ) {
      return -ENOSYS;
   }
   
   off_t cur_size = inode->size;
   int64_t add_to_usage = new_size - cur_size;
   
   pid_t calling_tid = fskit_fuse_get_pid();
   uid_t owner_uid = fskit_entry_get_owner( fent );
   gid_t owner_gid = fskit_entry_get_group( fent );
   
   uint64_t bytes_quota_user = eventfs->config.default_bytes_quota;
   uint64_t bytes_quota_group = eventfs->config.default_bytes_quota;
   
   uint64_t num_bytes_user = 0;
   uint64_t num_bytes_group = 0;
   bool unknown_user = false;
   bool unknown_group = false;
   
   // look up quotas
   eventfs_quota_rlock( eventfs );
   
   if( eventfs_quota_lookup( eventfs->user_quotas, owner_uid ) != NULL ) {
      
       bytes_quota_user = eventfs_quota_get_max_bytes( eventfs->user_quotas, owner_uid );
   }
   if( eventfs_usage_lookup( eventfs->user_usages, owner_uid ) != NULL ) {
       
       num_bytes_user = eventfs_usage_get_num_bytes( eventfs->user_usages, owner_uid );
   }
   else {
       
       unknown_user = true;
   }
   
   if( eventfs_quota_lookup( eventfs->group_quotas, owner_gid ) != NULL ) {
       
      bytes_quota_group = eventfs_quota_get_max_bytes( eventfs->group_quotas, owner_gid );
   }
   if( eventfs_usage_lookup( eventfs->group_usages, owner_gid ) != NULL ) {
       
      num_bytes_group = eventfs_usage_get_num_bytes( eventfs->group_usages, owner_gid );
   }
   else {
       
      unknown_group = true;
   }
   
   eventfs_quota_unlock( eventfs );
   
   if( unknown_user && unknown_group ) {
       
       // something weird is going on 
       return -EDQUOT;
   }
       
   // check quotas
   if( bytes_quota_user <= num_bytes_user + add_to_usage ) {
    
       // user has too many bytes
       return -EDQUOT;
   }
   
   if( bytes_quota_group <= num_bytes_group + add_to_usage ) {
       
       // group has too many bytes 
       return -EDQUOT;
   }
   
   // expand?
   if( (unsigned)new_size >= inode->contents_len ) {
      
      if( new_contents_len == 0 ) {
         new_contents_len = 1;
      }
      
      while( (unsigned)new_size > new_contents_len ) {
         
         new_contents_len *= 2;
      }
      
      char* tmp = (char*)realloc( inode->contents, new_contents_len );
      
      if( tmp == NULL ) {
         return -ENOMEM;
      }
      
      inode->contents = tmp;
      
      memset( inode->contents + inode->contents_len, 0, new_contents_len - inode->contents_len );
      
      inode->contents_len = new_contents_len;
   }
   else {
      
      memset( inode->contents + new_size, 0, inode->contents_len - new_size );
   }
   
   // new size 
   inode->size = new_size;
   
   // update usages 
   if( !unknown_user ) {
      
      eventfs_usage_change_num_bytes( eventfs->user_usages, owner_uid, add_to_usage );
   }
   
   if( !unknown_group ) {
      
      eventfs_usage_change_num_files( eventfs->group_usages, owner_gid, add_to_usage );
   }
   
   return 0;
}

// remove a file: either we're destroying it, or unlinking it.
// atomically removes the inode from the directory, *and* updates the head and/or tail pointers to point to the new deque's head and tail
// return 0 on success, and free up the given inode_data
// return -ENOENT if the parent dir no longer exists
// NOTE: fent cannot be locked.
int eventfs_remove_file( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* inode_data, bool destroy ) {
   
   eventfs_debug("eventfs_remove_file('%s', destroy=%d) from %d\n", fskit_route_metadata_get_path( route_metadata ), destroy, fskit_fuse_get_pid() );
   
   int rc = 0;
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)inode_data;
   struct fskit_entry* parent = fskit_route_metadata_get_parent( route_metadata );
   struct eventfs_dir_inode* dir_inode = NULL;
   struct eventfs_file_inode* old_file = NULL;
   char const* path = fskit_route_metadata_get_path( route_metadata );
   char* dir_path = NULL;
   char name[FSKIT_FILESYSTEM_NAMEMAX+1];
   
   uid_t owner_uid = fskit_entry_get_owner( fent );
   gid_t owner_gid = fskit_entry_get_group( fent );
   off_t cur_size = 0;
   int type = fskit_entry_get_type( fent );
   
   if( inode != NULL ) {
       
       cur_size = inode->size;
   }
   
   memset( name, 0, FSKIT_FILESYSTEM_NAMEMAX+1 );
   fskit_basename( path, name );
   
   if( parent != NULL ) {
        
        // parent is not NULL--we're detaching 
        // NOTE: parent is guaranteed to be write-locked 
        dir_inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );
        if( dir_inode == NULL ) {
            
            eventfs_error("BUG: no inode data for %p\n", parent );
            return -EIO;
        }
        
        if( !dir_inode->deleted ) {
        
            // not reaped by us
            // get parent path
            dir_path = fskit_dirname( fskit_route_metadata_get_path( route_metadata ), NULL );
            if( dir_path == NULL ) {
                return -ENOMEM;
            }
            
            if( !destroy ) {
                
                // only detaching...
                if( fent == dir_inode->fent_head ) {
                    
                    // detaching head symlink.  Recreate and retarget, or detach if empty.
                    // also, detach the associated file the head points to.
                    rc = eventfs_dir_inode_pophead( core, dir_path, dir_inode, parent );
                }
                else if( fent == dir_inode->fent_tail ) {
                    
                    // detach tail symlink.  Recreate and retarget, or detach if empty.
                    // also, deatch the associated file the tail points to.
                    rc = eventfs_dir_inode_poptail( core, dir_path, dir_inode, parent );
                }
                else {
                    
                    // detach a file in the middle 
                    rc = eventfs_dir_inode_remove( core, dir_path, dir_inode, parent, name );
                }
            }
            
            else {
                
                // freeing a fully-detached inode.
                // we can ignore the head and tail symlinks.
                // free a file inode's internal data, though. 
                if( !eventfs_dir_inode_is_empty( dir_inode ) && fent != dir_inode->fent_head && fent != dir_inode->fent_tail ) {
                    
                    fskit_entry_rlock( fent );
                    old_file = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
                    fskit_entry_unlock( fent );
                    
                    // deleting a file in the middle 
                    eventfs_dir_inode_remove( core, dir_path, dir_inode, parent, name );
                    
                    if( old_file != NULL ) {
                        eventfs_file_inode_free( old_file );
                        eventfs_safe_free( old_file );
                    }
                }
            }
            
            eventfs_safe_free( dir_path );
        }
        else {
            
            eventfs_debug("Parent of '%s' already reaped\n", fskit_route_metadata_get_path( route_metadata ));
        }
   }
   else if( destroy ) {
       
       // this entry was already detached; we're just getting around to freeing it.
       eventfs_debug("reclaim %s\n", path );
       
       if( inode != NULL ) {
           eventfs_file_inode_free( inode );
           eventfs_safe_free( inode );
       }
   }
   
   // debit usages
   if( type == FSKIT_ENTRY_TYPE_FILE ) {
       eventfs_quota_rlock( eventfs );
        
       if( eventfs_usage_lookup( eventfs->user_usages, owner_uid ) != NULL ) {
            
           // reduce user usage 
           if( destroy ) {
                eventfs_usage_change_num_bytes( eventfs->user_usages, owner_uid, -cur_size );
           }
           eventfs_usage_change_num_files( eventfs->user_usages, owner_uid, -1 );
       }
        
       if( eventfs_usage_lookup( eventfs->group_usages, owner_gid ) != NULL ) {
            
           if( destroy ) {
                eventfs_usage_change_num_bytes( eventfs->group_usages, owner_gid, -cur_size );
           }
           eventfs_usage_change_num_files( eventfs->group_usages, owner_gid, -1 );
       }
       
       eventfs_quota_unlock( eventfs );
   }
   
   return rc;
}


// remove a directory 
// return 0 on success, and free up the given inode data 
// return -ENOENT if the directory no longer exists.
int eventfs_destroy_dir( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* dent, void* inode_data ) {
   
   eventfs_debug("eventfs_destroy_dir('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   struct eventfs_dir_inode* inode = (struct eventfs_dir_inode*)inode_data;
   
   uid_t owner_uid = fskit_entry_get_owner( dent );
   gid_t owner_gid = fskit_entry_get_group( dent );
   
   // blow away the inode
   if( inode != NULL ) {
      
      eventfs_dir_inode_free( core, inode );
      eventfs_safe_free( inode );
      
      // be *sure* it's gone
      fskit_entry_set_user_data( dent, NULL );
   }
   
   // debit usages
   eventfs_quota_rlock( eventfs );
   
   if( eventfs_usage_lookup( eventfs->user_usages, owner_uid ) != NULL ) {
      
       // reduce user usage 
       eventfs_usage_change_num_dirs( eventfs->user_usages, owner_uid, -1 );
   }
   
   if( eventfs_usage_lookup( eventfs->group_usages, owner_gid ) != NULL ) {
       
       eventfs_usage_change_num_dirs( eventfs->group_usages, owner_gid, -1 );
   }
   
   eventfs_quota_unlock( eventfs );
   
   return 0;
}


// destroy a file or directory 
// return 0 on success, and free up the inode data 
// return -ENONET if the directory no longer eists 
int eventfs_destroy( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* inode_data ) {
    
    eventfs_debug("eventfs_destroy('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
    
    fskit_entry_rlock( fent );
    
    int type = fskit_entry_get_type( fent );
    
    fskit_entry_unlock( fent );
    
    if( type == FSKIT_ENTRY_TYPE_DIR ) {
        return eventfs_destroy_dir( core, route_metadata, fent, inode_data );
    }
    else if( type == FSKIT_ENTRY_TYPE_FILE || type == FSKIT_ENTRY_TYPE_LNK ) {
        return eventfs_remove_file( core, route_metadata, fent, inode_data, true );
    }
    else {
        // we have no state associated with non-regular-file inodes
        return 0;
    }
}

// detach a file
// return 0 on success, and free up the inode data 
// return -ENONET if the directory no longer eists 
int eventfs_detach( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* inode_data ) {
    
    eventfs_debug("eventfs_detach('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
    
    fskit_entry_rlock( fent );
    
    int type = fskit_entry_get_type( fent );
    
    fskit_entry_unlock( fent );
    
    if( type == FSKIT_ENTRY_TYPE_DIR ) {
        return 0;
    }
    else if( type == FSKIT_ENTRY_TYPE_FILE || type == FSKIT_ENTRY_TYPE_LNK ) {
        return eventfs_remove_file( core, route_metadata, fent, inode_data, false );
    }
    else {
        // we have no state associated with non-regular-file inodes
        return 0;
    }
}

// stat an entry.
// for non-root diretories, garbage-collect both it and and its children if the process that created it died.
// return 0 on success 
// return -ENOENT if the path does not exist
// return -EIO if the inode is invalid 
int eventfs_stat( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, struct stat* sb ) {
   
   eventfs_debug("eventfs_stat('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   int rc = 0;
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   struct eventfs_dir_inode* inode = NULL;
   char* name = fskit_route_metadata_get_name( route_metadata );

   if( fent == NULL ) {
      return -ENOENT;
   }
   
   fskit_entry_wlock( fent );
   
   // do the stat...
   fskit_entry_fstat( fent, sb );
   
   if( !S_ISDIR( sb->st_mode ) ) {
       
       // not a directory
       fskit_entry_unlock( fent );
       return 0;
   }
   else if( strcmp( name, "/" ) == 0 ) {
       
       // skip root
       fskit_entry_unlock( fent );
       return 0;
   }
   else {
       
       // non-root directory. verify that its creating process still exists 
       inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( fent );
       if( inode == NULL ) {
           
           // already detached
           fskit_entry_unlock( fent );
           return -ENOENT;
       }
       
       if( inode->deleted ) {
           
           fskit_entry_unlock( fent );
           return -ENOENT;
       }
       
       // skip if sticky 
       rc = fskit_fgetxattr( core, fent, "user.eventfs_sticky", NULL, 0 );
       if( rc >= 0 ) {
           
           // sticky set 
           eventfs_debug("directory '%s' will NOT share fate with its creator process\n", fskit_route_metadata_get_path( route_metadata ) );
           fskit_entry_unlock( fent );
           return 0;
       }
       else {
           
           eventfs_debug("directory '%s' will share fate with its creator process\n", fskit_route_metadata_get_path( route_metadata ) );
           rc = 0;
       }
       
       pid_t pid = pstat_get_pid( inode->ps );
   
       rc = eventfs_dir_inode_is_valid( inode );
       if( rc < 0 ) {
            
            char path[PATH_MAX+1];
            pstat_get_path( inode->ps, path );
            
            eventfs_error( "eventfs_dir_inode_is_valid(path=%s, pid=%d) rc = %d\n", path, pid, rc );
            
            // no longer valid
            rc = 0;
       }
   
       if( rc == 0 ) {
            
            // blow away this inode and its children
            inode->deleted = true;
            fskit_entry_set_user_data( fent, NULL );
            
            eventfs_dir_inode_free( core, inode );
            eventfs_safe_free( inode );
            
            uint64_t inode_number = fskit_entry_get_file_id( fent );
            rc = eventfs_deferred_remove( eventfs, fskit_route_metadata_get_path( route_metadata ), fent );
            
            if( rc != 0 ) {
                eventfs_error("eventfs_deferred_remove('%s' (%" PRIX64 ") rc = %d\n", fskit_route_metadata_get_path( route_metadata ), inode_number, rc );
            }
            else {
                eventfs_debug("Detached '%s' because it is orphaned (PID %d)\n", fskit_route_metadata_get_path( route_metadata ), pid );
                rc = -ENOENT;
            }
            
            fskit_entry_unlock( fent );
       }
       else {
            
            fskit_entry_unlock( fent );
            eventfs_debug("'%s' (created by %d) is still valid\n", fskit_route_metadata_get_path( route_metadata ), pid );
            rc = 0;
       }
   }
   
   return rc;
}

/*
// rename a file or directory
// if this is on a file, and the file is pointed to by the head or tail symlinks, then retarget the symlinks.
// return 0 on success
// return -ENOENT if the dir got blown away already 
// return -ENOMEM on OOM 
// return -EPERM if we tried to rename the head or tail symlinks
int eventfs_rename( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* old_fent, char const* new_path, struct fskit_entry* new_fent ) {
    
    eventfs_debug("eventfs_rename('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
    
    int rc = 0;
    struct eventfs_dir_inode* dir = NULL;
    struct fskit_entry* parent = fskit_route_metadata_get_new_parent( route_metadata );
    char* name = fskit_route_metadata_get_name( route_metadata );
    char new_name[FSKIT_FILESYSTEM_NAMEMAX+1];
    
    memset( new_name, 0, FSKIT_FILESYSTEM_NAMEMAX+1 );
    fskit_basename( new_path, new_name );
    
    if( fskit_entry_get_type( old_fent ) == FSKIT_ENTRY_TYPE_DIR ) {
        // nothing special to do 
        return 0;
    }
    
    dir = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );    
    if( dir == NULL ) {
        
        // already detached
        return -ENOENT;
    }
    
    if( dir->deleted ) {
        
        return -ENOENT;
    }

    if( dir->fent_head == old_fent || dir->fent_tail == old_fent ) {
        
        // deny rename on head and tail 
        return -EPERM;
    }
    
    rc = eventfs_dir_inode_rename_child( core, dir, old_fent, name, new_name );
    return rc;
}
*/

// link a file into a directory.
// preserve symlinks: append the new file to the directory's deque's tail
// return 0 on success 
// return -ENOENT if the parent directory got blown away already 
// return -ENOMEM on OOM 
int eventfs_link( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, char const* new_path ) {
    
    eventfs_debug("eventfs_link('%s', '%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), new_path, fskit_fuse_get_pid() );
    
    int rc = 0;
    struct eventfs_dir_inode* dir = NULL;
    struct eventfs_file_inode* file = NULL;
    char new_name[FSKIT_FILESYSTEM_NAMEMAX+1];
    struct fskit_entry* parent = fskit_route_metadata_get_new_parent( route_metadata );
    
    file = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
    dir = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );    
    if( dir == NULL ) {
        
        // already detached
        return -ENOENT;
    }
    
    if( dir->deleted ) {
        
        return -ENOENT;
    }
    
    memset( new_name, 0, FSKIT_FILESYSTEM_NAMEMAX+1 );
    fskit_basename( new_path, new_name );

    return eventfs_dir_inode_append( core, dir, parent, new_name );
}


// read a directory
// if we're scanning the root directory, stat each directory to verify that the creator process still exists
// we need concurrent per-inode locking (i.e. read-lock the directory)
int eventfs_readdir( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, struct fskit_dir_entry** dirents, size_t num_dirents ) {
   
   eventfs_debug("eventfs_readdir(%s, %zu) from %d\n", fskit_route_metadata_get_path( route_metadata ), num_dirents, fskit_fuse_get_pid() );
   
   int rc = 0;
   struct fskit_entry* child = NULL;
   struct eventfs_dir_inode* inode = NULL;
   char* name = fskit_route_metadata_get_name( route_metadata );
   
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   
   uid_t calling_uid = fskit_fuse_get_uid( eventfs->fuse_state );
   gid_t calling_gid = fskit_fuse_get_gid( eventfs->fuse_state );
   
   // skip non-directories 
   if( fskit_entry_get_type( fent ) != FSKIT_ENTRY_TYPE_DIR ) {
       return 0;
   }
   
   // skip non-root
   if( strcmp(name, "/") != 0 ) {
       return 0;
   }
   
   // will scan root
   int* omitted = EVENTFS_CALLOC( int, num_dirents );
   if( omitted == NULL ) {
       return -ENOMEM;
   }
   
   int omitted_idx = 0;
   
   // find dead directories and (1) omit them and (2) reap them
   for( unsigned int i = 0; i < num_dirents; i++ ) {
      
      // skip . and ..
      if( strcmp(dirents[i]->name, ".") == 0 || strcmp(dirents[i]->name, "..") == 0 ) {
         continue;
      }
      
      // find the associated fskit_entry
      child = fskit_dir_find_by_name( fent, dirents[i]->name );
      
      if( child == NULL ) {
         // strange, shouldn't happen...
         continue;
      }
      
      fskit_entry_rlock( child );
      
      // include all non-directories
      if( fskit_entry_get_type( child ) != FSKIT_ENTRY_TYPE_DIR ) {
          fskit_entry_unlock( child );
          continue;
      }
      
      // skip directories tagged with "user.eventfs_sticky"
      rc = fskit_fgetxattr( core, child, "user.eventfs_sticky", NULL, 0 );
      if( rc >= 0 ) {
          
          rc = 0;
          fskit_entry_unlock( child );
          continue;
      }
      else {
          
          rc = 0;
      }
      
      // get directory metadata
      inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( child );
      
      if( inode == NULL ) {
         // skip
         fskit_entry_unlock( child );
         continue;
      }
      
      // already marked for deletion?
      if( inode->deleted ) {
         // skip
         fskit_entry_unlock( child );
         
         omitted[ omitted_idx ] = i;
         omitted_idx++;
         continue;
      }
      
      // is this file still valid?
      int valid = eventfs_dir_inode_is_valid( inode );
      
      if( valid < 0 ) {
         
         char path[PATH_MAX+1];
         pstat_get_path( inode->ps, path );
         
         eventfs_error( "eventfs_dir_inode_is_valid(path=%s, pid=%d) rc = %d\n", path, pstat_get_pid( inode->ps ), valid );
         
         valid = 0;
      }
      
      fskit_entry_unlock( child );
      
      if( valid == 0 ) {
         
         // not valid--creator has died.
         // upgrade the lock to a write-lock, so we can garbage-collect 
         fskit_entry_wlock( child );
      
         inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( child );
      
         if( inode == NULL ) {
             
             // no longer valid
             omitted[ omitted_idx ] = i;
             omitted_idx++;
             fskit_entry_unlock( child );
             continue;
         }
         
         if( inode->deleted ) {
             // someone raced us 
             fskit_entry_unlock( child );
             
             omitted[ omitted_idx ] = i;
             omitted_idx++;
             continue;
         }
      
         // flag deleted
         inode->deleted = true;
         
         uint64_t child_id = fskit_entry_get_file_id( child );
         char* child_fp = fskit_fullpath( fskit_route_metadata_get_path( route_metadata ), dirents[i]->name, NULL );
         if( child_fp == NULL ) {
             
            fskit_entry_unlock( child );
            rc = -ENOMEM;
            break;
         }
         
         // garbage-collect
         rc = eventfs_deferred_remove( eventfs, child_fp, child );
         fskit_entry_unlock( child );
         
         if( rc != 0 ) {
            
            eventfs_error("eventfs_deferred_remove('%s' (%" PRIX64 ")) rc = %d\n", child_fp, child_id, rc );
         }
         
         free( child_fp );
         
         // omit this child from the listing
         omitted[ omitted_idx ] = i;
         omitted_idx++;
      }
   }
   
   for( int i = 0; i < omitted_idx; i++ ) {
      
      fskit_readdir_omit( dirents, omitted[i] );
   }
   
   eventfs_safe_free( omitted );
   
   return rc;
}


// parse opts, and remove eventfs-specific ones from argv.
int eventfs_getopts( struct eventfs_opts* opts, int* argc, char** argv ) {
    
   int rc = 0;
   int c = 0;
   int opt_index = 0;
   int new_argc = *argc;
   
   memset( opts, 0, sizeof(struct eventfs_opts));
   
   static struct option match[] = {
       {"config-file",          required_argument,      0,      'c'},
       {0,                      0,                      0,      0}
   };
   
   while( rc == 0 ) {
       
       c = getopt_long( *argc, argv, "c:fos", match, &opt_index );
       if( c == -1 ) {
           break;
       }
       
       switch( c ) {
           
           case 'c': {
               
               opts->config_path = strdup( optarg );
               if( opts->config_path == NULL ) {
                   
                   rc = -ENOMEM;
               }
               break;
           }
           
           case 's':
           case 'o':
           case 'f': {
               
               // FUSE opts; ignore 
               break;
           }
           
           default: {
               
               fprintf(stderr, "Unrecognized option '-%c'\n", c );
               rc = -EINVAL;
               break;
           }
       }
   }
   
   for( int i = 0; i < *argc && argv[i] != NULL; i++ ) {
       
       for( int j = 0; match[j].name != NULL; j++ ) {
           
           char short_opt[3];
           short_opt[0] = '-';
           short_opt[1] = match[j].val;
           short_opt[2] = 0;
           
           if( strcmp(argv[i], match[j].name) == 0 || strcmp(argv[i], short_opt) == 0 ) {
               
               if( match[j].has_arg == required_argument ) {
                   
                   // remove switch and arg 
                   int k = i;
                   for( ; k + 2 < *argc && argv[k+2] != NULL; k++ ) {
                       
                      argv[k] = argv[k+2];
                   }
                   
                   argv[ k+2 ] = NULL;
                   new_argc -= 2;
               }
               else {
                   
                   // remove only the siwtch 
                   int k = i;
                   for( ; k + 1 < *argc && argv[k+1] != NULL; k++ ) {
                       
                       argv[k] = argv[k+1];
                   }
                   
                   argv[k+1] = NULL;
                   new_argc -= 1;
               }
           }
       }
   }
   
   *argc = new_argc;
   
   return rc;
}


// run! 
int main( int argc, char** argv ) {
   
   int rc = 0;
   int rh = 0;
   struct fskit_fuse_state* state = NULL;
   struct fskit_core* core = NULL;
   struct eventfs_state eventfs;
   struct eventfs_opts opts;
   
   state = fskit_fuse_state_new();
   if( state == NULL ) {
       // OOM 
       exit(1);
   }
   
   // get opts 
   rc = eventfs_getopts( &opts, &argc, argv );
   if( rc != 0 ) {
       exit(1);
   }
   
   // default opts 
   if( opts.config_path == NULL ) {
       
       opts.config_path = strdup( EVENTFS_DEFAULT_CONFIG_PATH );
       if( opts.config_path == NULL ) {
           exit(1);
       }
   }
   
   // setup eventfs state 
   memset( &eventfs, 0, sizeof(struct eventfs_state) );
   
   eventfs.deferred_wq = eventfs_wq_new();
   if( eventfs.deferred_wq == NULL ) {
      exit(1);
   }
   
   rc = eventfs_wq_init( eventfs.deferred_wq );
   if( rc != 0 ) {
      fprintf(stderr, "eventfs_wq_init rc = %d\n", rc );
      exit(1);
   }
   
   struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
   rc = fuse_parse_cmdline( &args, &eventfs.mountpoint, NULL, NULL );
   if( eventfs.mountpoint == NULL ) {
      fprintf(stderr, "fuse_parse_cmdline rc = %d\n", rc );
      exit(1);
   }
   
   // automatically reap child processes 
   signal(SIGCHLD, SIG_IGN);
   
   // set up fskit state
   rc = fskit_fuse_init( state, &eventfs );
   if( rc != 0 ) {
      fprintf(stderr, "fskit_fuse_init rc = %d\n", rc );
      exit(1);
   }
   
   eventfs.fuse_state = state;
   
   // set up quotas
   rc = eventfs_config_load( opts.config_path, &eventfs.config, &eventfs.user_quotas, &eventfs.group_quotas );
   if( rc != 0 ) {
      fprintf(stderr, "eventfs_config_load: %s\n", strerror(-rc));
      exit(1);
   }
   
   rc = pthread_rwlock_init( &eventfs.quota_lock, NULL );
   if( rc != 0 ) {
      fprintf(stderr, "pthread_rwlock_init rc = %d\n", rc );
      exit(1);
   }
   
   // make sure the fs can access its methods through the VFS
   fskit_fuse_setting_enable( state, FSKIT_FUSE_SET_FS_ACCESS );
   
   core = fskit_fuse_get_core( state );
   
   // plug core into eventfs
   eventfs.core = core;
   
   // add handlers.  reads and writes must happen sequentially, since we seek and then perform I/O
   // NOTE: FSKIT_ROUTE_ANY matches any path, and is a macro for the regex "/([^/]+[/]*)+"
   rh = fskit_route_create( core, FSKIT_ROUTE_ANY, eventfs_create, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_create(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   rh = fskit_route_mkdir( core, FSKIT_ROUTE_ANY, eventfs_mkdir, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_mkdir(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   rh = fskit_route_read( core, FSKIT_ROUTE_ANY, eventfs_read, FSKIT_INODE_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_read(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   rh = fskit_route_write( core, FSKIT_ROUTE_ANY, eventfs_write, FSKIT_INODE_SEQUENTIAL );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_write(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   rh = fskit_route_trunc( core, FSKIT_ROUTE_ANY, eventfs_truncate, FSKIT_INODE_SEQUENTIAL );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_trunc(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
      
   rh = fskit_route_destroy( core, FSKIT_ROUTE_ANY, eventfs_destroy, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_destroy(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
     
   rh = fskit_route_detach( core, FSKIT_ROUTE_ANY, eventfs_detach, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_detach(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   rh = fskit_route_stat( core, FSKIT_ROUTE_ANY, eventfs_stat, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_stat(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   /*
   rh = fskit_route_rename( core, FSKIT_ROUTE_ANY, eventfs_rename, FSKIT_CONCURRENT );
   if( rh < 0 ) {
       fprintf(stderr, "fskit_route_rename(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
       exit(1);
   }
   */
   
   rh = fskit_route_link( core, FSKIT_ROUTE_ANY, eventfs_link, FSKIT_CONCURRENT );
   if( rh < 0 ) {
       fprintf(stderr, "fskit_route_link(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
       exit(1);
   }
   
   rh = fskit_route_readdir( core, FSKIT_ROUTE_ANY, eventfs_readdir, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      fprintf(stderr, "fskit_route_readdir(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      exit(1);
   }
   
   // set the root to be owned by the effective UID and GID of user
   fskit_chown( core, "/", 0, 0, geteuid(), getegid() );
   
   // begin taking deferred requests 
   rc = eventfs_wq_start( eventfs.deferred_wq );
   if( rc != 0 ) {
      fprintf(stderr, "eventfs_wq_start rc = %d\n", rc );
      exit(1);
   }
   
   // run 
   rc = fskit_fuse_main( state, argc, argv );
   
   // shutdown
   fskit_fuse_shutdown( state, NULL );
   fskit_fuse_state_free( state );
   
   eventfs_wq_stop( eventfs.deferred_wq );
   eventfs_wq_free( eventfs.deferred_wq );
   eventfs_safe_free( eventfs.deferred_wq );
   
   pthread_rwlock_destroy( &eventfs.quota_lock );
   eventfs_quota_free( eventfs.user_quotas );
   eventfs_quota_free( eventfs.group_quotas );
   eventfs_usage_free( eventfs.user_usages );
   eventfs_usage_free( eventfs.group_usages );
   
   eventfs_config_free( &eventfs.config );
   
   eventfs_safe_free( opts.config_path );
   
   return rc;
}

