/*
 *
 * Copyright (c) 2018-2022 The University of Waikato, Hamilton, New Zealand.
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

#define _GNU_SOURCE
#include <string.h>
#include <assert.h>
#include <regex.h>
#include <b64/cdecode.h>
#include <b64/cencode.h>

#include "email_worker.h"
#include "logger.h"

enum {
    OPENLI_IMAP_COMMAND_NONE = 0,
    OPENLI_IMAP_COMMAND_SERVREADY,
    OPENLI_IMAP_COMMAND_REPLY,
    OPENLI_IMAP_COMMAND_REPLY_ONGOING,
    OPENLI_IMAP_COMMAND_BYE,
    OPENLI_IMAP_COMMAND_GENERIC,
    OPENLI_IMAP_COMMAND_PREAUTH,
    OPENLI_IMAP_COMMAND_AUTH,
    OPENLI_IMAP_COMMAND_LOGOUT,
    OPENLI_IMAP_COMMAND_IDLE,
    OPENLI_IMAP_COMMAND_ID,
};

enum {
    OPENLI_IMAP_AUTH_NONE = 0,
    OPENLI_IMAP_AUTH_PLAIN,
    OPENLI_IMAP_AUTH_GSSAPI,
    OPENLI_IMAP_AUTH_OTHER
};

typedef struct imap_cc_index {

    int cc_start;
    int cc_end;
    uint8_t dir;

} imap_cc_index_t;

typedef struct imap_comm {
    uint8_t *commbuffer;
    int commbufsize;
    int commbufused;

    char *imap_command;
    char *tag;
    char *imap_reply;

    imap_cc_index_t *ccs;
    int cc_used;
    int cc_alloc;

    int reply_start;
    int reply_end;
} imap_command_t;

typedef struct imapsession {

    uint8_t *contbuffer;
    int contbufsize;
    int contbufused;
    int contbufread;

    imap_command_t *commands;
    int commands_size;

    char *auth_tag;
    char *mailbox;

    int reply_start;
    int next_comm_start;
    uint8_t next_command_type;
    char *next_comm_tag;
    char *next_command_name;

    int idle_command_index;
    int auth_command_index;
    int auth_read_from;
    int auth_type;

} imap_session_t;

static void init_imap_command(imap_command_t *comm) {
    comm->commbuffer = calloc(4096, sizeof(uint8_t));
    comm->commbufsize = 4096;
    comm->commbufused = 0;
    comm->tag = NULL;
    comm->imap_reply = NULL;
    comm->imap_command = NULL;

    comm->reply_start = 0;
    comm->reply_end = 0;

    comm->ccs = calloc(8, sizeof(imap_cc_index_t));
    comm->cc_used = 0;
    comm->cc_alloc = 8;
};

static inline int extend_command_buffer(imap_command_t *comm, int required) {
    while (comm->commbufsize - comm->commbufused <= required + 1) {
        comm->commbuffer = realloc(comm->commbuffer, comm->commbufsize + 4096);
        if (comm->commbuffer == NULL) {
            return -1;
        }
        comm->commbufsize += 4096;
    }
    return 0;
}

static void add_cc_to_imap_command(imap_command_t *comm, int start_ind,
        int end_ind, uint8_t dir) {

    /* dir 1 == from client (COMMAND), dir 0 == from server (RESPONSE) */
    if (comm->cc_alloc == comm->cc_used) {
        comm->ccs = realloc(comm->ccs,
            (comm->cc_alloc + 8) * sizeof(imap_cc_index_t));
        comm->cc_alloc += 8;
    }

    comm->ccs[comm->cc_used].cc_start = start_ind;
    comm->ccs[comm->cc_used].cc_end = end_ind;
    comm->ccs[comm->cc_used].dir = dir;

    comm->cc_used ++;

}

