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
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <errno.h>
#include <libtrace/message_queue.h>
#include <libtrace_parallel.h>

#include "configparser.h"
#include "logger.h"
#include "collector_buffer.h"
#include "provisioner.h"
#include "agency.h"

void clear_global_config(collector_global_t *glob) {
        int i;

	if (glob->inputs) {
        for (i = 0; i < glob->inputcount; i++) {
            if (glob->inputs[i].config.uri) {
                free(glob->inputs[i].config.uri);
            }
            if (glob->inputs[i].trace) {
                trace_destroy(glob->inputs[i].trace);
            }
            if (glob->inputs[i].pktcbs) {
                trace_destroy_callback_set(glob->inputs[i].pktcbs);
            }
        }
        free(glob->inputs);
    }

    if (glob->syncsendqs) {
        free(glob->syncsendqs);
    }

    if (glob->syncepollevs) {
        free(glob->syncepollevs);
    }

    if (glob->operatorid) {
        free(glob->operatorid);
    }

    if (glob->networkelemid) {
        free(glob->networkelemid);
    }

    if (glob->intpointid) {
        free(glob->intpointid);
    }

    if (glob->provisionerip) {
        free(glob->provisionerip);
    }

    if (glob->provisionerport) {
        free(glob->provisionerport);
    }

    pthread_mutex_destroy(&glob->syncq_mutex);
    free(glob);
}

static int parse_input_config(collector_global_t *glob, yaml_document_t *doc,
        yaml_node_t *inputs) {

    yaml_node_item_t *item;
    int i;

    for (item = inputs->data.sequence.items.start;
            item != inputs->data.sequence.items.top; item ++) {
        yaml_node_t *node = yaml_document_get_node(doc, *item);
        colinput_t *inp;
        yaml_node_pair_t *pair;

        /* Each sequence item is a new input */
        if (glob->inputcount == glob->inputalloced) {
            if (glob->inputalloced == 0) {
                glob->inputs = (colinput_t *)malloc(sizeof(colinput_t) * 10);
                glob->inputalloced = 10;
            } else {
                glob->inputs = (colinput_t *)realloc(glob->inputs,
                        sizeof(colinput_t) * (10 + glob->inputalloced));
                glob->inputalloced += 10;
            }
        }

        inp = &(glob->inputs[glob->inputcount]);
        inp->config.uri = NULL;
        inp->config.threadcount = 1;
        inp->trace = NULL;
        inp->pktcbs = NULL;

        /* Mappings describe the parameters for each input */
        for (pair = node->data.mapping.pairs.start;
                pair < node->data.mapping.pairs.top; pair ++) {
            yaml_node_t *key, *value;

            key = yaml_document_get_node(doc, pair->key);
            value = yaml_document_get_node(doc, pair->value);

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value, "uri") == 0 &&
                    inp->config.uri == NULL) {
                inp->config.uri = strdup((char *)value->data.scalar.value);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value, "threads") == 0) {
                inp->config.threadcount = strtoul(
                        (char *)value->data.scalar.value, NULL, 10);
            }
        }
        glob->inputcount ++;
        glob->totalthreads += inp->config.threadcount;
    }

    glob->syncsendqs = (libtrace_message_queue_t **)malloc(
            sizeof(libtrace_message_queue_t *) * glob->totalthreads);
    memset(glob->syncsendqs, 0,
            sizeof(libtrace_message_queue_t *) * glob->totalthreads);
    glob->syncepollevs = (void **)malloc(sizeof(void *) * glob->totalthreads);
    memset(glob->syncepollevs, 0, sizeof(void *) * glob->totalthreads);
    glob->queuealloced = glob->totalthreads;
    glob->registered_syncqs = 0;

    return 0;
}

