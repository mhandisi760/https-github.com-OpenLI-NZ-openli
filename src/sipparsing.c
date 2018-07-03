/*
 *
 * Copyright (c) 2018 The University of Waikato, Hamilton, New Zealand.
 * All rights reserved.
 *
 * This file is part of OpenLI.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * OpenLI is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenLI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#include <stdlib.h>
#include <string.h>
#include <libtrace.h>
#include <osip2/osip.h>
#include <osipparser2/osip_message.h>
#include <osipparser2/sdp_message.h>
#include "sipparsing.h"
#include "logger.h"


int parse_sip_packet(openli_sip_parser_t **parser, libtrace_packet_t *packet) {

    void *transport;
    char *payload = NULL;
    uint8_t proto;
    uint32_t rem, plen;
    int i;
    openli_sip_parser_t *p;

    if (*parser == NULL) {
    	p = (openli_sip_parser_t *)malloc(sizeof(openli_sip_parser_t));

        p->osip = NULL;
        p->sdp = NULL;
        *parser = p;
    } else if (p->osip) {
        p = *parser;
        osip_message_free(p->osip);

        if (p->sdp) {
            sdp_message_free(p->sdp);
            p->sdp = NULL;
        }
    }

	osip_message_init(&(p->osip));

    transport = trace_get_transport(packet, &proto, &rem);
    if (transport == NULL) {
        return -1;
    }
    if (proto == TRACE_IPPROTO_TCP) {
        libtrace_tcp_t *tcp = (libtrace_tcp_t *)transport;
        if (rem < sizeof(libtrace_tcp_t)) {
            return 0;
        }
        payload = trace_get_payload_from_tcp(tcp, &rem);
        if (payload == NULL || rem == 0) {
            return 0;
        }
    } else if (proto == TRACE_IPPROTO_UDP) {
        libtrace_udp_t *udp = (libtrace_udp_t *)transport;
        if (rem < sizeof(libtrace_udp_t)) {
            return 0;
        }
        payload = trace_get_payload_from_udp(udp, &rem);
        if (payload == NULL || rem == 0) {
            return 0;
        }
    } else {
        return -1;
    }

    plen = trace_get_payload_length(packet);
    if (plen == 0) {
        return 0;
    }

    /* Check for a CRLF keep alive */
    if (memcmp(payload, "\x0d\x0a\x0d\x0a", 4) == 0) {
        return 0;
    }

    if (memcmp(payload, "\x0d\x0a", 2) == 0 && plen == 2) {
        return 0;
    }

    /* 00 00 00 00 seems to be some sort of keep alive as well? */
    if (plen == 4 && memcmp(payload, "\x00\x00\x00\x00", 4) == 0) {
        return 0;
    }

    if (plen < rem) {
        rem = plen;
    }

    i = osip_message_parse(p->osip, (const char *)payload, rem);
    if (i != 0) {
        return -1;
    }

    /* Don't do an SDP parse until it is required -- collector processing
     * threads won't need to look at SDP, for instance. */

    return 1;
}

void release_sip_parser(openli_sip_parser_t *parser) {

    if (parser->osip) {
        osip_message_free(parser->osip);
    }
    if (parser->sdp) {
        sdp_message_free(parser->sdp);
    }
    free(parser);

}

static inline char *strip_sip_uri(char *uristr) {

    char *firstcol = NULL;
    char *chop = NULL;

    /* Examples of URIs that need stripping:
     *   sip:francisco@bestel.com:55060     (remove :55060)
     *   sip:200.57.7.195:55061;user=phone  (remove :50061;user=phone)
     */

    /* firstcol should point to the colon at the end of 'sip:<name>' portion
     * of the URI */
    firstcol = strchr((const char *)uristr, ':');
    if (firstcol == NULL) {
        return NULL;
    }

    /* A port (if present) will always come before '?' or ';' (i think!) */
    chop = strchr((const char *)(firstcol+1), ':');
    if (chop != NULL) {
        *chop = '\0';
        return uristr;
    }

    chop = strchr((const char *)(firstcol+1), ';');
    if (chop != NULL) {
        *chop = '\0';
    }

    chop = strchr((const char *)(firstcol+1), '?');
    if (chop != NULL) {
        *chop = '\0';
    }

    return uristr;
}

