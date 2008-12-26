/*****************************************************************************
 * myth.c: Myth Protocol input module for VLC 0.9.8
 *****************************************************************************
 * Copyright (C) 2001-2006 the VideoLAN team
 * Copyright (C) 2008 Loune Lam
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
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_input.h>

#include <assert.h>

#include <vlc_access.h>
#include <vlc_interface.h>

#include <vlc_network.h>
#include "vlc_url.h"

#include <vlc_demux.h>

#define IPPORT_MYTH 6543u


/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int   InOpen ( vlc_object_t * );
static void  InClose( vlc_object_t * );
static int  DemuxOpen ( vlc_object_t * );

#define CACHING_TEXT N_("Caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Caching value for MythTV streams. This " \
    "value should be set in milliseconds." )
#define USER_TEXT N_("FTP user name")
#define USER_LONGTEXT N_("User name that will " \
    "be used for the connection.")
#define PASS_TEXT N_("FTP password")
#define PASS_LONGTEXT N_("Password that will be " \
    "used for the connection.")
#define ACCOUNT_TEXT N_("FTP account")
#define ACCOUNT_LONGTEXT N_("Account that will be " \
    "used for the connection.")

vlc_module_begin();
    set_shortname( "MYTH" );
    set_description( N_("MYTH input") );
    set_capability( "access", 0 );
    set_category( CAT_INPUT );
    set_subcategory( SUBCAT_INPUT_ACCESS );
    add_integer( "myth-caching", 2 * 2 * DEFAULT_PTS_DELAY / 1000, NULL,
                 CACHING_TEXT, CACHING_LONGTEXT, true );
    add_shortcut( "myth" );
    add_shortcut( "mythlivetv" );
    set_callbacks( InOpen, InClose );

vlc_module_end();

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static ssize_t Read( access_t *, uint8_t *, size_t );
static int Seek( access_t *, int64_t );
static int Control( access_t *, int, va_list );

static void GetTitles( access_t * );

struct access_sys_t
{
    vlc_url_t  url;

    int        fd_cmd;
    int        fd_data;
	int        data_to_be_read;

	int        myth_protocol_version;
	
	char       file_transfer_id[10];

    char       sz_epsv_ip[NI_MAXNUMERICHOST];
    char       sz_our_ip[NI_MAXNUMERICHOST];
    bool       out;

	
    int           i_titles;
    input_title_t **titles;
};


#define GET_OUT_SYS( p_this ) \
    ((access_sys_t *)(((sout_access_out_t *)(p_this))->p_sys))

#define MAKEINT64(lo, hi) ((((int64_t)hi) << 32) | ((int64_t)lo))


static int myth_WriteCommand( vlc_object_t *p_access, access_sys_t *p_sys, int fd, 
                            const char *psz_fmt, ... )
{
    va_list      args;
    char         *psz_cmd;

    va_start( args, psz_fmt );
    if( vasprintf( &psz_cmd, psz_fmt, args ) == -1 )
        return VLC_EGENERIC;

    va_end( args );
	
	int len = strlen(psz_cmd);

	char lenstr[9];
	snprintf(lenstr, sizeof(lenstr), "%d", len);
	memset(lenstr+strlen(lenstr), ' ', 8-strlen(lenstr));
	lenstr[8] = '\0';

    msg_Info( p_access, "myth_WriteCommand:\"%s%s\"", lenstr, psz_cmd);

    if( net_Printf( VLC_OBJECT(p_access), fd, NULL, "%s%s",
                    lenstr, psz_cmd ) < 0 )
    {
        msg_Err( p_access, "failed to send command" );
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}
static int myth_ReadCommand( vlc_object_t *p_access, access_sys_t *p_sys, int fd, 
                            int *pi_len, char **ppsz_answer )
{
	/* read length */
	char lenstr[9];
	memset(lenstr, '\0', sizeof(lenstr));
	int i_Read = 0;
	while (i_Read != 8)
		i_Read = net_Read( p_access, fd, NULL, lenstr + i_Read, 8, false ); // TODO this looks wrong? should be += and len - 8 ???
	int len = atoi(lenstr);
    //msg_Info( p_access, "myth_ReadCommand-len:\"%d\"", len);

    char         *psz_line;
	psz_line = malloc(len+1);
	psz_line[len] = '\0';

	i_Read = 0;
	while (i_Read != len)
		i_Read = net_Read( p_access, fd, NULL, psz_line + i_Read, len, false ); // TODO this looks wrong? should be += and len - i_Read ???

    msg_Info( p_access, "myth_ReadCommand:\"%s%s\"", lenstr, psz_line);

	/* post process the final string and add \0 to the end of each token sp []:[] becomes \0]:[] */
	char *cend = psz_line + len;
	for (char *c = psz_line; c < cend; c++) {
		if (*c == '['
			&& c+1 < cend && c[1] == ']'
			&& c+2 < cend && c[2] == ':'
			&& c+3 < cend && c[3] == '['
			&& c+4 < cend && c[4] == ']') {
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
    }

    return VLC_SUCCESS;
}