static int parse_agency_list(libtrace_list_t *aglist, yaml_document_t *doc,
        yaml_node_t *inputs) {

    yaml_node_item_t *item;

    for (item = inputs->data.sequence.items.start;
            item != inputs->data.sequence.items.top; item ++) {
        yaml_node_t *node = yaml_document_get_node(doc, *item);
        yaml_node_pair_t *pair;
        liagency_t newag;

        newag.ipstr = NULL;
        newag.portstr = NULL;
        newag.agencyid = NULL;

        for (pair = node->data.mapping.pairs.start;
                pair < node->data.mapping.pairs.top; pair ++) {
            yaml_node_t *key, *value;

            key = yaml_document_get_node(doc, pair->key);
            value = yaml_document_get_node(doc, pair->value);

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value,
                            "address") == 0 && newag.ipstr == NULL) {
                newag.ipstr = strdup((char *)value->data.scalar.value);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value,
                            "port") == 0 && newag.portstr == NULL) {
                newag.portstr = strdup((char *)value->data.scalar.value);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value,
                            "agencyid") == 0 && newag.agencyid == NULL) {
                newag.agencyid = strdup((char *)value->data.scalar.value);
            }

        }
        if (newag.ipstr != NULL && newag.portstr != NULL &&
                newag.agencyid != NULL) {
            newag.knownliids = libtrace_list_init(sizeof(char *));
            libtrace_list_push_front(aglist, (void *)(&newag));
        } else {
            logger(LOG_DAEMON, "OpenLI: LEA configuration was incomplete -- skipping.");
        }
    }
    return 0;
}

static int parse_ipintercept_list(libtrace_list_t *ipints, yaml_document_t *doc,
        yaml_node_t *inputs) {

    yaml_node_item_t *item;
    int i;
    uint64_t nextid = 0;

    for (item = inputs->data.sequence.items.start;
            item != inputs->data.sequence.items.top; item ++) {
        yaml_node_t *node = yaml_document_get_node(doc, *item);
        ipintercept_t newcept;
        yaml_node_pair_t *pair;

        /* Each sequence item is a new intercept */
        newcept.internalid = nextid;
        nextid ++;

        newcept.liid = NULL;
        newcept.authcc = NULL;
        newcept.delivcc = NULL;
        newcept.cin = 0;
        newcept.ipaddr = NULL;
        newcept.ai_family = AF_UNSPEC;
        newcept.username = NULL;
        newcept.active = 1;
        newcept.destid = 0;
        newcept.targetagency = NULL;

        /* Mappings describe the parameters for each intercept */
        for (pair = node->data.mapping.pairs.start;
                pair < node->data.mapping.pairs.top; pair ++) {
            yaml_node_t *key, *value;

            key = yaml_document_get_node(doc, pair->key);
            value = yaml_document_get_node(doc, pair->value);

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value, "liid") == 0 &&
                    newcept.liid == NULL) {
                newcept.liid = strdup((char *)value->data.scalar.value);
                newcept.liid_len = strlen(newcept.liid);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value,
                            "authcountrycode") == 0 &&
                    newcept.authcc == NULL) {
                newcept.authcc = strdup((char *)value->data.scalar.value);
                newcept.authcc_len = strlen(newcept.authcc);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value,
                            "deliverycountrycode") == 0 &&
                    newcept.delivcc == NULL) {
                newcept.delivcc = strdup((char *)value->data.scalar.value);
                newcept.delivcc_len = strlen(newcept.delivcc);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value, "user") == 0 &&
                    newcept.username == NULL) {
                newcept.username = strdup((char *)value->data.scalar.value);
                newcept.username_len = strlen(newcept.username);
            }

            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value, "mediator") == 0
                    && newcept.destid == 0) {
                newcept.destid = strtoul((char *)value->data.scalar.value,
                        NULL, 10);
                if (newcept.destid == 0) {
                    logger(LOG_DAEMON, "OpenLI: 0 is not a valid value for the 'mediator' config option.");
                }
            }
            if (key->type == YAML_SCALAR_NODE &&
                    value->type == YAML_SCALAR_NODE &&
                    strcmp((char *)key->data.scalar.value, "agencyid") == 0
                    && newcept.targetagency == NULL) {
                newcept.targetagency = strdup((char *)value->data.scalar.value);
            }

        }

        if (newcept.liid != NULL && newcept.authcc != NULL &&
                newcept.delivcc != NULL && newcept.username != NULL &&
                newcept.destid > 0 && newcept.targetagency != NULL) {
            libtrace_list_push_front(ipints, (void *)(&newcept));
        } else {
            logger(LOG_DAEMON, "OpenLI: IP Intercept configuration was incomplete -- skipping.");
        }
    }

    return 0;
}

