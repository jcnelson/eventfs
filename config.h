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

#ifndef _EVENTFS_CONFIG_H_
#define _EVENTFS_CONFIG_H_

#include "os.h"

// default config 
#define EVENTFS_DEFAULT_CONFIG_PATH     "/etc/eventfs/eventfs.conf"

// global config
#define EVENTFS_GLOBAL_CONFIG           "eventfs-config"
#define EVENTFS_DEFAULT_DIR_QUOTA       "default_max_dirs"
#define EVENTFS_DEFAULT_FILE_QUOTA      "default_max_files"
#define EVENTFS_DEFAULT_DIR_SIZE        "default_max_files_per_dir"
#define EVENTFS_DEFAULT_MAX_BYTES       "default_max_bytes"
#define EVENTFS_QUOTAS_DIR              "quotas"

// quota file
#define EVENTFS_QUOTA_CONFIG            "eventfs-quota"
#define EVENTFS_QUOTA_USERNAME          "user"
#define EVENTFS_QUOTA_GROUPNAME         "group"
#define EVENTFS_QUOTA_MAX_DIRS          "max_dirs"
#define EVENTFS_QUOTA_MAX_FILES         "max_files"
#define EVENTFS_QUOTA_MAX_DIR_SIZE      "max_files_per_dir"
#define EVENTFS_QUOTA_MAX_BYTES         "max_bytes"

struct eventfs_quota_entry;

// global config structure 
struct eventfs_config {
    
    uint64_t default_dir_quota;
    uint64_t default_file_quota;
    uint64_t default_files_per_dir_quota;
    uint64_t default_bytes_quota;
    
    char* quotas_dir;
};

int eventfs_config_load( char const* path, struct eventfs_config* conf, struct eventfs_quota_entry** user_quotas, struct eventfs_quota_entry** group_quotas );
int eventfs_config_free( struct eventfs_config* conf );

#endif