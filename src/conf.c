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

/*! \file conf.c
 * \brief Configuration file functions.
 * \version $Id$
 */

#include "stdinc.h"
#include "list.h"
#include "ircd_defs.h"
#include "parse.h"
#include "conf.h"
#include "conf_cluster.h"
#include "conf_gecos.h"
#include "conf_pseudo.h"
#include "conf_resv.h"
#include "conf_service.h"
#include "conf_shared.h"
#include "server.h"
#include "channel.h"
#include "client.h"
#include "event.h"
#include "irc_string.h"
#include "s_bsd.h"
#include "ircd.h"
#include "listener.h"
#include "hostmask.h"
#include "modules.h"
#include "numeric.h"
#include "fdlist.h"
#include "log.h"
#include "send.h"
#include "memory.h"
#include "res.h"
#include "user.h"
#include "channel_mode.h"
#include "misc.h"
#include "conf_db.h"
#include "conf_class.h"
#include "motd.h"
#include "ipcache.h"
#include "isupport.h"
#include "whowas.h"


struct config_channel_entry ConfigChannel;
struct config_serverhide_entry ConfigServerHide;
struct config_general_entry ConfigGeneral;
struct config_log_entry ConfigLog = { .use_logging = 1 };
struct config_serverinfo_entry ConfigServerInfo;
struct config_admin_entry ConfigAdminInfo;
struct conf_parser_context conf_parser_ctx;

/* general conf items link list root, other than k lines etc. */
dlink_list connect_items;
dlink_list operator_items;

extern unsigned int lineno;
extern char linebuf[];
extern char conffilebuf[IRCD_BUFSIZE];
extern int yyparse(); /* defined in y.tab.c */


/* conf_dns_callback()
 *
 * inputs	- pointer to struct MaskItem
 *		- pointer to DNSReply reply
 * output	- none
 * side effects	- called when resolver query finishes
 * if the query resulted in a successful search, hp will contain
 * a non-null pointer, otherwise hp will be null.
 * if successful save hp in the conf item it was called with
 */
static void
conf_dns_callback(void *vptr, const struct irc_ssaddr *addr, const char *name, size_t namelength)
{
  struct MaskItem *const conf = vptr;

  conf->dns_pending = 0;

  if (addr)
    memcpy(&conf->addr, addr, sizeof(conf->addr));
  else
    conf->dns_failed = 1;
}

/* conf_dns_lookup()
 *
 * do a nameserver lookup of the conf host
 * if the conf entry is currently doing a ns lookup do nothing, otherwise
 * allocate a dns_query and start ns lookup.
 */
static void
conf_dns_lookup(struct MaskItem *conf)
{
  if (conf->dns_pending)
    return;

  conf->dns_pending = 1;

  if (conf->aftype == AF_INET)
    gethost_byname_type(conf_dns_callback, conf, conf->host, T_A);
  else
    gethost_byname_type(conf_dns_callback, conf, conf->host, T_AAAA);
}

/* map_to_list()
 *
 * inputs       - ConfType conf
 * output       - pointer to dlink_list to use
 * side effects - none
 */
static dlink_list *
map_to_list(enum maskitem_type type)
{
  switch (type)
  {
    case CONF_OPER:
      return &operator_items;
      break;
    case CONF_SERVER:
      return &connect_items;
      break;
    default:
      return NULL;
  }
}

struct MaskItem *
conf_make(enum maskitem_type type)
{
  struct MaskItem *const conf = xcalloc(sizeof(*conf));
  dlink_list *list = NULL;

  conf->type   = type;
  conf->active = 1;
  conf->aftype = AF_INET;

  if ((list = map_to_list(type)))
    dlinkAdd(conf, &conf->node, list);
  return conf;
}

void
conf_free(struct MaskItem *conf)
{
  dlink_node *node = NULL, *node_next = NULL;
  dlink_list *list = NULL;

  if ((list = map_to_list(conf->type)))
    dlinkFindDelete(list, conf);

  xfree(conf->name);

  if (conf->dns_pending)
    delete_resolver_queries(conf);
  if (conf->passwd)
    memset(conf->passwd, 0, strlen(conf->passwd));
  if (conf->spasswd)
    memset(conf->spasswd, 0, strlen(conf->spasswd));

  conf->class = NULL;

  xfree(conf->passwd);
  xfree(conf->spasswd);
  xfree(conf->reason);
  xfree(conf->certfp);
  xfree(conf->whois);
  xfree(conf->user);
  xfree(conf->host);
  xfree(conf->cipher_list);

  DLINK_FOREACH_SAFE(node, node_next, conf->hub_list.head)
  {
    xfree(node->data);
    dlinkDelete(node, &conf->hub_list);
    free_dlink_node(node);
  }

  DLINK_FOREACH_SAFE(node, node_next, conf->leaf_list.head)
  {
    xfree(node->data);
    dlinkDelete(node, &conf->leaf_list);
    free_dlink_node(node);
  }

  xfree(conf);
}

/* attach_iline()
 *
 * inputs       - client pointer
 *              - conf pointer
 * output       -
 * side effects - do actual attach
 */
static int
attach_iline(struct Client *client_p, struct MaskItem *conf)
{
  const struct ClassItem *const class = conf->class;
  int a_limit_reached = 0;

  struct ip_entry *ipcache = ipcache_record_find_or_add(&client_p->ip);
  ++ipcache->count_local;
  AddFlag(client_p, FLAGS_IPHASH);

  if (class->max_total && class->ref_count >= class->max_total)
    a_limit_reached = 1;
  else if (class->max_perip_local && ipcache->count_local > class->max_perip_local)
    a_limit_reached = 1;
  else if (class->max_perip_global &&
           (ipcache->count_local + ipcache->count_remote) > class->max_perip_global)
    a_limit_reached = 1;

  if (a_limit_reached)
  {
    if (!IsConfExemptLimits(conf))
      return TOO_MANY;   /* Already at maximum allowed */

    sendto_one_notice(client_p, &me, ":*** Your connection class is full, "
                      "but you have exceed_limit = yes;");
  }

  return conf_attach(client_p, conf);
}

