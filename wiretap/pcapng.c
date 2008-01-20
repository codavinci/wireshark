/* pcapng.c
 *
 * $Id$
 *
 * Wiretap Library
 * Copyright (c) 1998 by Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * File format support for pcap-ng file format
 * Copyright (c) 2007 by Ulf Lamping <ulf.lamping@web.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/* File format reference:
 *   http://www.winpcap.org/ntar/draft/PCAP-DumpFileFormat.html
 * Related Wiki page:
 *   http://wiki.wireshark.org/Development/PcapNg
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "wtap-int.h"
#include "file_wrappers.h"
#include "buffer.h"


static gboolean 
pcapng_read(wtap *wth, int *err, gchar **err_info,
    gint64 *data_offset);
static gboolean
pcapng_seek_read(wtap *wth, gint64 seek_off,
    union wtap_pseudo_header *pseudo_header, guchar *pd, int length,
    int *err, gchar **err_info);
static void
pcapng_close(wtap *wth);



/* pcapng: common block header for every block type */
typedef struct pcapng_block_header_s {
	guint32 block_type;
	guint32 block_total_length;
	/* x bytes block_body */
	/* guint32 block_total_length */
} pcapng_block_header_t;

/* pcapng: section header block */
typedef struct pcapng_section_header_block_s {
	/* pcapng_block_header_t */
	guint32 magic;
	guint16 version_major;
	guint16 version_minor;
	guint64 section_length; /* might be -1 for unknown */
	/* ... Options ... */
} pcapng_section_header_block_t;

/* pcapng: interface description block */
typedef struct pcapng_interface_description_block_s {
	guint16 linktype;
	guint16 reserved;
	guint32 snaplen;
	/* ... Options ... */
} pcapng_interface_description_block_t;

/* pcapng: packet block (obsolete) */
typedef struct pcapng_packet_block_s {
	guint16 interface_id;
	guint16 reserved;
	guint32 timestamp_high;
	guint32 timestamp_low;
	guint32 captured_len;
	guint32 packet_len;
	/* ... Packet Data ... */
	/* ... Padding ... */
	/* ... Options ... */
} pcapng_packet_block_t;

/* pcapng: enhanced packet block */
typedef struct pcapng_enhanced_packet_block_s {
	guint32 interface_id;
	guint32 timestamp_high;
	guint32 timestamp_low;
	guint32 captured_len;
	guint32 packet_len;
	/* ... Packet Data ... */
	/* ... Padding ... */
	/* ... Options ... */
} pcapng_enhanced_packet_block_t;

/* pcapng: simple packet block */
typedef struct pcapng_simple_packet_block_s {
	guint32 packet_len;
	/* ... Packet Data ... */
	/* ... Padding ... */
} pcapng_simple_packet_block_t;

/* pcapng: common option header for every option type */
typedef struct pcapng_option_header_s {
	guint16 option_code;
	guint16 option_length;
	/* ... x bytes Option Body ... */
    /* ... Padding ... */
} pcapng_option_header_t;

/* Block types */
#define BLOCK_TYPE_IDB 0x00000001 /* Interface Description Block */
#define BLOCK_TYPE_PB  0x00000002 /* Packet Block (obsolete) */
#define BLOCK_TYPE_SPB 0x00000003 /* Simple Packet Block */
#define BLOCK_TYPE_NRB 0x00000004 /* Name Resolution Block */
#define BLOCK_TYPE_ISB 0x00000005 /* Interface Statistics Block */
#define BLOCK_TYPE_EPB 0x00000006 /* Enhanced Packet Block */
#define BLOCK_TYPE_SHB 0x0A0D0D0A /* Section Header Block */



/* Capture section */
typedef struct wtapng_section_s {
	/* mandatory */
	guint64				section_length;
	/* options */
	gchar				*opt_comment;	/* NULL if not available */
	gchar				*shb_hardware;	/* NULL if not available */
	gchar				*shb_os;		/* NULL if not available */
	gchar				*shb_user_appl;	/* NULL if not available */
} wtapng_section_t;

/* Interface Description */
typedef struct wtapng_if_descr_s {
	/* mandatory */
	guint16				link_type;
	guint32				snap_len;
	/* options */
	gchar				*opt_comment;	/* NULL if not available */
	gchar				*if_name;		/* NULL if not available */
	gchar				*if_description;/* NULL if not available */
	/* XXX: if_IPv4addr */
	/* XXX: if_IPv6addr */
	/* XXX: if_MACaddr */
	/* XXX: if_EUIaddr */
	guint64				if_speed;		/* 0xFFFFFFFF if unknown */
	guint8				if_tsaccur;		/* default is 6 for microsecond resolution */
	/* XXX: if_filter */
	gchar				*if_os;			/* NULL if not available */
	gchar				if_fcslen;		/* -1 if unknown or changes between packets */
	/* XXX: guint64	if_tsoffset; */
} wtapng_if_descr_t;

/* Packets */
typedef struct wtapng_packet_s {
	/* mandatory */
	guint32				ts_high;		/* seconds since 1.1.1970 */
	guint32				ts_low;			/* fraction of seconds, depends on if_tsaccur */
	guint32				cap_len;        /* data length in the file */
	guint32				packet_len;     /* data length on the wire */
	/* options */
	gchar				*opt_comment;	/* NULL if not available */
	guint64				drop_count;		/* 0xFFFFFFFF if unknown */
	guint32             pack_flags;     /* XXX - 0 for now (any value for "we don't have it"?) */
	/* pack_hash */

	/* XXX - put the packet data / pseudo_header here as well? */
} wtapng_packet_t;

/* Simple Packets */
typedef struct wtapng_simple_packet_s {
	/* mandatory */
	guint32				cap_len;        /* data length in the file */
	guint32				packet_len;     /* data length on the wire */
	/* XXX - put the packet data / pseudo_header here as well? */
} wtapng_simple_packet_t;

