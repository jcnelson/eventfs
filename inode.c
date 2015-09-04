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
#include "inode.h"
#include "deferred.h"

// set up a pidfile inode 
// return 0 on success
// return -ENOMEM on OOM 
// return negative on failure to stat the process identified
int eventfs_dir_inode_init( struct eventfs_dir_inode* inode, pid_t pid, int verify_discipline ) {
   
   int rc = 0;
   int flags = 0;
   
   memset( inode, 0, sizeof(struct eventfs_dir_inode) );
   
   inode->ps = pstat_new();
   if( inode->ps == NULL ) {
      return -ENOMEM;
   }
   
   rc = pstat( pid, inode->ps, flags );
   if( rc != 0 ) {
       
      pstat_free( inode->ps );
      return rc;
   }
   
   inode->verify_discipline = verify_discipline;
   
   inode->fent_head = NULL;
   inode->fent_tail = NULL;
   
   return 0;
}


// verify that a given process created the given directory
// return 0 if not equal 
// return 1 if equal 
// return negative on error
static int eventfs_dir_inode_is_created_by_proc( struct eventfs_dir_inode* inode, struct pstat* proc_stat, int verify_discipline ) {
   
   struct stat sb;
   struct stat inode_sb;
   char bin_path[PATH_MAX+1];
   char inode_path[PATH_MAX+1];
   
   pstat_get_stat( proc_stat, &sb );
   pstat_get_stat( inode->ps, &inode_sb );
   
   pstat_get_path( proc_stat, bin_path );
   pstat_get_path( inode->ps, inode_path );
   
   if( !pstat_is_running( proc_stat ) ) {
   
      eventfs_debug("PID %d is not running\n", pstat_get_pid( proc_stat ) );
      return 0;
   }
   
   if( pstat_get_pid( proc_stat ) != pstat_get_pid( inode->ps ) ) {
      
      eventfs_debug("PID mismatch: %d != %d\n", pstat_get_pid( inode->ps ), pstat_get_pid( proc_stat ) );
      return 0;
   }
   
   if( verify_discipline & EVENTFS_VERIFY_INODE ) {
      
      if( pstat_is_deleted( proc_stat ) || inode_sb.st_ino != sb.st_ino ) {
         
         eventfs_debug("%d: Inode mismatch: %ld != %ld\n", pstat_get_pid( inode->ps ), inode_sb.st_ino, sb.st_ino );
         return 0;
      }
   }
   
   if( verify_discipline & EVENTFS_VERIFY_SIZE ) {
      if( pstat_is_deleted( proc_stat ) || inode_sb.st_size != sb.st_size ) {
         
         eventfs_debug("%d: Size mismatch: %jd != %jd\n", pstat_get_pid( inode->ps ), inode_sb.st_size, sb.st_size );
         return 0;
      }
   }
   
   if( verify_discipline & EVENTFS_VERIFY_MTIME ) {
      if( pstat_is_deleted( proc_stat )|| inode_sb.st_mtim.tv_sec != sb.st_mtim.tv_sec || inode_sb.st_mtim.tv_nsec != sb.st_mtim.tv_nsec ) {
         
         eventfs_debug("%d: Modtime mismatch: %ld.%ld != %ld.%ld\n", pstat_get_pid( inode->ps ), inode_sb.st_mtim.tv_sec, inode_sb.st_mtim.tv_nsec, sb.st_mtim.tv_sec, sb.st_mtim.tv_nsec );
         return 0;
      }
   }
   
   if( verify_discipline & EVENTFS_VERIFY_PATH ) {
       
      if( pstat_is_deleted( proc_stat ) || strcmp(bin_path, inode_path) != 0 ) {
         
         eventfs_debug("%d: Path mismatch: %s != %s\n", pstat_get_pid( inode->ps ), inode_path, bin_path );
         return 0;
      }
   }
   
   if( verify_discipline & EVENTFS_VERIFY_STARTTIME ) {
      
      if( pstat_get_starttime( proc_stat ) != pstat_get_starttime( inode->ps ) ) {
          
         eventfs_debug("%d: Start time mismatch: %" PRIu64 " != %" PRIu64 "\n", pstat_get_pid( inode->ps ), pstat_get_starttime( proc_stat ), pstat_get_starttime( inode->ps ) );
         return 0;
      }
   }
      
   return 1;
}