static int update_saved_auth_command(imap_session_t *sess, char *replace,
        const char *origtoken, const char *sesskey) {

    int i, replacelen;
    imap_command_t *comm = NULL;
    char *ptr;

    if (sess->auth_command_index == -1) {
        logger(LOG_INFO, "OpenLI: %s missing IMAP auth command index?", sesskey);
        return -1;
    }
    comm = &(sess->commands[sess->auth_command_index]);

    if (strcmp(comm->tag, sess->auth_tag) != 0) {
        logger(LOG_INFO, "OpenLI: %s IMAP auth command tags are mismatched? %s vs %s", sesskey, sess->auth_tag, comm->tag);
        return -1;
    }

    if (strcmp(comm->imap_command, "AUTHENTICATE") != 0) {
        logger(LOG_INFO, "OpenLI: %s unexpected type for saved IMAP auth command: %d", sesskey, comm->imap_command);
        return -1;
    }

    ptr = strstr(comm->commbuffer, origtoken);
    if (!ptr) {
        logger(LOG_INFO, "OpenLI: cannot find original auth token for IMAP auth command %s, session %s\n", sess->auth_tag, sesskey);
        return -1;
    }

    replacelen = strlen(replace);
    memcpy(ptr, replace, replacelen);
    ptr += replacelen;

    comm->commbufused = ((uint8_t *)ptr - comm->commbuffer);
    comm->reply_start = comm->commbufused;
    memset(ptr, 0, comm->commbufsize - comm->commbufused);

    assert(comm->cc_used > 0);

    comm->ccs[comm->cc_used - 1].cc_end = comm->commbufused;

    ptr = comm->commbuffer + comm->ccs[comm->cc_used - 1].cc_start;
    return 1;

}

static int save_imap_command(imap_session_t *sess, char *sesskey) {

    int i, index;
    int comm_start;

    imap_command_t *comm = NULL;

    for (i = 0; i < sess->commands_size; i++) {
        if (sess->commands[i].imap_command == NULL) {
            comm = &(sess->commands[i]);
            index = i;
            break;
        }
    }

    if (comm == NULL) {
        sess->commands = realloc(sess->commands,
                (sess->commands_size + 5) * sizeof(imap_command_t));
        for (i = sess->commands_size; i < sess->commands_size + 5; i++) {
            init_imap_command(&(sess->commands[i]));
        }
        comm = &(sess->commands[sess->commands_size]);
        index = sess->commands_size;
        sess->commands_size += 5;
    }

    if (extend_command_buffer(comm, sess->contbufread - sess->next_comm_start)
            < 0) {
        return -1;
    }

    comm_start = comm->commbufused;
    memcpy(comm->commbuffer + comm->commbufused,
            sess->contbuffer + sess->next_comm_start,
            sess->contbufread - sess->next_comm_start);
    comm->commbufused += (sess->contbufread - sess->next_comm_start);

    comm->commbuffer[comm->commbufused] = '\0';

    add_cc_to_imap_command(comm, comm_start, comm->commbufused, 1);

    comm->reply_start = comm->commbufused;
    comm->reply_end = 0;
    comm->imap_command = sess->next_command_name;
    comm->tag = sess->next_comm_tag;


    sess->next_comm_tag = NULL;
    sess->next_command_name = NULL;

    logger(LOG_INFO, "OpenLI: DEVDEBUG %s saved IMAP command %s, %s",
            sesskey, comm->tag, comm->imap_command);
    return index;
}

static void mask_plainauth_creds(char *mailbox, char *reencoded, int buflen) {

    char input[2048];
    char *ptr;
    base64_encodestate e;
    int spaces, toencode, cnt;

    /* reencode authtoken with replaced username and password */
    base64_init_encodestate(&e);
    snprintf(input, 2048, "%s XXX XXX", mailbox);
    toencode = strlen(input);
    ptr = input;
    spaces = 0;

    while(spaces < 2) {
        if (*ptr == '\0') {
            break;
        }

        if (*ptr == ' ') {
            *ptr = '\0';
            spaces ++;
        }
        ptr ++;
    }

    /* TODO try not to walk off the end of reencoded -- very unlikely, given
     * that we have 2048 bytes of space but you never know...
     */
    ptr = reencoded;
    cnt = base64_encode_block(input, toencode, ptr, &e);

    ptr += cnt;
    cnt = base64_encode_blockend(ptr, &e);

    ptr += cnt;
    *ptr = '\r'; ptr++;
    *ptr = '\n'; ptr++;
    *ptr = '\0'; ptr++;

}