static int yaml_parser(char *configfile, void *arg,
        int (*parse_mapping)(void *, yaml_document_t *, yaml_node_t *,
                yaml_node_t *)) {
    FILE *in = NULL;
    yaml_parser_t parser;
    yaml_document_t document;
    yaml_node_t *root, *key, *value;
    yaml_node_pair_t *pair;
    int ret = -1;

    if ((in = fopen(configfile, "r")) == NULL) {
        logger(LOG_DAEMON, "OpenLI: Failed to open config file: %s",
                strerror(errno));
        return -1;
    }

    yaml_parser_initialize(&parser);
    yaml_parser_set_input_file(&parser, in);

    if (!yaml_parser_load(&parser, &document)) {
        logger(LOG_DAEMON, "OpenLI: Malformed config file");
        goto yamlfail;
    }

    root = yaml_document_get_root_node(&document);
    if (!root) {
        logger(LOG_DAEMON, "OpenLI: Config file is empty!");
        goto endconfig;
    }

    if (root->type != YAML_MAPPING_NODE) {
        logger(LOG_DAEMON, "OpenLI: Top level of config should be a map");
        goto endconfig;
    }
    for (pair = root->data.mapping.pairs.start;
            pair < root->data.mapping.pairs.top; pair ++) {

        key = yaml_document_get_node(&document, pair->key);
        value = yaml_document_get_node(&document, pair->value);

        if (parse_mapping(arg, &document, key, value) == -1) {
            ret = -1;
            break;
        }
        ret = 0;
    }
endconfig:
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);

yamlfail:
    fclose(in);
    return ret;
}


static int ipintercept_parser(void *arg, yaml_document_t *doc,
        yaml_node_t *key, yaml_node_t *value) {
    libtrace_list_t *ipints = (libtrace_list_t *)arg;

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SEQUENCE_NODE &&
            strcmp((char *)key->data.scalar.value, "ipintercepts") == 0) {
        if (parse_ipintercept_list(ipints, doc, value) == -1) {
            return -1;
        }
    }
    return 0;
}

int parse_export_config(char *configfile, libtrace_list_t *dests) {
    /* TODO replace with useful config for exporting threads */

}

int parse_ipintercept_config(char *configfile, libtrace_list_t *ipints) {

    return yaml_parser(configfile, ipints, ipintercept_parser);
}

static int global_parser(void *arg, yaml_document_t *doc,
        yaml_node_t *key, yaml_node_t *value) {
    collector_global_t *glob = (collector_global_t *)arg;
    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SEQUENCE_NODE &&
            strcmp((char *)key->data.scalar.value, "inputs") == 0) {
        if (parse_input_config(glob, doc, value) == -1) {
            clear_global_config(glob);
            return -1;
        }
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "operatorid") == 0) {
        glob->operatorid = strdup((char *) value->data.scalar.value);
        glob->operatorid_len = strlen(glob->operatorid);

        /* Limited to 16 chars */
        if (glob->operatorid_len > 16) {
            logger(LOG_DAEMON, "OpenLI: Operator ID must be 16 characters or less!");
            clear_global_config(glob);
            return -1;
        }
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "networkelementid")
            == 0) {
        glob->networkelemid = strdup((char *) value->data.scalar.value);
        glob->networkelemid_len = strlen(glob->networkelemid);

        /* Limited to 16 chars */
        if (glob->networkelemid_len > 16) {
            logger(LOG_DAEMON, "OpenLI: Network Element ID must be 16 characters or less!");
            clear_global_config(glob);
            return -1;
        }
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "interceptpointid")
            == 0) {
        glob->intpointid = strdup((char *) value->data.scalar.value);
        glob->intpointid_len = strlen(glob->intpointid);

        /* Limited to 8 chars */
        if (glob->intpointid_len > 8) {
            logger(LOG_DAEMON, "OpenLI: Intercept Point ID must be 8 characters or less!");
            clear_global_config(glob);
            return -1;
        }
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "provisionerport") == 0) {
        glob->provisionerport = strdup((char *) value->data.scalar.value);
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "provisionerip") == 0) {
        glob->provisionerip = strdup((char *) value->data.scalar.value);
    }

    return 0;
}

