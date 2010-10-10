#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <time.h>

#include <zlib.h>

#include "fileformat.pb-c.h"
#include "osmformat.pb-c.h"

#include "osm.h"

void osm_pbf_timestamp(const long int deltatimestamp, char *timestamp) {
    struct tm *ts = gmtime(&deltatimestamp);
    if (ts == NULL) {
        timestamp[0] = '\0';
    }

    strftime(timestamp, 21, "%Y-%m-%dT%H:%M:%SZ" , ts);
}

unsigned char *osm_pbf_uncompress_blob(Blob *bmsg) {
    if (bmsg->has_zlib_data) {
        int ret;
        unsigned char *uncompressed;
        z_stream strm;
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        strm.avail_in = bmsg->zlib_data.len;
        strm.next_in = bmsg->zlib_data.data;
        strm.avail_out = bmsg->raw_size;
        uncompressed = (unsigned char *)malloc(bmsg->raw_size * sizeof(char));
        if (uncompressed == NULL) {
            fprintf(stderr, "Error allocating the decompression buffer\n");
            return NULL;
        }
        strm.next_out = uncompressed;

        ret = inflateInit(&strm);
        if (ret != Z_OK) {
            fprintf(stderr, "Zlib init failed\n");
            return NULL;
        }

        ret = inflate(&strm, Z_NO_FLUSH);

        (void)inflateEnd(&strm);

        if (ret != Z_STREAM_END) {
            fprintf(stderr, "Zlib compression failed\n");
            return NULL;
        }
        return uncompressed;
    }
    else if (bmsg->has_lzma_data) {
        fprintf(stderr, "LZMA data\n");
    }
    else if (bmsg->has_bzip2_data) {
        fprintf(stderr, "bzip2 data\n");
    }
    else {
        fprintf(stderr, "We cannot handle the %d non-raw bytes yet...\n", bmsg->raw_size);
        return NULL;
    }

    return NULL;
}

uint32_t osm_pbf_bh_length(OSM_File *F) {
    uint32_t length;
    char lenbuf[4];
    unsigned int i;
    unsigned char c;

    for (i=0; i<4 && (c=fgetc(F->file)) != EOF; i++) {
        lenbuf[i] = c;
    }
    // FIXME pbf-util.c:81: warning: dereferencing type-punned pointer
    //           will break strict-aliasing rules
    length = ntohl(*((uint32_t *) lenbuf));  // convert the buffer to a value
    return length;
}

void osm_pbf_free_bh(BlockHeader *bh) {
    block_header__free_unpacked(bh, &protobuf_c_system_allocator);
}

BlockHeader *osm_pbf_get_bh(OSM_File *F, uint32_t len) {
    BlockHeader *bh = NULL;
    unsigned char *buffer = NULL;
    unsigned char c;
    int i = 0;

    buffer = (unsigned char *) malloc(len * sizeof(char));
    // FIXME if (buffer == NULL) ... 
    for (i = 0; i < len && (c=fgetc(F->file)) != EOF; i++) {
        buffer[i] = c;
    }

    bh = block_header__unpack(NULL, len, buffer);
    free(buffer);
    if (bh == NULL) {
        fprintf(stderr, "Error unpacking BlockHeader message\n");
        free(buffer);
        return (BlockHeader *)NULL;
    }

    return bh;
}

void osm_pbf_free_blob(Blob *B, unsigned char *uncompressed) {
    if (!B->has_raw)
        free(uncompressed);
    blob__free_unpacked(B, &protobuf_c_system_allocator);
}

Blob *osm_pbf_get_blob(OSM_File *F, uint32_t len, unsigned char **uncompressed)
{
    Blob *B = NULL;
    unsigned char *buffer;
    unsigned char c;
    int i = 0;

    buffer = (unsigned char *) malloc(len * sizeof(char));
    for (i = 0; i < len && (c=fgetc(F->file)) != EOF; i++) {
        buffer[i] = c;
    }

    B = blob__unpack(NULL, len, buffer);
    if (B == NULL) {
        fprintf(stderr, "Error unpacking Blob message\n");
        free(buffer);
        return (Blob *)NULL;
    }

    free(buffer);
    if (B->has_raw)
        *uncompressed = (unsigned char *)B->raw.data;
    else {
        unsigned char *tmp = osm_pbf_uncompress_blob(B);
        if (tmp == NULL) {
            fprintf(stderr, "failed to uncompress Blob\n");
            return (Blob *)NULL;
        }
        *uncompressed = tmp;
    }
    return B;
}

void osm_pbf_free_primitive(PrimitiveBlock *P) {
    primitive_block__free_unpacked(P, &protobuf_c_system_allocator);
}

PrimitiveBlock *osm_pbf_unpack_data(Blob *B, unsigned char *uncompressed) {
    PrimitiveBlock *P = 
            primitive_block__unpack(NULL, B->raw_size, uncompressed);
    if (P == NULL) {
        fprintf(stderr, "Error unpacking PrimitiveBlock message\n");
        return (PrimitiveBlock *)NULL;
    }
    return P;
}

