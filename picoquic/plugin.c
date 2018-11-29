#include "plugin.h"
#include <libgen.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "memory.h"
#include "picoquic_internal.h"

int plugin_plug_elf_param_struct(protocol_operation_param_struct_t *popst, protoop_plugin_t *p, pluglet_type_enum pte, char *elf_fname) {
    /* Fast track: if we want to insert a replace plugin while there is already one, it will never work! */
    if ((pte == pluglet_replace || pte == pluglet_extern) && popst->replace) {
        printf("Replace pluglet already inserted!\n");
        return 1;
    }

    if (!popst->intern && (pte == pluglet_pre || pte == pluglet_post)) {
        printf("External pluglet cannot have observers!\n");
        return 1;
    }

    if (popst->intern && pte == pluglet_extern && (popst->core || popst->pre || popst->post)) {
        printf("An internal pluglet already exists!\n");
        return 1;
    }

    /* Then check if we can load the plugin! */
    pluglet_t *new_pluglet = load_elf_file(elf_fname);
    if (!new_pluglet) {
        printf("Failed to insert %s\n", elf_fname);
        return 1;
    }
    /* Record the plugin pluglet comes from */
    new_pluglet->p = p;

    /* We cope with (nearly) all bad cases, so now insert */
    observer_node_t *new_node;
    switch (pte) {
    case pluglet_extern:
        popst->intern = false;
        /* this falls through intentionally */
    case pluglet_replace:
        popst->replace = new_pluglet;
        break;
    case pluglet_pre:
        new_node = malloc(sizeof(observer_node_t));
        if (!new_node) {
            printf("Cannot allocate memory to insert pre node with pluglet %s for pid\n", elf_fname);
            release_elf(new_pluglet);
            return 1;
        }
        new_node->observer = new_pluglet;
        new_node->next = popst->pre;
        popst->pre = new_node;
        break;
    case pluglet_post:
        new_node = malloc(sizeof(observer_node_t));
        if (!new_node) {
            printf("Cannot allocate memory to insert pre node with pluglet %s\n", elf_fname);
            release_elf(new_pluglet);
            return 1;
        }
        new_node->observer = new_pluglet;
        new_node->next = popst->post;
        popst->post = new_node;
        break;
    }

    return 0;
}

int plugin_plug_elf_noparam(protocol_operation_struct_t *post, protoop_plugin_t *p, protoop_str_id_t pid, pluglet_type_enum pte, char *elf_fname) {
    protocol_operation_param_struct_t *popst = post->params;
    /* Sanity check */
    if (post->is_parametrable) {
        printf("Trying to insert NO_PARAM in parametrable protocol operation %s\n", pid);
        return 1;
    }

    return plugin_plug_elf_param_struct(popst, p, pte, elf_fname);
}

int plugin_plug_elf_param(protocol_operation_struct_t *post, protoop_plugin_t *p, protoop_str_id_t pid, param_id_t param, pluglet_type_enum pte, char *elf_fname) {
    protocol_operation_param_struct_t *popst;
    bool created_popst = false;
    /* Sanity check */
    if (!post->is_parametrable) {
        printf("Trying to insert parameter %u in non-parametrable protocol operation %s\n", param, pid);
        return 1;
    }
    HASH_FIND(hh, post->params, &param, sizeof(param_id_t), popst);
    /* It is possible to have a new parameter with the pluglet */
    if (!popst) {
        popst = create_protocol_operation_param(param, NULL);
        created_popst = true;
        if (!popst) {
            printf("ERROR: cannot allocate memory for param struct when pluglet...\n");
            return 1;
        }
    }

    int err = plugin_plug_elf_param_struct(popst, p, pte, elf_fname);

    if (err) {
        if (created_popst) {
            /* Remove it */
            free(popst);
        }
        printf("Failed to insert pluglet for parametrable protocol operation %s with param %u\n", pid, param);
        return 1;
    }

    if (created_popst) {
        /* Insert in hash */
        HASH_ADD(hh, post->params, param, sizeof(param_id_t), popst);
    }

    return 0;
}