// verify that a directory inode is still valid.
// that is, there's a process with the given PID running, and it's an instance of the same program that created it.
// to speed this up, only check the hash of the process binary if the modtime has changed
// return 1 if valid 
// return 0 if not valid 
// return negative on error
int eventfs_dir_inode_is_valid( struct eventfs_dir_inode* inode ) {
   
   int rc = 0;
   struct pstat* ps = pstat_new();
   if( ps == NULL ) {
      return -ENOMEM;
   }
   
   pid_t pid = pstat_get_pid( inode->ps );
   
   rc = pstat( pid, ps, 0 );
   if( rc < 0 ) {
       
      pstat_free( ps );
      eventfs_error("pstat(%d) rc = %d\n", pid, rc );
      return rc;
   }
   
   rc = eventfs_dir_inode_is_created_by_proc( inode, ps, inode->verify_discipline );
   pstat_free( ps );
   
   if( rc < 0 ) {
       
      eventfs_error("eventfs_dir_inode_is_created_by_proc(%d) rc = %d\n", pid, rc );
      return rc;
   }
   
   return rc;
}

// free a directory inode.
// must be empty (otherwise returns -ENOTEMPTY)
int eventfs_dir_inode_free( struct fskit_core* core, struct eventfs_dir_inode* inode ) {
   
   if( inode->ps != NULL ) {
      
      pstat_free( inode->ps );
      inode->ps = NULL;
   }
   
   memset( inode, 0, sizeof(struct eventfs_dir_inode) );
   return 0;
}


// set up a file inode 
int eventfs_file_inode_init( struct eventfs_file_inode* inode, char const* name ) {
    
   memset( inode, 0, sizeof(struct eventfs_file_inode) );
   
   char* name_dup = strdup( name );
   if( name_dup == NULL ) {
       return -ENOMEM;
   }
   
   inode->name = name_dup;
   return 0;
}

// free a file inode 
// must be removed from its parent directory already 
int eventfs_file_inode_free( struct eventfs_file_inode* inode ) {
    
   if( inode->contents != NULL ) {
       eventfs_safe_free( inode->contents );
   }
   
   if( inode->name != NULL ) {
       eventfs_safe_free( inode->name );
   }
   
   memset( inode, 0, sizeof(struct eventfs_file_inode) );
   return 0;
}


// update the deque head link when it itself gets unlinked.
// re-attach it to the parent inode, and retarget it to the next-oldest file.
// return 0 on success
// return -ENOMEM on OOM
// return -EIO on bug 
// NOTE: dent must be write-locked
static int eventfs_dir_head_symlink_restore( struct fskit_core* core, struct eventfs_dir_inode* dir, struct fskit_entry* dent, char* name_dup ) {

    // update head link
    // point symlink to new head
    int rc = 0;
    
    // regenerate the symlink 
    struct fskit_entry* new_head = fskit_entry_new();
    if( new_head == NULL ) {
        return -ENOMEM;
    }
    
    uint64_t inode_number = fskit_core_inode_alloc( core, dent, new_head );
    
    rc = fskit_entry_init_symlink( new_head, inode_number, "head", name_dup );
    if( rc != 0 ) {
        
        fskit_core_inode_free( core, inode_number );
        eventfs_safe_free( new_head );
        return -ENOMEM;
    }
    
    // add it to the parent 
    rc = fskit_entry_attach_lowlevel( dent, new_head );
    if( rc != 0 ) {
        
        fskit_core_inode_free( core, inode_number );
        eventfs_safe_free( new_head );
        
        eventfs_error("BUG: attach '%s' to %p rc = %d\n", "head", dir, rc );
        return rc;
    }
    
    dir->fent_head = new_head;
    
    return rc;
}
                