char *get_sip_cseq(openli_sip_parser_t *parser) {

    osip_cseq_t *cseq = osip_message_get_cseq(parser->osip);
    char *cseqstr;

    if (cseq == NULL) {
        return NULL;
    }

    if (osip_cseq_to_str(cseq, &cseqstr) != 0) {
        return NULL;
    }

    return cseqstr;
}

char *get_sip_from_uri(openli_sip_parser_t *parser) {

    char *uristr;
    osip_from_t *from = osip_message_get_from(parser->osip);

    if (from == NULL) {
        return NULL;
    }

    if (osip_uri_to_str_canonical(osip_from_get_url(from), &uristr) != 0) {
        return NULL;
    }

    /* Need to strip any extra uri components (e.g. port numbers,
     * additional arguments etc. */
    uristr = strip_sip_uri(uristr);

    return uristr;
}

char *get_sip_to_uri(openli_sip_parser_t *parser) {

    char *uristr;
    osip_to_t *to = osip_message_get_to(parser->osip);

    if (to == NULL) {
        return NULL;
    }

    if (osip_uri_to_str_canonical(osip_to_get_url(to), &uristr) != 0) {
        return NULL;
    }

    /* Need to strip any extra uri components (e.g. port numbers,
     * additional arguments etc. */
    uristr = strip_sip_uri(uristr);
    return uristr;
}

char *get_sip_to_uri_username(openli_sip_parser_t *parser) {

    char *uriuser;
    osip_to_t *to = osip_message_get_to(parser->osip);

    if (to == NULL) {
        return NULL;
    }

    uriuser = osip_uri_get_username(osip_to_get_url(to));
    return uriuser;
}

char *get_sip_to_uri_realm(openli_sip_parser_t *parser) {
    /* I use the term 'realm' here to be consistent with Authorization
     * header fields, but really this part of a To: uri is generally
     * called a 'host'.
     */
    char *urihost;
    osip_to_t *to = osip_message_get_to(parser->osip);

    if (to == NULL) {
        return NULL;
    }

    urihost = osip_uri_get_host(osip_to_get_url(to));
    return urihost;
}

int get_sip_to_uri_identity(openli_sip_parser_t *parser,
        openli_sip_identity_t *sipid) {

    sipid->username = get_sip_to_uri_username(parser);
    if (sipid->username == NULL) {
        return -1;
    }
    sipid->username_len = strlen(sipid->username);

    sipid->realm = get_sip_to_uri_realm(parser);
    if (sipid->realm == NULL) {
        return -1;
    }
    sipid->realm_len = strlen(sipid->realm);
    return 1;
}

static inline void strip_quotes(openli_sip_identity_t *sipid) {

    /* The removal of the trailing " is permanent, so we need to
     * be careful about detecting cases where we call strip_quotes
     * again on a term that will now only have a beginning quote,
     * e.g. "username
     */

    if (sipid->username[0] == '"') {
        if (sipid->username[sipid->username_len - 1] == '"') {
            sipid->username[sipid->username_len - 1] = '\0';
            sipid->username_len --;
        }
        sipid->username ++;
        sipid->username_len --;
    }

    if (sipid->realm[0] == '"') {
        if (sipid->realm[sipid->realm_len - 1] == '"') {
            sipid->realm[sipid->realm_len - 1] = '\0';
            sipid->realm_len --;
        }
        sipid->realm ++;
        sipid->realm_len --;
    }

}

int get_sip_auth_identity(openli_sip_parser_t *parser, int index,
        int *authcount, openli_sip_identity_t *sipid) {

    osip_authorization_t *auth;

    *authcount = osip_list_size(&(parser->osip->authorizations));

    if (*authcount == 0) {
        return 0;
    }

    if (index >= *authcount) {
        logger(LOG_DAEMON,
                "OpenLI: Error, requested auth username %d but packet only has %d auth headers.",
                index, *authcount);
        return -1;
    }

    if (osip_message_get_authorization(parser->osip, index, &auth) != 0) {
        logger(LOG_DAEMON,
                "OpenLI: Error while extracting auth header from SIP packet.");
        return -1;
    }

    sipid->username = osip_authorization_get_username(auth);
    sipid->username_len = strlen(sipid->username);
    sipid->realm = osip_authorization_get_realm(auth);
    sipid->realm_len = strlen(sipid->realm);

    strip_quotes(sipid);

    return 1;

}