int plugin_plug_elf(picoquic_cnx_t *cnx, protoop_plugin_t *p, protoop_str_id_t pid_str, param_id_t param, pluglet_type_enum pte, char *elf_fname) {
    protocol_operation_struct_t *post;
    /* TODO rework me */
    protoop_id_t pid = hash_value_str(pid_str);
    HASH_FIND_INT(cnx->ops, &pid, post);

    /* Two cases: either it exists, or not */
    if (!post) {
        int err;
        if (param != NO_PARAM) {
            err = register_param_protoop(cnx, pid, param, NULL);
        } else {
            err = register_noparam_protoop(cnx, pid, NULL);
        }
        if (err) {
            printf("Failed to allocate resources for pid %s\n", pid_str);
            return 1;
        }
        /* This is not optimal, but this should not be frequent */
        HASH_FIND_INT(cnx->ops, &pid, post);
    }

    /* Again, two cases: either it is parametric or not */
    return param != NO_PARAM ? plugin_plug_elf_param(post, p, pid_str, param, pte, elf_fname) :
        plugin_plug_elf_noparam(post, p, pid_str, pte, elf_fname);
}

int plugin_unplug(picoquic_cnx_t *cnx, protoop_str_id_t pid, param_id_t param, pluglet_type_enum pte) {
    protocol_operation_struct_t *post;
    HASH_FIND_STR(cnx->ops, pid, post);

    if (!post) {
        printf("Trying to unplug pluglet for non-existing proto op id %s...\n", pid);
        return 1;
    }

    protocol_operation_param_struct_t *popst;
    if (param == NO_PARAM) {
        if (post->is_parametrable) {
            printf("Trying to remove NO_PARAM from parametrable protocol operation %s\n", pid);
            return 1;
        }
        popst = post->params;
    } else {
        if (!post->is_parametrable) {
            printf("Trying to remove param %u from non-parametrable protocol operation %s\n", param, pid);
            return 1;
        }
        HASH_FIND(hh, post->params, &param, sizeof(param_id_t), popst);
        if (!popst) {
            printf("Trying to remove non-existing param %u for protocol operation %s\n", param, pid);
            return 1;
        }
    }

    /* For pre/post, it's difficult to ensure we remove the right observer right now...
     * But if we made the assumption that removal is either performed when a plugin
     * fails (when the connection is deleted, another function handles it), as the list
     * is implemented as a stack, we can simply remove the first one.
     */
    observer_node_t *to_remove;
    switch (pte) {
    case pluglet_extern:
        if (popst->intern) {
            printf("Trying to unplug non-existing external pluglet for proto op id %s\n", pid);
            return 1;
        }
        /* this falls through intentionally */
    case pluglet_replace:
        if (!popst->replace) {
            printf("Trying to unplug non-existing replace pluglet for proto op id %s...\n", pid);
            return 1;
        }
        release_elf(popst->replace);
        popst->replace = NULL;
        break;
    case pluglet_pre:
        if (!popst->pre) {
            printf("Trying to unplug non-existing pre pluglet for proto op id %s...\n", pid);
            return 1;
        }
        to_remove = popst->pre;
        popst->pre = to_remove->next;
        release_elf(to_remove->observer);
        free(to_remove);
        to_remove = NULL;
        break;
    case pluglet_post:
        if (!popst->post) {
            printf("Trying to unplug non-existing post pluglet for proto op id %s...\n", pid);
            return 1;
        }
        to_remove = popst->post;
        popst->post = to_remove->next;
        release_elf(to_remove->observer);
        free(to_remove);
        to_remove = NULL;
        break;
    }

    /* Cope with a special case of a protoop without core op and with no more plugins */
    if (!popst->core && !popst->replace && !popst->pre && !popst->post) {
        /* If it is parametrable, we just remove popst from post->params */
        if (post->is_parametrable) {
            HASH_DEL(post->params, popst);
        }
        else {
            HASH_DEL(cnx->ops, post);
            free(post);
            post = NULL;
        }
        /* And free popst */
        free(popst);
    }

    return 0;
}