static int get_auth_type(char *authmsg, imap_session_t *imapsess,
        const char *sesskey) {

    char *saveptr;
    char *tag = NULL;
    char *comm = NULL;
    char *authtype = NULL;
    char *lineend = NULL;

    lineend = strstr(authmsg, "\r\n");
    if (lineend == NULL) {
        return 0;
    }

    tag = strtok_r(authmsg, " ", &saveptr);
    if (!tag) {
        logger(LOG_INFO, "OpenLI: unable to derive tag from IMAP AUTHENTICATE command");
        return -1;
    }

    comm = strtok_r(NULL, " ", &saveptr);
    if (!comm) {
        logger(LOG_INFO, "OpenLI: unable to derive command from IMAP AUTHENTICATE command");
        return -1;
    }

    authtype = strtok_r(NULL,  " \r\n", &saveptr);

    if (!authtype) {
        logger(LOG_INFO, "OpenLI: unable to derive authentication type from IMAP AUTHENTICATE command");
        return -1;
    }

    if (strcasecmp(authtype, "PLAIN") == 0) {
        imapsess->auth_type = OPENLI_IMAP_AUTH_PLAIN;
        imapsess->auth_read_from += (5 + (authtype - authmsg));

        if (lineend == authtype + 5) {
            imapsess->auth_read_from += 2;
        } else {
            imapsess->auth_read_from += 1;
        }
    } else {
        logger(LOG_INFO, "OpenLI: unsupported IMAP authentication type '%s' -- will not be able to derive mailbox owner for session %s",
                authtype, sesskey);
        return -1;
    }


    return 1;
}

static int decode_plain_auth_content(char *authmsg, imap_session_t *imapsess,
        emailsession_t *sess) {

    char decoded[2048];
    char reencoded[2048];
    char *ptr;
    int cnt, r;
    char *mailbox;
    base64_decodestate s;

    /* auth plain can be split across two messages with a
     * "+" from the server in between :( */

    if (*authmsg == '+') {
        /* Client has not yet sent the auth token, so this line is
         * the server indicating that it is waiting for the token.
         * Skip the "+" line and remain in auth command state until
         * the token arrives.
         */

        /* XXX this type of auth pattern is NOT tested, due to a lack
         * of an example pcap...
         */
        imapsess->auth_read_from += strlen(authmsg);
        sess->server_octets += strlen(authmsg);
        return 0;
    }

    base64_init_decodestate(&s);
    cnt = base64_decode_block(authmsg, strlen(authmsg), decoded, &s);
    decoded[cnt] = '\0';

    /* username and password are also inside 'decoded', each term is
     * separated by null bytes (e.g. <mailbox> \0 <username> \0 <password>)
     */
    imapsess->mailbox = strdup(decoded);

    /* add "mailbox" as a recipient for this session */
    add_email_participant(sess, imapsess->mailbox, 0);

    /* TODO add config option to allow reencoding to be disabled? */
    mask_plainauth_creds(imapsess->mailbox, reencoded, 2048);

    /* replace saved imap command with re-encoded auth token */
    r = update_saved_auth_command(imapsess, reencoded, authmsg, sess->key);
    if (r < 0) {
        return r;
    }

    sess->client_octets += strlen(reencoded);

    imapsess->next_command_type = OPENLI_IMAP_COMMAND_NONE;
    imapsess->next_comm_start = 0;
    imapsess->reply_start = 0;

    sess->currstate = OPENLI_IMAP_STATE_AUTHENTICATED;
    return 1;
}

static int decode_authentication_command(emailsession_t *sess,
        imap_session_t *imapsess) {

    char *authmsg;
    int msglen, r;

    while (1) {
        msglen = imapsess->contbufread - imapsess->auth_read_from;
        authmsg = calloc(msglen + 1, sizeof(uint8_t));

        memcpy(authmsg, imapsess->contbuffer + imapsess->auth_read_from,
                msglen);

        if (imapsess->auth_type == OPENLI_IMAP_AUTH_NONE) {
            r = get_auth_type(authmsg, imapsess, sess->key);
            if (r > 0) {
                sess->client_octets += (msglen -
                        (imapsess->contbufread - imapsess->auth_read_from));
            }
            free(authmsg);
            if (r <= 0) {
                break;
            }
            continue;
        }

        if (imapsess->auth_type == OPENLI_IMAP_AUTH_PLAIN) {
            r = decode_plain_auth_content(authmsg, imapsess, sess);
            free(authmsg);
            return r;
        } else {
            free(authmsg);
            return -1;
        }
    }

    return 0;
}

