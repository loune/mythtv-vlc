/*****************************************************************************
 * myth.c: Myth Protocol input module
 *****************************************************************************
 * Copyright (C) 2001-2006 the VideoLAN team
 * Copyright (C) 2009 Loune Lam
 * $Id$
 *
 * Authors: Loune Lam <lpgcritter@nasquan.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Includes
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_playlist.h>
#include <vlc_input.h>

#include <assert.h>

#include <vlc_access.h>
#include <vlc_interface.h>

#include <vlc_network.h>
#include <vlc_services_discovery.h>
#include <vlc_url.h>

#define IPPORT_MYTH 6543u


/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int   InOpen ( vlc_object_t * );
static void  InClose( vlc_object_t * );
static int  SDOpen ( vlc_object_t * );
static void SDClose( vlc_object_t * );

#define CACHING_TEXT N_("Caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Caching value for MythTV streams. This " \
    "value should be set in milliseconds." )

#define MYTH_MINIMUM_VER 56

#define SERVER_URL_TEXT N_("MythTV Backend Server URL")
#define SERVER_URL_LONGTEXT N_("Enter the URL of myth backend starting with eg. myth://localhost/")

VLC_SD_PROBE_HELPER("access_myth", "MythTV Library", SD_CAT_LAN)

vlc_module_begin()
    set_shortname( "MythTV" )
    set_description( N_("MythTV VLC Plugin") )
    set_capability( "access", 0 )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_ACCESS )
    add_integer( "myth-caching", 2 * DEFAULT_PTS_DELAY / 1000, NULL,
                 CACHING_TEXT, CACHING_LONGTEXT, true )
    add_shortcut( "myth" )
    add_shortcut( "mythlivetv" )
    set_callbacks( InOpen, InClose )

    add_submodule()
        set_shortname( "MythTV Library")
        set_description( N_("MythTV Library") )
        set_category( CAT_PLAYLIST )
        set_subcategory( SUBCAT_PLAYLIST_SD )

        add_string( "mythbackend-url", NULL, NULL,
                    SERVER_URL_TEXT, SERVER_URL_LONGTEXT, false )
            change_autosave()

        set_capability( "services_discovery", 0 )
        set_callbacks( SDOpen, SDClose )
		
		VLC_SD_PROBE_SUBMODULE

vlc_module_end()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static ssize_t Read( access_t *, uint8_t *, size_t );
static int Seek( access_t *, int64_t );
static int Control( access_t *, int, va_list );

static void *SDRun( void *data );

static void GetCutList( access_t *, access_sys_t *, char*, char* );

#define MAKEINT64(lo, hi) ( ((int64_t)hi) << 32 | ((int64_t)(uint32_t)lo) )


typedef struct _myth_sys_t
{
    int        myth_proto_version;
    char       file_transfer_id[10];

    bool       b_supports_sql_query;

    char       sz_remote_ip[NI_MAXNUMERICHOST];
    char       sz_local_ip[NI_MAXNUMERICHOST];
} myth_sys_t;

struct services_discovery_sys_t
{
    myth_sys_t myth;
    /* playlist node */
    input_thread_t **pp_input;
    int i_input;

    vlc_thread_t thread;
    vlc_mutex_t lock;
    vlc_cond_t  wait;

    char **ppsz_urls;
    int i_urls;

    bool b_update;
};

struct access_sys_t
{
    myth_sys_t myth;
    vlc_url_t  url;

    int        fd_cmd;
    int        fd_data;
    int        i_data_to_be_read;
    mtime_t    i_filesize_last_updated;
    char      *psz_basename;
	bool       b_eofing;

    int64_t    realpos;

    int           i_titles;
    input_title_t **titles;
};

static int myth_WriteCommand( vlc_object_t *p_access, int fd, char* psz_cmd );
static int myth_ReadCommand( vlc_object_t *p_access, int fd, int *pi_len, char **ppsz_answer );
static int myth_Send( vlc_object_t *p_access, int fd, int *pi_len, char **ppsz_answer, const char *psz_fmt, ... );
static char* myth_token( char *psz_params, int i_len, int i_index );
static int myth_count_tokens( char *psz_params, int i_len );
static int myth_Connect( vlc_object_t *p_access, myth_sys_t *p_sys, vlc_url_t* url, bool b_fd_data );



/*
static int mysql_query( vlc_object_t *p_access, access_sys_t *p_sys, int fd, 
                            const char *psz_fmt, ... )
{
    va_list      args;
    char         *psz_cmd;

    va_start( args, psz_fmt );
    if( vasprintf( &psz_cmd, psz_fmt, args ) == -1 )
        return VLC_EGENERIC;

    va_end( args );

    msg_Info( p_access, "MYSQL Connecting" );
    
    int fd = net_ConnectTCP( p_access, p_sys->url.psz_host,
                                             3306 );
    if( fd == -1 )
    {
        msg_Err( p_access, "connection failed" );
        intf_UserFatal( p_access, false, _("Network interaction failed"),
                        _("VLC could not connect with the given server.") );
        return -1;
    }
    msg_Info( p_access, "MYSQL Connected" );


    if( mysql_WritePacket( p_access, p_sys, fd, "MYTH_PROTO_VERSION %ld", protocol_version ) < 0 )
    {
        msg_Err( p_access, "failed to introduce ourselves" );
        net_Close( fd );
        return -1;
    }



    
    return VLC_SUCCESS;
}
*/