/* verify_access()
 *
 * inputs       - pointer to client to verify
 * output       - 0 if success -'ve if not
 * side effect  - find the first (best) I line to attach.
 */
static int
verify_access(struct Client *client_p)
{
  struct MaskItem *conf = NULL;

  if (HasFlag(client_p, FLAGS_GOTID))
  {
    conf = find_address_conf(client_p->host, client_p->username,
                             &client_p->ip,
                             client_p->ip.ss.ss_family,
                             client_p->connection->password);
  }
  else
  {
    char non_ident[USERLEN + 1] = "~";

    strlcpy(non_ident + 1, client_p->username, sizeof(non_ident) - 1);
    conf = find_address_conf(client_p->host, non_ident,
                             &client_p->ip,
                             client_p->ip.ss.ss_family,
                             client_p->connection->password);
  }

  if (!conf)
    return NOT_AUTHORIZED;

  assert(IsConfClient(conf) || IsConfKill(conf));

  if (IsConfClient(conf))
  {
    if (IsConfRedir(conf))
    {
      sendto_one_numeric(client_p, &me, RPL_REDIR,
                         conf->name ? conf->name : "",
                         conf->port);
      return NOT_AUTHORIZED;
    }

    if (IsConfDoSpoofIp(conf))
    {
      if (IsConfSpoofNotice(conf))
        sendto_realops_flags(UMODE_SERVNOTICE, L_ADMIN, SEND_NOTICE, "%s spoofing: %s as %s",
                             client_p->name, client_p->host, conf->name);

      strlcpy(client_p->host, conf->name, sizeof(client_p->host));
    }

    return attach_iline(client_p, conf);
  }

  sendto_one_notice(client_p, &me, ":*** Banned: %s", conf->reason);
  return BANNED_CLIENT;
}

/* check_client()
 *
 * inputs	- pointer to client
 * output	- 0 = Success
 * 		  NOT_AUTHORIZED    (-1) = Access denied (no I line match)
 * 		  IRCD_SOCKET_ERROR (-2) = Bad socket.
 * 		  I_LINE_FULL       (-3) = I-line is full
 *		  TOO_MANY          (-4) = Too many connections from hostname
 * 		  BANNED_CLIENT     (-5) = K-lined
 * side effects - Ordinary client access check.
 *		  Look for conf lines which have the same
 * 		  status as the flags passed.
 */
int
check_client(struct Client *source_p)
{
  int i;

  if ((i = verify_access(source_p)))
    ilog(LOG_TYPE_IRCD, "Access denied: %s[%s]",
         source_p->name, source_p->sockhost);

  switch (i)
  {
    case TOO_MANY:
      sendto_realops_flags(UMODE_FULL, L_ALL, SEND_NOTICE,
                           "Too many on IP for %s (%s).",
                           client_get_name(source_p, SHOW_IP),
                           source_p->sockhost);
      ilog(LOG_TYPE_IRCD, "Too many connections on IP from %s.",
           client_get_name(source_p, SHOW_IP));
      ++ServerStats.is_ref;
      exit_client(source_p, "No more connections allowed on that IP");
      break;

    case I_LINE_FULL:
      sendto_realops_flags(UMODE_FULL, L_ALL, SEND_NOTICE,
                           "auth {} block is full for %s (%s).",
                           client_get_name(source_p, SHOW_IP),
                           source_p->sockhost);
      ilog(LOG_TYPE_IRCD, "Too many connections from %s.",
           client_get_name(source_p, SHOW_IP));
      ++ServerStats.is_ref;
      exit_client(source_p, "No more connections allowed in your connection class");
      break;

    case NOT_AUTHORIZED:
      /* jdc - lists server name & port connections are on */
      /*       a purely cosmetical change */
      sendto_realops_flags(UMODE_UNAUTH, L_ALL, SEND_NOTICE,
                           "Unauthorized client connection from %s on [%s/%u].",
                           client_get_name(source_p, SHOW_IP),
                           source_p->connection->listener->name,
                           source_p->connection->listener->port);
      ilog(LOG_TYPE_IRCD, "Unauthorized client connection from %s on [%s/%u].",
           client_get_name(source_p, SHOW_IP),
           source_p->connection->listener->name,
           source_p->connection->listener->port);

      ++ServerStats.is_ref;
      exit_client(source_p, "You are not authorized to use this server");
      break;

    case BANNED_CLIENT:
      ++ServerStats.is_ref;
      exit_client(source_p, "Banned");
      break;

    case 0:
    default:
      break;
  }

  return !(i < 0);
}

/*! \brief Disassociate configuration from the client. Also removes a class
 *         from the list if marked for deleting.
 * \param client_p Client to operate on
 * \param type     Type of conf to detach
 */
void
conf_detach(struct Client *client_p, enum maskitem_type type)
{
  dlink_node *node, *node_next;

  DLINK_FOREACH_SAFE(node, node_next, client_p->connection->confs.head)
  {
    struct MaskItem *conf = node->data;

    assert(conf->type & (CONF_CLIENT | CONF_OPER | CONF_SERVER));
    assert(conf->ref_count > 0);
    assert(conf->class->ref_count > 0);

    if (!(conf->type & type))
      continue;

    dlinkDelete(node, &client_p->connection->confs);
    free_dlink_node(node);

    if (conf->type == CONF_CLIENT)
      remove_from_cidr_check(&client_p->ip, conf->class);

    if (--conf->class->ref_count == 0 && conf->class->active == 0)
    {
      class_free(conf->class);
      conf->class = NULL;
    }

    if (--conf->ref_count == 0 && conf->active == 0)
      conf_free(conf);
  }
}

/*! \brief Associate a specific configuration entry to a *local* client (this
 *         is the one which used in accepting the connection). Note, that this
 *         automatically changes the attachment if there was an old one.
 * \param client_p Client to attach the conf to
 * \param conf Configuration record to attach
 */
