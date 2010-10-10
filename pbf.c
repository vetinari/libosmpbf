#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

#include "fileformat.pb-c.h"
#include "osmformat.pb-c.h"

#include "osm.h"

#define LIST_THRESHOLD 0.9

OSM_Data *osm_pbf_parse(OSM_File *F, 
              uint32_t mode, 
              OSM_BBox *bbox,
              int (*node_filter)(OSM_Node *),
              int (*way_filter)(OSM_Way *),
              int (*rel_filter)(OSM_Relation *) /*,
              int (*cset_filter)(OSM_Changeset *) */
        )
{
    uint32_t length;
    BlockHeader *bh = NULL;
    Blob      *blob = NULL;
    struct osm_members *mem_nodes = NULL;
    struct osm_members *mem_ways  = NULL;
    struct osm_members *bbn = NULL;
    enum {
        osm_pbf_initializer,
        osm_pbf_header,
        osm_pbf_data
    } state = osm_pbf_initializer;

    enum {
        bbox_no_bbox,
        bbox_nodes_in_box,
        bbox_rel_find,
        bbox_way_find,
        bbox_nodes_find
    } bbox_state = bbox_no_bbox;
   
    if (mode == 0) {
        fprintf(stderr, "mode cannot be 0...\n");
        return (OSM_Data *)NULL;
    }
    else if (mode == OSMDATA_BBOX) {
        if (bbox == NULL) {
            fprintf(stderr, "mode = OSMDATA_BBOX, but bbox is NULL\n");
            return (OSM_Data *)NULL;
        }
        else {
            bbox_state = bbox_nodes_in_box;
        }
    }

    OSM_Data *data = malloc(sizeof(OSM_Data));
    if (data == NULL) {
        fprintf(stderr, "failed to malloc OSM_Data: %s\n", strerror(errno));
        return (OSM_Data *)NULL;
    }
    data->nodes = malloc(sizeof(OSM_Node_List));
    data->nodes->num = 0;
    data->nodes->size = 65536;
    data->nodes->data = malloc(sizeof(OSM_Node) * 65536);

    data->ways = malloc(sizeof(OSM_Way_List));
    data->ways->num = 0;
    data->ways->size = 65536;
    data->ways->data = malloc(sizeof(OSM_Way) * 65536);
   
    data->relations = malloc(sizeof(OSM_Relation_List));
    data->relations->num = 0;
    data->relations->size = 65536;
    data->relations->data = malloc(sizeof(OSM_Relation) * 65536);

    if (mode != OSMDATA_DUMP) {
        mem_nodes = malloc(sizeof(struct osm_members));
        mem_nodes->data = malloc(sizeof(uint64_t) * 65536);
        mem_nodes->num  = 0;
        mem_nodes->size = 65536;
        mem_ways = malloc(sizeof(struct osm_members));
        mem_ways->data = malloc(sizeof(uint64_t) * 65536);
        mem_ways->num  = 0;
        mem_ways->size = 65536;
    }

    bbn = malloc(sizeof(struct osm_members));
    bbn->data = malloc(sizeof(uint64_t) * 65536);    
    bbn->size = 65536;
    bbn->num  = 0;

  restart:
    while (1) {
        length = osm_pbf_bh_length(F);
        if (length <= 0 || length > MAX_BLOCK_HEADER_SIZE) {
            if (length == -1) { /* @EOF */
                if (mode & (OSMDATA_DUMP|OSMDATA_NODE)) {
                    if (debug)
                        fprintf(stderr, "all parsing done.\n");
                    return data;
                }

                if (mode & (OSMDATA_WAY|OSMDATA_REL)) {
                    fseek(F->file, 0, SEEK_SET);
                    osm_sort_member(mem_ways);
                    osm_sort_member(mem_nodes);
                    if (mode == OSMDATA_REL) {
                        if (debug) 
                            fprintf(stderr, "parsing relations done: %u, %u, %u.\n",
                                             data->relations->num, mem_ways->num, mem_nodes->num);
                        
                        mode = OSMDATA_WAY;
                    }
                    else if (mode == OSMDATA_WAY) {
                        if (debug) 
                            fprintf(stderr, "parsing ways done: %u, n=%u\n",
                                            data->ways->num, mem_nodes->num);
                        mode = OSMDATA_NODE;
                    }
                    goto restart;
                }
                else if (mode == OSMDATA_BBOX) {
                    fseek(F->file, 0, SEEK_SET);
                    switch (bbox_state) {
                        case bbox_nodes_find:
                            if (debug)
                                fprintf(stderr, "nodes: %d\n", data->nodes->num);
                            return data;
                            break;
                        case bbox_way_find:
                            if (debug)
                                fprintf(stderr, "way members: %u\n", mem_ways->num);
                            bbox_state = bbox_nodes_find;
                            break;            
                        case bbox_rel_find:
                            if (debug)
                                fprintf(stderr, "rel members: ways=%u, nodes=%u\n", mem_ways->num, mem_nodes->num);
                            bbox_state = bbox_way_find;
                            break;            
                        case bbox_nodes_in_box:
                            bbox_state = bbox_rel_find;
                            osm_sort_member(bbn);
                            if (debug)
                                fprintf(stderr, "Nodes in BBOX: %d\n", bbn->num);
                            break;
                        default:
                            fprintf(stderr, "mode = OSMDATA_BBOX, but state "
                                            "is bbox_no_bbox\n");
                            exit(1);
                            break;
                    }
                    goto restart;
                } 
            }

            fprintf(stderr, "Block Header isn't present or exceeds "
                            "minimum/maximum size: %u\n", length);
            return (OSM_Data *)NULL;
        }

        bh = osm_pbf_get_bh(F, length);
        length = bh->datasize;
        if (length <= 0 || length > MAX_BLOB_SIZE) {
            fprintf(stderr, "Blob isn't present or exceeds "
                            "minimum/maximum size\n");
            return (OSM_Data *)NULL;
        }

        if (strcmp(bh->type, "OSMHeader") == 0) {
            state = osm_pbf_header;
        }
        else if (strcmp(bh->type, "OSMData") == 0) {
            state = osm_pbf_data;
        }
        osm_pbf_free_bh(bh);

        unsigned char *uncompressed;
        blob = osm_pbf_get_blob(F, length, &uncompressed);

        if (state == osm_pbf_header) {

        }
        else if (state == osm_pbf_data) {
            PrimitiveBlock *P = osm_pbf_unpack_data(blob, uncompressed);
            double lat_offset  = NANO_DEGREE * P->lat_offset;
            double lon_offset  = NANO_DEGREE * P->lon_offset;
            double granularity = NANO_DEGREE * P->granularity;

            unsigned int j;
            for (j = 0; j < P->n_primitivegroup; j++) {
                /* fprintf(stderr,"\t\t""Nodes: %li""\n"\
                   "\t\t""Ways: %li""\n"\
                   "\t\t""Relations: %li""\n",
                   (P->primitivegroup[j]->dense ?
                    P->primitivegroup[j]->dense->n_id :
                           P->primitivegroup[j]->n_nodes),
                   P->primitivegroup[j]->n_ways,
                   P->primitivegroup[j]->n_relations); */
                int k;
                if (mode & (OSMDATA_DUMP|OSMDATA_NODE) 
                    || (mode == OSMDATA_BBOX 
                        && (bbox_state == bbox_nodes_in_box 
                            || bbox_state == bbox_nodes_find)))
                {
                    if (P->primitivegroup[j]->n_nodes > 0) {
                        for (k = 0; k < P->primitivegroup[j]->n_nodes; k++) {
                            Node *node = P->primitivegroup[j]->nodes[k];

                            OSM_Node *n = malloc(sizeof(OSM_Node));
                            n->id = node->id;
                            n->lat = lat_offset + (node->lat * granularity);
                            n->lon = lon_offset + (node->lon * granularity);
                            if (bbox_state == bbox_nodes_in_box) {
                                if (n->lon    >= bbox->left_lon
                                    && n->lon <= bbox->right_lon
                                    && n->lat >= bbox->bottom_lat
                                    && n->lat <= bbox->top_lat)
                                {
                                    uint64_t *mid = malloc(sizeof(uint64_t));
                                    *mid = n->id;
                                    osm_add_members(bbn, 1, mid, 0);
                                    /* 
                                    osm_realloc_bbox_nodes(bbn);
                                    bbn->id[ bbn->num ] = n->id;
                                    bbn->num += 1;
                                    */
                                    if (debug) 
                                        fprintf(stderr, "NODE %lu (%.7f, %.7f) is in bbox\n", n->id, n->lon, n->lat);
                                    free(n);
                                    continue;
                                }
                            }
                            if (node->info) {
                                Info *I = node->info;
                                if (I->has_version) 
                                    n->version = I->version;
                                else n->version = 0;
                                if (I->has_changeset)
                                    n->changeset = I->changeset;
                                else n->changeset = 0;

                                if (I->has_user_sid) {
                        
                                    ProtobufCBinaryData user = 
                                        P->stringtable->s[I->user_sid];
                                    n->user = strndup((const char *)user.data, user.len);
                                }
                                else n->user = strdup("");
                                if (I->has_uid)
                                    n->uid = I->uid;
                                else n->uid = 0;
                                if (I->has_timestamp) {
                                    n->timestamp = I->timestamp * (P->date_granularity / 1000);
                                } else n->timestamp = 0;
                                
                            } /* node->has_info */
                            OSM_Tag_List *tl = NULL;
                            if (node->n_keys && node->n_vals) {
                                tl       = malloc(sizeof(OSM_Tag_List));
                                tl->num  = node->n_keys;
                                tl->size = node->n_keys;
                                tl->data = malloc(sizeof(OSM_Tag) * node->n_keys);
                                int x;
                                for (x=0; x<node->n_keys; x++) {
                                    ProtobufCBinaryData key = 
                                        P->stringtable->s[node->keys[x]];
                                    ProtobufCBinaryData val = 
                                        P->stringtable->s[node->vals[x]];
                                    tl->data[x].key = 
                                                strndup((const char *)key.data, key.len);
                                    tl->data[x].val =
                                                strndup((const char *)val.data, val.len);
                                }
                            }
                            n->tags = tl;
                            if (mode == OSMDATA_BBOX) {
                                if (bbox_state == bbox_nodes_find) {
                                    if (osm_is_member(mem_nodes, n->id) == -1) {
                                        if (osm_is_member(bbn, n->id) == -1) {
                                            osm_free_node(n);
                                            continue;
                                        }
                                        else {
                                            if (node_filter != NULL && !node_filter(n)) {
                                                osm_free_node(n);
                                                continue;
                                            }
                                        }
                                    }
 
                                    if (debug) 
                                        fprintf(stderr, "NODE %lu (%.7f, %.7f) is needed\n", n->id, n->lon, n->lat);
                                        
                                }
                            }
                            else {
                                if (mode == OSMDATA_NODE && node_filter != NULL) {
                                    if ((osm_is_member(mem_nodes, n->id) == -1) 
                                        && 
                                        !node_filter(n))
                                        {
                                            osm_free_node(n);
                                            continue;
                                        }
                                }
                                else if (mode == OSMDATA_NODE
                                            &&
                                         osm_is_member(mem_nodes, n->id) == -1) 
                                {
                                        osm_free_node(n);
                                        continue;
                                }
                                else if (node_filter != NULL && !node_filter(n))
                                {
                                    osm_free_node(n);
                                    continue;
                                }
                            }
                            osm_realloc_node_list(data->nodes);
                            data->nodes->data[ data->nodes->num ] = n;
                            data->nodes->num += 1;
                            // osm_pbf_add_node(node_list, n);
                        }
                    } /* P->primitivegroup[j]->n_nodes > 0 */
                    if (P->primitivegroup[j]->dense) {
                        int l = 0;
                        unsigned long int deltaid = 0;
                        long int deltalat = 0;
                        long int deltalon = 0;
                        long int deltatimestamp = 0;
                        long int deltachangeset = 0;
                        long int deltauid = 0;
                        long int deltauser_sid = 0;
                        int k;
                        DenseNodes *D = P->primitivegroup[j]->dense;

                        for (k=0; k<D->n_id; k++) {
                            deltaid  += D->id[k];
                            deltalat += D->lat[k];
                            deltalon += D->lon[k];

                            OSM_Node *n = malloc(sizeof(OSM_Node));
                            n->tags = NULL;
                            n->id  = deltaid;
                            n->lat = lat_offset + (deltalat * granularity);
                            n->lon = lon_offset + (deltalon * granularity);
                            n->user = "";
                            n->timestamp = 0;
                            n->version   = 0;
                            n->uid       = 0;
                            n->changeset = 0;
                            if (D->denseinfo) {
                                DenseInfo *I = D->denseinfo;
                                deltatimestamp += I->timestamp[k];
                                deltachangeset += I->changeset[k];
                                deltauid       += I->uid[k];
                                deltauser_sid  += I->user_sid[k];

                                n->version   = I->version[k];
                                n->changeset = deltachangeset;
                                n->user = strndup((const char *)P->stringtable->s[deltauser_sid].data, P->stringtable->s[deltauser_sid].len);
                                n->uid       = deltauid;
                                n->timestamp = deltatimestamp * (P->date_granularity / 1000);
                            }

                            OSM_Tag_List *tl = NULL;
                            if (l < D->n_keys_vals) {
                                while (D->keys_vals[l] != 0 && l < D->n_keys_vals) {
                                    if (tl == NULL) {
                                        tl       = malloc(sizeof(OSM_Tag_List));
                                        tl->num  = 0;
                                        tl->size = 16;
                                        tl->data = malloc(sizeof(OSM_Tag) * 16);
                                    } 
                                    else {
                                        osm_realloc_tag_list(tl);
                                    }
                                    int o = D->keys_vals[l];
                                    int p = D->keys_vals[l+1];
                                    tl->data[tl->num].key = 
                                        strndup((const char *)P->stringtable->s[o].data, P->stringtable->s[o].len);
                                    tl->data[tl->num].val = 
                                        strndup((const char *)P->stringtable->s[p].data, P->stringtable->s[p].len);
                                    tl->num += 1;
                                    l += 2;
                                }
                                l += 1;
                            }
                            n->tags = tl;

                            if (mode == OSMDATA_BBOX) {
                                if (bbox_state == bbox_nodes_in_box) {
                                    if (   n->lat >= bbox->bottom_lat
                                        && n->lat <= bbox->top_lat
                                        && n->lon >= bbox->left_lon 
                                        && n->lon <= bbox->right_lon) 
                                    {

                                        // if (node_filter == NULL
                                        //     || node_filter(N))
                                        {
                                            uint64_t *mid
                                                = malloc(sizeof(uint64_t));
                                            *mid = n->id;
                                            osm_add_members(bbn, 1, mid, 0);
                                            if (debug) 
                                                fprintf(stderr, "NODE %lu (%.7f, %.7f) is in bbox\n", n->id, n->lon, n->lat);
                                        }
                                    }
                                    osm_free_node(n);
                                    continue;
                                }
                                else if (bbox_state == bbox_nodes_find) {
                                    if (osm_is_member(mem_nodes, n->id) == -1) {
                                        if (osm_is_member(bbn, n->id) == -1) {
                                            osm_free_node(n);
                                            continue;
                                        }
                                        else {
                                            if (node_filter != NULL && !node_filter(n)) {
                                                osm_free_node(n);
                                                continue;
                                            }
                                        }
                                    }
                                    
                                    if (debug) 
                                        fprintf(stderr, "NODE %lu (%.7f, %.7f) is needed\n", n->id, n->lon, n->lat);
                                        
                                }
                            }
                            else {
                                if (mode == OSMDATA_NODE && node_filter != NULL)
                                {
                                    if ((osm_is_member(mem_nodes, n->id) == -1) 
                                        && 
                                        !node_filter(n))
                                    {
                                        osm_free_node(n);
                                        continue;
                                    }
                                }
                                else if (mode == OSMDATA_NODE
                                            &&
                                         osm_is_member(mem_nodes, n->id) == -1) 
                                {
                                        osm_free_node(n);
                                        continue;
                                }
                                else if (node_filter != NULL && !node_filter(n))
                                {
                                    osm_free_node(n);
                                    continue;
                                }
                            }
                            osm_realloc_node_list(data->nodes);
                            data->nodes->data[ data->nodes->num ] = n;
                            data->nodes->num += 1;
                        }
                    } /* P->primitivegroup[j]->dense */
                } /* mode == OSMDATA_DUMP || OSMDATA_NODE */

                if (mode & (OSMDATA_DUMP|OSMDATA_WAY) 
                    || (mode == OSMDATA_BBOX && bbox_state == bbox_way_find)) {
                    if (P->primitivegroup[j]->n_ways > 0) {
                        int k = 0;
                        for (k = 0; k < P->primitivegroup[j]->n_ways; k++) {
                            Way *W = P->primitivegroup[j]->ways[k];

                            OSM_Way *way = malloc(sizeof(OSM_Way));
                            uint64_t *ref = NULL;
                            int l;
                            way->id = W->id;
                            if (W->info) {
                                Info *I = W->info;
                                if (I->has_version)
                                    way->version = I->version;
                                else way->version = 0;

                                if (I->has_changeset)
                                    way->changeset = I->changeset;
                                else way->changeset = 0;

                                if (I->has_user_sid) {
                                    ProtobufCBinaryData user = 
                                        P->stringtable->s[I->user_sid];
                                    way->user = strndup((const char *)user.data, user.len);
                                } else way->user = strdup("");

                                if (I->has_uid)
                                    way->uid = I->uid;
                                else 
                                    way->uid = 0;
            
                                if (I->has_timestamp)
                                    way->timestamp = I->timestamp * (P->date_granularity / 1000);
                                else way->timestamp = 0;
                            }

                            if (W->n_refs == 0) {
                                way->nodes = malloc(sizeof(uint64_t));
                                way->nodes[0] = 0;
                            }
                            else {
                                ref = malloc(sizeof(uint64_t) * W->n_refs);
                                long int deltaref = 0;
                                way->nodes = malloc(sizeof(uint64_t)*(W->n_refs+1));
                                way->nodes[W->n_refs] = 0;
                                for (l = 0; l < W->n_refs; l++) {
                                    deltaref += W->refs[l];
                                    way->nodes[l] = deltaref;
                                    ref[l] = deltaref;
                                }
                            }

                            OSM_Tag_List *tl = NULL;
                            if (W->n_keys > 0) {
                                tl       = malloc(sizeof(OSM_Tag_List));
                                tl->num  = W->n_keys;
                                tl->size = W->n_keys;
                                tl->data = malloc(sizeof(OSM_Tag) * W->n_keys);
                            }
                            for (l=0; l<W->n_keys; l++) {
                                ProtobufCBinaryData key = 
                                    P->stringtable->s[W->keys[l]];
                                ProtobufCBinaryData val = 
                                    P->stringtable->s[W->vals[l]];
                                tl->data[l].key = 
                                            strndup((const char *)key.data, key.len);
                                tl->data[l].val =
                                            strndup((const char *)val.data, val.len);
                                // fprintf(stderr, "WAY=%lu, k='%s', v='%s'\n", W->id, tl->data[l].key, tl->data[l].val);
                            }
                            way->tags = tl;

                            if (bbox_state == bbox_way_find) {
                                int b = 0;
                                int bbox_member = 0;
                                for (b=0; b<W->n_refs; b++) {
                                    if (osm_is_member(bbn, ref[b]) != -1) {
                                        if (way_filter == NULL || way_filter(way)) {
                                            if (debug) 
                                                fprintf(stderr, "way %lu: member %lu is in bbox\n",
                                                                W->id, ref[b]);
                                            bbox_member = 1;
                                            break;
                                        }
                                    }
                                }
                                if (bbox_member == 0 && osm_is_member(mem_ways, W->id) == -1) { 
                                    osm_free_way(way);
                                    free(ref);
                                    continue;
                                }
                            }
                            else {
                                if (mode == OSMDATA_WAY && way_filter != NULL) {
                                    if ((osm_is_member(mem_ways, W->id) == -1) 
                                        && 
                                        !way_filter(way))
                                        {
                                            osm_free_way(way);
                                            free(ref);
                                            continue;
                                        }
                                }
                                else if (mode == OSMDATA_WAY
                                            &&
                                         osm_is_member(mem_ways, W->id) == -1) 
                                {
                                        osm_free_way(way);
                                        free(ref);
                                        continue;
                                }
                                else if (way_filter != NULL && !way_filter(way))
                                {
                                    osm_free_way(way);
                                    free(ref);
                                    continue;
                                }
                            }
                            if (W->n_refs) 
                                osm_add_members(mem_nodes, W->n_refs, ref, 1);
                            if (debug)
                                fprintf(stderr, "adding % 6d members to way=%lu list\n", (int)W->n_refs, W->id);
                            osm_realloc_way_list(data->ways);
                            data->ways->data[ data->ways->num ] = way;
                            data->ways->num += 1;
                        }
                    } /* P->primitivegroup[j]->n_ways > 0 */
                } /* mode == OSMDATA_DUMP || OSMDATA_WAY */

                if (mode & (OSMDATA_DUMP|OSMDATA_REL) 
                    || (mode == OSMDATA_BBOX && bbox_state == bbox_rel_find))
                {
                    if (P->primitivegroup[j]->n_relations > 0) {
                        int k;
                        for (k=0; k<P->primitivegroup[j]->n_relations; k++) {
                            Relation *R = P->primitivegroup[j]->relations[k];
                            OSM_Relation *rel = malloc(sizeof(OSM_Relation));
                            
                            rel->id = R->id;
                            if (R->info) {
                                Info *I = R->info;
        
                                if (I->has_version)
                                    rel->version = I->version;
                                else rel->version = 0;

                                if (I->has_changeset)
                                    rel->changeset = I->changeset;
                                else rel->changeset = 0;

                                if (I->has_user_sid) {
                                    ProtobufCBinaryData user = 
                                        P->stringtable->s[I->user_sid];
                                    rel->user = strndup((const char *)user.data, user.len);
                                } else rel->user = strdup("");

                                if (I->has_uid)
                                    rel->uid = I->uid;
                                else 
                                    rel->uid = 0;
            
                                if (I->has_timestamp)
                                    rel->timestamp = I->timestamp * (P->date_granularity / 1000);
                                else rel->timestamp = 0;
                            }

                            uint64_t *wref = NULL;
                            uint64_t *nref = NULL;
                            int num_wref = 0;
                            int num_nref = 0;
                            if (R->n_memids == 0) {
                                rel->member = NULL;
                            }
                            else {
                                long int deltamemids = 0;
                                rel->member = malloc(sizeof(OSM_Rel_Member_List)); 
                                rel->member->num = R->n_memids;
                                rel->member->size = R->n_memids;
                                rel->member->data = malloc(sizeof(OSM_Rel_Member) * R->n_memids); 
                                wref = malloc(sizeof(uint64_t) * R->n_memids);
                                nref = malloc(sizeof(uint64_t) * R->n_memids);
                                int l;
                                for (l=0; l<R->n_memids; l++) {
                                    ProtobufCBinaryData role = 
                                        P->stringtable->s[R->roles_sid[l]];
                                    deltamemids += R->memids[l];
                                    rel->member->data[l].ref = deltamemids;
                                    // ref[l] = deltamemids;
                                    switch (R->types[l]) {
                                        case RELATION__MEMBER_TYPE__NODE:
                                            rel->member->data[l].type = OSM_REL_MEMBER_TYPE_NODE;
                                            nref[num_nref] = deltamemids;
                                            ++num_nref; 
                                            break;
                                        case RELATION__MEMBER_TYPE__WAY:
                                            rel->member->data[l].type = OSM_REL_MEMBER_TYPE_WAY;
                                            wref[num_wref] = deltamemids;
                                            ++num_wref; 
                                            break;
                                        case RELATION__MEMBER_TYPE__RELATION:
                                            rel->member->data[l].type = OSM_REL_MEMBER_TYPE_RELATION;
                                            // ref[l] = deltamemids; FIXME - relations in relations
                                            break;
                                        default:
                                            fprintf(stderr, "unknown relation member type %d\n", R->types[l]);
                                            break;
                                    }
                                    rel->member->data[l].role = strndup((const char *)role.data, role.len);
                                }
                            }
                            OSM_Tag_List *tl = NULL;
                            if (R->n_keys != 0) {
                                tl       = malloc(sizeof(OSM_Tag_List));
                                tl->num  = R->n_keys;
                                tl->size = R->n_keys;
                                tl->data = malloc(sizeof(OSM_Tag) * R->n_keys);
                                int l;
                                for (l=0; l<R->n_keys; l++) {
                                    ProtobufCBinaryData key = 
                                        P->stringtable->s[R->keys[l]];
                                    ProtobufCBinaryData val = 
                                        P->stringtable->s[R->vals[l]];
                                    tl->data[l].key = 
                                                strndup((const char *)key.data, key.len);
                                    tl->data[l].val =
                                                strndup((const char *)val.data, val.len);
                                }
                            }
                            rel->tags = tl;
                            
                            if (bbox_state == bbox_rel_find) {
                                int b = 0;
                                int bbox_member = 0;
                                for (b=0; b<num_nref; b++) {
                                    if (osm_is_member(bbn, nref[b]) != -1) {
                                        if (rel_filter == NULL || rel_filter(rel)) {
                                            if (debug) 
                                                fprintf(stderr, "rel %lu: member %lu is in bbox\n",
                                                                    rel->id, nref[b]);
                                            bbox_member = 1;
                                            break;
                                        }
                                    }
                                }
                                if (bbox_member == 0) {
                                    osm_free_relation(rel);
                                    free(wref);
                                    free(nref);
                                    continue;
                                }
                            }
                            else {
                                if (rel_filter != NULL) {
                                    if (!rel_filter(rel)) {
                                        osm_free_relation(rel);
                                        free(wref);
                                        free(nref);
                                        continue;
                                    }
                                }
                            }

                            if (num_nref)
                                osm_add_members(mem_nodes, num_nref, nref, 1);
                            if (num_wref)
                                osm_add_members(mem_ways, num_wref, wref, 1);

                            osm_realloc_rel_list(data->relations);
                            data->relations->data[ data->relations->num ] = rel;
                            data->relations->num += 1;
                        }
                    }
                } /* mode == OSMDATA_DUMP || OSMDATA_REL */
            } /* for (j = 0; j < P->n_primitivegroup; j++) */ 
            osm_pbf_free_primitive(P);
        }
        osm_pbf_free_blob(blob, uncompressed);
    }
    return data;
}