bool insert_pluglet_from_plugin_line(picoquic_cnx_t *cnx, char *line, protoop_plugin_t *p,
    char *plugin_dirname, protoop_str_id_t inserted_pid, param_id_t *param, pluglet_type_enum *pte)
{
    /* Part one: extract protocol operation id */
    char *token = strsep(&line, " ");
    if (token == NULL) {
        printf("No token for protocol operation id extracted!\n");
        return false;
    }
    strncpy(inserted_pid, token, strlen(token) + 1);

    /* Part one bis: extract param, if any */
    token = strsep(&line, " ");

    if (token == NULL) {
        printf("No param or keyword!\n");
        return false;
    }

    if (strncmp(token, "param", 5) == 0) {
        char *errmsg = NULL;
        token = strsep(&line, " ");
        if (token == NULL) {
            printf("No param value!\n");
            return false;
        }
        *param = (param_id_t) strtoul(token, &errmsg, 0);
        if (errmsg != NULL && strncmp(errmsg, "", 1) != 0) {
            printf("Invalid parameter %s, num is %u!\n", token, *param);
            return false;
        }
        token = strsep(&line, " ");
        if (token == NULL) {
            printf("No keyword value!\n");
            return false;
        }
    } else {
        *param = NO_PARAM;
    }

    /* Part two: extract pluglet type */
    if (strncmp(token, "replace", 7) == 0) {
        *pte = pluglet_replace;
    } else if (strncmp(token, "pre", 3) == 0) {
        *pte = pluglet_pre;
    } else if (strncmp(token, "post", 4) == 0) {
        *pte = pluglet_post;
    } else if (strncmp(token, "extern", 6) == 0) {
        *pte = pluglet_extern;
    } else {
        printf("Cannot extract the type of the pluglet: %s\n", token);
        return false;
    }

    /* Part three: insert the plugin */
    token = strsep(&line, " ");
    if (token == NULL) {
        printf("No token for ebpf filename extracted!\n");
        return false;
    }

    /* Handle end of line */
    token[strcspn(token, "\r\n")] = 0;

    char abs_path[250];
    strncpy(abs_path, plugin_dirname, 250);
    strcat(abs_path, "/");
    strcat(abs_path, token);
    return plugin_plug_elf(cnx, p, inserted_pid, *param, *pte, abs_path) == 0;
}

protoop_plugin_t* plugin_parse_first_plugin_line(picoquic_cnx_t* cnx, char *line) {
    /* Part one: extract plugin id */
    char *token = strsep(&line, " ");
    if (token == NULL) {
        printf("No token for protocol operation id extracted!\n");
        return false;
    }

    /* Handle end of line  FIXME Move me later when parameters are present */
    token[strcspn(token, "\r\n")] = 0;

    if (strchr(token, '.') == token + strlen(token)) {
        /* No hierarchical name found, refuse it! */
        printf("The name of the plugin is not hierarchical; discard it!\n");
        return NULL;
    }

    protoop_plugin_t *p = calloc(1, sizeof(protoop_plugin_t));
    if (!p) {
        printf("Cannot allocate memory for plugin!\n");
        return NULL;
    }

    strncpy(p->name, token, PROTOOPPLUGINNAME_MAX);
    p->block_queue = queue_init();
    if (!p->block_queue) {
        printf("Cannot allocate memory for sending queue!\n");
        free(p);
        return NULL;
    }
    /* TODO make this value configurable */
    p->max_budget = 7500;
    p->budget = p->max_budget;
    return p;
}

typedef struct pid_node {
    char pid[100];
    param_id_t param;
    pluglet_type_enum pte;
    struct pid_node *next;
} pid_node_t;

int plugin_insert_plugin(picoquic_cnx_t *cnx, const char *plugin_fname) {
    FILE *file = fopen(plugin_fname, "r");

    if (file == NULL) {
        fprintf(stderr, "Failed to open %s: %s\n", plugin_fname, strerror(errno));
        return 1;
    }

    char buf[250];
    strncpy(buf, plugin_fname, 250);
    char *line = NULL;
    size_t len = 0;
    ssize_t read = 0;
    bool ok = true;
    char *plugin_dirname = dirname(buf);
    char inserted_pid[100];
    param_id_t param;
    pluglet_type_enum pte;
    pid_node_t *pid_stack_top = NULL;
    pid_node_t *tmp = NULL;

    read = getline(&line, &len, file);
    if (read == -1) {
        printf("Error in the file %s\n", plugin_fname);
        return 1;
    }

    protoop_plugin_t *p = plugin_parse_first_plugin_line(cnx, line);
    if (!p) {
        printf("Cannot extract plugin line in file %s\n", plugin_fname);
        return 1;
    }

    while (ok && (read = getline(&line, &len, file)) != -1) {
        /* Skip blank lines */
        if (len <= 1) {
            continue;
        }
        ok = insert_pluglet_from_plugin_line(cnx, line, p, plugin_dirname, (protoop_str_id_t ) inserted_pid, &param, &pte);
        if (ok) {
            /* Keep track of the inserted pids */
            tmp = (pid_node_t *) malloc(sizeof(pid_node_t));
            if (!tmp) {
                printf("No enough memory to allocate stack nodes; abort\n");
                ok = false;
                break;
            }
            strncpy(tmp->pid, inserted_pid, strlen(inserted_pid) + 1);
            tmp->param = param;
            tmp->pte = pte;
            tmp->next = pid_stack_top;
            pid_stack_top = tmp;
        }
    }

    while (pid_stack_top != NULL) {
        if (!ok) {
            /* Unplug previously plugged code */
            plugin_unplug(cnx, pid_stack_top->pid, pid_stack_top->param, pid_stack_top->pte);
        }
        tmp = pid_stack_top->next;
        free(pid_stack_top);
        pid_stack_top = tmp;
    }

    if (!ok) {
        free(p);
    } else {
        init_memory_management(p);
        HASH_ADD_STR(cnx->plugins, name, p);
    }

    if (line) {
        free(line);
    }

    fclose(file);

    return ok ? 0 : 1;
}