int
conf_attach(struct Client *client_p, struct MaskItem *conf)
{
  if (dlinkFind(&client_p->connection->confs, conf))
    return 1;

  if (conf->type == CONF_CLIENT)
    if (cidr_limit_reached(IsConfExemptLimits(conf),
                           &client_p->ip, conf->class))
      return TOO_MANY;    /* Already at maximum allowed */

  conf->class->ref_count++;
  conf->ref_count++;

  dlinkAdd(conf, make_dlink_node(), &client_p->connection->confs);

  return 0;
}

/* find_conf_name()
 *
 * inputs	- pointer to conf link list to search
 *		- pointer to name to find
 *		- int mask of type of conf to find
 * output	- NULL or pointer to conf found
 * side effects	- find a conf entry which matches the name
 *		  and has the given mask.
 */
struct MaskItem *
find_conf_name(dlink_list *list, const char *name, enum maskitem_type type)
{
  dlink_node *node = NULL;

  DLINK_FOREACH(node, list->head)
  {
    struct MaskItem *conf = node->data;

    if (conf->type == type)
    {
      if (conf->name && !irccmp(conf->name, name))
        return conf;
    }
  }

  return NULL;
}

/*! \brief Find a connect {} conf that has a name that matches \a name.
 * \param name Name to match
 * \param compare Pointer to function to be used for string matching
 */
struct MaskItem *
connect_find(const char *name, int (*compare)(const char *, const char *))
{
  dlink_node *node;

  DLINK_FOREACH(node, connect_items.head)
  {
    struct MaskItem *conf = node->data;

    if (!compare(name, conf->name))
      return conf;
  }

  return NULL;
}

/* find_exact_name_conf()
 *
 * inputs       - type of link list to look in
 *		- pointer to name string to find
 *		- pointer to user
 *		- pointer to host
 * output       - NULL or pointer to found struct MaskItem
 * side effects - looks for an exact match on name field
 */
struct MaskItem *
operator_find(const struct Client *who, const char *name)
{
  dlink_node *node = NULL;

  DLINK_FOREACH(node, operator_items.head)
  {
    struct MaskItem *conf = node->data;

    if (!irccmp(conf->name, name))
    {
      if (!who)
        return conf;

      if (!match(conf->user, who->username))
      {
        switch (conf->htype)
        {
          case HM_HOST:
            if (!match(conf->host, who->host) || !match(conf->host, who->sockhost))
              if (!conf->class->max_total || conf->class->ref_count < conf->class->max_total)
                return conf;
            break;
          case HM_IPV4:
            if (who->ip.ss.ss_family == AF_INET)
              if (match_ipv4(&who->ip, &conf->addr, conf->bits))
                if (!conf->class->max_total || conf->class->ref_count < conf->class->max_total)
                  return conf;
            break;
          case HM_IPV6:
            if (who->ip.ss.ss_family == AF_INET6)
              if (match_ipv6(&who->ip, &conf->addr, conf->bits))
                if (!conf->class->max_total || conf->class->ref_count < conf->class->max_total)
                  return conf;
            break;
          default:
            assert(0);
        }
      }
    }
  }

  return NULL;
}

/* set_default_conf()
 *
 * inputs	- NONE
 * output	- NONE
 * side effects	- Set default values here.
 *		  This is called **PRIOR** to parsing the
 *		  configuration file.  If you want to do some validation
 *		  of values later, put them in validate_conf().
 */
static void
set_default_conf(void)
{
  /* verify init_class() ran, this should be an unnecessary check
   * but its not much work.
   */
  assert(class_default == class_get_list()->tail->data);

  ConfigServerInfo.network_name = xstrdup(NETWORK_NAME_DEFAULT);
  ConfigServerInfo.network_desc = xstrdup(NETWORK_DESC_DEFAULT);

  memset(&ConfigServerInfo.ip, 0, sizeof(ConfigServerInfo.ip));
  ConfigServerInfo.specific_ipv4_vhost = 0;
  memset(&ConfigServerInfo.ip6, 0, sizeof(ConfigServerInfo.ip6));
  ConfigServerInfo.specific_ipv6_vhost = 0;

  ConfigServerInfo.default_max_clients = MAXCLIENTS_MAX;
  ConfigServerInfo.max_nick_length = 9;
  ConfigServerInfo.max_topic_length = 80;
  ConfigServerInfo.hub = 0;

  log_iterate(log_free);

  ConfigLog.use_logging = 1;

  ConfigChannel.disable_fake_channels = 0;
  ConfigChannel.invite_client_count = 10;
  ConfigChannel.invite_client_time = 300;
  ConfigChannel.invite_delay_channel = 5;
  ConfigChannel.invite_expire_time = 1800;
  ConfigChannel.knock_client_count = 1;
  ConfigChannel.knock_client_time = 300;
  ConfigChannel.knock_delay_channel = 60;
  ConfigChannel.max_channels = 25;
  ConfigChannel.max_invites = 20;
  ConfigChannel.max_bans = 100;
  ConfigChannel.max_bans_large = 500;
  ConfigChannel.default_join_flood_count = 18;
  ConfigChannel.default_join_flood_time = 6;

  ConfigServerHide.flatten_links = 0;
  ConfigServerHide.flatten_links_delay = 300;
  ConfigServerHide.hidden = 0;
  ConfigServerHide.hide_servers = 0;
  ConfigServerHide.hide_services = 0;
  ConfigServerHide.hidden_name = xstrdup(NETWORK_NAME_DEFAULT);
  ConfigServerHide.hide_server_ips = 0;
  ConfigServerHide.disable_remote_commands = 0;

  ConfigGeneral.away_count = 2;
  ConfigGeneral.away_time = 10;
  ConfigGeneral.max_watch = 50;
  ConfigGeneral.whowas_history_length = 15000;
  ConfigGeneral.cycle_on_host_change = 1;
  ConfigGeneral.dline_min_cidr = 16;
  ConfigGeneral.dline_min_cidr6 = 48;
  ConfigGeneral.kline_min_cidr = 16;
  ConfigGeneral.kline_min_cidr6 = 48;
  ConfigGeneral.invisible_on_connect = 1;
  ConfigGeneral.tkline_expire_notices = 1;
  ConfigGeneral.ignore_bogus_ts = 0;
  ConfigGeneral.disable_auth = 0;
  ConfigGeneral.kill_chase_time_limit = 90;
  ConfigGeneral.default_floodcount = 8;
  ConfigGeneral.default_floodtime = 1;
  ConfigGeneral.failed_oper_notice = 1;
  ConfigGeneral.dots_in_ident = 0;
  ConfigGeneral.min_nonwildcard = 4;
  ConfigGeneral.min_nonwildcard_simple = 3;
  ConfigGeneral.max_accept = 50;
  ConfigGeneral.anti_nick_flood = 0;
  ConfigGeneral.max_nick_time = 20;
  ConfigGeneral.max_nick_changes = 5;
  ConfigGeneral.anti_spam_exit_message_time = 0;
  ConfigGeneral.ts_warn_delta = 30;
  ConfigGeneral.ts_max_delta = 600;
  ConfigGeneral.warn_no_connect_block = 1;
  ConfigGeneral.stats_e_disabled = 0;
  ConfigGeneral.stats_i_oper_only = 1;  /* 1 = masked */
  ConfigGeneral.stats_k_oper_only = 1;  /* 1 = masked */
  ConfigGeneral.stats_o_oper_only = 1;
  ConfigGeneral.stats_m_oper_only = 1;
  ConfigGeneral.stats_P_oper_only = 0;
  ConfigGeneral.stats_u_oper_only = 0;
  ConfigGeneral.caller_id_wait = 60;
  ConfigGeneral.opers_bypass_callerid = 1;
  ConfigGeneral.pace_wait = 10;
  ConfigGeneral.pace_wait_simple = 1;
  ConfigGeneral.short_motd = 0;
  ConfigGeneral.ping_cookie = 0;
  ConfigGeneral.no_oper_flood = 0;
  ConfigGeneral.max_targets = MAX_TARGETS_DEFAULT;
  ConfigGeneral.oper_only_umodes = UMODE_DEBUG | UMODE_LOCOPS | UMODE_HIDDEN | UMODE_FARCONNECT |
                                   UMODE_UNAUTH | UMODE_EXTERNAL | UMODE_BOTS | UMODE_NCHANGE |
                                   UMODE_SPY | UMODE_FULL | UMODE_SKILL | UMODE_REJ | UMODE_CCONN;
  ConfigGeneral.oper_umodes = UMODE_BOTS | UMODE_LOCOPS | UMODE_SERVNOTICE | UMODE_WALLOP;
  ConfigGeneral.throttle_count = 1;
  ConfigGeneral.throttle_time = 1;
}