/* Name Resolution */
typedef struct wtapng_name_res_s {
	/* options */
	gchar				*opt_comment;	/* NULL if not available */
	/* XXX */
} wtapng_name_res_t;

/* Interface Statistics */
typedef struct wtapng_if_stats_s {
	/* mandatory */
	guint32				ts_high;
	guint32				ts_low;
	/* options */
	gchar				*opt_comment;	/* NULL if not available */
	/* XXX */
	/*guint32				isb_starttime_high;*/
	/*guint32				isb_starttime_low;*/
	/*guint32				isb_endtime_high;*/
	/*guint32				isb_endtime_low;*/
	/*guint64				isb_ifrecv;*/
	/*guint64				isb_ifdrop;*/
	/*guint64				isb_filteraccept;*/
	/*guint64				isb_osdrop;*/
	/*guint64				isb_usrdeliv;*/
} wtapng_if_stats_t;


typedef struct wtapng_block_s {
	guint32					type;		/* block_type as defined by pcapng */
	union {
		wtapng_section_t	    section;
		wtapng_if_descr_t	    if_descr;
		wtapng_packet_t		    packet;
		wtapng_simple_packet_t  simple_packet;
		wtapng_name_res_t	    name_res;
		wtapng_if_stats_t	    if_stats;
	} data;

	/* XXX - currently don't know how to handle these! */
	const union wtap_pseudo_header *pseudo_header;
	const guchar *frame_buffer;
} wtapng_block_t;


static int
pcapng_read_option(FILE_T fh, pcapng_t *pn, pcapng_option_header_t *oh, char *content, int len, int *err, gchar **err_info _U_)
{
	int	bytes_read;
	int	block_read;
	guint64 file_offset64;


    /* read option header */
    errno = WTAP_ERR_CANT_READ;
    bytes_read = file_read(oh, 1, sizeof oh, fh);
    if (bytes_read != sizeof oh) {
	    g_warning("pcapng_read_option: failed to read option");
	    *err = file_error(fh);
	    if (*err != 0)
		    return -1;
	    return 0;
    }
    block_read = sizeof oh;
    if(pn->byte_swapped) {
        oh->option_code      = BSWAP16(oh->option_code);
        oh->option_length    = BSWAP32(oh->option_length);
    }

    /* sanity check: option length */
    if (oh->option_length > len) {
	    g_warning("pcapng_read_option: option_length %u larger than buffer (%u)", 
            oh->option_length, len);
        return 0;
    }

    /* read option content */
    errno = WTAP_ERR_CANT_READ;
    bytes_read = file_read(content, 1, oh->option_length, fh);
    if (bytes_read != oh->option_length) {
        g_warning("pcapng_read_if_descr_block: failed to read content of option %u", oh->option_code);
	    *err = file_error(fh);
	    if (*err != 0)
		    return -1;
	    return 0;
    }
    block_read += oh->option_length;

    /* jump over potential padding bytes at end of option */
    if( (oh->option_length % 4) != 0) {
        file_offset64 = file_seek(fh, 4 - (oh->option_length % 4), SEEK_CUR, err);
        if (file_offset64 <= 0) {
	        if (*err != 0)
		        return -1;
	        return 0;
        }
        block_read += 4 - (oh->option_length % 4);
    }

    return block_read;
}


static int
pcapng_read_section_header_block(FILE_T fh, pcapng_block_header_t *bh, pcapng_t *pn, wtapng_block_t *wblock, int *err, gchar **err_info _U_)
{
	int	bytes_read;
	int	block_read;
	int to_read;
	pcapng_section_header_block_t shb;
    pcapng_option_header_t oh;
    char option_content[100]; /* XXX - size might need to be increased, if we see longer options */


	/* read block content */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&shb, 1, sizeof shb, fh);
	if (bytes_read != sizeof shb) {
		*err = file_error(fh);
		if (*err != 0)
			return -1;
		return 0;
	}
    block_read = bytes_read;

	/* is the magic number one we expect? */
	switch(shb.magic) {
		case(0x1A2B3C4D):
			/* this seems pcapng with correct byte order */
			pn->byte_swapped		= FALSE;
			pn->version_major		= shb.version_major;
			pn->version_minor		= shb.version_minor;

			g_warning("pcapng_read_section_header_block: SHB (little endian) V%u.%u, len %u",
				pn->version_major, pn->version_minor, bh->block_total_length);
			break;
		case(0x4D3C2B1A):
			/* this seems pcapng with swapped byte order */
			pn->byte_swapped		= TRUE;
			pn->version_major		= BSWAP16(shb.version_major);
			pn->version_minor		= BSWAP16(shb.version_minor);
			
			/* tweak the block length to meet current swapping that we know now */
			bh->block_total_length	= BSWAP32(bh->block_total_length);

			g_warning("pcapng_read_section_header_block: SHB (big endian) V%u.%u, len %u",
				pn->version_major, pn->version_minor, bh->block_total_length);
			break;
		default:
			/* Not a "pcapng" magic number we know about. */
			g_warning("pcapng_read_section_header_block: unknown magic number %u (probably not an pcapng file)", shb.magic);
			return 0;
	}

	/* we currently only understand SHB V1.0 */
	if(pn->version_major != 1 || pn->version_minor != 0) {
		g_warning("pcapng_read_section_header_block: unknown SHB version %u.%u", 
			pn->version_major, pn->version_minor);
		return 0;
	}

	/* XXX - no proper way to read/swap 64 bit values -> so ignore the 64bit section_length for now! */
	wblock->data.section.section_length = -1;	/* shb.section_length */

	/* Option defaults */
	wblock->data.section.opt_comment	= NULL;
	wblock->data.section.shb_hardware	= NULL;
	wblock->data.section.shb_os			= NULL;
	wblock->data.section.shb_user_appl	= NULL;

	/* Options */
	errno = WTAP_ERR_CANT_READ;
	to_read = bh->block_total_length
        - sizeof(pcapng_block_header_t) 
        - sizeof (pcapng_section_header_block_t) 
        - sizeof(bh->block_total_length);
    while(to_read > 0) {
        /* read option */
        bytes_read = pcapng_read_option(fh, pn, &oh, option_content, sizeof(option_content), err, err_info);
	    if (bytes_read <= 0) {
            g_warning("pcapng_read_section_header_block: failed to read option");
		    return bytes_read;
	    }
        block_read += bytes_read;
        to_read -= bytes_read;

        /* handle option content */
        switch(oh.option_code) {
            case(0): /* opt_endofopt */
                if(to_read != 0) {
		            g_warning("pcapng_read_section_header_block: %u bytes after opt_endofopt", to_read);
                }
                /* padding should be ok here, just get out of this */
                to_read = 0;
                break;
            case(1): /* opt_comment */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.section.opt_comment = g_strndup(option_content, sizeof(option_content));
                    g_warning("pcapng_read_section_header_block: opt_comment %s", wblock->data.section.opt_comment);
                } else {
		            g_warning("pcapng_read_section_header_block: opt_comment length %u seems strange", oh.option_length);
                }
                break;
            case(2): /* shb_hardware */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.section.shb_hardware = g_strndup(option_content, sizeof(option_content));
		            g_warning("pcapng_read_section_header_block: shb_hardware %s", wblock->data.section.shb_hardware);
                } else {
		            g_warning("pcapng_read_section_header_block: shb_hardware length %u seems strange", oh.option_length);
                }
                break;
            case(3): /* shb_os */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.section.shb_os = g_strndup(option_content, sizeof(option_content));
		            g_warning("pcapng_read_section_header_block: shb_os %s", wblock->data.section.shb_os);
                } else {
		            g_warning("pcapng_read_section_header_block: shb_os length %u seems strange", oh.option_length);
                }
                break;
            case(4): /* shb_userappl */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.section.shb_user_appl = g_strndup(option_content, sizeof(option_content));
		            g_warning("pcapng_read_section_header_block: shb_userappl %s", wblock->data.section.shb_user_appl);
                } else {
		            g_warning("pcapng_read_section_header_block: shb_userappl length %u seems strange", oh.option_length);
                }
                break;
            default:
		        g_warning("pcapng_read_section_header_block: unknown option %u - ignoring %u bytes",
                    oh.option_code, oh.option_length);
        }
    }

	return block_read;
}


