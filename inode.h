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

#ifndef _EVENTFS_INODE_H_
#define _EVENTFS_INODE_H_

#include <fskit/fskit.h>
#include <pstat/libpstat.h>

#include "util.h"

#define EVENTFS_PIDFILE_BUF_LEN   50

#define EVENTFS_VERIFY_INODE      0x1
#define EVENTFS_VERIFY_MTIME      0x2
#define EVENTFS_VERIFY_SIZE       0x4
#define EVENTFS_VERIFY_PATH       0x8
#define EVENTFS_VERIFY_STARTTIME  0x10

#define EVENTFS_VERIFY_ALL        0x1F

#define EVENTFS_VERIFY_DEFAULT    (EVENTFS_VERIFY_INODE | EVENTFS_VERIFY_MTIME | EVENTFS_VERIFY_SIZE | EVENTFS_VERIFY_STARTTIME)

// information for a file inode (as a deque entry)
struct eventfs_file_inode {
   char* name;                                          // name of the file
   char* contents;                                      // contents of the file
   off_t size;                                          // size of the file
   size_t contents_len;                                 // size of the contents buffer
   
   // deque pointers 
   struct eventfs_file_inode* prev;
   struct eventfs_file_inode* next;
};

// information for a directory inode 
struct eventfs_dir_inode {
   struct pstat* ps;                                    // process owner status
   bool deleted;                                        // if true, then consider the associated fskit entry deleted
   int verify_discipline;                               // bit flags of EVENTFS_VERIFY_* that control how strict we are in verifying the accessing process
   
   struct eventfs_file_inode* head;                     // head inode: symlink to the oldest file inode 
   struct eventfs_file_inode* tail;                     // tail inode: symlink to the newest file inode
   
   // head and tail inodes (for fast lookup)
   struct fskit_entry* fent_head;
   struct fskit_entry* fent_tail;
};


int eventfs_file_inode_init( struct eventfs_file_inode* inode, char const* name );
int eventfs_file_inode_free( struct eventfs_file_inode* inode );

int eventfs_dir_inode_init( struct eventfs_dir_inode* inode, pid_t pid, int verify_discipline );
int eventfs_dir_inode_free( struct fskit_core* core, struct eventfs_dir_inode* inode );

// deque operations we expose
int eventfs_dir_inode_append( struct fskit_core* core, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode* file );
int eventfs_dir_inode_remove( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode* file );
int eventfs_dir_inode_pophead( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode** file );
int eventfs_dir_inode_poptail( struct fskit_core* core, char const* dir_path, struct eventfs_dir_inode* dir, struct fskit_entry* dent, struct eventfs_file_inode** file );

// keep symlinks consistent 
int eventfs_dir_inode_retarget_head( struct eventfs_dir_inode* dir, char* target );
int eventfs_dir_inode_retarget_tail( struct eventfs_dir_inode* dir, char* target );

// validity check (on stat and readdir)
int eventfs_dir_inode_is_valid( struct eventfs_dir_inode* inode );

#endif 