static void
validate_conf(void)
{
  if (EmptyString(ConfigServerInfo.network_name))
    ConfigServerInfo.network_name = xstrdup(NETWORK_NAME_DEFAULT);

  if (EmptyString(ConfigServerInfo.network_desc))
    ConfigServerInfo.network_desc = xstrdup(NETWORK_DESC_DEFAULT);
}

/* read_conf()
 *
 * inputs       - file descriptor pointing to config file to use
 * output       - None
 * side effects	- Read configuration file.
 */
static void
read_conf(FILE *file)
{
  lineno = 1;

  set_default_conf();  /* Set default values prior to conf parsing */
  conf_parser_ctx.pass = 1;
  yyparse();  /* Pick up the classes first */

  rewind(file);

  conf_parser_ctx.pass = 2;
  yyparse();  /* Load the values from the conf */
  validate_conf();  /* Check to make sure some values are still okay. */
                    /* Some global values are also loaded here. */
  whowas_trim();  /* Attempt to trim whowas list if necessary */
  class_delete_marked();  /* Delete unused classes that are marked for deletion */
}

/* conf_rehash()
 *
 * Actual REHASH service routine. Called with sig == 0 if it has been called
 * as a result of an operator issuing this command, else assume it has been
 * called as a result of the server receiving a HUP signal.
 */
void
conf_rehash(int sig)
{
  if (sig)
  {
    sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                         "Got signal SIGHUP, reloading configuration file(s)");
    ilog(LOG_TYPE_IRCD, "Got signal SIGHUP, reloading configuration file(s)");
  }

  restart_resolver();

  /* don't close listeners until we know we can go ahead with the rehash */

  read_conf_files(0);

  load_conf_modules();
  check_conf_klines();
}

/* lookup_confhost()
 *
 * start DNS lookups of all hostnames in the conf
 * line and convert an IP addresses in a.b.c.d number for to IP#s.
 */
void
lookup_confhost(struct MaskItem *conf)
{
  struct addrinfo hints, *res;

  /*
   * Do name lookup now on hostnames given and store the
   * ip numbers in conf structure.
   */
  memset(&hints, 0, sizeof(hints));

  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  /* Get us ready for a bind() and don't bother doing dns lookup */
  hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

  if (getaddrinfo(conf->host, NULL, &hints, &res))
  {
    conf_dns_lookup(conf);
    return;
  }

  assert(res);

  memcpy(&conf->addr, res->ai_addr, res->ai_addrlen);
  conf->addr.ss_len = res->ai_addrlen;
  conf->addr.ss.ss_family = res->ai_family;

  freeaddrinfo(res);
}

/* conf_connect_allowed()
 *
 * inputs	- pointer to inaddr
 *		- int type ipv4 or ipv6
 * output	- BANNED or accepted
 * side effects	- none
 */
int
conf_connect_allowed(struct irc_ssaddr *addr, int aftype)
{
  struct ip_entry *ip_found = NULL;
  const struct MaskItem *conf = find_dline_conf(addr, aftype);

  if (conf)
  {
    /* DLINE exempt also gets you out of static limits/pacing... */
    if (conf->type == CONF_EXEMPT)
      return 0;
    return BANNED_CLIENT;
  }

  ip_found = ipcache_record_find_or_add(addr);

  if ((CurrentTime - ip_found->last_attempt) < ConfigGeneral.throttle_time)
  {
    if (ip_found->connection_count >= ConfigGeneral.throttle_count)
      return TOO_FAST;

    ++ip_found->connection_count;
  }
  else
    ip_found->connection_count = 1;

  ip_found->last_attempt = CurrentTime;
  return 0;
}

