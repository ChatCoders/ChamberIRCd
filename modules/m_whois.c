/*
 *  ircd-ratbox: A slightly useful ircd.
 *  m_whois.c: Shows who a user is.
 *
 *  Copyright (C) 1990 Jarkko Oikarinen and University of Oulu, Co Center
 *  Copyright (C) 1996-2002 Hybrid Development Team
 *  Copyright (C) 2002-2005 ircd-ratbox development team
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 *
 */

#include "stdinc.h"
#include "common.h"
#include "client.h"
#include "hash.h"
#include "channel.h"
#include "hash.h"
#include "ircd.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_serv.h"
#include "send.h"
#include "match.h"
#include "s_conf.h"
#include "logger.h"
#include "msg.h"
#include "parse.h"
#include "modules.h"
#include "hook.h"
#include "s_newconf.h"
#include "s_user.h"

static void do_whois(struct Client *client_p, struct Client *source_p, int parc, const char *parv[]);
static void single_whois(struct Client *source_p, struct Client *target_p, int operspy);

static int m_whois(struct Client *, struct Client *, int, const char **);
static int ms_whois(struct Client *, struct Client *, int, const char **);

struct Message whois_msgtab = {
    "WHOIS", 0, 0, 0, MFLG_SLOW,
    {mg_unreg, {m_whois, 2}, {ms_whois, 2}, mg_ignore, mg_ignore, {m_whois, 2}}
};

int doing_whois_hook;
int doing_whois_global_hook;

mapi_clist_av1 whois_clist[] = { &whois_msgtab, NULL };
mapi_hlist_av1 whois_hlist[] = {
    { "doing_whois",	&doing_whois_hook },
    { "doing_whois_global",	&doing_whois_global_hook },
    { NULL, NULL }
};

DECLARE_MODULE_AV1(whois, NULL, NULL, whois_clist, whois_hlist, NULL, "$Revision: 3536 $");

/*
 * m_whois
 *      parv[1] = nickname masklist
 */
static int
m_whois(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
    static time_t last_used = 0;

    if(parc > 2) {
        if(EmptyString(parv[2])) {
            sendto_one(source_p, form_str(ERR_NONICKNAMEGIVEN),
                       me.name, source_p->name);
            return 0;
        }

        if(!IsOper(source_p)) {
            /* seeing as this is going across servers, we should limit it */
            if((last_used + ConfigFileEntry.pace_wait_simple) > rb_current_time()) {
                sendto_one(source_p, form_str(RPL_LOAD2HI),
                           me.name, source_p->name, "WHOIS");
                sendto_one_numeric(source_p, RPL_ENDOFWHOIS,
                                   form_str(RPL_ENDOFWHOIS), parv[2]);
                return 0;
            } else
                last_used = rb_current_time();
        }

        if(hunt_server(client_p, source_p, ":%s WHOIS %s :%s", 1, parc, parv) !=
           HUNTED_ISME)
            return 0;

        parv[1] = parv[2];

    }
    do_whois(client_p, source_p, parc, parv);

    return 0;
}

/*
 * ms_whois
 *      parv[1] = server to reply
 *      parv[2] = nickname to whois
 */
static int
ms_whois(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
    struct Client *target_p;

    /* note: early versions of ratbox allowed users to issue a remote
     * whois with a blank parv[2], so we cannot treat it as a protocol
     * violation. --anfl
     */
    if(parc < 3 || EmptyString(parv[2])) {
        sendto_one(source_p, form_str(ERR_NONICKNAMEGIVEN),
                   me.name, source_p->name);
        return 0;
    }

    /* check if parv[1] exists */
    if((target_p = find_client(parv[1])) == NULL) {
        sendto_one_numeric(source_p, ERR_NOSUCHSERVER,
                           form_str(ERR_NOSUCHSERVER),
                           IsDigit(parv[1][0]) ? "*" : parv[1]);
        return 0;
    }

    /* if parv[1] isnt my client, or me, someone else is supposed
     * to be handling the request.. so send it to them
     */
    if(!MyClient(target_p) && !IsMe(target_p)) {
        sendto_one(target_p, ":%s WHOIS %s :%s",
                   get_id(source_p, target_p),
                   get_id(target_p, target_p), parv[2]);
        return 0;
    }

    /* ok, the target is either us, or a client on our server, so perform the whois
     * but first, parv[1] == server to perform the whois on, parv[2] == person
     * to whois, so make parv[1] = parv[2] so do_whois is ok -- fl_
     */
    parv[1] = parv[2];
    do_whois(client_p, source_p, parc, parv);

    return 0;
}