/* "Interface Description Block" */
static int
pcapng_read_if_descr_block(FILE_T fh, pcapng_block_header_t *bh, pcapng_t *pn, wtapng_block_t *wblock, int *err, gchar **err_info _U_)
{
	int	bytes_read;
	int	block_read;
	int to_read;
	pcapng_interface_description_block_t idb;
    pcapng_option_header_t oh;
    char option_content[100]; /* XXX - size might need to be increased, if we see longer options */


	/* read block content */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&idb, 1, sizeof idb, fh);
	if (bytes_read != sizeof idb) {
		g_warning("pcapng_read_if_descr_block: failed to read IDB");
		*err = file_error(fh);
		if (*err != 0)
			return -1;
		return 0;
	}
    block_read = bytes_read;

	/* mandatory values */
	if(pn->byte_swapped) {
		wblock->data.if_descr.link_type = BSWAP16(idb.linktype);
		wblock->data.if_descr.snap_len	= BSWAP32(idb.snaplen);
	} else {
		wblock->data.if_descr.link_type	= idb.linktype;
		wblock->data.if_descr.snap_len	= idb.snaplen;
	}

	g_warning("pcapng_read_if_descr_block: IDB link_type %u, snap %u",
		wblock->data.if_descr.link_type, wblock->data.if_descr.snap_len);

	/* XXX - sanity check of snapshot length */
	/* XXX - while a very big snapshot length is valid, it's more likely that it's a bug in the file */
	/* XXX - so do a sanity check for now, it's likely e.g. a byte swap order problem */
	if(wblock->data.if_descr.snap_len > 65535) {
		g_warning("pcapng_read_if_descr_block: snapshot length %u unrealistic", 
			wblock->data.if_descr.snap_len);
		/*wblock->data.if_descr.snap_len = 65535;*/
		return 0;
	}

	/* Option defaults */
	wblock->data.if_descr.opt_comment		= NULL;
	wblock->data.if_descr.if_name			= NULL;
	wblock->data.if_descr.if_description	= NULL;
	/* XXX: if_IPv4addr */
	/* XXX: if_IPv6addr */
	/* XXX: if_MACaddr */
	/* XXX: if_EUIaddr */
	wblock->data.if_descr.if_speed			= 0xFFFFFFFF;	/* "unknown" */
	wblock->data.if_descr.if_tsaccur		= 6;			/* default is 6 for microsecond resolution */
	/* XXX: if_filter */
	wblock->data.if_descr.if_os				= NULL;
	wblock->data.if_descr.if_fcslen			= (gchar) -1;	/* unknown or changes between packets */
	/* XXX: guint64	if_tsoffset; */


	/* Options */
	errno = WTAP_ERR_CANT_READ;
	to_read = bh->block_total_length 
        - sizeof(pcapng_block_header_t) 
        - sizeof (pcapng_interface_description_block_t) 
        - sizeof(bh->block_total_length);
    while(to_read > 0) {
        /* read option */
        bytes_read = pcapng_read_option(fh, pn, &oh, option_content, sizeof(option_content), err, err_info);
	    if (bytes_read <= 0) {
            g_warning("pcapng_read_if_descr_block: failed to read option");
		    return bytes_read;
	    }
        block_read += bytes_read;
        to_read -= bytes_read;

        /* handle option content */
        switch(oh.option_code) {
            case(0): /* opt_endofopt */
                if(to_read != 0) {
		            g_warning("pcapng_read_if_descr_block: %u bytes after opt_endofopt", to_read);
                }
                /* padding should be ok here, just get out of this */
                to_read = 0;
                break;
            case(1): /* opt_comment */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.section.opt_comment = g_strndup(option_content, sizeof(option_content));
                    g_warning("pcapng_read_if_descr_block: opt_comment %s", wblock->data.section.opt_comment);
                } else {
		            g_warning("pcapng_read_if_descr_block: opt_comment length %u seems strange", oh.option_length);
                }
                break;
            case(2): /* if_name */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.if_descr.if_name = g_strndup(option_content, sizeof(option_content));
		            g_warning("pcapng_read_if_descr_block: if_name %s", wblock->data.if_descr.if_name);
                } else {
		            g_warning("pcapng_read_if_descr_block: if_name length %u seems strange", oh.option_length);
                }
                break;
            case(3): /* if_description */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.if_descr.if_description = g_strndup(option_content, sizeof(option_content));
		            g_warning("pcapng_read_if_descr_block: if_description %s", wblock->data.if_descr.if_description);
                } else {
		            g_warning("pcapng_read_if_descr_block: if_description length %u seems strange", oh.option_length);
                }
                break;
            case(8): /* if_speed */
                if(oh.option_length == 8) {
                    wblock->data.if_descr.if_speed = *((guint64 *)option_content);
                    if(pn->byte_swapped) 
		                wblock->data.if_descr.if_speed = BSWAP64(wblock->data.if_descr.if_speed);

		            g_warning("pcapng_read_if_descr_block: if_speed %" G_GINT64_MODIFIER "u (bps)", wblock->data.if_descr.if_speed);
                } else {
		            g_warning("pcapng_read_if_descr_block: if_speed length %u not 8 as expected", oh.option_length);
                }
                break;
            case(9): /* if_tsaccur */
                if(oh.option_length == 1) {
                    wblock->data.if_descr.if_tsaccur = option_content[0];
		            g_warning("pcapng_read_if_descr_block: if_tsaccur %u", wblock->data.if_descr.if_tsaccur);
                } else {
		            g_warning("pcapng_read_if_descr_block: if_tsaccur length %u not 1 as expected", oh.option_length);
                }
                break;
            case(13): /* if_fcslen */
                if(oh.option_length == 1) {
                    wblock->data.if_descr.if_fcslen = option_content[0];
                    pn->if_fcslen = wblock->data.if_descr.if_fcslen;
		            g_warning("pcapng_read_if_descr_block: if_fcslen %u", wblock->data.if_descr.if_fcslen);
                    /* XXX - add sanity check */
                } else {
		            g_warning("pcapng_read_if_descr_block: if_fcslen length %u not 1 as expected", oh.option_length);
                }
                break;
            default:
		        g_warning("pcapng_read_if_descr_block: unknown option %u - ignoring %u bytes",
                    oh.option_code, oh.option_length);
        }
    }

	return block_read;
}