/* cleanup_tklines()
 *
 * inputs       - NONE
 * output       - NONE
 * side effects - call function to expire temporary k/d lines
 *                This is an event started off in ircd.c
 */
void
cleanup_tklines(void *unused)
{
  hostmask_expire_temporary();
  gecos_expire();
  resv_expire();
}

/* oper_privs_as_string()
 *
 * inputs        - pointer to client_p
 * output        - pointer to static string showing oper privs
 * side effects  - return as string, the oper privs as derived from port
 */
static const struct oper_flags
{
  const unsigned int flag;
  const unsigned char c;
} flag_table[] = {
  { OPER_FLAG_ADMIN,          'A' },
  { OPER_FLAG_CLOSE,          'B' },
  { OPER_FLAG_CONNECT,        'C' },
  { OPER_FLAG_CONNECT_REMOTE, 'D' },
  { OPER_FLAG_DIE,            'E' },
  { OPER_FLAG_DLINE,          'F' },
  { OPER_FLAG_GLOBOPS,        'G' },
  { OPER_FLAG_JOIN_RESV,      'H' },
  { OPER_FLAG_KILL,           'I' },
  { OPER_FLAG_KILL_REMOTE,    'J' },
  { OPER_FLAG_KLINE,          'K' },
  { OPER_FLAG_LOCOPS,         'L' },
  { OPER_FLAG_MODULE,         'M' },
  { OPER_FLAG_NICK_RESV,      'N' },
  { OPER_FLAG_OPME,           'O' },
  { OPER_FLAG_REHASH,         'P' },
  { OPER_FLAG_REMOTEBAN,      'Q' },
  { OPER_FLAG_RESTART,        'R' },
  { OPER_FLAG_RESV,           'S' },
  { OPER_FLAG_SET,            'T' },
  { OPER_FLAG_SQUIT,          'U' },
  { OPER_FLAG_SQUIT_REMOTE,   'V' },
  { OPER_FLAG_UNDLINE,        'W' },
  { OPER_FLAG_UNKLINE,        'X' },
  { OPER_FLAG_UNRESV,         'Y' },
  { OPER_FLAG_UNXLINE,        'Z' },
  { OPER_FLAG_WALLOPS,        'a' },
  { OPER_FLAG_XLINE,          'b' },
  { 0, '\0' }
};

const char *
oper_privs_as_string(const unsigned int flags)
{
  static char buf[sizeof(flag_table) / sizeof(flag_table[0])];
  char *p = buf;

  for (const struct oper_flags *tab = flag_table; tab->flag; ++tab)
    if (flags & tab->flag)
      *p++ = tab->c;

  if (p == buf)
    *p++ = '0';

  *p = '\0';

  return buf;
}

/*
 * Input: A client to find the active operator {} name for.
 * Output: The nick!user@host{oper} of the oper.
 *         "oper" is server name for remote opers
 * Side effects: None.
 */
const char *
get_oper_name(const struct Client *client_p)
{
  static char buffer[IRCD_BUFSIZE];

  if (IsServer(client_p))
    return client_p->name;

  if (MyConnect(client_p))
  {
    const dlink_node *const node = client_p->connection->confs.head;

    if (node)
    {
      const struct MaskItem *const conf = node->data;

      if (conf->type == CONF_OPER)
      {
        snprintf(buffer, sizeof(buffer), "%s!%s@%s{%s}", client_p->name,
                 client_p->username, client_p->host, conf->name);
        return buffer;
      }
    }

    /*
     * Probably should assert here for now. If there is an oper out there
     * with no operator {} conf attached, it would be good for us to know...
     */
    assert(0);  /* Oper without oper conf! */
  }

  snprintf(buffer, sizeof(buffer), "%s!%s@%s{%s}", client_p->name,
           client_p->username, client_p->host, client_p->servptr->name);
  return buffer;
}

/* clear_out_old_conf()
 *
 * inputs       - none
 * output       - none
 * side effects - Clear out the old configuration
 */
static void
clear_out_old_conf(void)
{
  dlink_node *node = NULL, *node_next = NULL;
  dlink_list *free_items [] = {
    &connect_items, &operator_items, NULL
  };

  dlink_list ** iterator = free_items; /* C is dumb */

  /* We only need to free anything allocated by yyparse() here.
   * Resetting structs, etc, is taken care of by set_default_conf().
   */

  for (; *iterator; iterator++)
  {
    DLINK_FOREACH_SAFE(node, node_next, (*iterator)->head)
    {
      struct MaskItem *conf = node->data;

      conf->active = 0;
      dlinkDelete(&conf->node, *iterator);

      if (!conf->ref_count)
        conf_free(conf);
    }
  }

  /*
   * Don't delete the class table, rather mark all entries for deletion.
   * The table is cleaned up by class_delete_marked. - avalon
   */
  class_mark_for_deletion();

  clear_out_address_conf();

  modules_conf_clear();  /* Clear modules {} items */

  motd_clear();  /* Clear motd {} items and re-cache default motd */

  cluster_clear();  /* Clear cluster {} items */

  gecos_clear();  /* Clear gecos {} items */

  resv_clear();  /* Clear resv {} items */

  service_clear();  /* Clear service {} items */

  shared_clear();  /* Clear shared {} items */

  pseudo_clear();  /* Clear pseudo {} items */

  /* Clean out ConfigServerInfo */
  xfree(ConfigServerInfo.description);
  ConfigServerInfo.description = NULL;
  xfree(ConfigServerInfo.network_name);
  ConfigServerInfo.network_name = NULL;
  xfree(ConfigServerInfo.network_desc);
  ConfigServerInfo.network_desc = NULL;
  xfree(ConfigServerInfo.rsa_private_key_file);
  ConfigServerInfo.rsa_private_key_file = NULL;
  xfree(ConfigServerInfo.ssl_certificate_file);
  ConfigServerInfo.ssl_certificate_file = NULL;
  xfree(ConfigServerInfo.ssl_dh_param_file);
  ConfigServerInfo.ssl_dh_param_file = NULL;
  xfree(ConfigServerInfo.ssl_dh_elliptic_curve);
  ConfigServerInfo.ssl_dh_elliptic_curve = NULL;
  xfree(ConfigServerInfo.ssl_cipher_list);
  ConfigServerInfo.ssl_cipher_list = NULL;
  xfree(ConfigServerInfo.ssl_message_digest_algorithm);
  ConfigServerInfo.ssl_message_digest_algorithm = NULL;

  /* Clean out ConfigAdminInfo */
  xfree(ConfigAdminInfo.name);
  ConfigAdminInfo.name = NULL;
  xfree(ConfigAdminInfo.email);
  ConfigAdminInfo.email = NULL;
  xfree(ConfigAdminInfo.description);
  ConfigAdminInfo.description = NULL;

  /* Clean out ConfigServerHide */
  xfree(ConfigServerHide.flatten_links_file);
  ConfigServerHide.flatten_links_file = NULL;
  xfree(ConfigServerHide.hidden_name);
  ConfigServerHide.hidden_name = NULL;

  /* Clean out listeners */
  listener_close_marked();
}