static char* myth_token( char *params, int len, int num ) {
	
	char *cend = params + len;
	char *c = params;
	for (; c < cend && num > 0; c++) {
		if (*c == '\0') {
			num--;
			c += 4;
		}
	}
	if (num != 0)
		return NULL;
	return c;

}


static int ConnectFD( vlc_object_t *p_access, access_sys_t *p_sys, bool b_fd_data )
{
	int protocol_version = 40;

    msg_Info( p_access, "Connecting" );
	
    int fd = net_ConnectTCP( p_access, p_sys->url.psz_host,
                                             p_sys->url.i_port );
    if( fd == -1 )
    {
        msg_Err( p_access, "connection failed" );
        intf_UserFatal( p_access, false, _("Network interaction failed"),
                        _("VLC could not connect with the given server.") );
        return -1;
    }
    msg_Info( p_access, "Connected" );


    if( myth_WriteCommand( p_access, p_sys, fd, "MYTH_PROTO_VERSION %ld", protocol_version ) < 0 )
    {
        msg_Err( p_access, "failed to introduce ourselves" );
        net_Close( fd );
        return -1;
    }
	
	char *psz_params;
	int i_len;

    myth_ReadCommand( p_access, p_sys, fd, &i_len, &psz_params );
    
	msg_Info( p_access, "myth_0token:\"%s\"", myth_token(psz_params, i_len, 0));
	msg_Info( p_access, "myth_1token:\"%s\"", myth_token(psz_params, i_len, 1));
	msg_Info( p_access, "myth_2token:\"%s\"", myth_token(psz_params, i_len, 2));

	char *acceptreject = myth_token(psz_params, i_len, 0);

	if (!strncmp(acceptreject, "ACCEPT", 6)) {
		msg_Info( p_access, "MythBackend is Protocol Version %s", myth_token(psz_params, i_len, 1) );
	} else {
        msg_Err( p_access, "MythBackend Protocol mismatch, server is %s, we are %d", myth_token(psz_params, i_len, 1), protocol_version );
        net_Close( fd );
        return -1;
	}

    if( net_GetPeerAddress( fd, p_sys->sz_epsv_ip, NULL ) || 
		net_GetSockAddress( fd, p_sys->sz_our_ip, NULL ) )
    {
        net_Close( fd );
        return -1;
    }
    
	if (b_fd_data) {
		myth_WriteCommand( p_access, p_sys, fd, "ANN FileTransfer %s[]:[]myth://%s:%d/%s", p_sys->sz_our_ip, p_sys->url.psz_host, p_sys->url.i_port, p_sys->url.psz_path );
		p_sys->fd_data = fd;

		myth_ReadCommand( p_access, p_sys, fd, &i_len, &psz_params );
		acceptreject = myth_token(psz_params, i_len, 0);
		if (!strncmp(acceptreject, "OK", 2)) {
			msg_Info( p_access, "Stream starting" );
			((access_t*)p_access)->info.i_size = MAKEINT64(atoi( myth_token(psz_params, i_len, 3)), atoi( myth_token(psz_params, i_len, 2)));
		} else {
			msg_Err( p_access, "Some error occured while trying to stream" );
			net_Close( fd );
			return -1;
		}
		strncpy(p_sys->file_transfer_id, myth_token(psz_params, i_len, 1), sizeof(p_sys->file_transfer_id)-1);
		p_sys->file_transfer_id[sizeof(p_sys->file_transfer_id)-1] = '\0';
	} else {
		myth_WriteCommand( p_access, p_sys, fd, "ANN Playback %s 1", p_sys->sz_our_ip);
		p_sys->fd_cmd = fd;
		myth_ReadCommand( p_access, p_sys, fd, &i_len, &psz_params );
		acceptreject = myth_token(psz_params, i_len, 0);
		if (strncmp(acceptreject, "OK", 2)) {
			msg_Err( p_access, "Some error occured while announcing" );
			net_Close( fd );
			return -1;
		}
	}
	
	free(psz_params);

    return 0;
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

    /* FTP URLs are relative to user's default directory (RFC1738)
    For absolute path use ftp://foo.bar//usr/local/etc/filename */

    if( url->psz_path && *url->psz_path == '/' )
        url->psz_path++;

    return VLC_SUCCESS;
}