// update the deque tail link when it itself gets unlinked.
// regenerate and re-attach it to the parent inode, and retarget it to the next-youngest file.
// return 0 on success 
// return -ENOMEM on OOM 
// NOTE: dent must be write-locked
static int eventfs_dir_tail_symlink_restore( struct fskit_core* core, struct eventfs_dir_inode* dir, struct fskit_entry* dent, char* name_dup ) {

    // update tail link 
    // point symlink to new tail 
    int rc = 0;
    
    // regenerate the symlink 
    struct fskit_entry* new_tail = fskit_entry_new();
    if( new_tail == NULL ) {
        return -ENOMEM;
    }
    
    uint64_t inode_number = fskit_core_inode_alloc( core, dent, new_tail );
    
    rc = fskit_entry_init_symlink( new_tail, inode_number, "tail", name_dup );
    if( rc != 0 ) {
        
        fskit_core_inode_free( core, inode_number );
        eventfs_safe_free( new_tail );
        return -ENOMEM;
    }
    
    // add it to the parent 
    rc = fskit_entry_attach_lowlevel( dent, new_tail );
    if( rc != 0 ) {
        
        fskit_core_inode_free( core, inode_number );
        eventfs_safe_free( new_tail );
        
        eventfs_error("BUG: attach '%s' to %p rc = %d\n", "tail", dir, rc );
        return rc;
    }
    
    dir->fent_tail = new_tail;
    
    return rc;
}


// make the directory empty:
// * make the deque pointers NULL
// * detach and unref the symlinks 
// NOTE: path refers to the inode to be detached by fskit
// return 0 on success 
// return -ENOMEM on OOM
// NOTE: dent must be write-locked, but its head and tail symlinks cannot be 
static int eventfs_dir_inode_set_empty( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct fskit_entry* detached_child ) {
    
    struct eventfs_state* eventfs = (struct eventfs_state*)fskit_core_get_user_data( core );
    int rc = 0;
    
    if( dir->head == NULL && dir->tail == NULL ) {
        // already empty
        return 0;
    }
    
    eventfs_debug("set %p empty\n", dir );
    
    int head_type = 0;
    int tail_type = 0;
    
    char* detach_path = NULL;
    
    if( detached_child != dir->fent_head ) {
    
        // destroying tail or a file; detach head ourselves
        fskit_entry_rlock( dir->fent_head );
        head_type = fskit_entry_get_type( dir->fent_head );
        fskit_entry_unlock( dir->fent_head );
        
        if( head_type == FSKIT_ENTRY_TYPE_DEAD ) {
            
            // already destroyed
            eventfs_safe_free( dir->fent_head );
            dir->fent_head = NULL;
        }
        else {
            
            // destroy head symlink
            detach_path = fskit_fullpath( dir_path, "head", NULL );
            if( detach_path == NULL ) {
                return -ENOMEM;
            }
            
            fskit_entry_wlock( dir->fent_head );
            
            fskit_entry_detach_lowlevel( dent, dir->fent_head );
            rc = fskit_entry_try_destroy_and_free( core, detach_path, dent, dir->fent_head );
            
            if( rc > 0 ) {
                // destroyed 
                rc = 0;
            }
            else {
                // not destroyed
                fskit_entry_unlock( dir->fent_head );
            }
            
            eventfs_safe_free( detach_path );
            
            dir->fent_head = NULL;
        }
    }
    
    if( detached_child != dir->fent_tail ) {
        
        // destroying head or a file; detach tail ourselves
        fskit_entry_rlock( dir->fent_tail );
        tail_type = fskit_entry_get_type( dir->fent_tail );
        fskit_entry_unlock( dir->fent_tail );
        
        if( tail_type == FSKIT_ENTRY_TYPE_DEAD ) {
            
            // tail is already destroyed
            eventfs_safe_free( dir->fent_tail );
            dir->fent_tail = NULL;
        }
        else {
            
            // destroy tail symlink
            detach_path = fskit_fullpath( dir_path, "tail", NULL );
            if( detach_path == NULL ) {
                return -ENOMEM;
            }
            
            fskit_entry_wlock( dir->fent_tail );
            
            fskit_entry_detach_lowlevel( dent, dir->fent_tail );
            rc = fskit_entry_try_destroy_and_free( core, detach_path, dent, dir->fent_tail );
            
            if( rc > 0 ) {
                // destroyed 
                rc = 0;
            }
            else {
                // not destroyed
                fskit_entry_unlock( dir->fent_tail );
            }
            
            eventfs_safe_free( detach_path );
            
            dir->fent_tail = NULL;
        }
    }
    
    dir->head = NULL;
    dir->tail = NULL;
    
    return 0;
}