/* do_whois
 *
 * inputs	- pointer to
 * output	-
 * side effects -
 */
static void
do_whois(struct Client *client_p, struct Client *source_p, int parc, const char *parv[])
{
    struct Client *target_p;
    char *nick;
    char *p = NULL;
    int operspy = 0;

    nick = LOCAL_COPY(parv[1]);
    if((p = strchr(nick, ',')))
        *p = '\0';

    if(IsOperSpy(source_p) && *nick == '!') {
        operspy = 1;
        nick++;
    }

    if(MyClient(source_p))
        target_p = find_named_person(nick);
    else
        target_p = find_person(nick);

    if(target_p != NULL) {
        if(operspy) {
            char buffer[BUFSIZE];

            rb_snprintf(buffer, sizeof(buffer), "%s!%s@%s %s",
                        target_p->name, target_p->username,
                        target_p->host, target_p->servptr->name);
            report_operspy(source_p, "WHOIS", buffer);
        }

        single_whois(source_p, target_p, operspy);
    } else
        sendto_one_numeric(source_p, ERR_NOSUCHNICK,
                           form_str(ERR_NOSUCHNICK),
                           nick);

    sendto_one_numeric(source_p, RPL_ENDOFWHOIS,
                       form_str(RPL_ENDOFWHOIS), parv[1]);
    return;
}

/*
 * single_whois()
 *
 * Inputs	- source_p client to report to
 *		- target_p client to report on
 * Output	- if found return 1
 * Side Effects	- do a single whois on given client
 * 		  writing results to source_p
 */