/****************************************************************************
 * Open: connect to ftp server and ask for file
 ****************************************************************************/
static int InOpen( vlc_object_t *p_this )
{
    access_t     *p_access = (access_t*)p_this;
    access_sys_t *p_sys;
    char         *psz_arg;

    /* Init p_access */
    STANDARD_READ_ACCESS_INIT
    p_sys->fd_data = -1;
	p_sys->data_to_be_read = 0;
    p_sys->out = false;

    if( parseURL( &p_sys->url, p_access->psz_path ) )
        goto exit_error;

    if( ConnectFD( p_this, p_sys, false ) )
        goto exit_error;

	// stream
    if( ConnectFD( p_this, p_sys, true ) )
        goto exit_error;

	/*
    /* get size 
    if( ftp_SendCommand( p_this, p_sys, "SIZE %s", p_sys->url.psz_path ? : "" ) < 0 ||
        ftp_ReadCommand( p_this, p_sys, NULL, &psz_arg ) != 2 )
    {
        msg_Err( p_access, "cannot get file size" );
        net_Close( p_sys->fd_cmd );
        goto exit_error;
    }
    p_access->info.i_size = atoll( &psz_arg[4] );
    free( psz_arg );
    msg_Dbg( p_access, "file size: %"PRId64, p_access->info.i_size );

    /* Start the 'stream' 
    if( ftp_StartStream( p_this, p_sys, 0 ) < 0 )
    {
        msg_Err( p_access, "cannot retrieve file" );
        net_Close( p_sys->fd_cmd );
        goto exit_error;
    }
*/
    /* Update default_pts to a suitable value for ftp access */
    var_Create( p_access, "myth-caching", VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
	
	//msg_Err( p_access, "PREV DEMUX: %s", p_access->psz_demux);
    

    return VLC_SUCCESS;

exit_error:
    vlc_UrlClean( &p_sys->url );
    free( p_sys );
    return VLC_EGENERIC;
}


/*****************************************************************************
 * Close: free unused data structures
 *****************************************************************************/
static void Close( vlc_object_t *p_access, access_sys_t *p_sys )
{
    msg_Info( p_access, "stopping stream" );

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

    msg_Info( p_access, "seeking to %"PRId64, i_pos );

	char *psz_params;
	int i_plen;
	myth_WriteCommand( p_access, p_sys, p_sys->fd_cmd, "QUERY_FILETRANSFER %s[]:[]SEEK[]:[]%d[]:[]%d[]:[]0[]:[]0[]:[]0", p_sys->file_transfer_id, (int32_t)(i_pos >> 32), (int32_t)i_pos);
	myth_ReadCommand( p_access, p_sys, p_sys->fd_cmd, &i_plen, &psz_params );

    return VLC_SUCCESS;
}

static int Seek( access_t *p_access, int64_t i_pos )
{
    access_sys_t *p_sys = p_access->p_sys;
    int val = _Seek( (vlc_object_t *)p_access, p_access->p_sys, i_pos );
    if( val )
        return val;

    p_access->info.b_eof = false;
    p_access->info.i_pos = i_pos;

	//input_Control( p_access->p_input, INPUT_SET_NAME, strdup("ABC") );

    return VLC_SUCCESS;
}


/*****************************************************************************
 * Read:
 *****************************************************************************/
static ssize_t Read( access_t *p_access, uint8_t *p_buffer, size_t i_len )
{
    msg_Info( p_access, "reading..." );
    access_sys_t *p_sys = p_access->p_sys;
    int i_read;

    assert( p_sys->fd_data != -1 );
    assert( p_sys->fd_cmd != -1 );
    assert( !p_sys->out );

    if( p_access->info.b_eof )
        return 0;

	char *psz_params;
	int i_plen;

    //msg_Info( p_access, "request %ld", i_len );
	int i_dlen = 0;

	//ssize_t i_totalread = 0;

	//while (i_len2 > 0) {
		myth_WriteCommand( p_access, p_sys, p_sys->fd_cmd, "QUERY_FILETRANSFER %s[]:[]REQUEST_BLOCK[]:[]%ld", p_sys->file_transfer_id, 32767 );
		//myth_ReadCommand( p_access, p_sys, p_sys->fd_cmd, &i_plen, &psz_params );

		//i_dlen = atoi( myth_token(psz_params, i_plen, 0) );
		i_dlen = 32767;

		if (i_dlen == -1)
			return 0;

		i_dlen += p_sys->data_to_be_read = 0;

		i_read = net_Read( p_access, p_sys->fd_data, NULL, p_buffer, i_dlen,
					   false );

		p_sys->data_to_be_read = i_dlen - i_read;
		//msg_Info( p_access, "data_to_be_read %ld", p_sys->data_to_be_read );

		if( i_read == 0 ) {
			msg_Err( p_access, "SET EOF", p_sys->data_to_be_read );
			p_access->info.b_eof = true;
		}
		else if( i_read > 0 )
			p_access->info.i_pos += i_read;

		//msg_Info( p_access, "i_len2 %ld", i_len2 );
		//if (i_read == 0)
		//	break;
	//}
    //msg_Info( p_access, "i_totalread %ld", i_totalread );

	// update the size if live tv
	size_t newsize = p_access->info.i_size;
	if(p_access->info.i_size != newsize) {
        p_access->info.i_size = newsize;
        p_access->info.i_update |= INPUT_UPDATE_SIZE;
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

        /* */
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 0;
            break;

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
				double f, *pf;
				bool *pb;
				int64_t *pi64;
				input_title_t ***ppp_title;
				int          *pi_int;
				int i;

				msg_Err( p_access, "DEMUX_GET_TITLE_INFO DEMUX_GET_TITLE_INFO DEMUX_GET_TITLE_INFO");
				
				GetTitles(p_access);


				ppp_title = (input_title_t***)va_arg( args, input_title_t*** );
				pi_int    = (int*)va_arg( args, int* );
				*((int*)va_arg( args, int* )) = 0; /* Title offset */
				*((int*)va_arg( args, int* )) = 1; /* Chapter offset */

				//* Duplicate title infos 
				*pi_int = p_sys->i_titles;
				*ppp_title = malloc( sizeof( input_title_t ** ) * p_sys->i_titles );
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

                p_access->info.i_update |= INPUT_UPDATE_SEEKPOINT;
				p_access->info.i_seekpoint = i_skp;
                return VLC_SUCCESS;
            }
            return VLC_EGENERIC;

        default:
            msg_Warn( p_access, "unimplemented query in control: %d", i_query);
            return VLC_EGENERIC;

    }
    return VLC_SUCCESS;
}









static void GetTitles( access_t *p_demux )
{
    access_sys_t *p_sys = p_demux->p_sys;
    input_title_t *t;
    seekpoint_t *s;
    int32_t i_titles;
    int i;

	p_sys->i_titles = 0;

    /* Menu */
    t = vlc_input_title_New();
    t->b_menu = true;
    t->psz_name = strdup( "DVD Menu" );

    s = vlc_seekpoint_New();
	s->i_byte_offset = 100000000;
    s->psz_name = strdup( "Resume" );
    TAB_APPEND( t->i_seekpoint, t->seekpoint, s );

	TAB_APPEND( p_sys->i_titles, p_sys->titles, t );
}