static int save_imap_reply(imap_session_t *sess, char *sesskey,
        char **origcommand) {

    int i;
    int comm_start;

    imap_command_t *comm = NULL;

    for (i = 0; i < sess->commands_size; i++) {
        if (sess->commands[i].tag == NULL) {
            break;
        }
        if (strcmp(sess->commands[i].tag, sess->next_comm_tag) == 0) {
            comm = &(sess->commands[i]);
            break;
        }
    }

    if (comm == NULL) {
        logger(LOG_INFO, "OpenLI: %s unable to match IMAP reply (%s, %s) to any existing commands?", sesskey, sess->next_comm_tag, sess->next_command_name);
        free(sess->next_comm_tag);
        sess->next_comm_tag = NULL;
        sess->next_command_name = NULL;
        return 0;
    }

    logger(LOG_INFO, "OpenLI: DEVDEBUG %s got IMAP reply for %s, %s --> %s",
            sesskey, comm->tag, comm->imap_command, sess->next_command_name);

    if (extend_command_buffer(comm, sess->contbufread - sess->reply_start)
            < 0) {
        return -1;
    }

    comm_start = comm->commbufused;
    memcpy(comm->commbuffer + comm->commbufused,
            sess->contbuffer + sess->reply_start,
            sess->contbufread - sess->reply_start);
    comm->commbufused += (sess->contbufread - sess->reply_start);

    add_cc_to_imap_command(comm, comm_start, comm->commbufused, 0);

    comm->commbuffer[comm->commbufused] = '\0';
    comm->reply_end = comm->commbufused;
    comm->imap_reply = sess->next_command_name;

    *origcommand = comm->imap_command;

    free(sess->next_comm_tag);
    sess->next_comm_tag = NULL;
    sess->next_command_name = NULL;

    return 1;
}

static void reset_imap_saved_command(imap_command_t *comm) {

    comm->commbufused = 0;
    comm->reply_start = 0;
    comm->reply_end = 0;
    comm->cc_used = 0;

    if (comm->tag) {
        free(comm->tag);
        comm->tag = NULL;
    }
    if (comm->imap_command) {
        free(comm->imap_command);
        comm->imap_command = NULL;
    }
    if (comm->imap_reply) {
        free(comm->imap_reply);
        comm->imap_reply = NULL;
    }
}

void free_imap_session_state(emailsession_t *sess, void *imapstate) {
    imap_session_t *imapsess;
    int i;

    if (imapstate == NULL) {
        return;
    }
    imapsess = (imap_session_t *)imapstate;

    for (i = 0; i < imapsess->commands_size; i++) {
        if (imapsess->commands[i].commbuffer) {
            free(imapsess->commands[i].commbuffer);
        }
        if (imapsess->commands[i].tag) {
            free(imapsess->commands[i].tag);
        }
        if (imapsess->commands[i].imap_command) {
            free(imapsess->commands[i].imap_command);
        }
        if (imapsess->commands[i].imap_reply) {
            free(imapsess->commands[i].imap_reply);
        }
        if (imapsess->commands[i].ccs) {
            free(imapsess->commands[i].ccs);
        }
    }

    if (imapsess->next_comm_tag) {
        free(imapsess->next_comm_tag);
    }
    if (imapsess->next_command_name) {
        free(imapsess->next_command_name);
    }

    if (imapsess->auth_tag) {
        free(imapsess->auth_tag);
    }

    /* Don't free 'mailbox', as this is owned by the participant list for
     * the overall email session.
     */

    free(imapsess->commands);
    free(imapsess->contbuffer);
    free(imapsess);
}

static int append_content_to_imap_buffer(imap_session_t *imapsess,
        openli_email_captured_t *cap) {

    /* +1 to account for a null terminator */
    while (imapsess->contbufsize - imapsess->contbufused <=
                cap->msg_length + 1) {
        imapsess->contbuffer = realloc(imapsess->contbuffer,
                imapsess->contbufsize + 4096);
        if (imapsess->contbuffer == NULL) {
            return -1;
        }
        imapsess->contbufsize += 4096;
    }

    memcpy(imapsess->contbuffer + imapsess->contbufused,
            cap->content, cap->msg_length);
    imapsess->contbufused += cap->msg_length;
    imapsess->contbuffer[imapsess->contbufused] = '\0';

    assert(imapsess->contbufused <= imapsess->contbufsize);
    return 0;
}

#define ADVANCE_ID_PTR \
        ptr = strchr(ptr, '"'); \
        if (ptr == NULL) { \
            break; \
        } \
        ptr ++; \
        if (*ptr == '\r' || *ptr == '\0') { \
            break; \
        }