static int 
pcapng_read_packet_block(FILE_T fh, pcapng_block_header_t *bh, pcapng_t *pn, wtapng_block_t *wblock,int *err, gchar **err_info _U_)
{
	int bytes_read;
	int block_read;
	int to_read;
	guint64 file_offset64;
	pcapng_enhanced_packet_block_t epb;
	guint32 block_total_length;
    pcapng_option_header_t oh;
    char option_content[100]; /* XXX - size might need to be increased, if we see longer options */


	/* "(Enhanced) Packet Block" read fixed part */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&epb, 1, sizeof epb, fh);
	if (bytes_read != sizeof epb) {
		g_warning("pcapng_read_packet_block: failed to read packet data");
		*err = file_error(fh);
		return 0;
	}
	block_read = bytes_read;

	/* XXX - as we currently ignore the interface id, both packet blocks are the same for us */
	if(pn->byte_swapped) {
		wblock->data.packet.ts_high		= BSWAP32(epb.timestamp_high);
		wblock->data.packet.ts_low		= BSWAP32(epb.timestamp_low);
		wblock->data.packet.cap_len		= BSWAP32(epb.captured_len);
		wblock->data.packet.packet_len	= BSWAP32(epb.packet_len);
	} else {
		wblock->data.packet.ts_high		= epb.timestamp_high;
		wblock->data.packet.ts_low		= epb.timestamp_low;
		wblock->data.packet.cap_len		= epb.captured_len;
		wblock->data.packet.packet_len	= epb.packet_len;
	}

	/*g_warning("pcapng_read_packet_block: packet data: packet_len %u captured_len %u",
		wblock->data.packet.packet_len, wblock->data.packet.cap_len);*/

	/* xxx - convert timestamps if we have an odd if_tsaccur */

	/* XXX - implement other linktypes then Ethernet */
	/* (or even better share the code with libpcap.c) */

	/* Ethernet FCS length, might be overwritten by "per packet" options */
	((union wtap_pseudo_header *) wblock->pseudo_header)->eth.fcs_len = pn->if_fcslen;

	/* "(Enhanced) Packet Block" read capture data */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read((guchar *) (wblock->frame_buffer), 1, wblock->data.packet.cap_len, fh);
	if (bytes_read != (int) wblock->data.packet.cap_len) {
		*err = file_error(fh);
		g_warning("pcapng_read_packet_block: couldn't read %u bytes of captured data", 
			wblock->data.packet.cap_len);
		if (*err == 0)
			*err = WTAP_ERR_SHORT_READ;
		return FALSE;
	}
	block_read += bytes_read;

    /* jump over potential padding bytes at end of the packet data */
    if( (wblock->data.packet.cap_len % 4) != 0) {
        file_offset64 = file_seek(fh, 4 - (wblock->data.packet.cap_len % 4), SEEK_CUR, err);
        if (file_offset64 <= 0) {
	        if (*err != 0)
		        return -1;
	        return 0;
        }
        block_read += 4 - (wblock->data.packet.cap_len % 4);
    }

    /* add padding bytes to "block total length" */
	/* (the "block total length" of some example files don't contain the packet data padding bytes!) */
	if(bh->block_total_length % 4) {
		block_total_length = bh->block_total_length + 4 - (bh->block_total_length % 4);
	} else {
		block_total_length = bh->block_total_length;
	}

	/* Option defaults */
    wblock->data.packet.opt_comment = NULL;
    wblock->data.packet.drop_count  = -1;
    wblock->data.packet.pack_flags  = 0;    /* XXX - is 0 ok to signal "not used"? */

	/* Options */
	errno = WTAP_ERR_CANT_READ;
	to_read = block_total_length 
        - sizeof(pcapng_block_header_t) 
        - block_read    /* fixed and variable part, including padding */
        - sizeof(bh->block_total_length);
    while(to_read > 0) {
        /* read option */
        bytes_read = pcapng_read_option(fh, pn, &oh, option_content, sizeof(option_content), err, err_info);
	    if (bytes_read <= 0) {
            g_warning("pcapng_read_packet_block: failed to read option");
		    return bytes_read;
	    }
        block_read += bytes_read;
        to_read -= bytes_read;

        /* handle option content */
        switch(oh.option_code) {
            case(0): /* opt_endofopt */
                if(to_read != 0) {
		            g_warning("pcapng_read_packet_block: %u bytes after opt_endofopt", to_read);
                }
                /* padding should be ok here, just get out of this */
                to_read = 0;
                break;
            case(1): /* opt_comment */
                if(oh.option_length > 0 && oh.option_length < sizeof(option_content)) {
                    wblock->data.section.opt_comment = g_strndup(option_content, sizeof(option_content));
                    g_warning("pcapng_read_packet_block: opt_comment %s", wblock->data.section.opt_comment);
                } else {
		            g_warning("pcapng_read_packet_block: opt_comment length %u seems strange", oh.option_length);
                }
                break;
            case(2): /* pack_flags / epb_flags */
                if(oh.option_length == 4) {
                    wblock->data.packet.pack_flags = *((guint32 *)option_content);
                    if(pn->byte_swapped) 
		                wblock->data.packet.pack_flags = BSWAP32(wblock->data.packet.pack_flags);
		            g_warning("pcapng_read_if_descr_block: pack_flags %u (ignored)", wblock->data.packet.pack_flags);
                } else {
		            g_warning("pcapng_read_if_descr_block: pack_flags length %u not 4 as expected", oh.option_length);
                }
                break;
            default:
		        g_warning("pcapng_read_packet_block: unknown option %u - ignoring %u bytes",
                    oh.option_code, oh.option_length);
        }
    }

	return block_read;
}