// insert a file inode into a directory, at the very end of the deque.
// if needed, allocate and attach the head and tail symlinks.
// return 0 on success 
// return -ENOENT if the dir is deleted 
// return -ENOMEM on OOM
// NOTE: dent must be write-locked
int eventfs_dir_inode_append( struct fskit_core* core, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode* file ) {
    
    int rc = 0;
    if( dir->deleted ) {
        return -ENOENT;
    }
    
    char* name_dup_tail = strdup(file->name);
    if( name_dup_tail == NULL ) {
        return -ENOMEM;
    }
    
    if( dir->head == NULL && dir->tail == NULL ) {
        
        // first entry--allocate and attach symlinks
        struct fskit_entry* fent_head = fskit_entry_new();
        struct fskit_entry* fent_tail = fskit_entry_new();
        char* name_dup_head = strdup(file->name);
        
        if( fent_head == NULL || fent_tail == NULL || name_dup_head == NULL ) {
            
            eventfs_safe_free( fent_head );
            eventfs_safe_free( fent_tail );
            eventfs_safe_free( name_dup_head );
            eventfs_safe_free( name_dup_tail );
            return -ENOMEM;
        }
        
        uint64_t head_inode_number = fskit_core_inode_alloc( core, dent, fent_head );
        uint64_t tail_inode_number = fskit_core_inode_alloc( core, dent, fent_tail );
    
        rc = fskit_entry_init_symlink( fent_head, head_inode_number, "head", name_dup_head );
        eventfs_safe_free( name_dup_head );
        
        if( rc != 0 ) {
           
            fskit_core_inode_free( core, head_inode_number );
            fskit_core_inode_free( core, tail_inode_number );
            eventfs_safe_free( fent_head );
            eventfs_safe_free( fent_tail );
            eventfs_safe_free( name_dup_tail );
            return rc;
        }
        
        rc = fskit_entry_init_symlink( fent_tail, tail_inode_number, "tail", name_dup_tail );
        eventfs_safe_free( name_dup_tail );
        
        if( rc != 0 ) {
            
            fskit_entry_destroy( core, fent_head, false );
            fskit_core_inode_free( core, tail_inode_number );
            eventfs_safe_free( fent_head );
            eventfs_safe_free( fent_tail );
            return rc;
        }
        
        rc = fskit_entry_attach_lowlevel( dent, fent_head );
        if( rc != 0 ) {
            
            fskit_entry_destroy( core, fent_head, false );
            fskit_entry_destroy( core, fent_tail, false );
            eventfs_safe_free( fent_head );
            eventfs_safe_free( fent_tail );
            return rc;
        }
        
        rc = fskit_entry_attach_lowlevel( dent, fent_tail );
        if( rc != 0 ) {
            
            fskit_entry_destroy( core, fent_head, false );
            fskit_entry_destroy( core, fent_tail, false );
            eventfs_safe_free( fent_head );
            eventfs_safe_free( fent_tail );
            return rc;
        }
        
        dir->fent_head = fent_head;
        dir->fent_tail = fent_tail;
        
        dir->head = file;
        dir->tail = file;
        
        file->prev = NULL;
        file->next = NULL;
        
        return rc;
    }
    else {
        
        // second or more
        file->next = NULL;
        file->prev = dir->tail;
        
        dir->tail->next = file;
        dir->tail = dir->tail->next;
        
        // retarget tail symlink target
        eventfs_dir_inode_retarget_tail( dir, name_dup_tail );
        return 0;
    }
}


