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

#ifndef _EVENTFS_UTIL_H_
#define _EVENTFS_UTIL_H_

#include "os.h"

#define EVENTFS_CALLOC( type, nmemb ) (type*)calloc( sizeof(type), nmemb )

#define eventfs_debug fskit_debug 
#define eventfs_error fskit_error 
#define eventfs_safe_free(ptr) do { if( (ptr) != NULL ) { free( ptr ); (ptr) = NULL; } } while(0)

#endif