static int 
pcapng_read_simple_packet_block(FILE_T fh, pcapng_block_header_t *bh, pcapng_t *pn, wtapng_block_t *wblock,int *err, gchar **err_info _U_)
{
	int bytes_read;
	int block_read;
	guint64 file_offset64;
	pcapng_simple_packet_block_t spb;


	/* "Simple Packet Block" read fixed part */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&spb, 1, sizeof spb, fh);
	if (bytes_read != sizeof spb) {
		g_warning("pcapng_read_simple_packet_block: failed to read packet data");
		*err = file_error(fh);
		return 0;
	}
	block_read = bytes_read;

	if(pn->byte_swapped) {
        wblock->data.simple_packet.packet_len	= BSWAP32(spb.packet_len);
	} else {
		wblock->data.simple_packet.packet_len	= spb.packet_len;
	}

	wblock->data.simple_packet.cap_len = bh->block_total_length 
        - sizeof(pcapng_simple_packet_block_t) 
        - sizeof(bh->block_total_length);

	/*g_warning("pcapng_read_simple_packet_block: packet data: packet_len %u",
		wblock->data.simple_packet.packet_len);*/

	/* XXX - implement other linktypes then Ethernet */
	/* (or even better share the code with libpcap.c) */

	/* Ethernet FCS length, might be overwritten by "per packet" options */
	((union wtap_pseudo_header *) wblock->pseudo_header)->eth.fcs_len = pn->if_fcslen;

	/* "Simple Packet Block" read capture data */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read((guchar *) (wblock->frame_buffer), 1, wblock->data.simple_packet.cap_len, fh);
	if (bytes_read != (int) wblock->data.simple_packet.cap_len) {
		*err = file_error(fh);
		g_warning("pcapng_read_simple_packet_block: couldn't read %u bytes of captured data", 
			wblock->data.simple_packet.cap_len);
		if (*err == 0)
			*err = WTAP_ERR_SHORT_READ;
		return FALSE;
	}
	block_read += bytes_read;

    /* jump over potential padding bytes at end of the packet data */
    if( (wblock->data.simple_packet.cap_len % 4) != 0) {
        file_offset64 = file_seek(fh, 4 - (wblock->data.simple_packet.cap_len % 4), SEEK_CUR, err);
        if (file_offset64 <= 0) {
	        if (*err != 0)
		        return -1;
	        return 0;
        }
        block_read += 4 - (wblock->data.simple_packet.cap_len % 4);
    }

	return block_read;
}


static int 
pcapng_read_unknown_block(FILE_T fh, pcapng_block_header_t *bh, pcapng_t *pn _U_, wtapng_block_t *wblock _U_,int *err, gchar **err_info _U_)
{
	int block_read;
	guint64 file_offset64;
	guint32 block_total_length;


    /* add padding bytes to "block total length" */
	/* (the "block total length" of some example files don't contain any padding bytes!) */
	if(bh->block_total_length % 4) {
		block_total_length = bh->block_total_length + 4 - (bh->block_total_length % 4);
	} else {
		block_total_length = bh->block_total_length;
	}

	block_read = block_total_length - sizeof(bh->block_total_length);

    /* jump over this unknown block */
    file_offset64 = file_seek(fh, block_read, SEEK_CUR, err);
    if (file_offset64 <= 0) {
        if (*err != 0)
	        return -1;
        return 0;
    }

	return block_read;
}