// remove a file inode from a directory that is neither the head or tail.
// return 0 on success 
// return -ENOMEM on OOM
int eventfs_dir_inode_remove( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode* file ) {
    
    int rc = 0;
    
    if( dir->deleted ) {
        return -ENOENT;
    }
    
    // is this the last file?
    if( dir->head == dir->tail ) {
        
        // destroy head and tail symlink
        rc = eventfs_dir_inode_set_empty( core, dir_path, dir, dent, NULL );
        
        return rc;
    }
    else {
        
        for( struct eventfs_file_inode* ptr = dir->head; ptr != NULL; ptr = ptr->next ) {
            
            if( ptr == file ) {
                
                if( ptr == dir->head ) {
                    
                    char* new_dir_head_name = strdup( dir->head->next->name );
                    if( new_dir_head_name == NULL ) {
                        return -ENOMEM;
                    }
                    
                    // update head link
                    rc = eventfs_dir_inode_retarget_head( dir, new_dir_head_name );
                    
                    if( rc != 0 ) {
                        
                        return rc;
                    }
                    
                    // shrink deque
                    dir->head = dir->head->next;
                    dir->head->prev = NULL;
                }
                else if( ptr == dir->tail ) {
                    
                    // update tail link 
                    char* new_dir_tail_name = strdup( dir->tail->prev->name );
                    if( new_dir_tail_name == NULL ) {
                        return -ENOMEM;
                    }
                    
                    // update tail link 
                    rc = eventfs_dir_inode_retarget_tail( dir, new_dir_tail_name );
                    
                    if( rc != 0 ) {
                        
                        return rc;
                    }
                    
                    // shrink deque
                    dir->tail = dir->tail->prev;
                    dir->tail->next = NULL;
                }
                else {
                    
                    // remove from middle of deque
                    ptr->prev->next = ptr->next;
                    ptr->next->prev = ptr->prev;
                }
                
                return rc;
            }
        }
    }
    
    return -ENOENT;
}


// remove the head inode, and advance the head pointer.
// put the dead file inode's data into *file 
// return 0 on success 
// return -ENOENT if dir is deleted 
// dent must be write-locked
int eventfs_dir_inode_pophead( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode** file ) {
    
    int rc = 0;
    
    if( dir->deleted ) {
        return -ENOENT;
    }
    
    else if( dir->head == NULL ) {
        
        // already empty 
        *file = NULL;
        return 0;
    }
    
    // find target
    struct fskit_entry* fent = fskit_dir_find_by_name( dent, dir->head->name );
    if( fent == NULL ) {
        return -ENOENT;
    }

    // target's path 
    char* target_path = fskit_fullpath( dir_path, dir->head->name, NULL );
    if( target_path == NULL ) {
        return -ENOMEM;
    }
    
    // new head target, if there is another file in this directory
    char* new_dir_head_name = NULL;
    if( dir->head->next != NULL ) {
        new_dir_head_name = strdup( dir->head->next->name );
        if( new_dir_head_name == NULL ) {
            
            eventfs_safe_free( target_path );
            return -ENOMEM;
        }
    }
    
    fskit_entry_wlock( fent );
    
    // extract head inode data 
    *file = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
    fskit_entry_set_user_data( fent, NULL );
    
    // detach head-pointed file
    fskit_entry_detach_lowlevel( dent, fent );
    rc = fskit_entry_try_destroy_and_free( core, target_path, dent, fent );
    if( rc > 0 ) {
        // destroyed 
        rc = 0;
    }
    else {
        // not destroyed 
        fskit_entry_unlock( fent );
    }
    
    eventfs_safe_free( target_path );

    if( dir->head == dir->tail ) {
        
        // destroy head and tail symlink, and destroy file
        rc = eventfs_dir_inode_set_empty( core, dir_path, dir, dent, NULL );
        
        return rc;
    }
    
    else {
        
        // restore dir head pointer
        rc = eventfs_dir_head_symlink_restore( core, dir, dent, new_dir_head_name );
        eventfs_safe_free( new_dir_head_name );
        
        if( rc != 0 ) {
            
            return rc;
        }
        
        // shrink deque
        dir->head = dir->head->next;
        dir->head->prev = NULL;
        
        return rc;
    }
}


