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

#define INI_MAX_LINE 4096 
#define INI_STOP_ON_FIRST_ERROR 1

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>

#include "ini.h"
#include "config.h"
#include "quota.h"
#include "util.h"

// quotas to parse for users and groups
struct quota_parse_ctx {
    
    struct eventfs_config* config;
    eventfs_quota** user_quotas;
    eventfs_quota** group_quotas;
    
    eventfs_quota* cur_quota;
};

// get a passwd struct for a user.
// on success, fill in pwd and *pwd_buf (the caller must free *pwd_buf, but not pwd).
// return 0 on success
// return -EINVAL if any argument is NULL 
// return -ENOMEM on OOM 
// return -ENOENT if the username cnanot be loaded
static int eventfs_get_passwd( char const* username, struct passwd* pwd, char** pwd_buf ) {
   
   struct passwd* result = NULL;
   char* buf = NULL;
   int buf_len = 0;
   int rc = 0;
   
   if( pwd == NULL || username == NULL || pwd_buf == NULL ) {
      return -EINVAL;
   }
   
   memset( pwd, 0, sizeof(struct passwd) );
   
   buf_len = sysconf( _SC_GETPW_R_SIZE_MAX );
   if( buf_len <= 0 ) {
      buf_len = 65536;
   }
   
   buf = EVENTFS_CALLOC( char, buf_len );
   if( buf == NULL ) {
      return -ENOMEM;
   }
   
   rc = getpwnam_r( username, pwd, buf, buf_len, &result );
   
   if( result == NULL ) {
      
      if( rc == 0 ) {
         free( buf );
         return -ENOENT;
      }
      else {
         rc = -errno;
         free( buf );
         
         eventfs_error("getpwnam_r(%s) errno = %d\n", username, rc);
         return rc;
      }
   }
   
   *pwd_buf = buf;
   
   // success!
   return rc;
}


// get a group struct for a group.
// on success, fill in grp and *grp_buf (the caller must free *grp_buf, but not grp).
// return 0 on success
// return -ENOMEM on OOM 
// return -EINVAL if any argument is NULL 
// return -ENOENT if the group cannot be found
int eventfs_get_group( char const* groupname, struct group* grp, char** grp_buf ) {
   
   struct group* result = NULL;
   char* buf = NULL;
   int buf_len = 0;
   int rc = 0;
   
   if( grp == NULL || groupname == NULL || grp_buf == NULL ) {
      return -EINVAL;
   }
   
   memset( grp, 0, sizeof(struct group) );
   
   buf_len = sysconf( _SC_GETGR_R_SIZE_MAX );
   if( buf_len <= 0 ) {
      buf_len = 65536;
   }
   
   buf = EVENTFS_CALLOC( char, buf_len );
   if( buf == NULL ) {
      return -ENOMEM;
   }
   
   rc = getgrnam_r( groupname, grp, buf, buf_len, &result );
   
   if( result == NULL ) {
      
      if( rc == 0 ) {
         free( buf );
         return -ENOENT;
      }
      else {
         rc = -errno;
         free( buf );
         
         eventfs_error("getgrnam_r(%s) errno = %d\n", groupname, rc);
         return rc;
      }
   }
   
   // success!
   return rc;
}


// ini parser callback for the global config
// return 1 on parsed
// return 0 on not parsed
static int eventfs_ini_config_parser( void* userdata, char const* section, char const* name, char const* value ) {
    
    struct quota_parse_ctx* ctx = (struct quota_parse_ctx*)userdata;
    struct eventfs_config* config = ctx->config;
    
    if( strcmp(section, EVENTFS_GLOBAL_CONFIG) == 0 ) {
        
        if( strcmp(name, EVENTFS_DEFAULT_DIR_QUOTA) == 0 ) {
            
            // dir quota 
            char* tmp = NULL;
            uint64_t val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                config->default_dir_quota = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_DEFAULT_FILE_QUOTA) == 0 ) {
            
            // file quota 
            char* tmp = NULL;
            uint64_t val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                config->default_file_quota = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_DEFAULT_DIR_SIZE) == 0 ) {
            
            // per-directory default quota 
            char* tmp = NULL;
            uint64_t val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                config->default_files_per_dir_quota = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_DEFAULT_MAX_BYTES) == 0 ) {
            
            // maximum number of bytes a user may have 
            char* tmp = NULL;
            uint64_t val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                config->default_bytes_quota = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_QUOTAS_DIR) == 0 ) {
            
            // user quota dir 
            char* tmp = strdup( value );
            if( tmp == NULL ) {
                
                // OOM 
                return 0;
            }
            else {
                
                config->quotas_dir = tmp;
                return 1;
            }
        }
        
        else {
            
            // unknown 
            eventfs_error("Unknown key '%s'\n", name );
            return 0;
        }
    }
    else {
        
        // not recognized 
        eventfs_error("Section not recognized: '%s'\n", section);
        return 0;
    }
}