static int 
pcapng_read_block(FILE_T fh, pcapng_t *pn, wtapng_block_t *wblock, int *err, gchar **err_info)
{
	int	block_read;
	int	bytes_read;
	pcapng_block_header_t bh;
	guint32 block_total_length;


	/* Try to read the (next) block header */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&bh, 1, sizeof bh, fh);
	if (bytes_read != sizeof bh) {
		g_warning("pcapng_read_block: end of file");
		*err = file_error(fh);
		if (*err != 0)
			return -1;
		return 0;
	}
    block_read = bytes_read;
	if(pn->byte_swapped) {
		bh.block_type			= BSWAP32(bh.block_type);
		bh.block_total_length	= BSWAP32(bh.block_total_length);
	}

	wblock->type = bh.block_type;

	/*g_warning("pcapng_read_block: block_type 0x%x", bh.block_type);*/

	switch(bh.block_type) {
		case(BLOCK_TYPE_SHB):
			bytes_read = pcapng_read_section_header_block(fh, &bh, pn, wblock, err, err_info);
			break;
		case(BLOCK_TYPE_IDB):
			bytes_read = pcapng_read_if_descr_block(fh, &bh, pn, wblock, err, err_info);
			break;
		case(BLOCK_TYPE_PB):
			bytes_read = pcapng_read_packet_block(fh, &bh, pn, wblock, err, err_info);
			break;
		case(BLOCK_TYPE_SPB):
			bytes_read = pcapng_read_simple_packet_block(fh, &bh, pn, wblock, err, err_info);
			break;
		case(BLOCK_TYPE_EPB):
			bytes_read = pcapng_read_packet_block(fh, &bh, pn, wblock, err, err_info);
			break;
		default:
			g_warning("pcapng_read_block: Unknown block_type: 0x%x (block ignored)", bh.block_type);
			bytes_read = pcapng_read_unknown_block(fh, &bh, pn, wblock, err, err_info);
	}
    if(bytes_read <= 0) {
	    return bytes_read;
    }
    block_read += bytes_read;

	/* sanity check: first and second block lengths must match */
	errno = WTAP_ERR_CANT_READ;
	bytes_read = file_read(&block_total_length, 1, sizeof block_total_length, fh);
	if (bytes_read != sizeof block_total_length) {
		g_warning("pcapng_read_block: couldn't read second block length");
		*err = file_error(fh);
		return 0;
	}
    block_read += bytes_read;

    if(pn->byte_swapped)
        block_total_length = BSWAP32(block_total_length);

	if( !(block_total_length == bh.block_total_length) ) {
		g_warning("pcapng_read_block: total block lengths (first %u and second %u) don't match", 
			bh.block_total_length, block_total_length);
		return 0;
	}

	return block_read;
}


/* classic wtap: open capture file */
int 
pcapng_open(wtap *wth, int *err, gchar **err_info)
{
	int bytes_read;
	pcapng_t pn;
	wtapng_block_t wblock;


	/* we don't know the byte swapping of the file yet */
	pn.byte_swapped = FALSE;
    pn.if_fcslen = -1;
    pn.version_major = -1;
    pn.version_minor = -1;

	/* we don't expect any packet blocks yet */
	wblock.frame_buffer = NULL;
	wblock.pseudo_header = NULL;


	/* read first block */
	bytes_read = pcapng_read_block(wth->fh, &pn, &wblock, err, err_info);
	if (bytes_read <= 0) {
		*err = file_error(wth->fh);
		g_warning("pcapng_open_new: couldn't read first SHB");
		if (*err != 0)
			return -1;
		return 0;
	}

	wth->data_offset += bytes_read;

	/* first block must be a "Section Header Block" */
	if(wblock.type != BLOCK_TYPE_SHB) {
		g_warning("pcapng_open_new: first block type %u not SHB", wblock.type);
		return 0;
	}


	/* read second block */
	bytes_read = pcapng_read_block(wth->fh, &pn, &wblock, err, err_info);
	if (bytes_read <= 0) {
		*err = file_error(wth->fh);
		g_warning("pcapng_open_new: couldn't read IDB");
		if (*err != 0)
			return -1;
		return 0;
	}

	wth->data_offset += bytes_read;

	/* second block must be an "Interface Description Block" */
	if(wblock.type != 1) {
		g_warning("pcapng_open_new: second block type %u not IDB", wblock.type);
		return 0;
	}

	/* read "Interface Description Block" specific settings */
	wth->file_encap = wblock.data.if_descr.link_type;
	wth->snapshot_length = wblock.data.if_descr.snap_len;


	/* Seems, this is a pcapng file */
	wth->capture.pcapng = g_malloc(sizeof(pcapng_t));
	*wth->capture.pcapng = pn;

	wth->subtype_read		= pcapng_read;
	wth->subtype_seek_read	= pcapng_seek_read;
	wth->subtype_close		= pcapng_close;

	wth->file_type			= WTAP_FILE_PCAPNG;
    switch(wblock.data.if_descr.if_tsaccur) {
        case(6):
	    wth->tsprecision	= WTAP_FILE_TSPREC_USEC;	/* usec is the default (without the if_tsaccur option) */
        break;
        case(9):
	    wth->tsprecision	= WTAP_FILE_TSPREC_NSEC;
        break;
        default:
            g_warning("pcapng_open: if_tsaccur %u not implemented, timestamp conversion omitted",
                wblock.data.if_descr.if_tsaccur);
    }

	return 1;
}


