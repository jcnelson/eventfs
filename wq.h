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

#ifndef _EVENTFS_WQ_H_
#define _EVENTFS_WQ_H_

#include "os.h"
#include "util.h"

struct eventfs_wreq;

// eventfs workqueue callback type
typedef int (*eventfs_wq_func_t)( struct eventfs_wreq* wreq, void* cls );

// eventfs workqueue request
struct eventfs_wreq {

   // callback to do work
   eventfs_wq_func_t work;

   // user-supplied arguments
   void* work_data;
   
   struct eventfs_wreq* next;     // pointer to next work element
};

// eventfs workqueue
struct eventfs_wq {
   
   // worker thread
   pthread_t thread;

   // is the thread running?
   volatile bool running;

   // things to do
   struct eventfs_wreq* work;
   struct eventfs_wreq* tail;

   // lock governing access to work
   pthread_mutex_t work_lock;

   // semaphore to signal the availability of work
   sem_t work_sem;
};

struct eventfs_wq* eventfs_wq_new();
int eventfs_wq_init( struct eventfs_wq* wq );
int eventfs_wq_start( struct eventfs_wq* wq );
int eventfs_wq_stop( struct eventfs_wq* wq );
int eventfs_wq_free( struct eventfs_wq* wq );

int eventfs_wreq_init( struct eventfs_wreq* wreq, eventfs_wq_func_t work, void* work_data );
int eventfs_wreq_free( struct eventfs_wreq* wreq );

int eventfs_wq_add( struct eventfs_wq* wq, struct eventfs_wreq* wreq );

#endif