static void
single_whois(struct Client *source_p, struct Client *target_p, int operspy)
{
    char buf[BUFSIZE];
    rb_dlink_node *ptr;
    struct membership *msptr;
    struct Channel *chptr;
    int cur_len = 0;
    int mlen;
    char *t;
    int tlen;
    hook_data_client hdata;
    int visible;
    int extra_space = 0;
    int i;
    char *m;
    int showsecret = 0;
    struct Metadata *md;

    if(ConfigFileEntry.secret_channels_in_whois && IsOperSpy(source_p))
        showsecret = 1;

    if(target_p->user == NULL) {
        s_assert(0);
        return;
    }

    sendto_one_numeric(source_p, RPL_WHOISUSER, form_str(RPL_WHOISUSER),
                       target_p->name, target_p->username,
                       target_p->host, target_p->info);

    cur_len = mlen = rb_sprintf(buf, form_str(RPL_WHOISCHANNELS),
                                get_id(&me, source_p), get_id(source_p, source_p),
                                target_p->name);

    /* Make sure it won't overflow when sending it to the client
     * in full names; note that serverhiding may require more space
     * for a different server name (not done here) -- jilles
     */
    if (!MyConnect(source_p)) {
        extra_space = strlen(source_p->name) - 9;
        if (extra_space < 0)
            extra_space = 0;
        extra_space += strlen(me.name) - 2; /* make sure >= 0 */
        cur_len += extra_space;
    }

    t = buf + mlen;

    if (!IsService(target_p) && (!(target_p->umodes & UMODE_HIDECHANS) || IsOper(source_p))) {
        RB_DLINK_FOREACH(ptr, target_p->user->channel.head) {
            msptr = ptr->data;
            chptr = msptr->chptr;

            visible = ShowChannel(source_p, chptr);

            if(visible || operspy || showsecret) {
                if((cur_len + strlen(chptr->chname) + 3) > (BUFSIZE - 5)) {
                    sendto_one(source_p, "%s", buf);
                    cur_len = mlen + extra_space;
                    t = buf + mlen;
                }

                tlen = rb_sprintf(t, "%s%s%s ",
                                  visible ? "" : "*",
                                  find_channel_status(msptr, 1),
                                  chptr->chname);
                t += tlen;
                cur_len += tlen;
            }
        }
    }

    if(cur_len > mlen + extra_space)
        sendto_one(source_p, "%s", buf);

    /* What sense does it make that they cant see the own server they are on?
    */
    if(SeesServers(source_p) || source_p == target_p) {
        sendto_one_numeric(source_p, RPL_WHOISSERVER, form_str(RPL_WHOISSERVER),
                           target_p->name, target_p->servptr->name,
                           target_p->servptr->info);
    } else {
        sendto_one_numeric(source_p, RPL_WHOISSERVER, form_str(RPL_WHOISSERVER),
                           target_p->name, source_p->servptr->mname,
                           source_p->servptr->info);
    }

    if(target_p->user->away)
        sendto_one_numeric(source_p, RPL_AWAY, form_str(RPL_AWAY),
                           target_p->name, target_p->user->away);

    /* added UMODE_HIDEOPER, idea suggested by Matt on irc.thudsnet.us
     * rather than the config hide oper option which would hide all IRC operators.
     * each irc operator can decide if they wish to hide rather than forced
     */

    if (IsOper(target_p) && (!(target_p->umodes & UMODE_HIDEOPER) || IsOper(source_p))) {
        if((md = user_metadata_find(target_p, "OPERSTRING")) != NULL)
            sendto_one_numeric(source_p, 313, "%s :%s", target_p->name, md->value);
        else
            sendto_one_numeric(source_p, RPL_WHOISOPERATOR, form_str(RPL_WHOISOPERATOR),
                               target_p->name,
                               IsService(target_p) ? ConfigFileEntry.servicestring :
                               (IsAdmin(target_p) ? GlobalSetOptions.adminstring :
                                GlobalSetOptions.operstring));

        /* Show the oper block name and the privset name used. */
        if(MyClient(target_p) && !EmptyString(target_p->localClient->opername) &&
           ((source_p == target_p) || IsOper(source_p) || IsOperAdmin(source_p))) {
            char buf[BUFSIZE];
            rb_snprintf(buf, sizeof(buf), "is opered as %s",
                        target_p->localClient->opername);
            sendto_one_numeric(source_p, RPL_WHOISSPECIAL, form_str(RPL_WHOISSPECIAL),
                               target_p->name, buf);
        }

        if((md = user_metadata_find(target_p, "SWHOIS")) != NULL)
            sendto_one_numeric(source_p, 320, "%s :%s", target_p->name, md->value);
    }

    if(IsSSLClient(target_p)) {
        char cbuf[256] = "is using a secure connection";

        if (MyClient(target_p) && target_p->localClient->cipher_string != NULL &&
            (!ConfigFileEntry.tls_ciphers_oper_only || source_p == target_p || IsOper(source_p)))
            rb_snprintf_append(cbuf, sizeof(cbuf), " [%s]", target_p->localClient->cipher_string);

        sendto_one_numeric(source_p, RPL_WHOISSECURE, form_str(RPL_WHOISSECURE),
                           target_p->name, cbuf);
        if((source_p == target_p || IsOper(source_p)) &&
           target_p->certfp != NULL)
            sendto_one_numeric(source_p, RPL_WHOISCERTFP,
                               form_str(RPL_WHOISCERTFP),
                               target_p->name, target_p->certfp);
    }
    if(!ConfigFileEntry.hide_uids_in_whois && (source_p == target_p || IsOper(source_p)))
        sendto_one_numeric(source_p, RPL_WHOISYOURID,
                           form_str(RPL_WHOISYOURID),
                           target_p->name, target_p->id);

    if(IsWebClient(target_p))
        sendto_one_numeric(source_p, RPL_WHOISWEBIRC,
                           form_str(RPL_WHOISWEBIRC),
                           target_p->name);

    if(IsSetBot(target_p))
        sendto_one_numeric(source_p, RPL_WHOISBOT,
                           form_str(RPL_WHOISBOT),
                           target_p->name);
    if(IsOper(source_p) || source_p == target_p) {
        m = buf;
        *m++ = '+';

        for (i = 0; i < 128; i++) /* >= 127 is extended ascii */
            if (target_p->umodes & user_modes[i])
                *m++ = (char) i;
        if (target_p->umodes & user_modes['s'])

        {
            *m++ = ' ';
            *m++ = '+';

            for (i = 0; i < 128 ; i++)
                if (snomask_modes[i] && (target_p->snomask & snomask_modes[i]))
                    *m++ = (char) i;

        }
        *m = '\0';

        sendto_one_numeric(source_p, RPL_WHOISMODES,
                           form_str(RPL_WHOISMODES),
                           target_p->name, buf);
    }

    if(MyClient(target_p)) {
        if (IsOper(source_p) || source_p == target_p) {
            int isDynSpoof = 0;

            if (IsDynSpoof(target_p))
                isDynSpoof = 1;

            /* trick here: show a nonoper their own IP if
             * dynamic spoofed but not if auth{} spoofed
             * -- jilles */
            if (isDynSpoof)
                ClearDynSpoof(target_p);

            sendto_one_numeric(source_p, RPL_WHOISHOST,
                               form_str(RPL_WHOISHOST),
                               target_p->name, target_p->orighost,
                               show_ip(source_p, target_p) ? target_p->sockhost : "255.255.255.255");

            if (isDynSpoof)
                SetDynSpoof(target_p);
        } else if(ConfigFileEntry.use_whois_actually && show_ip(source_p, target_p))
            sendto_one_numeric(source_p, RPL_WHOISACTUALLY,
                               form_str(RPL_WHOISACTUALLY),
                               target_p->name, target_p->sockhost);

        if (!IsService(target_p) && (!(target_p->umodes & UMODE_HIDEIDLE) || IsOper(source_p)))
            sendto_one_numeric(source_p, RPL_WHOISIDLE, form_str(RPL_WHOISIDLE),
                               target_p->name,
                               rb_current_time() - target_p->localClient->last,
                               target_p->localClient->firsttime);

        if (MyClient(target_p) && (source_p != target_p) && (target_p->umodes & UMODE_WHO)) {
            sendto_one_notice(target_p,
                              ":*** Notice -- %s (%s@%s) is doing a whois on you",
                              source_p->name,
                              source_p->username, source_p->host);
        }
    } else {
        if (IsOper(source_p) || source_p == target_p) {
            int isDynSpoof = 0;

            if (IsDynSpoof(target_p))
                isDynSpoof = 1;

            if (isDynSpoof)
                ClearDynSpoof(target_p);

            sendto_one_numeric(source_p, RPL_WHOISHOST,
                               form_str(RPL_WHOISHOST),
                               target_p->name, target_p->orighost,
                               show_ip(source_p, target_p) && !EmptyString(target_p->sockhost) && strcmp(target_p->sockhost, "0")? target_p->sockhost : "255.255.255.255");

            if (isDynSpoof)
                SetDynSpoof(target_p);
        } else if(ConfigFileEntry.use_whois_actually && show_ip(source_p, target_p) &&
                  !EmptyString(target_p->sockhost) && strcmp(target_p->sockhost, "0")) {
            sendto_one_numeric(source_p, RPL_WHOISACTUALLY,
                               form_str(RPL_WHOISACTUALLY),
                               target_p->name, target_p->sockhost);

        }
    }

    hdata.client = source_p;
    hdata.target = target_p;

    /* doing_whois_hook must only be called for local clients,
     * doing_whois_global_hook must only be called for local targets
     */
    /* it is important that these are called *before* RPL_ENDOFWHOIS is
     * sent, services compatibility code depends on it. --anfl
     */
    if(MyClient(source_p))
        call_hook(doing_whois_hook, &hdata);
    else
        call_hook(doing_whois_global_hook, &hdata);

    return;
}