/* classic wtap: read packet */
static gboolean 
pcapng_read(wtap *wth, int *err, gchar **err_info,
    gint64 *data_offset)
{
	int bytes_read;
	wtapng_block_t wblock;


	*data_offset = wth->data_offset;

	/* XXX - this probably won't work well with unlimited / per packet snapshot length */
	buffer_assure_space(wth->frame_buffer, wth->snapshot_length);

	wblock.frame_buffer = buffer_start_ptr(wth->frame_buffer);
	wblock.pseudo_header = &wth->pseudo_header;

	/* read next block */
    while(1) {
	    bytes_read = pcapng_read_block(wth->fh, wth->capture.pcapng, &wblock, err, err_info);
	    if (bytes_read <= 0) {
		    *err = file_error(wth->fh);
		    /*g_warning("pcapng_read: couldn't read packet block");*/
		    if (*err != 0)
			    return -1;
		    return 0;
	    }

	    /* block must be a "Packet Block" or an "Enhanced Packet Block" -> otherwise continue */
	    if(wblock.type == BLOCK_TYPE_PB || wblock.type == BLOCK_TYPE_EPB) {
            break;
        }

        /* XXX - improve handling of "unknown" blocks */
		g_warning("pcapng_read: block type 0x%x not PB/EPB", wblock.type);
    }

	wth->phdr.caplen	= wblock.data.packet.cap_len;
	wth->phdr.len		= wblock.data.packet.packet_len;
	wth->phdr.ts.secs	= wblock.data.packet.ts_high;
	wth->phdr.ts.nsecs	= wblock.data.packet.ts_low;

	/*g_warning("Read length: %u Packet length: %u", bytes_read, wth->phdr.caplen);*/
	wth->data_offset += bytes_read;

    return TRUE;
}


/* classic wtap: seek to file position and read packet */
static gboolean
pcapng_seek_read(wtap *wth, gint64 seek_off,
    union wtap_pseudo_header *pseudo_header, guchar *pd, int length _U_,
    int *err, gchar **err_info)
{
	guint64 bytes_read64;
	int bytes_read;
	wtapng_block_t wblock;


	/* seek to the right file position */
	bytes_read64 = file_seek(wth->random_fh, seek_off, SEEK_SET, err);
	if (bytes_read64 <= 0) {
		return FALSE;	/* Seek error */
	}

	wblock.frame_buffer = pd;
	wblock.pseudo_header = pseudo_header;

	/* read the block */
	bytes_read = pcapng_read_block(wth->random_fh, wth->capture.pcapng, &wblock, err, err_info);
	if (bytes_read <= 0) {
		*err = file_error(wth->fh);
		g_warning("pcapng_seek_read: couldn't read packet block");
		return FALSE;
	}

	/* block must be a "Packet Block" or an "Enhanced Packet Block" */
	if(wblock.type != BLOCK_TYPE_PB && wblock.type != BLOCK_TYPE_EPB) {
		g_warning("pcapng_seek_read: block type %u not PB/EPB", wblock.type);
		return FALSE;
	}

	return TRUE;
}


/* classic wtap: close capture file */
static void
pcapng_close(wtap *wth)
{
	g_free(wth->capture.pcapng);
}



static gboolean
pcapng_write_section_header_block(wtap_dumper *wdh, wtapng_block_t *wblock, int *err)
{
	pcapng_block_header_t bh;
	pcapng_section_header_block_t shb;
	size_t nwritten;


	/* write block header */
	bh.block_type = wblock->type;
	bh.block_total_length = sizeof(bh) + sizeof(shb) /* + options */ + 4;

	nwritten = wtap_dump_file_write(wdh, &bh, sizeof bh);
	if (nwritten != sizeof bh) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof bh;

	/* write block fixed content */
	/* XXX - get these values from wblock? */
	shb.magic = 0x1A2B3C4D;
	shb.version_major = 1;
	shb.version_minor = 0;
	shb.section_length = -1;

	nwritten = wtap_dump_file_write(wdh, &shb, sizeof shb);
	if (nwritten != sizeof shb) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof shb;

	/* XXX - write (optional) block options */

	/* write block footer */
	nwritten = wtap_dump_file_write(wdh, &bh.block_total_length, sizeof bh.block_total_length);
	if (nwritten != sizeof bh.block_total_length) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof bh.block_total_length;

	return TRUE;
}



static gboolean
pcapng_write_if_descr_block(wtap_dumper *wdh, wtapng_block_t *wblock, int *err)
{
	pcapng_block_header_t bh;
	pcapng_interface_description_block_t idb;
	size_t nwritten;


	/* write block header */
	bh.block_type = wblock->type;
	bh.block_total_length = sizeof(bh) + sizeof(idb) /* + options */ + 4;

	nwritten = wtap_dump_file_write(wdh, &bh, sizeof bh);
	if (nwritten != sizeof bh) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof bh;

	/* write block fixed content */
	idb.linktype	= wblock->data.if_descr.link_type;
	idb.reserved	= 0;
	idb.snaplen		= wblock->data.if_descr.snap_len;

	nwritten = wtap_dump_file_write(wdh, &idb, sizeof idb);
	if (nwritten != sizeof idb) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof idb;

	/* XXX - write (optional) block options */

	/* write block footer */
	nwritten = wtap_dump_file_write(wdh, &bh.block_total_length, sizeof bh.block_total_length);
	if (nwritten != sizeof bh.block_total_length) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof bh.block_total_length;

	return TRUE;
}