// remove the tail inode, and retract the tail pointer.
// put the dead file inode's data into *file 
// return 0 on success 
// return -ENOENT if dir is deleted 
// dent must be write-locked
int eventfs_dir_inode_poptail( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode** file ) {
    
    int rc = 0;
    
    if( dir->deleted ) {
        return -ENOENT;
    }
    
    if( dir->tail == NULL ) {
        
        // already empty 
        *file = NULL;
        return 0;
    }
    
    struct fskit_entry* fent = fskit_dir_find_by_name( dent, dir->tail->name );
    if( fent == NULL ) {
        return -ENOENT;
    }
    
    // target's path 
    char* target_path = fskit_fullpath( dir_path, dir->tail->name, NULL );
    if( target_path == NULL ) {
        return -ENOMEM;
    }
    
    // new target
    char* new_dir_tail_name = NULL;
    if( dir->tail->prev != NULL ) {
        
        new_dir_tail_name = strdup( dir->tail->prev->name );
        if( new_dir_tail_name == NULL ) {
            eventfs_safe_free( target_path );
            return -ENOMEM;
        }
    }
    
    fskit_entry_wlock( fent );
    
    // extract head inode data 
    *file = (struct eventfs_file_inode*)fskit_entry_get_user_data( fent );
    fskit_entry_set_user_data( fent, NULL );
    
    // detach tail-pointed file
    fskit_entry_detach_lowlevel( dent, fent );
    rc = fskit_entry_try_destroy_and_free( core, target_path, dent, fent );
    if( rc > 0 ) {
        // destroyed 
        rc = 0;
    }
    else {
        // not destroyed 
        fskit_entry_unlock( fent );
    }
    
    eventfs_safe_free( target_path );
    
    if( dir->head == dir->tail ) {
        
        // destroy head and tail symlink
        rc = eventfs_dir_inode_set_empty( core, dir_path, dir, dent, NULL );
        
        return rc;
    }
    
    else {
        
        // restore dir tail pointer 
        rc = eventfs_dir_tail_symlink_restore( core, dir, dent, new_dir_tail_name );
        eventfs_safe_free( new_dir_tail_name );
        
        if( rc != 0 ) {
            
            return rc;
        }
        
        // shrink deque 
        dir->tail = dir->tail->prev;
        dir->tail->next = NULL;
        
        return rc;
    }
}


// retarget the 'head' symlink
// return 0 on success
// return -ENOENT if the directory is marked as deleted
// NOTE: takes ownership of the 'target' memory ('target' should be malloc'ed)
int eventfs_dir_inode_retarget_head( struct eventfs_dir_inode* dir, char* target ) {
    
    int rc = 0;
    if( dir->deleted ) {
        return -ENOENT;
    }
    
    char* old_target = fskit_entry_swap_symlink_target( dir->fent_head, target );
    
    if( old_target != NULL ) {
        eventfs_safe_free( old_target );
    }
    
    return 0;
}


// retarget the 'tail' symlink 
// return 0 on success 
// return -ENOENT if the directory is marked as deleted 
// NOTE: takes ownership of the 'target' memory ('target' should be malloc'ed)
int eventfs_dir_inode_retarget_tail( struct eventfs_dir_inode* dir, char* target ) {
    
    int rc = 0;
    if( dir->deleted ) {
        return -ENOENT;
    }
    
    char* old_target = fskit_entry_swap_symlink_target( dir->fent_tail, target );
    
    if( old_target != NULL ) {
        eventfs_safe_free( old_target );
    }
    
    return 0;
}