void *get_opaque_data(picoquic_cnx_t *cnx, opaque_id_t oid, size_t size, int *allocated) {
    if (!cnx->current_plugin) {
        printf("ERROR: get_opaque_data can only be called by pluglets with plugins!\n");
        return NULL;
    }
    picoquic_opaque_meta_t *ometas = cnx->current_plugin->opaque_metas;
    if (oid >= OPAQUE_ID_MAX) {
        printf("ERROR: pluglet from plugin %s ask for opaque id %u >= max opaque id %d\n", cnx->current_plugin->name, oid, OPAQUE_ID_MAX);
        /* Invalid ID */
        return NULL;
    }
    if (ometas[oid].start_ptr) {
        if (ometas[oid].size != size) {
            /* The size requested is not correct */
            return NULL;
        }
        *allocated = 0;
        return ometas[oid].start_ptr;
    }
    /* Try to allocate memory with my_malloc */
    ometas[oid].start_ptr = my_malloc(cnx, size);
    if (!ometas[oid].start_ptr) {
        /* No space left... */
        return NULL;
    }

    /* Keep track of some meta data and returns the pointer */
    ometas[oid].size = size;
    *allocated = 1;
    return ometas[oid].start_ptr;
}

protoop_arg_t plugin_run_protoop(picoquic_cnx_t *cnx, const protoop_params_t *pp) {
    if (pp->inputc > PROTOOPARGS_MAX) {
        printf("Too many arguments for protocol operation with hash %d : %d > %d\n",
            pp->pid, pp->inputc, PROTOOPARGS_MAX);
        return PICOQUIC_ERROR_PROTOCOL_OPERATION_TOO_MANY_ARGUMENTS;
    }

    char *error_msg = NULL;

    /* First save previous args, and update context with new ones
     * Notice that we store ALL array of protoop_inputv and protoop_outputv.
     * With this, even if the called pluglet tried to modify the input arguments,
     * they will remain unchanged at caller side.
     */
    protoop_plugin_t *old_plugin = cnx->current_plugin;
    int caller_inputc = cnx->protoop_inputc;
    int caller_outputc = cnx->protoop_outputc_callee;
    uint64_t *caller_inputv[PROTOOPARGS_MAX];
    uint64_t *caller_outputv[PROTOOPARGS_MAX];
    memcpy(caller_inputv, cnx->protoop_inputv, sizeof(uint64_t) * PROTOOPARGS_MAX);
    memcpy(caller_outputv, cnx->protoop_outputv, sizeof(uint64_t) * PROTOOPARGS_MAX);
    memcpy(cnx->protoop_inputv, pp->inputv, sizeof(uint64_t) * pp->inputc);
    cnx->protoop_inputc = pp->inputc;

#ifdef DBG_PLUGIN_PRINTF
    for (int i = 0; i < pp->inputc; i++) {
        DBG_PLUGIN_PRINTF("Arg %d: 0x%lx", i, inputv[i]);
    }
#endif

    /* Also set protoop_outputv to 0, to prevent callee to see caller state */
    memset(cnx->protoop_outputv, 0, sizeof(uint64_t) * PROTOOPARGS_MAX);
    cnx->protoop_outputc_callee = 0;

    DBG_PLUGIN_PRINTF("Running operation with id 0x%x with %d inputs", pid, inputc);

    /* Either we have a pluglet, and we run it, or we stick to the default ops behaviour */
    protoop_arg_t status;
    protocol_operation_struct_t *post;
    HASH_FIND_INT(cnx->ops, &pp->pid, post);
    if (!post) {
        /* TODO we should store the asked str_pid somewhere... */
        printf("FATAL ERROR: no protocol operation with hash id %d\n", pp->pid);
        exit(-1);
    }

    protocol_operation_param_struct_t *popst;
    if (post->is_parametrable) {
        HASH_FIND(hh, post->params, &pp->param, sizeof(param_id_t), popst);
        if (!popst) {
            param_id_t default_behaviour = NO_PARAM;
            HASH_FIND(hh, post->params, &default_behaviour, sizeof(param_id_t), popst);
            if (!popst) {
                printf("FATAL ERROR: no protocol operation with id %d and param %u, no default behaviour!\n", pp->pid, pp->param);
                exit(-1);
            }
        }
    } else {
        popst = post->params;
    }

    if (pp->caller_is_intern != popst->intern) {
        if (pp->caller_is_intern) {
            printf("FATAL ERROR: Intern caller cannot call extern protocol operation with id %d and param %u\n", pp->pid, pp->param);
        } else {
            printf("FATAL ERROR: Extern caller cannot call intern protocol operation with id %d and param %u\n", pp->pid, pp->param);
        }
        exit(-1);
    }

    /* First, is there any pre to run? */
    observer_node_t *tmp = popst->pre;
    while (tmp) {
        /* TODO: restrict the memory accesible by the observers */
        cnx->current_plugin = tmp->observer->p;
        exec_loaded_code(tmp->observer, (void *)cnx, (void *)cnx->current_plugin->memory, sizeof(cnx->current_plugin->memory), &error_msg);
        tmp = tmp->next;
    }

    /* The actual protocol operation */
    if (popst->replace) {
        DBG_PLUGIN_PRINTF("Running pluglet at proto op id %d", pid);
        cnx->current_plugin = popst->replace->p;
        status = (protoop_arg_t) exec_loaded_code(popst->replace, (void *)cnx, (void *)cnx->current_plugin->memory, sizeof(cnx->current_plugin->memory), &error_msg);
        if (error_msg) {
            /* TODO fixme str_pid */
            fprintf(stderr, "Error when running %d: %s\n", pp->pid, error_msg);
        }
    } else if (popst->core) {
        cnx->current_plugin = NULL;
        status = popst->core(cnx);
    } else {
        /* TODO fixme str_pid */
        printf("FATAL ERROR: no replace nor core operation for protocol operation with hash id %d\n", pp->pid);
        exit(-1);
    }

    /* The previous plugin look at the last replace one */
    cnx->previous_plugin = cnx->current_plugin;

    /* Finally, is there any post to run? */
    tmp = popst->post;
    if (tmp) {
        cnx->protoop_output = status;
    }
    while (tmp) {
        /* TODO: restrict the memory accesible by the observers */
        cnx->current_plugin = tmp->observer->p;
        exec_loaded_code(tmp->observer, (void *)cnx, (void *)cnx->current_plugin->memory, sizeof(cnx->current_plugin->memory), &error_msg);
        tmp = tmp->next;
    }
    cnx->protoop_output = 0;

    int outputc = cnx->protoop_outputc_callee;

    DBG_PLUGIN_PRINTF("Protocol operation with id 0x%x returns 0x%lx with %d additional outputs", pid, status, outputc);

    /* Copy the output of the caller to the provided output pointer (if any)... */
    if (pp->outputv) {
        memcpy(pp->outputv, cnx->protoop_outputv, sizeof(uint64_t) * outputc);
#ifdef DBG_PLUGIN_PRINTF
        for (int i = 0; i < outputc; i++) {
            DBG_PLUGIN_PRINTF("Out %d: 0x%lx", i, outputv[i]);
        }
#endif
    } else if (outputc > 0) {
        printf("WARNING: no output value provided for protocol operation with id %d and param %u that returns %d additional outputs\n", pp->pid, pp->param, outputc);
        printf("HINT: this is probably not what you want, so maybe check if you called the right protocol operation...\n");
    }

    /* ... and restore ALL the previous inputs and outputs */
    memcpy(cnx->protoop_inputv, caller_inputv, sizeof(uint64_t) * PROTOOPARGS_MAX);
    memcpy(cnx->protoop_outputv, caller_outputv, sizeof(uint64_t) * PROTOOPARGS_MAX);
    cnx->protoop_inputc = caller_inputc;

    /* Also reset outputc to zero; if this protoop was called by another one that does not have any output,
     * it will likely not specify the outputc value, as it expects it to remain 0...
     */
    cnx->protoop_outputc_callee = caller_outputc;

    /* Also restore the plugin context */
    cnx->current_plugin = old_plugin;

    return status;
}