static int parse_id_command(emailsession_t *sess, imap_session_t *imapsess) {
    char *ptr;
    char *comm_str = imapsess->contbuffer + imapsess->next_comm_start;

    char *field_start, *field_end, *val_start, *val_end;
    char field_str[2048];
    char val_str[2048];

    char *server_ip, *server_port, *client_ip, *client_port;
    int ret = 0;

    ptr = strchr(comm_str, '(');
    if (!ptr) {
        return 0;
    }

    /* ID commands can contain custom field that specify the "real" server
     * and client IPs and ports for an IMAP session, i.e. in cases where
     * the IMAP session has been delivered to our collector via a proxy
     * IMAP server.
     *
     * In that situation, we want to replace the server and client addresses
     * that we saved from the original packet captures with the addresses
     * described in the ID content.
     */

    ptr ++;
    field_start = field_end = val_start = val_end = NULL;
    server_ip = client_ip = server_port = client_port = NULL;

    while (1) {
        if (*ptr == ')' || *ptr == '\r' || *ptr == '\0') {
            break;
        }

        ADVANCE_ID_PTR
        field_start = ptr;

        ADVANCE_ID_PTR
        field_end = ptr - 1;

        if (strncmp(ptr, " NIL", 4) == 0) {
            val_start = ptr + 1;
            val_end = ptr + 4;
            ptr += 4;
        } else {
            ADVANCE_ID_PTR
            val_start = ptr;
            ADVANCE_ID_PTR
            val_end = ptr - 1;
        }

        memset(field_str, 0, 2048);
        memcpy(field_str, field_start, field_end - field_start);
        memset(val_str, 0, 2048);
        memcpy(val_str, val_start, val_end - val_start);
        field_start = field_end = val_start = val_end = NULL;

        if (strcmp(field_str, "x-originating-ip") == 0) {
            client_ip = strdup(val_str);
        } else if (strcmp(field_str, "x-originating-port") == 0) {
            client_port = strdup(val_str);
        } else if (strcmp(field_str, "x-connected-ip") == 0) {
            server_ip = strdup(val_str);
        } else if (strcmp(field_str, "x-connected-port") == 0) {
            server_port = strdup(val_str);
        }

    }

    if (field_start || field_end || val_start || val_end) {
        ret = 0;
    } else {
        ret = 1;
    }

    if (server_ip && server_port) {
        replace_email_session_serveraddr(sess, server_ip, server_port);
    }

    if (client_ip && client_port) {
        replace_email_session_clientaddr(sess, client_ip, client_port);
    }

    if (server_ip) { free(server_ip); }
    if (client_ip) { free(client_ip); }
    if (server_port) { free(server_port); }
    if (client_port) { free(client_port); }

    return ret;
}

static int find_next_crlf(imap_session_t *sess, int start_index) {
    int rem;
    uint8_t *found;

    assert(sess->contbufused >= start_index);

    rem = sess->contbufused - start_index;

    found = (uint8_t *)memmem(sess->contbuffer + start_index, rem, "\r\n", 2);

    if (found) {
        sess->contbufread = (found - sess->contbuffer) + 2;
        return 1;
    }
    return 0;
}

static int find_command_end(emailsession_t *sess, imap_session_t *imapsess) {
    int r, ind;

    r = find_next_crlf(imapsess, imapsess->next_comm_start);
    if (r == 0) {
        return 0;
    }

    ind = save_imap_command(imapsess, sess->key);
    if (ind < 0) {
        return ind;
    }

    if (imapsess->next_command_type == OPENLI_IMAP_COMMAND_AUTH) {
        sess->currstate = OPENLI_IMAP_STATE_AUTHENTICATING;
        imapsess->auth_command_index = ind;

        return decode_authentication_command(sess, imapsess);

        /* Don't count client octets just yet, since we could be rewriting
         * the auth tokens shortly...
         */

    } else {
        sess->client_octets += (imapsess->contbufread - imapsess->next_comm_start);

    }

    /* TODO if command was ID, update session endpoint details using
     * command content */

    if (imapsess->next_command_type == OPENLI_IMAP_COMMAND_ID) {
        parse_id_command(sess, imapsess);
    }

    if (imapsess->next_command_type == OPENLI_IMAP_COMMAND_LOGOUT) {
        sess->currstate = OPENLI_IMAP_STATE_LOGOUT;
    } else if (imapsess->next_command_type == OPENLI_IMAP_COMMAND_IDLE) {
        sess->currstate = OPENLI_IMAP_STATE_IDLING;
        imapsess->idle_command_index = ind;
    }

    imapsess->next_command_type = OPENLI_IMAP_COMMAND_NONE;
    imapsess->next_comm_start = 0;
    imapsess->reply_start = 0;

    return 1;
}

