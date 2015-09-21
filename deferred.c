/*
   eventfs: a self-cleaning filesystem for event queues.
   Copyright (C) 2015  Jude Nelson

   This program is dual-licensed: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License version 3 or later as
   published by the Free Software Foundation. For the terms of this
   license, see LICENSE.LGPLv3+ or <http://www.gnu.org/licenses/>.

   You are free to use this program under the terms of the GNU Lesser General
   Public License, but WITHOUT ANY WARRANTY; without even the implied
   warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   See the GNU Lesser General Public License for more details.

   Alternatively, you are free to use this program under the terms of the
   Internet Software Consortium License, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
   For the terms of this license, see LICENSE.ISC or
   <http://www.isc.org/downloads/software-support-policy/isc-license/>.
*/

#include "deferred.h"
#include "eventfs.h"

// deferred remove-all context
struct eventfs_deferred_remove_ctx {

   struct fskit_core* core;
   char* fs_path;               // path to the entry to remove
   fskit_entry_set* children;   // the (optional) children to remove (not yet garbage-collected)
};


// helper to asynchronously try to unlink an inode and its children
static int eventfs_deferred_remove_cb( struct eventfs_wreq* wreq, void* cls ) {

   struct eventfs_deferred_remove_ctx* ctx = (struct eventfs_deferred_remove_ctx*)cls;
   struct fskit_detach_ctx* dctx = NULL;
   int rc = 0;
   
   eventfs_debug("DEFERRED: remove '%s'\n", ctx->fs_path );
   
   // remove the children 
   if( ctx->children != NULL ) {
       
      dctx = fskit_detach_ctx_new();
      if( dctx == NULL ) {
         return -ENOMEM;
      }

      rc = fskit_detach_ctx_init( dctx );
      if( rc != 0 ) {
         return rc;
      }

      // proceed to detach
      while( true ) {
         
         rc = fskit_detach_all_ex( ctx->core, ctx->fs_path, &ctx->children, dctx );
         if( rc == 0 ) {
             break;
         }
         else if( rc == -ENOMEM ) {
             continue;
         }
         else {
             break;
         }
      }
      
      fskit_detach_ctx_free( dctx );
      eventfs_safe_free( dctx );
      
      fskit_entry_set_free( ctx->children );
   }

   eventfs_safe_free( ctx->fs_path );
   eventfs_safe_free( ctx );
   
   return 0;
}


// Garbage-collect the given inode, and queue it for unlinkage.
// If the inode is a directory, recursively garbage-collect its children as well, and queue them and their descendents for unlinkage
// return 0 on success
// NOTE: child must be write-locked
int eventfs_deferred_remove( struct eventfs_state* eventfs, char const* child_path, struct fskit_entry* child ) {

   struct eventfs_deferred_remove_ctx* ctx = NULL;
   struct fskit_core* core = eventfs->core;
   struct eventfs_wreq* work = NULL;
   fskit_entry_set* children = NULL;
   int rc = 0;

   // asynchronously unlink it and its children
   ctx = EVENTFS_CALLOC( struct eventfs_deferred_remove_ctx, 1 );
   if( ctx == NULL ) {
       return -ENOMEM;
   }
   
   work = EVENTFS_CALLOC( struct eventfs_wreq, 1 );
   if( work == NULL ) {
       
       eventfs_safe_free( ctx );
       return -ENOMEM;
   }
   
   // set up the deferred unlink request 
   ctx->core = core;
   ctx->fs_path = strdup( child_path );
   
   if( ctx->fs_path == NULL ) {
       
       eventfs_safe_free( work );
       eventfs_safe_free( ctx );
       return -ENOMEM;
   }
   
   // garbage-collect this child
   rc = fskit_entry_tag_garbage( child, &children );
   if( rc != 0 ) {
       
       eventfs_safe_free( ctx );
       eventfs_safe_free( work );
       
       eventfs_error("fskit_entry_garbage_collect('%s') rc = %d\n", child_path, rc );
       return rc;
   }
   
   ctx->children = children;
   
   // deferred removal 
   eventfs_wreq_init( work, eventfs_deferred_remove_cb, ctx );
   eventfs_wq_add( eventfs->deferred_wq, work );
   
   return 0;
}


// callback to sweep the filesystem to remove dead directory inodes.
// basically, just run `ls` on the mountpoint.  The readdir() callback will queue
// dead directories for garbage-collection.
// return 0 on success
static int eventfs_deferred_reap_cb( struct eventfs_wreq* wreq, void* cls ) {
    
    int rc = 0;
    int status = 0;
    char* const mountpoint = (char* const)cls;
    
    char* const argv[] = {
        "/bin/ls", 
        (char*)mountpoint,
        NULL
    };
    
    char* const envp[] = {
        NULL
    };
    
    // send child's output to NULL
    int devnull = open("/dev/null", O_WRONLY );
    if( devnull < 0 ) {
        
        rc = -errno;
        eventfs_error("Failed to open /dev/null: %s\n", strerror(-rc));
        return rc;
    }
    
    pid_t child = fork();
    if( child < 0 ) {
        
        rc = -errno;
        close( devnull );
        eventfs_error("fork: %s\n", strerror( rc ) );
        return 0;
    }
    else if( child == 0 ) {
        
        // child!
        // async-safe only now...
        
        // send stdout and stderr to /dev/null 
        dup2( devnull, STDOUT_FILENO );
        dup2( devnull, STDERR_FILENO );
        
        execve( "/bin/ls", argv, envp );
        
        // if we get here, we error'ed
        _exit(1);
    }
    else {
        
        close( devnull );
        
        // explicitly set SIGCHLD to SIG_IDN, so the kernel should reap it
        return 0;
    }
}

// sweep the filesystem periodically to remove dead directory inodes.
// run in a separate process to prevent FUSE deadlocks
// return 0 on success
int eventfs_deferred_reap( struct eventfs_state* eventfs ) {
    
    int rc = 0;
    struct eventfs_wreq* work = NULL;
    
    work = EVENTFS_CALLOC( struct eventfs_wreq, 1 );
    if( work == NULL ) {
        return -ENOMEM;
    }
    
    // deferred removal 
   eventfs_wreq_init( work, eventfs_deferred_reap_cb, eventfs->mountpoint );
   eventfs_wq_add( eventfs->deferred_wq, work );
   return 0;
}