static int myth_WriteCommand( vlc_object_t *p_access, int fd, char* psz_cmd )
{
    int len = strlen( psz_cmd );

    char lenstr[9];
    snprintf(lenstr, sizeof(lenstr), "%d", len);
    memset(lenstr+strlen(lenstr), ' ', 8-strlen(lenstr));
    lenstr[8] = '\0';

    //msg_Info( p_access, "myth_WriteCommand:\"%s%s\"", lenstr, psz_cmd);

    if( net_Printf( VLC_OBJECT(p_access), fd, NULL, "%s%s",
                    lenstr, psz_cmd ) < 0 )
    {
        msg_Err( p_access, "failed to send command" );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

static int myth_ReadCommand( vlc_object_t *p_access, int fd, int *pi_len, char **ppsz_answer )
{
    /* read length */
    char lenstr[9];
    memset(lenstr, '\0', sizeof(lenstr));
    int i_Read = 0;
    int i_TotalRead = 0;
	char *psz_line = NULL;

    assert( fd != -1 );

    while ( i_TotalRead < 8 )
	{
        if ((i_Read = net_Read( p_access, fd, NULL, lenstr + i_TotalRead, 8 - i_TotalRead, false )) <= 0)
            goto exit_error;
        i_TotalRead += i_Read;
    }

    int len = atoi(lenstr);
    //msg_Info( p_access, "myth_ReadCommand-len:\"%d\"", len);

    psz_line = malloc(len+1);
	if ( !psz_line )
		return VLC_ENOMEM;
    psz_line[len] = '\0';

    i_TotalRead = 0;
    while ( i_TotalRead < len )
    {
        if ((i_Read = net_Read( p_access, fd, NULL, psz_line + i_TotalRead, len - i_TotalRead, false )) <= 0)
            goto exit_error;
        i_TotalRead += i_Read;
    }

    //msg_Info( p_access, "myth_ReadCommand:\"%s%s\"", lenstr, psz_line);

    /* post process the final string and add \0 to the end of each token sp []:[] becomes \0]:[] */
    char *cend = psz_line + len;
    for ( char *c = psz_line; c < cend; c++ )
	{
        if (*c == '['
            && c+1 < cend && c[1] == ']'
            && c+2 < cend && c[2] == ':'
            && c+3 < cend && c[3] == '['
            && c+4 < cend && c[4] == ']')
		{
            *c = '\0';
        }
    }

    if( pi_len )
    {
        *pi_len = len;
    }

    if( ppsz_answer )
    {
        *ppsz_answer = psz_line;
    }
    else
    {
        free( psz_line );
		psz_line = NULL;
    }

    return VLC_SUCCESS;
exit_error:
	if( psz_line )
	{
		free( psz_line );
		psz_line = NULL;
	}

    if( pi_len )
    {
        *pi_len = 0;
    }

    return VLC_EGENERIC;
}

static int myth_Send( vlc_object_t *p_access, int fd, int *pi_len, char **ppsz_answer, const char *psz_fmt, ... )
{
    va_list      args;
    char         *psz_cmd;

    va_start( args, psz_fmt );
    if( vasprintf( &psz_cmd, psz_fmt, args ) == -1 )
        return VLC_EGENERIC;
	
    va_end( args );

	if( myth_WriteCommand( p_access, fd, psz_cmd ) )
	{
		free( psz_cmd );
		return VLC_EGENERIC;
	}
	
	free( psz_cmd );

	if ( myth_ReadCommand( p_access, fd, pi_len, ppsz_answer ) )
	{
		return VLC_EGENERIC;
	}

	return VLC_SUCCESS;
}

static char* myth_token( char *psz_params, int i_len, int i_index )
{
    char *cend = psz_params + i_len;
    char *c = psz_params;
    for (; c < cend && i_index > 0; c++)
	{
        if (*c == '\0')
		{
            i_index--;
            c += 4;
        }
    }

    if (i_index != 0)
        return NULL;

    return c;
}

static int myth_count_tokens( char *psz_params, int i_len )
{
    int i_result = 1;
    char *cend = psz_params + i_len;
    char *c = psz_params;
    for (; c < cend; c++)
	{
        if (*c == '\0')
		{
            i_result++;
        }
    }

    return i_result;
}

static int myth_Connect( vlc_object_t *p_access, myth_sys_t *p_sys, vlc_url_t* url, bool b_fd_data )
{
    char *psz_params;
    int i_len;
	
	for ( int i = 0; i < 2; i++ )
	{
		msg_Dbg( p_access, "Connecting to %s:%d...", url->psz_host, url->i_port );
    
		int fd = net_ConnectTCP( p_access, url->psz_host, url->i_port );
		if( fd == -1 )
		{
			msg_Err( p_access, "Connection failed" );
			//intf_UserFatal( p_access, false, _("Network interaction failed"),
			//                _("VLC could not connect with the given server.") );
			return 0;
		}

		msg_Dbg( p_access, "Connected" );

		if( net_GetPeerAddress( fd, p_sys->sz_remote_ip, NULL ) || 
			net_GetSockAddress( fd, p_sys->sz_local_ip, NULL ) )
		{
			net_Close( fd );
			return 0;
		}

		if( myth_Send( p_access, fd, &i_len, &psz_params, "MYTH_PROTO_VERSION %d", p_sys->myth_proto_version ) )
		{
			msg_Err( p_access, "Failed to introduce ourselves." );
			net_Close( fd );
			return 0;
		}
    
		char *acceptreject = myth_token(psz_params, i_len, 0);

		if ( !strncmp(acceptreject, "ACCEPT", 6) )
		{
			p_sys->myth_proto_version = atoi( myth_token( psz_params, i_len, 1 ) );
			msg_Info( p_access, "MythBackend is Protocol Version %d", p_sys->myth_proto_version);

		}
		else
		{
			msg_Err( p_access, "MythBackend Protocol mismatch, server is %s, we are expecting %d", myth_token(psz_params, i_len, 1), p_sys->myth_proto_version );
			
			int i_server_ver = atoi( myth_token( psz_params, i_len, 1 ) );
			net_Close( fd );
			free( psz_params );

			if ( i_server_ver >= MYTH_MINIMUM_VER )
			{
				// let's try reconnecting with specified version.
				msg_Info( p_access, "Changing version to %d", i_server_ver );
				p_sys->myth_proto_version = i_server_ver;
				continue;
			}
			else
			{
				break;
			}

		}

		free( psz_params );

		if ( b_fd_data )
		{
			if ( myth_Send( p_access, fd, &i_len, &psz_params, "ANN FileTransfer VLC_%s[]:[]myth://%s:%d/%s[]:[]Default", p_sys->sz_local_ip, url->psz_host, url->i_port, url->psz_path ) )
			{
				return 0;
			}

			acceptreject = myth_token(psz_params, i_len, 0);
			if ( !strncmp( acceptreject, "OK", 2) )
			{
				((access_t*)p_access)->info.i_size = MAKEINT64( atoi( myth_token(psz_params, i_len, 3)), atoi( myth_token(psz_params, i_len, 2)));
				msg_Info( p_access, "Stream starting %"PRId64" B", ((access_t*)p_access)->info.i_size );
			}
			else
			{
				msg_Err( p_access, "Some error occured while trying to stream" );
				net_Close( fd );
				free( psz_params );
				return 0;
			}

			strncpy(p_sys->file_transfer_id, myth_token(psz_params, i_len, 1), sizeof(p_sys->file_transfer_id)-1);
			p_sys->file_transfer_id[sizeof(p_sys->file_transfer_id)-1] = '\0';
			free( psz_params );
		}
		else
		{
			if ( myth_Send( p_access, fd, &i_len, &psz_params, "ANN Playback VLC_%s 0", p_sys->sz_local_ip) )
			{
				msg_Err( p_access, "Some error occured while sending announce." );
				return 0;
			}

			acceptreject = myth_token(psz_params, i_len, 0);
			if ( strncmp( acceptreject, "OK", 2 ) )
			{
				msg_Err( p_access, "Reply to announce is NOT OK." );
				net_Close( fd );
				free( psz_params );
				return 0;
			}

			free( psz_params );
		}

		return fd;
	}

	return 0;
}



static int ConnectFD( vlc_object_t *p_access, access_sys_t *p_sys, bool b_fd_data )
{
    char *psz_params;
    int i_len;

    int fd = myth_Connect( p_access, &p_sys->myth, &p_sys->url, b_fd_data );

    if ( !fd )
	{
        return VLC_EGENERIC;
    }
    
    if ( b_fd_data )
	{
        p_sys->fd_data = fd;
    }
	else
	{
        p_sys->fd_cmd = fd;

        input_thread_t *p_input = access_GetParentInput( (access_t *) p_access );
        if( !p_input )
        {
            msg_Dbg( p_access, "Unable to find parent input thread. Access may not be from video." );
            //pl_Release( p_access );
            return VLC_SUCCESS;
        }

        if ( myth_Send( p_access, fd, &i_len, &psz_params, "QUERY_RECORDINGS Play" ) )
		{
			vlc_object_release( p_input );
			return VLC_EGENERIC;
		}
        
        /* Set meta data */
        int i_tokens = myth_count_tokens( psz_params, i_len );
        int i_rows = atoi( myth_token(psz_params, i_len, 0) );
        int i_fields = (i_tokens-1) / i_rows;
        for ( int i = 0; i < i_rows; i++ )
		{
            char* psz_url = myth_token( psz_params, i_len, 1 + i * i_fields + 8 );
            char *psz_ctitle = myth_token( psz_params, i_len, 1 + i * i_fields + 0 );
            char *psz_csubtitle = myth_token( psz_params, i_len, 1 + i * i_fields + 1 );

            input_item_t *p_item = NULL;

            if (strstr(psz_url, p_sys->url.psz_path)) {
                /* found our program in all the recordings */
                //char *psz_startdate;
                //strftime(buffer, sizeof(buffer)-1, "%d-%m-%Y", atoi ( myth_token( psz_params, i_len, 1 + i * i_fields + 11 ) ));
#if defined( HAVE_GMTIME_R )
                struct tm tmres;
                char   buffer[256];

                time_t i_date = 0;
                memset( buffer, 0, 256 );
                if( gmtime_r( &i_date, &tmres ) &&
                    asctime_r( &tmres, buffer ) )
                {
                    buffer[strlen( buffer)-1]= '\0';
                    psz_startdate = strdup( buffer );
                }
#endif
                int64_t i_filesize = MAKEINT64( atoi( myth_token( psz_params, i_len, 1 + i * i_fields + 10 ) ), atoi( myth_token( psz_params, i_len, 1 + i * i_fields + 9 )) );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Myth Protocol"), "%d", p_sys->myth.myth_proto_version );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Title"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 0 ) );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Sub title"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 1 ) );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Description"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 2 ) );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Category"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 3 ) );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Channel"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 7 )  );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Show start"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 11 ) );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("Show end"), "%s", myth_token( psz_params, i_len, 1 + i * i_fields + 12 )  );
                input_Control( p_input, INPUT_ADD_INFO, _("MythTV"), _("File size"), "%"PRId64" MB", i_filesize / 1000000  );
                
                p_sys->psz_basename = strdup( myth_token( psz_params, i_len, 1 + i * i_fields + 8 ) );

                p_item = input_GetItem( p_input );
                input_item_SetDate( p_item, "test" );

                asprintf( &psz_ctitle, "%s: %s", psz_ctitle, psz_csubtitle );
                input_Control( p_input, INPUT_SET_NAME, psz_ctitle );

                input_item_SetDescription( p_item, strdup(myth_token( psz_params, i_len, 1 + i * i_fields + 2 )) );
                
                GetCutList( (access_t *) p_access, p_sys, myth_token( psz_params, i_len, 1 + i * i_fields + 4 ), myth_token( psz_params, i_len, 1 + i * i_fields + 26 ) );
                
                break;
            }
        }

        free( psz_params );
        vlc_object_release( p_input );
    }

    return VLC_SUCCESS;
}