// ini parser callback for a quota file 
// the first non-section line must be 'user' or 'group'
// return 1 if parsed 
// return 0 if not 
static int eventfs_ini_quota_parser( void* userdata, char const* section, char const* name, char const* value ) {
    
    int rc = 0;
    
    struct quota_parse_ctx* ctx = (struct quota_parse_ctx*)userdata;
    
    struct eventfs_config* config = ctx->config;
    eventfs_quota** user_quotas = ctx->user_quotas;
    eventfs_quota** group_quotas = ctx->group_quotas;
    
    if( strcmp(section, EVENTFS_QUOTA_CONFIG) == 0 ) {
        
        if( strcmp(name, EVENTFS_QUOTA_USERNAME) == 0 ) {
            
            // username.  find UID 
            char* pwd_buf = NULL;
            struct passwd pwd;
            eventfs_quota* user_quota = NULL;
            uid_t uid = 0;
            
            // look up the uid...
            rc = eventfs_get_passwd( value, &pwd, &pwd_buf );
            if( rc != 0 ) {
                
                eventfs_error("eventfs_get_passwd(%s) rc = %d\n", value, rc );
                return 0;
            }
            
            uid = pwd.pw_uid;
            free( pwd_buf );
            
            // new quota entry?
            user_quota = eventfs_quota_lookup( *user_quotas, uid );
            if( user_quota == NULL ) {
                
                // haven't seen before 
                rc = eventfs_quota_set( user_quotas, uid, config->default_file_quota, config->default_dir_quota, config->default_files_per_dir_quota, config->default_bytes_quota );
                if( rc != 0 ) {
                    
                    // probably OOM 
                    return 0;
                }
                else {
                    
                    ctx->cur_quota = eventfs_quota_lookup( *user_quotas, uid );
                    return 1;
                }
            }
            else {
                
                // duplicate
                eventfs_error( "Duplicate user '%s'\n", value);
                return 0;
            }
        }
        
        else if( strcmp(name, EVENTFS_QUOTA_GROUPNAME) == 0 ) {
            
            // group name.  find GID 
            char* grp_buf = NULL;
            struct group grp;
            eventfs_quota* group_quota = NULL;
            gid_t gid = 0;
            
            rc = eventfs_get_group( value, &grp, &grp_buf );
            if( rc != 0 ) {
                
                eventfs_error("eventfs_get_group(%s) rc = %d\n", value, rc );
                return 0;
            }
            
            gid = grp.gr_gid;
            free( grp_buf );
            
            // new quota entry?
            group_quota = eventfs_quota_lookup( *group_quotas, gid );
            if( group_quota == NULL ) {
                
                // haven't seen before 
                rc = eventfs_quota_set( group_quotas, gid, config->default_file_quota, config->default_dir_quota, config->default_files_per_dir_quota, config->default_bytes_quota );
                if( rc != 0 ) {
                    
                    // probably OOM 
                    return 0;
                }
                else {
                    
                    ctx->cur_quota = eventfs_quota_lookup( *group_quotas, gid );
                    return 1;
                }
            }
            else {
                
                // duplicate 
                eventfs_error("Duplicate group '%s'\n", value);
                return 0;
            }
        }
        
        // need to have a quota entry 
        if( ctx->cur_quota == NULL ) {
            
            eventfs_error("No quota yet defined.  The first field of the file must be '%s' or '%s'\n", EVENTFS_QUOTA_USERNAME, EVENTFS_QUOTA_GROUPNAME);
            return 0;
        }
        
        if( strcmp(name, EVENTFS_QUOTA_MAX_DIRS) == 0 ) {
            
            // dir quota 
            char* tmp = NULL;
            uint64_t val = 0;
            
            val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                ctx->cur_quota->max_dirs = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_QUOTA_MAX_FILES) == 0 ) {
            
            // file quota 
            char* tmp = NULL;
            uint64_t val = 0;
            
            val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                ctx->cur_quota->max_files = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_QUOTA_MAX_DIR_SIZE) == 0 ) {
            
            // max files per dir 
            char* tmp = NULL;
            uint64_t val = 0;
            
            val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s'\n", name, value );
                return 0;
            }
            else {
                
                ctx->cur_quota->max_files_per_dir = val;
                return 1;
            }
        }
        
        else if( strcmp(name, EVENTFS_QUOTA_MAX_BYTES) == 0 ) {
            
            // max number of bytes 
            char* tmp = NULL;
            uint64_t val = 0;
            
            val = strtoull( value, &tmp, 10 );
            if( *tmp != '\0' ) {
                
                eventfs_error("Unable to parse '%s=%s\n", name, value );
                return 0;
            }
            else {
                
                ctx->cur_quota->max_bytes = val;
                return 1;
            }
        }
        
        else {
            
            // unknown 
            eventfs_error("Unknown field '%s'\n", name);
            return 0;
        }
    }
    
    else {
        
        // unknown section 
        eventfs_error("Unknown section '%s'\n", section);
        return 0;
    }
}