static void
conf_handle_tls(int cold)
{
  if (!tls_new_cred())
  {
    if (cold)
    {
      ilog(LOG_TYPE_IRCD, "Error while initializing TLS");
      exit(EXIT_FAILURE);
    }
    else
    {
      /* Failed to load new settings/certs, old ones remain active */
      sendto_realops_flags(UMODE_SERVNOTICE, L_ALL, SEND_NOTICE,
                           "Error reloading TLS settings, check the ircd log"); // report_crypto_errors logs this
    }
  }
}

/* read_conf_files()
 *
 * inputs       - cold start YES or NO
 * output       - none
 * side effects - read all conf files needed, ircd.conf kline.conf etc.
 */
void
read_conf_files(int cold)
{
  const char *filename = NULL;
  char chanmodes[IRCD_BUFSIZE] = "";
  char chanlimit[IRCD_BUFSIZE] = "";

  conf_parser_ctx.boot = cold;
  filename = ConfigGeneral.configfile;

  /* We need to know the initial filename for the yyerror() to report
     FIXME: The full path is in conffilenamebuf first time since we
             don't know anything else

     - Gozem 2002-07-21
  */
  strlcpy(conffilebuf, filename, sizeof(conffilebuf));

  if ((conf_parser_ctx.conf_file = fopen(filename, "r")) == NULL)
  {
    if (cold)
    {
      ilog(LOG_TYPE_IRCD, "Unable to read configuration file '%s': %s",
           filename, strerror(errno));
      exit(EXIT_FAILURE);
    }
    else
    {
      sendto_realops_flags(UMODE_SERVNOTICE, L_ADMIN, SEND_NOTICE,
                           "Unable to read configuration file '%s': %s",
                           filename, strerror(errno));
      return;
    }
  }

  if (!cold)
    clear_out_old_conf();

  read_conf(conf_parser_ctx.conf_file);
  fclose(conf_parser_ctx.conf_file);

  log_iterate(log_reopen);
  conf_handle_tls(cold);

  isupport_add("NICKLEN", NULL, ConfigServerInfo.max_nick_length);
  isupport_add("NETWORK", ConfigServerInfo.network_name, -1);

  snprintf(chanmodes, sizeof(chanmodes), "beI:%u", ConfigChannel.max_bans);
  isupport_add("MAXLIST", chanmodes, -1);
  isupport_add("MAXTARGETS", NULL, ConfigGeneral.max_targets);
  isupport_add("CHANTYPES", "#", -1);

  snprintf(chanlimit, sizeof(chanlimit), "#:%u",
           ConfigChannel.max_channels);
  isupport_add("CHANLIMIT", chanlimit, -1);
  snprintf(chanmodes, sizeof(chanmodes), "%s", "beI,k,l,cimnprstuCLMNORST");
  isupport_add("CHANNELLEN", NULL, CHANNELLEN);
  isupport_add("TOPICLEN", NULL, ConfigServerInfo.max_topic_length);
  isupport_add("CHANMODES", chanmodes, -1);
}

/* conf_add_class_to_conf()
 *
 * inputs       - pointer to config item
 * output       - NONE
 * side effects - Add a class pointer to a conf
 */
void
conf_add_class_to_conf(struct MaskItem *conf, const char *name)
{
  if (EmptyString(name) || (conf->class = class_find(name, 1)) == NULL)
  {
    conf->class = class_default;

    if (conf->type == CONF_CLIENT || conf->type == CONF_OPER)
      sendto_realops_flags(UMODE_SERVNOTICE, L_ADMIN, SEND_NOTICE,
                           "Warning *** Defaulting to default class for %s@%s",
                           conf->user, conf->host);
    else
      sendto_realops_flags(UMODE_SERVNOTICE, L_ADMIN, SEND_NOTICE,
                           "Warning *** Defaulting to default class for %s",
                           conf->name);
  }
}

/* yyerror()
 *
 * inputs	- message from parser
 * output	- NONE
 * side effects	- message to opers and log file entry is made
 */
void
yyerror(const char *msg)
{
  if (conf_parser_ctx.pass != 1)
    return;

  const char *p = stripws(linebuf);
  sendto_realops_flags(UMODE_SERVNOTICE, L_ADMIN, SEND_NOTICE,
                       "\"%s\", line %u: %s: %s",
                       conffilebuf, lineno, msg, p);
  ilog(LOG_TYPE_IRCD, "\"%s\", line %u: %s: %s",
       conffilebuf, lineno, msg, p);
}

void
conf_error_report(const char *msg)
{
  const char *p = stripws(linebuf);
  sendto_realops_flags(UMODE_SERVNOTICE, L_ADMIN, SEND_NOTICE,
                       "\"%s\", line %u: %s: %s",
                       conffilebuf, lineno, msg, p);
  ilog(LOG_TYPE_IRCD, "\"%s\", line %u: %s: %s",
       conffilebuf, lineno, msg, p);
}