static int find_reply_end(emailsession_t *sess, imap_session_t *imapsess) {
    int r;
    char *origcommand = NULL;

    r = find_next_crlf(imapsess, imapsess->next_comm_start);
    if (r == 0) {
        return 0;
    }
    sess->server_octets += (imapsess->contbufread - imapsess->next_comm_start);

    if ((r = save_imap_reply(imapsess, sess->key, &origcommand)) < 0) {
        return r;
    }

    imapsess->next_command_type = OPENLI_IMAP_COMMAND_NONE;
    imapsess->next_comm_start = 0;
    imapsess->reply_start = 0;

    /* TODO send any IRIs or CCs */

    if (origcommand && strcasecmp(origcommand, "LOGOUT") == 0) {
        sess->currstate = OPENLI_IMAP_STATE_SESSION_OVER;
        return 0;
    }

    if (origcommand && strcasecmp(origcommand, "AUTHENTICATE") == 0) {
        sess->currstate = OPENLI_IMAP_STATE_AUTHENTICATED;

    }

    return r;
}

static int find_partial_reply_end(emailsession_t *sess,
        imap_session_t *imapsess) {
    int r;

    r = find_next_crlf(imapsess, imapsess->next_comm_start);
    if (r == 0) {
        return 0;
    }
    sess->server_octets += (imapsess->contbufread - imapsess->next_comm_start);

    logger(LOG_INFO, "OpenLI: DEVDEBUG %s got partial IMAP reply ",
            sess->key);
    imapsess->next_command_type = OPENLI_IMAP_COMMAND_NONE;
    imapsess->next_comm_start = 0;

    return 1;
}


static int find_server_ready_end(imap_session_t *imapsess) {

    int r;

    r = find_next_crlf(imapsess, imapsess->next_comm_start);
    if (r == 0) {
        return 0;
    }

    return 1;
}

static int find_server_ready(imap_session_t *imapsess) {

    uint8_t *found = NULL;
    assert(imapsess->contbufused >= imapsess->contbufread);

    if (imapsess->contbufused - imapsess->contbufread < 5) {
        return 0;
    }

    found = (uint8_t *)strcasestr(
            (const char *)(imapsess->contbuffer + imapsess->contbufread),
                    "* OK ");
    if (found != NULL) {
        imapsess->next_comm_start = (found - imapsess->contbuffer);
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_SERVREADY;
        return 1;
    }
    return 0;
}

static int read_imap_while_auth_state(emailsession_t *sess,
        imap_session_t *imapsess) {

    /* Our goal here is to just consume any unconventional exchanges
     * between client and server that might be occurring during
     * authentication (e.g. challenges, responses for GSSAPI, etc.).
     */

    uint8_t *msgstart = imapsess->contbuffer + imapsess->contbufread;
    /* TODO */

    /* XXX need some test cases for anything other than just
     * PLAIN auth followed by immediate success
     */

    /* First step, find the next \r\n so we're only working with a
     * complete message */

    /* Is this the server reply to the AUTHENTICATE command? */
        /* If yes, rewind to the start of the reply tag so our normal
         * processing can be applied when we return...
         */

    /* Does this begin with a '+'? This is from the server (I think?) */

    /* Otherwise, this is probably a client message */

}