int get_sip_proxy_auth_identity(openli_sip_parser_t *parser, int index,
        int *authcount, openli_sip_identity_t *sipid) {

    osip_proxy_authorization_t *auth;

    *authcount = osip_list_size(&(parser->osip->proxy_authorizations));

    if (*authcount == 0) {
        return 0;
    }

    if (index >= *authcount) {
        logger(LOG_DAEMON,
                "OpenLI: Error, requested proxy auth username %d but packet only has %d auth headers.",
                index, *authcount);
        return -1;
    }

    if (osip_message_get_proxy_authorization(parser->osip, index, &auth) != 0) {
        logger(LOG_DAEMON,
                "OpenLI: Error while extracting proxy auth header from SIP packet.");
        return -1;
    }

    sipid->username = osip_proxy_authorization_get_username(auth);
    sipid->username_len = strlen(sipid->username);
    sipid->realm = osip_proxy_authorization_get_realm(auth);
    sipid->realm_len = strlen(sipid->realm);

    strip_quotes(sipid);
    return 1;
}

char *get_sip_callid(openli_sip_parser_t *parser) {
    char *callidstr;
    osip_call_id_t *cid;

    cid = osip_message_get_call_id(parser->osip);
    if (cid == NULL) {
        return NULL;
    }

    callidstr = osip_call_id_get_number(cid);
    return callidstr;
}

static inline int parse_sdp_body(openli_sip_parser_t *parser) {
    osip_body_t *body;

    sdp_message_init(&(parser->sdp));
    if (osip_message_get_body(parser->osip, 0, &body) != 0) {
        return -1;
    }
    if (sdp_message_parse(parser->sdp, body->body) != 0) {
        return -1;
    }
    return 0;
}

char *get_sip_session_id(openli_sip_parser_t *parser) {

    char *sessid;

    if (!parser->sdp) {
        if (parse_sdp_body(parser) == -1) {
            return NULL;
        }
    }
    sessid = sdp_message_o_sess_id_get(parser->sdp);
    return sessid;
}

char *get_sip_session_address(openli_sip_parser_t *parser) {
    char *sessaddr;

    if (!parser->sdp) {
        if (parse_sdp_body(parser) == -1) {
            return NULL;
        }
    }
    sessaddr = sdp_message_o_addr_get(parser->sdp);
    return sessaddr;
}

char *get_sip_session_username(openli_sip_parser_t *parser) {
    char *sessuname;

    if (!parser->sdp) {
        if (parse_sdp_body(parser) == -1) {
            return NULL;
        }
    }
    sessuname = sdp_message_o_username_get(parser->sdp);
    return sessuname;
}

char *get_sip_session_version(openli_sip_parser_t *parser) {

    char *sessv;

    if (!parser->sdp) {
        if (parse_sdp_body(parser) == -1) {
            return NULL;
        }
    }
    sessv = sdp_message_o_sess_version_get(parser->sdp);
    return sessv;
}

char *get_sip_media_ipaddr(openli_sip_parser_t *parser) {
    char *ipaddr;

    if (!parser->sdp) {
        if (parse_sdp_body(parser) == -1) {
            return NULL;
        }
    }
    ipaddr = sdp_message_c_addr_get(parser->sdp, -1, 0);
    return ipaddr;
}

char *get_sip_media_port(openli_sip_parser_t *parser) {
    char *port;

    if (!parser->sdp) {
        if (parse_sdp_body(parser) == -1) {
            return NULL;
        }
    }
    port = sdp_message_m_port_get(parser->sdp, 0);
    return port;
}

int sip_is_invite(openli_sip_parser_t *parser) {
    if (MSG_IS_INVITE(parser->osip)) {
        return 1;
    }
    return 0;
}

int sip_is_200ok(openli_sip_parser_t *parser) {

    if (MSG_IS_RESPONSE(parser->osip)) {
        if (osip_message_get_status_code(parser->osip) == 200) {
            return 1;
        }
    }

    return 0;
}

int sip_is_183sessprog(openli_sip_parser_t *parser) {

    if (MSG_IS_RESPONSE(parser->osip)) {
        if (osip_message_get_status_code(parser->osip) == 183) {
            return 1;
        }
    }

    return 0;
}

int sip_is_bye(openli_sip_parser_t *parser) {
    if (MSG_IS_BYE(parser->osip)) {
        return 1;
    }
    return 0;
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