/*
 * valid_tkline()
 *
 * inputs       - pointer to ascii string to check
 *              - whether the specified time is in seconds or minutes
 * output       - -1 not enough parameters
 *              - 0 if not an integer number, else the number
 * side effects - none
 * Originally written by Dianora (Diane, db@db.net)
 */
uintmax_t
valid_tkline(const char *data, const int minutes)
{
  const unsigned char *p = (const unsigned char *)data;
  unsigned char tmpch = '\0';
  uintmax_t result = 0;

  while ((tmpch = *p++))
  {
    if (!IsDigit(tmpch))
      return 0;

    result *= 10;
    result += (tmpch & 0xF);
  }

  /*
   * In the degenerate case where oper does a /quote kline 0 user@host :reason
   * i.e. they specifically use 0, I am going to return 1 instead as a return
   * value of non-zero is used to flag it as a temporary kline
   */
  if (result == 0)
    result = 1;

  /*
   * If the incoming time is in seconds convert it to minutes for the purpose
   * of this calculation
   */
  if (minutes == 0)
    result = result / 60;

  if (result > MAX_TDKLINE_TIME)
    result = MAX_TDKLINE_TIME;

  result = result * 60;  /* Turn it into seconds */

  return result;
}

/* valid_wild_card_simple()
 *
 * inputs       - data to check for sufficient non-wildcard characters
 * outputs      - 1 if valid, else 0
 * side effects - none
 */
int
valid_wild_card_simple(const char *data)
{
  const unsigned char *p = (const unsigned char *)data;
  unsigned char tmpch = '\0';
  unsigned int nonwild = 0, wild = 0;

  while ((tmpch = *p++))
  {
    if (tmpch == '\\' && *p)
    {
      ++p;
      if (++nonwild >= ConfigGeneral.min_nonwildcard_simple)
        return 1;
    }
    else if (!IsMWildChar(tmpch))
    {
      if (++nonwild >= ConfigGeneral.min_nonwildcard_simple)
        return 1;
    }
    else
      ++wild;
  }

  return !wild;
}

/* valid_wild_card()
 *
 * input        - pointer to client
 *		- int flag, 0 for no warning oper 1 for warning oper
 *		- count of following varargs to check
 * output       - 0 if not valid, 1 if valid
 * side effects - NOTICE is given to source_p if warn is 1
 */
int
valid_wild_card(int count, ...)
{
  unsigned char tmpch = '\0';
  unsigned int nonwild = 0;
  va_list args;

  /*
   * Now we must check the user and host to make sure there
   * are at least NONWILDCHARS non-wildcard characters in
   * them, otherwise assume they are attempting to kline
   * *@* or some variant of that. This code will also catch
   * people attempting to kline *@*.tld, as long as NONWILDCHARS
   * is greater than 3. In that case, there are only 3 non-wild
   * characters (tld), so if NONWILDCHARS is 4, the kline will
   * be disallowed.
   * -wnder
   */

  va_start(args, count);

  while (count--)
  {
    const unsigned char *p = va_arg(args, const unsigned char *);
    if (p == NULL)
      continue;

    while ((tmpch = *p++))
    {
      if (!IsKWildChar(tmpch))
      {
        /*
         * If we find enough non-wild characters, we can
         * break - no point in searching further.
         */
        if (++nonwild >= ConfigGeneral.min_nonwildcard)
        {
          va_end(args);
          return 1;
        }
      }
    }
  }

  va_end(args);
  return 0;
}

/* find_user_host()
 *
 * inputs       - pointer to client placing kline
 *              - pointer to user_host_or_nick
 *              - pointer to user buffer
 *              - pointer to host buffer
 * output       - 0 if not ok to kline, 1 to kline i.e. if valid user host
 * side effects -
 */
static int
find_user_host(struct Client *source_p, char *user_host_or_nick,
               char *luser, char *lhost)
{
  struct Client *target_p = NULL;
  char *hostp = NULL;

  if (lhost == NULL)
  {
    strlcpy(luser, user_host_or_nick, USERLEN*4 + 1);
    return 1;
  }

  if ((hostp = strchr(user_host_or_nick, '@')) || *user_host_or_nick == '*')
  {
    /* Explicit user@host mask given */
    if (hostp)                            /* I'm a little user@host */
    {
      *(hostp++) = '\0';                       /* short and squat */

      if (*user_host_or_nick)
        strlcpy(luser, user_host_or_nick, USERLEN*4 + 1); /* here is my user */
      else
        strcpy(luser, "*");

      if (*hostp)
        strlcpy(lhost, hostp, HOSTLEN + 1);    /* here is my host */
      else
        strcpy(lhost, "*");
    }
    else
    {
      luser[0] = '*';             /* no @ found, assume its *@somehost */
      luser[1] = '\0';
      strlcpy(lhost, user_host_or_nick, HOSTLEN*4 + 1);
    }

    return 1;
  }
  else
  {
    /* Try to find user@host mask from nick */
    /* Okay to use source_p as the first param, because source_p == client_p */
    if ((target_p =
        find_chasing(source_p, user_host_or_nick)) == NULL)
      return 0;  /* find_chasing sends ERR_NOSUCHNICK */

    if (HasFlag(target_p, FLAGS_EXEMPTKLINE))
    {
      if (IsClient(source_p))
        sendto_one_notice(source_p, &me, ":%s is E-lined", target_p->name);
      return 0;
    }

    /*
     * Turn the "user" bit into "*user", blow away '~'
     * if found in original user name (non-idented)
     */
    strlcpy(luser, target_p->username, USERLEN*4 + 1);

    if (target_p->username[0] == '~')
      luser[0] = '*';

    strlcpy(lhost, target_p->sockhost, HOSTLEN*4 + 1);
    return 1;
  }

  return 0;
}