static gboolean
pcapng_write_packet_block(wtap_dumper *wdh, wtapng_block_t *wblock, int *err)
{
	pcapng_block_header_t bh;
	pcapng_enhanced_packet_block_t epb;
	size_t nwritten;
	guint32 zero_pad = 0;


	guint32 cap_pad_len = 0;
    if (wblock->data.packet.cap_len % 4) {
        cap_pad_len += 4 - (wblock->data.packet.cap_len % 4);
    }

	/* write (enhanced) packet block header */
	bh.block_type = wblock->type;
	bh.block_total_length = sizeof(bh) + sizeof(epb) /* + pseudo header */ + wblock->data.packet.cap_len + cap_pad_len /* + options */ + 4;

	nwritten = wtap_dump_file_write(wdh, &bh, sizeof bh);
	if (nwritten != sizeof bh) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof bh;

	/* write block fixed content */
	epb.interface_id	= 1;	/* XXX */
	epb.timestamp_high	= wblock->data.packet.ts_high;
	epb.timestamp_low	= wblock->data.packet.ts_low;
	epb.captured_len	= wblock->data.packet.cap_len;
	epb.packet_len		= wblock->data.packet.packet_len;

	nwritten = wtap_dump_file_write(wdh, &epb, sizeof epb);
	if (nwritten != sizeof epb) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof epb;

	/* XXX - write pseudo header */

	/* write packet data */
	nwritten = wtap_dump_file_write(wdh, wblock->frame_buffer, wblock->data.packet.cap_len);
	if (nwritten != wblock->data.packet.cap_len) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += wblock->data.packet.cap_len;

	/* write padding (if any) */
	if(cap_pad_len != 0) {
		nwritten = wtap_dump_file_write(wdh, &zero_pad, cap_pad_len);
		if (nwritten != cap_pad_len) {
			if (nwritten == 0 && wtap_dump_file_ferror(wdh))
				*err = wtap_dump_file_ferror(wdh);
			else
				*err = WTAP_ERR_SHORT_WRITE;
			return FALSE;
		}
		wdh->bytes_dumped += cap_pad_len;
	}

	/* XXX - write (optional) block options */

	/* write block footer */
	nwritten = wtap_dump_file_write(wdh, &bh.block_total_length, sizeof bh.block_total_length);
	if (nwritten != sizeof bh.block_total_length) {
		if (nwritten == 0 && wtap_dump_file_ferror(wdh))
			*err = wtap_dump_file_ferror(wdh);
		else
			*err = WTAP_ERR_SHORT_WRITE;
		return FALSE;
	}
	wdh->bytes_dumped += sizeof bh.block_total_length;

	return TRUE;
}


static gboolean
pcapng_write_block(wtap_dumper *wdh, /*pcapng_t *pn, */wtapng_block_t *wblock, int *err)
{
	switch(wblock->type) {
		case(BLOCK_TYPE_SHB):
			return pcapng_write_section_header_block(wdh, wblock, err);
			break;
		case(BLOCK_TYPE_IDB):
			return pcapng_write_if_descr_block(wdh, wblock, err);
			break;
		case(BLOCK_TYPE_PB):
			/* Packet Block is obsolete */
			return FALSE;
			break;
		case(BLOCK_TYPE_EPB):
			return pcapng_write_packet_block(wdh, wblock, err);
			break;
		default:
			g_warning("Unknown block_type: 0x%x", wblock->type);
			return FALSE;
	}
}


static gboolean pcapng_dump(wtap_dumper *wdh,
	const struct wtap_pkthdr *phdr,
	const union wtap_pseudo_header *pseudo_header _U_,
	const guchar *pd, int *err)
{
	wtapng_block_t wblock;

	wblock.frame_buffer = pd;
	wblock.pseudo_header = pseudo_header;

	/* write the (enhanced) packet block */
	wblock.type = BLOCK_TYPE_EPB;

	wblock.data.packet.ts_high		= (guint32) phdr->ts.secs;
	wblock.data.packet.ts_low		= phdr->ts.nsecs;	/* XXX - convert */
	wblock.data.packet.cap_len		= phdr->caplen;
	wblock.data.packet.packet_len	= phdr->len;

	/* currently unused */
	wblock.data.packet.drop_count = -1;
	wblock.data.packet.opt_comment = NULL;

	if (!pcapng_write_block(wdh, &wblock, err)) {
		return FALSE;
	}

	return TRUE;
}


/* Returns TRUE on success, FALSE on failure; sets "*err" to an error code on
   failure */
gboolean 
pcapng_dump_open(wtap_dumper *wdh, gboolean cant_seek _U_, int *err)
{
	wtapng_block_t wblock;

	wblock.frame_buffer = NULL;
	wblock.pseudo_header = NULL;


	/* This is a pcapng file */
	wdh->subtype_write = pcapng_dump;
	wdh->subtype_close = NULL;

	/* write the section header block */
	wblock.type = BLOCK_TYPE_SHB;
	wblock.data.section.section_length = -1;

	/* XXX - options unused */
	wblock.data.section.opt_comment		= NULL;
	wblock.data.section.shb_hardware	= NULL;
	wblock.data.section.shb_os			= NULL;
	wblock.data.section.shb_user_appl	= NULL;

	if (!pcapng_write_block(wdh, &wblock, err)) {
		return FALSE;
	}

	/* write the interface description block */
	wblock.type = BLOCK_TYPE_IDB;
	wblock.data.if_descr.link_type	= wdh->encap;
	wblock.data.if_descr.snap_len	= wdh->snaplen;

	/* XXX - options unused */
	wblock.data.if_descr.if_speed	= -1;
	wblock.data.if_descr.if_tsaccur	= 6;	/* default: usec */
	wblock.data.if_descr.if_os		= NULL;
	wblock.data.if_descr.if_fcslen  = -1;

	if (!pcapng_write_block(wdh, &wblock, err)) {
		return FALSE;
	}

	return TRUE;
}


/* Returns 0 if we could write the specified encapsulation type,
   an error indication otherwise. */
int pcapng_dump_can_write_encap(int encap)
{
	/* Per-packet encapsulations aren't supported. */
	if (encap == WTAP_ENCAP_PER_PACKET)
		return WTAP_ERR_ENCAP_PER_PACKET_UNSUPPORTED;

	/* XXX - for now we only support Ethernet */
	if (encap != WTAP_ENCAP_ETHERNET)
		return WTAP_ERR_UNSUPPORTED_ENCAP;

	return 0;
}