static int parseURL( vlc_url_t *url, const char *path )
{
    if( path == NULL )
        return VLC_EGENERIC;

    /* *** Parse URL and get server addr/port and path *** */
    while( *path == '/' )
        path++;

    vlc_UrlParse( url, path, 0 );

    if( url->psz_host == NULL || *url->psz_host == '\0' )
        return VLC_EGENERIC;

    if( url->i_port <= 0 )
        url->i_port = IPPORT_MYTH; /* default port */

	if( url->psz_path != NULL && url->psz_path[0] == '/' )
		url->psz_path++;

    return VLC_SUCCESS;
}


/****************************************************************************
 * Open: connect to mythbackend
 ****************************************************************************/
static int InOpen( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys;

    /* Init p_access */
    STANDARD_READ_ACCESS_INIT
    p_sys->fd_cmd = -1;
    p_sys->fd_data = -1;
    p_sys->i_data_to_be_read = 0;
    p_sys->i_filesize_last_updated = 0;
    p_sys->b_eofing = false;
    p_sys->myth.myth_proto_version = MYTH_MINIMUM_VER;

    p_sys->i_titles = 0;

    if( parseURL( &p_sys->url, p_access->psz_path ) )
        goto exit_error;

    if( ConnectFD( p_this, p_sys, false ) )
        goto exit_error;

    // stream
    if( ConnectFD( p_this, p_sys, true ) )
        goto exit_error;

    
    /* Update default_pts to a suitable value for ftp access */
    var_Create( p_access, "myth-caching", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
    

    return VLC_SUCCESS;

exit_error:
	InClose( p_this );

    return VLC_EGENERIC;
}


/*****************************************************************************
 * Close: free now unused data structures
 *****************************************************************************/
static void Close( vlc_object_t *p_access, access_sys_t *p_sys )
{
    msg_Info( p_access, "stopping stream" );
	
	if ( p_sys->fd_data != -1 )
		net_Close( p_sys->fd_data );

    if ( p_sys->fd_cmd != -1 )
		net_Close( p_sys->fd_cmd );

    /* free memory */
    vlc_UrlClean( &p_sys->url );
    free( p_sys );
}

static void InClose( vlc_object_t *p_this )
{
    Close( p_this, ((access_t *)p_this)->p_sys);
}


/*****************************************************************************
 * Seek: try to go at the right place
 *****************************************************************************/
static int _Seek( vlc_object_t *p_access, access_sys_t *p_sys, int64_t i_pos )
{
    if( i_pos < 0 )
        return VLC_EGENERIC;

    msg_Info( p_access, "seeking to %"PRId64" / %"PRId64, i_pos, ((access_t *)p_access)->info.i_size );

    char *psz_params;
    int i_plen;
    if ( myth_Send( p_access, p_sys->fd_cmd, &i_plen, &psz_params, "QUERY_FILETRANSFER %s[]:[]SEEK[]:[]%d[]:[]%d[]:[]0[]:[]0[]:[]0", p_sys->myth.file_transfer_id, (int32_t)(i_pos >> 32), (int32_t)(i_pos)) )
	{
		return VLC_EGENERIC;
	}

	free( psz_params );
    return VLC_SUCCESS;
}

static int Seek( access_t *p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;
    int val = _Seek( (vlc_object_t *)p_access, p_access->p_sys, i_pos );
    if( val )
        return val;

    p_sys->b_eofing = false;
    p_access->info.b_eof = false;
    p_access->info.i_pos = i_pos;

    return VLC_SUCCESS;
}


/*****************************************************************************
 * Read:
 *****************************************************************************/
static ssize_t Read( access_t *p_access, uint8_t *p_buffer, size_t i_len )
{
    int i_read;
    int i_will_receive = 0;
    int i_requestlen = 65536;

	int i_plen;
    char *psz_params;

    access_sys_t *p_sys = p_access->p_sys;

    assert( p_sys->fd_data != -1 );
    assert( p_sys->fd_cmd != -1 );

    if( p_access->info.b_eof )
        return 0;

    msg_Dbg( p_access, "Read %d", i_len );

	/* pipeline reading, request new data when our buffer is half finished */
	if ( !p_sys->b_eofing && p_sys->i_data_to_be_read <= i_requestlen / 2 )
	{
		msg_Dbg( p_access, "REQUEST_BLOCK %d", i_requestlen );
		if( myth_Send( VLC_OBJECT( p_access ), p_sys->fd_cmd, &i_plen, &psz_params, "QUERY_FILETRANSFER %s[]:[]REQUEST_BLOCK[]:[]%d", p_sys->myth.file_transfer_id, i_requestlen ) )
		{
			return VLC_EGENERIC;
		}
		
		i_will_receive = atoi( myth_token(psz_params, i_plen, 0) );

		msg_Dbg( p_access, "i_will_receive %d", i_will_receive );
		if ( i_will_receive <= 0 )
		{
			msg_Dbg( p_access, "SET EOFing" );
			p_sys->b_eofing = true;
		}
		else
		{
			p_sys->i_data_to_be_read += i_will_receive;
		}

		free( psz_params );
	}

	/* check if last block now read */
	if ( p_sys->b_eofing && p_sys->i_data_to_be_read == 0 )
	{
		msg_Dbg( p_access, "SET EOF from eofing" );
		p_access->info.b_eof = true;
		return 0;
	}

    if ( p_sys->psz_basename && mdate() - p_sys->i_filesize_last_updated > 2000000 )
	{
        // update the file size every 2 seconds
        p_sys->i_filesize_last_updated = mdate();

        if ( myth_Send( VLC_OBJECT( p_access ), p_sys->fd_cmd, &i_plen, &psz_params, "QUERY_RECORDING BASENAME %s", p_sys->psz_basename ) )
		{
			return VLC_EGENERIC;
		}

        int64_t i_newsize = MAKEINT64( atoi( myth_token( psz_params, i_plen, 1 + 10 ) ), atoi( myth_token( psz_params, i_plen, 1 + 9 )) );

        if ( p_access->info.i_size != i_newsize )
		{
            p_access->info.i_size = i_newsize;
            p_access->info.i_update |= INPUT_UPDATE_SIZE;
            msg_Dbg( p_access, "new file size %"PRId64, i_newsize );
        }

		free( psz_params );
    }

    i_read = net_Read( p_access, p_sys->fd_data, NULL, p_buffer, i_len, false );
    
    msg_Dbg( p_access, "i_read %d", i_read );

    if( i_read <= 0 )
	{
        msg_Dbg( p_access, "SET EOF because i_read nothing" );
        p_access->info.b_eof = true;
    }
    else
	{
        p_access->info.i_pos += i_read;
        p_sys->i_data_to_be_read -= i_read;
        msg_Dbg( p_access, "i_data_to_be_read %d", p_sys->i_data_to_be_read );

        /* update seekpoint to reflect the current position */
        if ( p_sys->i_titles > 0 )
		{
            int i;

            input_title_t *t = p_sys->titles[p_access->info.i_title];
            for( i = 0; i < t->i_seekpoint; i++ )
            {
                if (p_access->info.i_pos <= t->seekpoint[i]->i_byte_offset)
                    break;
            }

            i = (i == 0) ? 0 : i - 1;

            p_access->info.i_seekpoint = i;
            p_access->info.i_update |= INPUT_UPDATE_SEEKPOINT;
        }
    }
        

    return i_read;
}


/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    bool   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;
    vlc_value_t  val;

    int         i_skp;
    size_t      i_idx;
    access_sys_t *p_sys = p_access->p_sys;

    switch( i_query )
    {
        /* */
        case ACCESS_CAN_SEEK:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = true;
            break;
        case ACCESS_CAN_FASTSEEK:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = false;
            break;
        case ACCESS_CAN_PAUSE:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = true;    /* FIXME */
            break;
        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (bool*)va_arg( args, bool* );
            *pb_bool = true;    /* FIXME */
            break;

        /* 
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 0;
            break;
*/
        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            var_Get( p_access, "myth-caching", &val );
            *pi_64 = (int64_t)var_GetInteger( p_access, "myth-caching" ) * INT64_C(1000);
            break;

        /* */
        case ACCESS_SET_PAUSE_STATE:
            pb_bool = (bool*)va_arg( args, bool* );
            if ( !pb_bool )
              return Seek( p_access, p_access->info.i_pos );
            break;

        case ACCESS_SET_PRIVATE_ID_STATE:
        case ACCESS_GET_CONTENT_TYPE:
        case ACCESS_GET_META:
            return VLC_EGENERIC;
        case ACCESS_GET_TITLE_INFO:
            {
                access_sys_t *p_sys = p_access->p_sys;

                input_title_t ***ppp_title;
                int          *pi_int;
                int i;

                ppp_title = (input_title_t***)va_arg( args, input_title_t*** );
                pi_int    = (int*)va_arg( args, int* );
                *((int*)va_arg( args, int* )) = 0; /* Title offset */
                *((int*)va_arg( args, int* )) = 1; /* Chapter offset */

                //* Duplicate title infos 
                *pi_int = p_sys->i_titles;
                *ppp_title = malloc( sizeof( input_title_t ** ) * p_sys->i_titles );
				if ( !*ppp_title )
					return VLC_ENOMEM;

                for( i = 0; i < p_sys->i_titles; i++ )
                {
                    (*ppp_title)[i] = vlc_input_title_Duplicate( p_sys->titles[i] );
                }

                return VLC_SUCCESS;
            }
        case ACCESS_SET_TITLE:
            /* TODO handle editions as titles */
            i_idx = (int)va_arg( args, int );
            msg_Err( p_access, "ACCESS_SET_TITLE %d", i_idx);
            //if( i_idx < p_sys->used_segments.size() )
            //{
            //    p_sys->JumpTo( *p_sys->used_segments[i_idx], NULL );
            //    return VLC_SUCCESS;
            //}
            return VLC_EGENERIC;

        case ACCESS_SET_SEEKPOINT:
            i_skp = (int)va_arg( args, int );
            msg_Err( p_access, "ACCESS_SET_SEEKPOINT %d", i_skp);


            // TODO change the way it works with the << & >> buttons on the UI (+1/-1 instead of a number)
            if( p_sys->i_titles && i_skp < p_sys->titles[0]->i_seekpoint)
            {
                //Seek( p_access, (int64_t)p_sys->titles[0]->seekpoint[i_skp]->i_byte_offset);

                /* do the seeking */
                input_thread_t *p_input = access_GetParentInput( p_access );
                input_Control( p_input, INPUT_SET_POSITION, (double)p_sys->titles[0]->seekpoint[i_skp]->i_byte_offset / p_access->info.i_size );
                vlc_object_release( p_input );

                //p_access->info.i_update = 0;
                //p_access->info.i_size = 0;
                //p_sys->realpos = p_access->info.i_pos;
                //p_access->info.i_pos = 0;
                //p_access->info.b_eof = false;
                //p_access->info.i_title = 0;
                //p_access->info.i_seekpoint = 0;

                //p_access->info.i_update |= INPUT_UPDATE_SEEKPOINT;
                //p_access->info.i_seekpoint = i_skp;
                return VLC_SUCCESS;
            }


            return VLC_EGENERIC;

        default:
            msg_Warn( p_access, "unimplemented query in control: %d", i_query);
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}









static void GetCutList( access_t *p_access, access_sys_t *p_sys, char* psz_channel, char* psz_starttime )
{
    input_title_t *t;
    seekpoint_t *s;

    int i_len;
    char *psz_params;

    
    /* Menu */
    t = vlc_input_title_New();
    t->b_menu = true;
    t->psz_name = strdup( "Cuts" );

    s = vlc_seekpoint_New();
    s->i_byte_offset = 0;
    s->psz_name = strdup( "Start" );
    TAB_APPEND( t->i_seekpoint, t->seekpoint, s );
    
    if ( myth_Send( VLC_OBJECT( p_access ), p_sys->fd_cmd, &i_len, &psz_params, "QUERY_COMMBREAK %s %s", psz_channel, psz_starttime ) )
	{
		return;
	}

    //msg_Info( p_access, "QUERY_COMMBREAK %s %s", psz_channel, psz_starttime );
    int i_tokens = myth_count_tokens( psz_params, i_len );
    int i_rows = atoi( myth_token(psz_params, i_len, 0) );
    int i_fields = (i_tokens-1) / i_rows;
    for ( int i = 0; i < i_rows; i++ ) {
        char *psz_results;
        int i_results;
        int64_t i_frame = atoi( myth_token( psz_params, i_len, 1 + i * i_fields + 2 ) );
        int64_t i_byte = 0;

        /* get byte from frame */
        if ( myth_Send( VLC_OBJECT( p_access ), p_sys->fd_cmd, &i_results, &psz_results, "SQL_QUERY[]:[]SELECT offset FROM recordedseek WHERE chanid=%s AND UNIX_TIMESTAMP(starttime)=%s AND mark <= %"PRId64" ORDER BY mark DESC LIMIT 1", psz_channel, psz_starttime, i_frame ) )
		{
			return;
		}
        
        int i_rrows = atoi( myth_token( psz_results, i_results, 0) );
        if (i_rrows > 0) {
            i_byte = atoll( myth_token( psz_results, i_results, 1 ) );
        }

        /* Add the seek points */
        s = vlc_seekpoint_New();
        s->i_byte_offset = i_byte;
        if ( !strcmp( myth_token( psz_params, i_len, 1 + i * i_fields + 0 ), "4") ) {
            s->psz_name = strdup( "Commercial" );
        } else {
            s->psz_name = strdup( "Show" );
        }
        TAB_APPEND( t->i_seekpoint, t->seekpoint, s );

		free( psz_results );
        //msg_Info( p_access, "CUT frame %"PRId64, i_byte );
    }

    free( psz_params );

    /*
    s = vlc_seekpoint_New();
    s->i_byte_offset = 100000000;
    s->psz_name = strdup( "Commercial" );
    TAB_APPEND( t->i_seekpoint, t->seekpoint, s );

    s = vlc_seekpoint_New();
    s->i_byte_offset = 200000000;
    s->psz_name = strdup( "Segment" );
    TAB_APPEND( t->i_seekpoint, t->seekpoint, s ); */
    
    TAB_APPEND( p_sys->i_titles, p_sys->titles, t );

    
}







static int UrlsChange( vlc_object_t *p_this, char const *psz_var,
                       vlc_value_t oldval, vlc_value_t newval,
                       void *p_data )
{
    VLC_UNUSED(p_this); VLC_UNUSED(psz_var); VLC_UNUSED(oldval);
    VLC_UNUSED(newval);
    services_discovery_sys_t *p_sys  = (services_discovery_sys_t *)p_data;

    vlc_mutex_lock( &p_sys->lock );
    p_sys->b_update = true;
    
    vlc_cond_signal( &p_sys->wait );
    vlc_mutex_unlock( &p_sys->lock );

    return VLC_SUCCESS;
}


/*****************************************************************************
 * Open: initialize and create stuff
 *****************************************************************************/
static int SDOpen( vlc_object_t *p_this )
{
    services_discovery_t *p_sd = ( services_discovery_t* )p_this;
    services_discovery_sys_t *p_sys  = malloc(
                                    sizeof( services_discovery_sys_t ) );
    if( !p_sys )
        return VLC_ENOMEM;

    p_sys->i_urls = 0;
    p_sys->ppsz_urls = NULL;
    p_sys->i_input = 0;
    p_sys->pp_input = NULL;
    vlc_mutex_init( &p_sys->lock );
    vlc_cond_init( &p_sys->wait );
    p_sys->b_update = true;
	
    p_sys->myth.myth_proto_version = MYTH_MINIMUM_VER;
    p_sd->p_sys  = p_sys;

    /* Give us a name */
    //services_discovery_SetLocalizedName( p_sd, _("MythTV") );

    var_Create( p_sd, "mythbackend-url", VLC_VAR_STRING | VLC_VAR_DOINHERIT );
    var_AddCallback( p_sd, "mythbackend-url", UrlsChange, p_sys );
	
    if (vlc_clone (&p_sys->thread, SDRun, p_sd, VLC_THREAD_PRIORITY_LOW))
    {
        var_DelCallback( p_sd, "mythbackend-url", UrlsChange, p_sys );
        vlc_cond_destroy( &p_sys->wait );
        vlc_mutex_destroy( &p_sys->lock );
        free (p_sys);
        return VLC_EGENERIC;
    }

	msg_Dbg( p_sd, "SD Open" );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close:
 *****************************************************************************/
static void SDClose( vlc_object_t *p_this )
{
    services_discovery_t *p_sd = ( services_discovery_t* )p_this;
    services_discovery_sys_t *p_sys  = p_sd->p_sys;
    int i;
	
    vlc_cancel (p_sys->thread);
    vlc_join (p_sys->thread, NULL);

    var_DelCallback( p_sd, "mythbackend-url", UrlsChange, p_sys );
    vlc_cond_destroy( &p_sys->wait );
    vlc_mutex_destroy( &p_sys->lock );

    for( i = 0; i < p_sys->i_input; i++ )
    {
        if( p_sd->p_sys->pp_input[i] )
        {
            //input_StopThread( p_sd->p_sys->pp_input[i] );
            vlc_object_release( p_sd->p_sys->pp_input[i] );
            p_sd->p_sys->pp_input[i] = NULL;
        }
    }
    free( p_sd->p_sys->pp_input );
    for( i = 0; i < p_sys->i_urls; i++ ) free( p_sys->ppsz_urls[i] );
    free( p_sys->ppsz_urls );
    free( p_sys );

	msg_Dbg( p_sd, "SD Close" );
}



/*****************************************************************************
 * Run
 *****************************************************************************/
static void *SDRun( void *data )
{
    services_discovery_t *p_sd = data;
    services_discovery_sys_t *p_sys  = p_sd->p_sys;
    char* psz_backendurl = var_GetNonEmptyString( p_sd, "mythbackend-url" );
	
	msg_Dbg( p_sd, "SD Run" );

    if ( !psz_backendurl )
	{
        input_item_t *p_item = input_item_NewWithType( VLC_OBJECT( p_sd ),
            strdup("mythnotavailable://localhost/"), strdup("Please set your Mythbackend URL in the preferences (Show All, under Input > Access Modules > MythTV) and restart VLC."), 0, NULL, 0, -1, ITEM_TYPE_FILE );
        services_discovery_AddItem( p_sd, p_item, NULL );
        vlc_gc_decref( p_item );

        return NULL;
    }

    vlc_url_t url;
    parseURL( &url, psz_backendurl );

    char *psz_params;
    int i_len;
    vlc_object_t *p_access = p_sd;

    int fd = myth_Connect( p_access, &p_sys->myth, &url, false );

    if ( !fd )
	{
        return NULL;
    }

    if ( myth_Send( VLC_OBJECT( p_sd ), fd, &i_len, &psz_params, "QUERY_RECORDINGS Play" ) )
	{
		return NULL;
	}

    bool addToPlayList = true;

    int i_tokens = myth_count_tokens( psz_params, i_len );
    int i_rows = atoi( myth_token(psz_params, i_len, 0) );
    int i_fields = (i_tokens-1) / i_rows;
    for ( int i = 0; i < i_rows; i++ )
	{
        char* psz_url = myth_token( psz_params, i_len, 1 + i * i_fields + 8 );
        char *psz_ctitle = myth_token( psz_params, i_len, 1 + i * i_fields + 0 );
        char *psz_csubtitle = myth_token( psz_params, i_len, 1 + i * i_fields + 1 );

		/* concatenate .png to the URL */
		int i_url = strlen(psz_url);
		char* psz_arturl = malloc( i_url + 5 );
		strncpy( psz_arturl, psz_url, i_url );
		strncpy( psz_arturl + i_url, ".png", 5 );

        input_item_t *p_item = NULL;

        char *psz_name;
        asprintf( &psz_name, "%s: %s", psz_ctitle, psz_csubtitle );
        p_item = input_item_NewWithType( VLC_OBJECT( p_sd ),
            strdup(psz_url), psz_name, 0, NULL, 0,
                                         -1, ITEM_TYPE_FILE );
        
        input_item_SetDescription( p_item, strdup(myth_token( psz_params, i_len, 1 + i * i_fields + 2 )) );
        input_item_SetGenre( p_item, strdup(myth_token( psz_params, i_len, 1 + i * i_fields + 3 )) );
        input_item_SetAlbum( p_item, strdup(psz_ctitle) );
		input_item_SetArtURL( p_item, psz_arturl );
        input_item_SetDuration( p_item, (atoll(myth_token( psz_params, i_len, 1 + i * i_fields + 27 )) - atoll(myth_token( psz_params, i_len, 1 + i * i_fields + 26 ))) * 1000000 );

        services_discovery_AddItem( p_sd, p_item, NULL );
        vlc_gc_decref( p_item );
    }

    free( psz_params );

    net_Close( fd );

}