/* XXX should this go into a separate file ? -Dianora */
/* parse_aline
 *
 * input        - pointer to cmd name being used
 *              - pointer to client using cmd
 *              - parc parameter count
 *              - parv[] list of parameters to parse
 *		- parse_flags bit map of things to test
 *		- pointer to user or string to parse into
 *              - pointer to host or NULL to parse into if non NULL
 *              - pointer to optional tkline time or NULL
 *              - pointer to target_server to parse into if non NULL
 *              - pointer to reason to parse into
 *
 * output       - 1 if valid, 0 if not valid
 * side effects - A generalised k/d/x etc. line parser,
 *               "ALINE [time] user@host|string [ON] target :reason"
 *                will parse returning a parsed user, host if
 *                h_p pointer is non NULL, string otherwise.
 *                if tkline_time pointer is non NULL a tk line will be set
 *                to non zero if found.
 *                if tkline_time pointer is NULL and tk line is found,
 *                error is reported.
 *                if target_server is NULL and an "ON" is found error
 *                is reported.
 *                if reason pointer is NULL ignore pointer,
 *                this allows use of parse_a_line in unkline etc.
 *
 * - Dianora
 */
int
parse_aline(const char *cmd, struct Client *source_p,
            int parc, char **parv,
            char **up_p, char **h_p, uintmax_t *tkline_time,
            char **target_server, char **reason)
{
  uintmax_t found_tkline_time=0;
  static char default_reason[] = CONF_NOREASON;
  static char user[USERLEN*4+1];
  static char host[HOSTLEN*4+1];

  parv++;
  parc--;

  found_tkline_time = valid_tkline(*parv, TK_MINUTES);

  if (found_tkline_time)
  {
    parv++;
    parc--;

    if (tkline_time)
      *tkline_time = found_tkline_time;
    else
    {
      sendto_one_notice(source_p, &me, ":temp_line not supported by %s", cmd);
      return 0;
    }
  }

  if (parc == 0)
  {
    sendto_one_numeric(source_p, &me, ERR_NEEDMOREPARAMS, cmd);
    return 0;
  }

  if (h_p == NULL)
    *up_p = *parv;
  else
  {
    if (find_user_host(source_p, *parv, user, host) == 0)
      return 0;

    *up_p = user;
    *h_p = host;
  }

  parc--;
  parv++;

  if (parc)
  {
    if (irccmp(*parv, "ON") == 0)
    {
      parc--;
      parv++;

      if (!HasOFlag(source_p, OPER_FLAG_REMOTEBAN))
      {
        sendto_one_numeric(source_p, &me, ERR_NOPRIVS, "remoteban");
        return 0;
      }

      if (parc == 0 || EmptyString(*parv))
      {
        sendto_one_numeric(source_p, &me, ERR_NEEDMOREPARAMS, cmd);
        return 0;
      }

      *target_server = *parv;
      parc--;
      parv++;
    }
    else
    {
      /* Make sure target_server *is* NULL if no ON server found
       * caller probably NULL'd it first, but no harm to do it again -db
       */
      if (target_server)
        *target_server = NULL;
    }
  }

  if (reason)
  {
    if (parc && !EmptyString(*parv))
      *reason = *parv;
    else
      *reason = default_reason;
  }

  return 1;
}

/* match_conf_password()
 *
 * inputs       - pointer to given password
 *              - pointer to Conf
 * output       - 1 or 0 if match
 * side effects - none
 */
int
match_conf_password(const char *password, const struct MaskItem *conf)
{
  const char *encr = NULL;

  if (EmptyString(password) || EmptyString(conf->passwd))
    return 0;

  if (conf->flags & CONF_FLAGS_ENCRYPTED)
    encr = crypt(password, conf->passwd);
  else
    encr = password;

  return encr && !strcmp(encr, conf->passwd);
}

/*
 * split_nuh
 *
 * inputs	- pointer to original mask (modified in place)
 *		- pointer to pointer where nick should go
 *		- pointer to pointer where user should go
 *		- pointer to pointer where host should go
 * output	- NONE
 * side effects	- mask is modified in place
 *		  If nick pointer is NULL, ignore writing to it
 *		  this allows us to use this function elsewhere.
 *
 * mask				nick	user	host
 * ----------------------	------- ------- ------
 * Dianora!db@db.net		Dianora	db	db.net
 * Dianora			Dianora	*	*
 * db.net                       *       *       db.net
 * OR if nick pointer is NULL
 * Dianora			-	*	Dianora
 * Dianora!			Dianora	*	*
 * Dianora!@			Dianora	*	*
 * Dianora!db			Dianora	db	*
 * Dianora!@db.net		Dianora	*	db.net
 * db@db.net			*	db	db.net
 * !@				*	*	*
 * @				*	*	*
 * !				*	*	*
 */
void
split_nuh(struct split_nuh_item *const iptr)
{
  char *p = NULL, *q = NULL;

  if (iptr->nickptr)
    strlcpy(iptr->nickptr, "*", iptr->nicksize);

  if (iptr->userptr)
    strlcpy(iptr->userptr, "*", iptr->usersize);

  if (iptr->hostptr)
    strlcpy(iptr->hostptr, "*", iptr->hostsize);

  if ((p = strchr(iptr->nuhmask, '!')))
  {
    *p = '\0';

    if (iptr->nickptr && *iptr->nuhmask)
      strlcpy(iptr->nickptr, iptr->nuhmask, iptr->nicksize);

    if ((q = strchr(++p, '@')))
    {
      *q++ = '\0';

      if (*p)
        strlcpy(iptr->userptr, p, iptr->usersize);

      if (*q)
        strlcpy(iptr->hostptr, q, iptr->hostsize);
    }
    else
    {
      if (*p)
        strlcpy(iptr->userptr, p, iptr->usersize);
    }
  }
  else
  {
    /* No ! found so lets look for a user@host */
    if ((p = strchr(iptr->nuhmask, '@')))
    {
      /* if found a @ */
      *p++ = '\0';

      if (*iptr->nuhmask)
        strlcpy(iptr->userptr, iptr->nuhmask, iptr->usersize);

      if (*p)
        strlcpy(iptr->hostptr, p, iptr->hostsize);
    }
    else
    {
      /* No @ found */
      if (!iptr->nickptr || strpbrk(iptr->nuhmask, ".:"))
        strlcpy(iptr->hostptr, iptr->nuhmask, iptr->hostsize);
      else
        strlcpy(iptr->nickptr, iptr->nuhmask, iptr->nicksize);
    }
  }
}