// parse a quota file 
// return 0 on success 
// return -ENOMEM on OOM 
// return -errno for all filesystem-related errors 
int eventfs_config_load_quota( struct quota_parse_ctx* ctx, char const* path ) {
    
    int rc = 0;
    eventfs_quota* quota = NULL;
    
    FILE* f = fopen( path, "r" );
    if( f == NULL ) {
        
        rc = -errno;
        eventfs_error("Failed to open '%s': %s\n", path, strerror(-rc));
        return rc;
    }
    
    rc = ini_parse_file( f, eventfs_ini_quota_parser, ctx );
    fclose( f );
    
    if( rc != 0 ) {
        
        eventfs_error("Failed to parse '%s'\n", path );
        return -EPERM;
    }
    
    // reset...
    ctx->cur_quota = NULL;
    return 0;
}

// load the global config 
// return 0 on success
// return -ENOENT if not found 
// return -ENOMEM on OOM 
// return -EPERM for parse error
// return -errno for all filesystem related errors
int eventfs_config_load_global( struct quota_parse_ctx* ctx, char const* path ) {
    
    int rc = 0;
    
    FILE* f = fopen( path, "r" );
    if( f == NULL ) {
        
        rc = -errno;
        eventfs_error("Failed to open '%s': %s\n", path, strerror(-rc));
        return rc;
    }
    
    rc = ini_parse_file( f, eventfs_ini_config_parser, ctx );
    fclose( f );
    
    if( rc != 0 ) {
     
        eventfs_error("Failed to parse '%s'\n", path );
        return -EPERM;
    }
    
    return 0;
}


// scan the quotas directory, and load all quota files 
// return 0 on success
// return -EPERM on failure 
// return -ENOMEM on OOM 
int eventfs_config_load_all_quotas( struct quota_parse_ctx* ctx, char const* quotas_dir ) {
    
    int rc = 0;
    int num_dirents = 0;
    struct dirent** dirents = NULL;
    char* quota_path = NULL;
    
    rc = scandir( quotas_dir, &dirents, NULL, NULL );
    if( rc < 0 ) {
        
        rc = -errno;
        eventfs_error("Failed to read '%s': %s\n", quotas_dir, strerror(-rc) );
        
        return rc;
    }
    else {
    
        num_dirents = rc;
    }
    
    // load each quota 
    for( int i = 0; i < num_dirents; i++ ) {
        
        quota_path = fskit_fullpath( quotas_dir, dirents[i]->d_name, NULL );
        if( quota_path == NULL ) {
            
            rc = -ENOMEM;
            break;
        }
        
        rc = eventfs_config_load_quota( ctx, quota_path );
        if( rc != 0 ) {
            
            rc = -EPERM;
            eventfs_safe_free( quota_path );
            break;
        }
        
        eventfs_safe_free( quota_path );
    }
    
    // free memory 
    for( int i = 0; i < num_dirents; i++ ) {
        
        eventfs_safe_free( dirents[i] );
    }
    
    eventfs_safe_free( dirents );
    
    return rc;
}


// load all configuration data 
// return 0 on success 
// return -EPERM on failure
// return -ENOMEM on OOM
int eventfs_config_load( char const* path, struct eventfs_config* conf, struct eventfs_quota_entry** user_quotas, struct eventfs_quota_entry** group_quotas ) {
    
    int rc = 0;
    struct quota_parse_ctx ctx;
    char* quotas_dir = NULL;
    
    memset( &ctx, 0, sizeof(struct quota_parse_ctx));
    
    ctx.config = conf;
    ctx.user_quotas = user_quotas;
    ctx.group_quotas = group_quotas;
    
    rc = eventfs_config_load_global( &ctx, path );
    if( rc != 0 ) {
        
        return -EPERM;
    }
    
    // need a quotas dir 
    if( conf->quotas_dir == NULL ) {
        
        eventfs_error("No '%s' field defined\n", EVENTFS_QUOTAS_DIR );
        return -EPERM;
    }
    
    if( conf->quotas_dir[0] != '/' ) {
        
        // make absolute path
        char* tmp = fskit_dirname( path, NULL );
        if( tmp == NULL ) {
            
            eventfs_config_free( conf );
            return -ENOMEM;
        }
        
        quotas_dir = fskit_fullpath( tmp, conf->quotas_dir, NULL );
        eventfs_safe_free( tmp );
        
        if( quotas_dir == NULL ) {
            
            eventfs_config_free( conf );
            return -ENOMEM;
        }
    }
    else {
        
        quotas_dir = conf->quotas_dir;
    }
    
    // load quotas...
    rc = eventfs_config_load_all_quotas( &ctx, quotas_dir );
    if( rc != 0 ) {
        
        eventfs_config_free( conf );
        eventfs_quota_free( *user_quotas );
        eventfs_quota_free( *group_quotas );
        
        *user_quotas = NULL;
        *group_quotas = NULL;
        
        if( quotas_dir != conf->quotas_dir ) {
            eventfs_safe_free( quotas_dir );
        }
        
        return rc;
    }
    
    if( quotas_dir != conf->quotas_dir ) {
        eventfs_safe_free( quotas_dir );
    }
    
    return 0;
}


// free a config structure 
int eventfs_config_free( struct eventfs_config* conf ) {
    
    if( conf->quotas_dir != NULL ) {
        
        eventfs_safe_free( conf->quotas_dir );
    }
    
    memset( conf, 0, sizeof(struct eventfs_config) );
    return 0;
}