static int read_imap_while_idle_state(emailsession_t *sess,
        imap_session_t *imapsess) {

    uint8_t *msgstart = imapsess->contbuffer + imapsess->contbufread;
    imap_command_t *comm;
    uint8_t *found = NULL;
    int idle_server_length = 0;
    int comm_start;

    assert(imapsess->idle_command_index >= 0);

    comm = &(imapsess->commands[imapsess->idle_command_index]);

    /* check for "+ " -- server response to the idle command*/

    if (imapsess->reply_start == 0) {
        found = (uint8_t *)strstr(msgstart, "+ ");
        if (!found) {
            return 0;
        }

        imapsess->reply_start = found - imapsess->contbuffer;
    }

    /* all untagged messages are updates from the server
     * add them to our reply */

    /* check for "DONE\r\n" -- client message to end idling state */
    /*      make sure we add everything from reply_start to the start
     *      of "DONE" as a separate server->client CC, then add the
     *      "DONE" as a client->server CC.
     */
    found = (uint8_t *)strstr(msgstart, "\r\nDONE\r\n");
    if (!found) {
        return 0;
    }

    idle_server_length = (found + 2 - imapsess->contbuffer) -
            imapsess->reply_start;

    imapsess->contbufread = (found - imapsess->contbuffer) + 8;

    if (extend_command_buffer(comm, idle_server_length + 6) < 0) {
        return -1;
    }

    comm_start = comm->commbufused;
    memcpy(comm->commbuffer + comm->commbufused,
            imapsess->contbuffer + imapsess->reply_start,
            idle_server_length + 6);
    comm->commbufused += (idle_server_length + 6);
    comm->commbuffer[comm->commbufused] = '\0';

    add_cc_to_imap_command(comm, comm_start,
            comm_start + idle_server_length, 0);
    add_cc_to_imap_command(comm, comm_start + idle_server_length,
            comm_start + idle_server_length + 6, 1);

    sess->server_octets += idle_server_length;
    sess->client_octets += 6;

    imapsess->reply_start = 0;
    sess->currstate = OPENLI_IMAP_STATE_AUTHENTICATED;

    return 1;
}

static int find_next_imap_message(emailsession_t *sess,
        imap_session_t *imapsess) {

    char *tag;
    char *comm_resp;
    uint8_t *spacefound = NULL;
    uint8_t *spacefound2 = NULL;
    uint8_t *crlffound = NULL;
    uint8_t *msgstart = imapsess->contbuffer + imapsess->contbufread;


    if (sess->currstate == OPENLI_IMAP_STATE_AUTHENTICATING) {
        /* Handle various auth response behaviours, as per RFC9051 */
        return read_imap_while_auth_state(sess, imapsess);
    }

    if (sess->currstate == OPENLI_IMAP_STATE_IDLING) {
        return read_imap_while_idle_state(sess, imapsess);
    }

    spacefound = (uint8_t *)strchr(msgstart, ' ');
    if (!spacefound) {
        return 0;
    }

    tag = calloc((spacefound - msgstart) + 1, sizeof(char *));
    memcpy(tag, msgstart, spacefound - msgstart);
    tag[spacefound - msgstart] = '\0';

    /* Most commands are "<tag> <type> <extra context>\r\n", but some
     * have no extra context and are just "<tag> <type>\r\n".
     * Therefore if we see a \r\n BEFORE the next space, we want to
     * treat that as our string boundary.
     */
    spacefound2 = (uint8_t *)strchr(spacefound + 1, ' ');
    crlffound = (uint8_t *)strstr(spacefound + 1, "\r\n");

    if (!spacefound2 && !crlffound) {
        free(tag);
        return 0;
    }

    if (spacefound2 == NULL || (crlffound != NULL && crlffound < spacefound2)) {
        spacefound2 = crlffound;
    }

    comm_resp = calloc((spacefound2 - spacefound), sizeof(char *));
    memcpy(comm_resp, spacefound + 1, (spacefound2 - spacefound) - 1);
    comm_resp[spacefound2 - spacefound - 1] = '\0';

    if (strcmp(tag, "*") == 0) {
        if (strcasecmp(comm_resp, "BYE") == 0 &&
                sess->currstate != OPENLI_IMAP_STATE_LOGOUT) {

            /* server is doing an immediate shutdown */

            /* TODO force an IRI for connection termination?
             *      dump CCs for any incomplete commands (including the
             *      sudden BYE)?
             */
            sess->currstate = OPENLI_IMAP_STATE_SESSION_OVER;
            free(tag);
            free(comm_resp);
            return 0;

        } else if (strcasecmp(comm_resp, "PREAUTH") == 0) {
            //imapsess->next_command_type = OPENLI_IMAP_COMMAND_PREAUTH;
        } else {
            /* a partial reply to a command, more to come... */
            imapsess->next_command_type = OPENLI_IMAP_COMMAND_REPLY_ONGOING;
            free(comm_resp);
            comm_resp = NULL;

            if (imapsess->reply_start == 0) {
                imapsess->reply_start = msgstart - imapsess->contbuffer;
            }
        }
    } else if (strcasecmp(comm_resp, "OK") == 0 ||
            strcasecmp(comm_resp, "NO") == 0 ||
            strcasecmp(comm_resp, "BAD") == 0) {

        /* this is a reply that completes the response to a command */
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_REPLY;
        if (imapsess->reply_start == 0) {
            imapsess->reply_start = msgstart - imapsess->contbuffer;
        }
    } else if (strcasecmp(comm_resp, "ID") == 0) {
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_ID;
    } else if (strcasecmp(comm_resp, "IDLE") == 0) {
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_IDLE;
    } else if (strcasecmp(comm_resp, "LOGOUT") == 0) {
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_LOGOUT;
    } else if (strcasecmp(comm_resp, "AUTHENTICATE") == 0) {
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_AUTH;
        if (imapsess->auth_tag) {
            free(imapsess->auth_tag);
        }
        imapsess->auth_tag = strdup(tag);
        imapsess->auth_read_from = msgstart - imapsess->contbuffer;
        sess->currstate = OPENLI_IMAP_STATE_AUTH_STARTED;
    } else {
        /* just a regular IMAP command that requires no special treatment */
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_GENERIC;
    }

    if (imapsess->next_comm_tag) {
        free(imapsess->next_comm_tag);
    }
    imapsess->next_comm_tag = tag;

    if (imapsess->next_command_name) {
        free(imapsess->next_command_name);
    }
    imapsess->next_command_name = comm_resp;
    imapsess->next_comm_start = msgstart - imapsess->contbuffer;

    return 1;
}

