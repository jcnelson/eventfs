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


// create a eventfs file 
// return 0 on success
// return -ENOMEM on OOM 
// return negative on failure to initialize the inode
int eventfs_create( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, mode_t mode, void** inode_data, void** handle_data ) {
   
   eventfs_debug("eventfs_create(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   int rc = 0;
   pid_t calling_tid = fskit_fuse_get_pid();
   
   // NOTE: parent will be write-locked
   struct fskit_entry* parent = fskit_route_metadata_get_parent( route_metadata );
   
   struct eventfs_dir_inode* parent_inode = NULL;
   struct eventfs_file_inode* inode = EVENTFS_CALLOC( struct eventfs_file_inode, 1 );
   char name[FSKIT_FILESYSTEM_NAMEMAX+1];
   
   memset( name, 0, FSKIT_FILESYSTEM_NAMEMAX + 1 );
   
   fskit_entry_rlock( fent );
   fskit_entry_copy_name( fent, name, FSKIT_FILESYSTEM_NAMEMAX );
   fskit_entry_unlock( fent );
   
   if( inode == NULL ) {
      return -ENOMEM;
   }
   
   rc = eventfs_file_inode_init( inode, name );
   if( rc != 0 ) {
       
      // phantom process?
      eventfs_safe_free( inode );
      return rc;
   }
   
   // attach to parent (will already be write-locked)
   parent_inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );
   if( parent_inode == NULL ) {
       
       eventfs_error("BUG: parent %p has no inode data!\n", parent );
       eventfs_file_inode_free( inode );
       eventfs_safe_free( inode );
       
       return -EIO;
   }
   
   rc = eventfs_dir_inode_append( core, parent_inode, parent, inode );
   if( rc != 0 ) {
       
       // failed 
       eventfs_file_inode_free( inode );
       eventfs_safe_free( inode );
       
       return rc;
   }
   
   *inode_data = (void*)inode;
   
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
   pid_t calling_tid = fskit_fuse_get_pid();
   struct eventfs_dir_inode* inode = NULL;
   
   struct fskit_entry* head = NULL;
   struct fskit_entry* tail = NULL;
   
   char* path = fskit_route_metadata_get_path( route_metadata );
   
   if( fskit_depth( path ) > 1 ) {
       
       // not allowed 
       return -EPERM;
   }
   
   inode = EVENTFS_CALLOC( struct eventfs_dir_inode, 1 );
   if( inode == NULL ) {
       
       // OOM 
       return -ENOMEM;
   }
   
   // set up directory state
   rc = eventfs_dir_inode_init( inode, calling_tid, EVENTFS_VERIFY_DEFAULT );
   if( rc != 0 ) {
       
       // phantom process?
       return rc;
   }
   
   *inode_data = (void*)inode;
   
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
int eventfs_write( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, char* buf, size_t buflen, off_t offset, void* handle_data ) {
   
   eventfs_debug("eventfs_write(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
   size_t new_contents_len = inode->contents_len;
   
   if( inode == NULL ) {
      return -ENOSYS;
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
   
   return buflen;
}


// truncate a file 
// return 0 on success, and reset the size and RAM buffer 
// return -ENOMEM on OOM 
// return -ENOSYS if for some reason we don't have an inode (should *never* happen)
// use under the FSKIT_INODE_SEQUENTIAL consistency discipline--the entry will be write-locked when we call this method.
int eventfs_truncate( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, off_t new_size, void* inode_data ) {
   
   eventfs_debug("eventfs_truncate(%s) from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
   size_t new_contents_len = inode->contents_len;
   
   if( inode == NULL ) {
      return -ENOSYS;
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
   
   return 0;
}

// remove a file
// atomically removes the inode from the directory, *and* updates the head and/or tail pointers to point to the new deque's head and tail
// return 0 on success, and free up the given inode_data
// return -ENOENT if the parent dir no longer exists
// NOTE: fent cannot be locked.
int eventfs_detach_file( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* inode_data ) {
   
   eventfs_debug("eventfs_detach_file('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   int rc = 0;
   struct eventfs_file_inode* inode = (struct eventfs_file_inode*)inode_data;
   struct fskit_entry* parent = fskit_route_metadata_get_parent( route_metadata );
   struct eventfs_dir_inode* dir_inode = NULL;
   struct eventfs_file_inode* old_file = NULL;
   char const* path = fskit_route_metadata_get_path( route_metadata );
   char* dir_path = NULL;
   
   if( parent == NULL ) {
       
       // this entry was already detached; we're just getting around to freeing it.
       eventfs_debug("reclaim %s\n", path );
       
       if( inode != NULL ) {
           eventfs_file_inode_free( inode );
           eventfs_safe_free( inode );
       }
       return 0;
   }
   
   // parent is not NULL--we're detaching 
   // NOTE: parent is guaranteed to be write-locked 
   dir_inode = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );
   if( dir_inode == NULL ) {
       
       eventfs_error("BUG: no inode data for %p\n", parent );
       return -EIO;
   }
   
   if( dir_inode->deleted ) {
       
       // nothing to do 
       return 0;
   }
   
   // get parent path
   dir_path = fskit_dirname( fskit_route_metadata_get_path( route_metadata ), NULL );
   if( dir_path == NULL ) {
       return -ENOMEM;
   }
   
   if( fent == dir_inode->fent_head ) {
       
       // deleting head symlink.  Recreate and retarget, or destroy if empty.
       // also, destroy the associated file the head points to.
       rc = eventfs_dir_inode_pophead( core, dir_path, dir_inode, parent, &old_file );
   }
   else if( fent == dir_inode->fent_tail ) {
       
       // deleting tail symlink.  Recreate and retarget, or destroy if empty.
       // also, destory the associated file the tail points to.
       rc = eventfs_dir_inode_poptail( core, dir_path, dir_inode, parent, &old_file );
   }
   else {
       
       // extract file inode data
       fskit_entry_wlock( fent );
       
       old_file = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
       fskit_entry_set_user_data( fent, NULL );
       
       fskit_entry_unlock( fent );
       
       // deleting a file in the middle 
       rc = eventfs_dir_inode_remove( core, dir_path, dir_inode, parent, old_file );
   }
   
   if( old_file != NULL ) {
       
       eventfs_file_inode_free( old_file );
       eventfs_safe_free( old_file );
   }
   
   eventfs_safe_free( dir_path );
   
   return rc;
}


// remove a directory 
// return 0 on success, and free up the given inode data 
// return -ENOENT if the directory no longer exists.
int eventfs_detach_dir( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* dent, void* inode_data ) {
   
   eventfs_debug("eventfs_detach_dir('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
   
   struct eventfs_dir_inode* inode = (struct eventfs_dir_inode*)inode_data;
   
   // blow away the inode
   if( inode != NULL ) {
      
      eventfs_dir_inode_free( core, inode );
      eventfs_safe_free( inode );
      
      // be *sure* it's gone
      fskit_entry_set_user_data( dent, NULL );
   }
   
   return 0;
}


// detach a file or directory 
// return 0 on success, and free up the inode data 
// return -ENONET if the directory no longer eists 
int eventfs_detach( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* inode_data ) {
    
    eventfs_debug("eventfs_detach('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
    
    fskit_entry_rlock( fent );
    
    int type = fskit_entry_get_type( fent );
    
    fskit_entry_unlock( fent );
    
    if( type == FSKIT_ENTRY_TYPE_DIR ) {
        return eventfs_detach_dir( core, route_metadata, fent, inode_data );
    }
    else if( type == FSKIT_ENTRY_TYPE_FILE || type == FSKIT_ENTRY_TYPE_LNK ) {
        return eventfs_detach_file( core, route_metadata, fent, inode_data );
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
   char name[FSKIT_FILESYSTEM_NAMEMAX+1];
   
   memset( name, 0, FSKIT_FILESYSTEM_NAMEMAX+1 );
   
   fskit_entry_wlock( fent );
   
   fskit_entry_copy_name( fent, name, FSKIT_FILESYSTEM_NAMEMAX );
   
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
    
    if( fskit_entry_get_type( old_fent ) == FSKIT_ENTRY_TYPE_DIR ) {
        // nothing special to do 
        return 0;
    }
    
    dir = (struct eventfs_dir_inode*)fskit_entry_get_user_data( new_fent );    
    if( dir == NULL ) {
        
        // already detached
        fskit_entry_unlock( new_fent );
        return -ENOENT;
    }
    
    if( dir->deleted ) {
        
        fskit_entry_unlock( new_fent );
        return -ENOENT;
    }

    if( dir->fent_head == new_fent || dir->fent_tail == new_fent ) {
        
        // deny rename on head and tail 
        return -EPERM;
    }
    
    else if( dir->fent_head == new_fent ) {
        
        // make the head point to the new entry 
        char* new_name = fskit_entry_get_name( new_fent );
        if( new_name == NULL ) {
            return -ENOMEM;
        }
        
        eventfs_dir_inode_retarget_head( dir, new_name );
    }
    
    else {
        
        // make the tail point to the new entry 
        char* new_name = fskit_entry_get_name( new_fent );
        if( new_name == NULL ) {
            return -ENOMEM;
        }
        
        eventfs_dir_inode_retarget_tail( dir, new_name );
    }
    
    return 0;
}


// link a file into a directory.
// preserve symlinks: append the new file to the directory's deque's tail
// return 0 on success 
// return -ENOENT if the parent directory got blown away already 
// return -ENOMEM on OOM 
int eventfs_link( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, char const* new_path ) {
    
    eventfs_debug("eventfs_link('%s') from %d\n", fskit_route_metadata_get_path( route_metadata ), fskit_fuse_get_pid() );
    
    int rc = 0;
    struct eventfs_dir_inode* dir = NULL;
    struct eventfs_file_inode* file = NULL;
    
    struct fskit_entry* parent = fskit_route_metadata_get_parent( route_metadata );
    
    file = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
    dir = (struct eventfs_dir_inode*)fskit_entry_get_user_data( parent );    
    if( dir == NULL ) {
        
        // already detached
        return -ENOENT;
    }
    
    if( dir->deleted ) {
        
        return -ENOENT;
    }

    return eventfs_dir_inode_append( core, dir, parent, file );
}


// read a directory
// if we're scanning the root directory, stat each directory to verify that the creator process still exists
// we need concurrent per-inode locking (i.e. read-lock the directory)
int eventfs_readdir( struct fskit_core* core, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, struct fskit_dir_entry** dirents, size_t num_dirents ) {
   
   eventfs_debug("eventfs_readdir(%s, %zu) from %d\n", fskit_route_metadata_get_path( route_metadata ), num_dirents, fskit_fuse_get_pid() );
   
   int rc = 0;
   struct fskit_entry* child = NULL;
   struct eventfs_dir_inode* inode = NULL;
   char name[FSKIT_FILESYSTEM_NAMEMAX+1];
   
   struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
   
   memset( name, 0, FSKIT_FILESYSTEM_NAMEMAX+1 );
   fskit_entry_copy_name( fent, name, FSKIT_FILESYSTEM_NAMEMAX );
   
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
      
      // skip non-directories 
      if( fskit_entry_get_type( child ) != FSKIT_ENTRY_TYPE_DIR ) {
          fskit_entry_unlock( child );
          continue;
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


// run! 
int main( int argc, char** argv ) {
   
   int rc = 0;
   int rh = 0;
   struct fskit_fuse_state state;
   struct fskit_core* core = NULL;
   struct eventfs_state eventfs;
   
   // setup eventfs state 
   memset( &eventfs, 0, sizeof(struct eventfs_state) );
   
   eventfs.deferred_unlink_wq = eventfs_wq_new();
   if( eventfs.deferred_unlink_wq == NULL ) {
      exit(1);
   }
   
   rc = eventfs_wq_init( eventfs.deferred_unlink_wq );
   if( rc != 0 ) {
      fprintf(stderr, "eventfs_wq_init rc = %d\n", rc );
      exit(1);
   }
   
   // set up fskit state
   rc = fskit_fuse_init( &state, &eventfs );
   if( rc != 0 ) {
      fprintf(stderr, "fskit_fuse_init rc = %d\n", rc );
      exit(1);
   }
   
   // make sure the fs can access its methods through the VFS
   fskit_fuse_setting_enable( &state, FSKIT_FUSE_SET_FS_ACCESS );
   
   core = fskit_fuse_get_core( &state );
   
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
   
   rh = fskit_route_rename( core, FSKIT_ROUTE_ANY, eventfs_rename, FSKIT_CONCURRENT );
   if( rh < 0 ) {
       fprintf(stderr, "fakit_route_rename(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
       exit(1);
   }
   
   rh = fskit_route_link( core, FSKIT_ROUTE_ANY, eventfs_link, FSKIT_CONCURRENT );
   if( rh < 0 ) {
       fprintf(stderr, "fakit_route_link(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
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
   rc = eventfs_wq_start( eventfs.deferred_unlink_wq );
   if( rc != 0 ) {
      fprintf(stderr, "eventfs_wq_start rc = %d\n", rc );
      exit(1);
   }
   
   // run 
   rc = fskit_fuse_main( &state, argc, argv );
   
   // shutdown
   fskit_fuse_shutdown( &state, NULL );
   
   eventfs_wq_stop( eventfs.deferred_unlink_wq );
   eventfs_wq_free( eventfs.deferred_unlink_wq );
   eventfs_safe_free( eventfs.deferred_unlink_wq );
   
   return rc;
}