collector_global_t *parse_global_config(char *configfile) {

    collector_global_t *glob = NULL;

    glob = (collector_global_t *)malloc(sizeof(collector_global_t));

    glob->inputcount = 0;
    glob->inputalloced = 0;
    glob->inputs = NULL;
    glob->totalthreads = 0;
    glob->queuealloced = 0;
    glob->registered_syncqs = 0;
    glob->syncsendqs = NULL;
    glob->syncepollevs = 0;
    glob->intpointid = NULL;
    glob->intpointid_len = 0;
    glob->operatorid = NULL;
    glob->operatorid_len = 0;
    glob->networkelemid = NULL;
    glob->networkelemid_len = 0;
    glob->syncthreadid = 0;
    glob->exportthreadid = 0;
    glob->sync_epollfd = -1;
    glob->export_epollfd = -1;
    glob->configfile = configfile;
    glob->export_epoll_evs = NULL;
    glob->provisionerip = NULL;
    glob->provisionerport = NULL;

    pthread_mutex_init(&glob->syncq_mutex, NULL);

    if (yaml_parser(configfile, glob, global_parser) == -1) {
        return NULL;
    }

    if (glob->networkelemid == NULL) {
        logger(LOG_DAEMON, "OpenLI: No network element ID specified in config file. Exiting.\n");
        clear_global_config(glob);
        glob = NULL;
    }

    if (glob->operatorid == NULL) {
        logger(LOG_DAEMON, "OpenLI: No operator ID specified in config file. Exiting.\n");
        clear_global_config(glob);
        glob = NULL;
    }

    return glob;

}

static int provisioning_parser(void *arg, yaml_document_t *doc,
        yaml_node_t *key, yaml_node_t *value) {

    provision_state_t *state = (provision_state_t *)arg;

    libtrace_list_t *ipints = state->ipintercepts;

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SEQUENCE_NODE &&
            strcmp((char *)key->data.scalar.value, "ipintercepts") == 0) {
        if (parse_ipintercept_list(state->ipintercepts, doc, value) == -1) {
            return -1;
        }
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SEQUENCE_NODE &&
            strcmp((char *)key->data.scalar.value, "agencies") == 0) {
        if (parse_agency_list(state->leas, doc, value) == -1) {
            return -1;
        }
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "clientport") == 0) {
        state->listenport = strdup((char *) value->data.scalar.value);
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "clientaddr") == 0) {
        state->listenaddr = strdup((char *) value->data.scalar.value);
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "updateport") == 0) {
        state->pushport = strdup((char *) value->data.scalar.value);
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "updateaddr") == 0) {
        state->pushaddr = strdup((char *) value->data.scalar.value);
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "mediationport") == 0) {
        state->mediateport = strdup((char *) value->data.scalar.value);
    }

    if (key->type == YAML_SCALAR_NODE &&
            value->type == YAML_SCALAR_NODE &&
            strcmp((char *)key->data.scalar.value, "mediationaddr") == 0) {
        state->mediateaddr = strdup((char *) value->data.scalar.value);
    }
    return 0;
}

int parse_provisioning_config(char *configfile, provision_state_t *state) {

    return yaml_parser(configfile, state, provisioning_parser);
}
// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