static int process_next_imap_state(openli_email_worker_t *state,
        emailsession_t *sess, imap_session_t *imapsess, uint64_t timestamp) {

    int r;

    if (sess->currstate == OPENLI_IMAP_STATE_INIT) {
        r = find_server_ready(imapsess);
        if (r == 1) {
            sess->currstate = OPENLI_IMAP_STATE_SERVER_READY;
        }
    }

    if (sess->currstate == OPENLI_IMAP_STATE_SERVER_READY) {
        r = find_server_ready_end(imapsess);
        if (r == 1) {
            sess->currstate = OPENLI_IMAP_STATE_PRE_AUTH;
            sess->server_octets +=
                    (imapsess->contbufread - imapsess->next_comm_start);
            imapsess->next_comm_start = 0;
            imapsess->next_command_type = OPENLI_IMAP_COMMAND_NONE;
            logger(LOG_INFO, "OpenLI DEVDEBUG: IMAP Server Ready %s",
                    sess->key);
        }
        return r;
    }

    if (imapsess->next_command_type == OPENLI_IMAP_COMMAND_NONE) {
        r = find_next_imap_message(sess, imapsess);
        return r;
    } else if (imapsess->next_command_type == OPENLI_IMAP_COMMAND_REPLY) {
        r = find_reply_end(sess, imapsess);

        return r;
    } else if (imapsess->next_command_type ==
            OPENLI_IMAP_COMMAND_REPLY_ONGOING) {
        r = find_partial_reply_end(sess, imapsess);
        return r;
    } else {
        r = find_command_end(sess, imapsess);
        return r;
    }

    return 0;
}

int update_imap_session_by_ingestion(openli_email_worker_t *state,
        emailsession_t *sess, openli_email_captured_t *cap) {

    imap_session_t *imapsess;
    int r, i;

    if (sess->proto_state == NULL) {
        imapsess = calloc(1, sizeof(imap_session_t));
        imapsess->contbuffer = calloc(1024, sizeof(uint8_t));
        imapsess->contbufused = 0;
        imapsess->contbufread = 0;
        imapsess->contbufsize = 1024;
        imapsess->commands = calloc(5, sizeof(imap_command_t));
        imapsess->commands_size = 5;
        imapsess->next_command_type = OPENLI_IMAP_COMMAND_NONE;
        imapsess->idle_command_index = -1;
        imapsess->auth_command_index = -1;

        for (i = 0; i < imapsess->commands_size; i++) {
            init_imap_command(&(imapsess->commands[i]));
        }

        sess->proto_state = (void *)imapsess;
    } else {
        imapsess = (imap_session_t *)sess->proto_state;
    }

    if (append_content_to_imap_buffer(imapsess, cap) < 0) {
        logger(LOG_INFO, "OpenLI: Failed to append IMAP message content to session buffer for %s", sess->key);
        return -1;
    }

    while (1) {
        if ((r = process_next_imap_state(state, sess, imapsess,
                cap->timestamp)) <= 0) {
            break;
        }
    }

    if (sess->currstate == OPENLI_IMAP_STATE_SESSION_OVER) {
        return 1;
    }

    return 0;
}

// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
