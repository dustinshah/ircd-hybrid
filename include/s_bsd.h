/*
 *  ircd-hybrid: an advanced, lightweight Internet Relay Chat Daemon (ircd)
 *
 *  Copyright (c) 1997-2018 ircd-hybrid development team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 *  USA
 */

/*! \file s_bsd.h
 * \brief A header for the network subsystem.
 * \version $Id$
 */

#ifndef INCLUDED_s_bsd_h
#define INCLUDED_s_bsd_h

#include "fdlist.h"

/* Type of IO */
enum
{
  COMM_SELECT_READ  = 1,
  COMM_SELECT_WRITE = 2
};

/* How long can comm_select() wait for network events [milliseconds] */
enum { SELECT_DELAY = 500 };

struct Client;
struct Listener;

extern void add_connection(struct Listener *, struct irc_ssaddr *, int);
extern void report_error(int, const char *, const char *, int);

extern int get_sockerr(int);
extern int ignoreErrno(int);

extern void comm_settimeout(fde_t *, uintmax_t, void (*)(fde_t *, void *), void *);
extern void comm_setflush(fde_t *, uintmax_t, void (*)(fde_t *, void *), void *);
extern void comm_checktimeouts(void *);
extern void comm_connect_tcp(fde_t *, const char *, unsigned short, struct sockaddr *, int,
                             void (fde_t *, int, void *), void *, int, uintmax_t);
extern const char *comm_errstr(int);
extern int comm_socket(int, int, int);
extern int comm_accept(int, struct irc_ssaddr *);

/* These must be defined in the network IO loop code of your choice */
extern void netio_init(void);
extern void comm_setselect(fde_t *, unsigned int, void (*)(fde_t *, void *), void *, uintmax_t);
extern void comm_select(void);
extern void remove_ipv6_mapping(struct irc_ssaddr *);
#endif /* INCLUDED_s_bsd_h */
