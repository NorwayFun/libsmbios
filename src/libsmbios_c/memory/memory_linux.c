/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim:expandtab:autoindent:tabstop=4:shiftwidth=4:filetype=c:cindent:textwidth=0:
 *
 * Copyright (C) 2005 Dell Inc.
 *  by Michael Brown <Michael_E_Brown@dell.com>
 * Licensed under the Open Software License version 2.1
 *
 * Alternatively, you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.

 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#define LIBSMBIOS_C_SOURCE

// Include compat.h first, then system headers, then public, then private
#include "smbios_c/compat.h"

#include <stdarg.h>     // va_list
#include <stdlib.h>
#include <ctype.h>      // isalnum
#include <stdio.h>
#include <string.h>     // memcpy
#include <errno.h>
#include <sys/mman.h>   // mmap

#include "smbios_c/obj/memory.h"
#include "smbios_c/types.h"
#include "memory_impl.h"

struct linux_data
{
    char *filename;
    FILE *fd;
    int mem_errno;
    bool rw;
    void *lastMapping;
    u64 lastMappedOffset;
    u64 mappingSize;
};

#define READ_MMAP 0
#define WRITE_MMAP 1

static void closefds(struct linux_data *private_data)
{
    fnprintf("\n");
    if (private_data->lastMapping)
    {
        fnprintf("\t\tmunmap(%p)\n", private_data->lastMapping);
        munmap(private_data->lastMapping, private_data->mappingSize);
    }

    private_data->lastMapping = 0;
    private_data->lastMappedOffset = -1;

    if (private_data->fd)
        fclose(private_data->fd);

    private_data->fd = 0;
}

static FILE * reopen(struct linux_data *private_data, int rw)
{
    char *openMode = rw ? "r+b": "rb";
    fnprintf(" file: %s,  rw: %d\n", private_data->filename, rw );
    if(private_data->fd)
        fclose(private_data->fd);

    private_data->rw = rw;
    private_data->lastMapping = 0;
    private_data->lastMappedOffset = -1;
    private_data->fd = fopen( private_data->filename, openMode ); // re-open for write
    return private_data->fd;
}

#ifdef DEBUG_MEMORY_C
static void debug_dump_buffer(const char *fn, const char *s, const u8 *buffer, size_t start, size_t toCopy)
{
    dbg_printf("%s %s: ", fn, s);
    for(int i=0;i<(toCopy>100?100:toCopy);++i)
        if (isalnum(buffer[start + i]))
            dbg_printf("%c", buffer[start + i]);
        else
            dbg_printf("*");
    dbg_printf("'\n");
}
#else
#define debug_dump_buffer(...) do {} while(0)
#endif

static void remap(struct linux_data *private_data, u64 offset, bool rw)
{
    int flags = rw ? PROT_WRITE : PROT_READ;
    off_t mmoff = offset % private_data->mappingSize;

    fnprintf("\n");

    // no need to remap if we already have the correct area mapped.
    if (offset-mmoff == private_data->lastMappedOffset)
        goto out;

    private_data->lastMappedOffset = offset-mmoff;

    if (private_data->lastMapping)
        munmap(private_data->lastMapping, private_data->mappingSize);

    private_data->lastMapping = mmap
                ( 0,
                  private_data->mappingSize,
                  flags,
                  MAP_SHARED,
                  fileno(private_data->fd),
                  private_data->lastMappedOffset); // last arg, offset, must be mod pagesize.
out:
    return;
}

static size_t trycopy(struct linux_data *private_data, u8 *buffer, u64 offset, size_t length, bool rw)
{
    off_t mmoff = offset % private_data->mappingSize;

    fnprintf("\t\tbuffer(%p), offset(%lld), length(%zd), mmoff(%lld)\n", buffer, offset, length, (u64)mmoff);

    if( length + mmoff > (private_data->mappingSize) )
        length = (private_data->mappingSize) - mmoff;

    fnprintf("\t\tCOPYING(%zu)\n", length);
    if (rw)
        memcpy(((u8 *)(private_data->lastMapping) + mmoff),
                buffer, length);
    else
        memcpy(buffer,
                ((const u8 *)(private_data->lastMapping) + mmoff), length);

    debug_dump_buffer(__PRETTY_FUNCTION__, "BUFFER", buffer, 0, length);
    debug_dump_buffer(__PRETTY_FUNCTION__, "MEMORY", (const u8 *)(private_data->lastMapping), mmoff, length);

    return length;
}

static int copy_mmap(const struct memory_access_obj *this, u8 *buffer, u64 offset, size_t length, bool rw)
{
    struct linux_data *private_data = (struct linux_data *)this->private_data;
    private_data->mem_errno = errno = 0;
    int retval = -1;

    size_t bytesCopied = 0;

    fnprintf("buffer(%p) offset(%lld) length(%zd) rw(%d)\n", buffer, offset, length, rw);
    fnprintf("->rw: %d  fd: %p\n", private_data->rw, private_data->fd);

    if( (rw && !private_data->rw) || !private_data->fd)
        if (!reopen(private_data, rw))
            goto err_out;

    fnprintf("Start of copy loop\n");
    while( bytesCopied < length )
    {
        fnprintf("\tLOOP: bytesCopied(%zd) length(%zd)\n", bytesCopied, length);
        remap(private_data, offset + bytesCopied, rw);
        fnprintf("\tlastMapping(%p)\n", private_data->lastMapping);
        if (private_data->lastMapping == (void *)-1)
            goto err_out;

        bytesCopied += trycopy(
                private_data,
                buffer + bytesCopied,
                offset + bytesCopied,
                length - bytesCopied,
                rw);
    }

    retval = 0;
    goto out;

err_out:
    fnprintf("%s - ERR_OUT: %d \n", __PRETTY_FUNCTION__, errno);
    perror("ERR_OUT: ");
    fnprintf("%s\n", strerror(errno));
    private_data->mem_errno = errno;
    if (private_data->lastMapping == (void*)-1)
        private_data->lastMapping = 0;

out:
    // close on error, or if close hint
    fnprintf("\t\t out: lastMapping(%p)\n", private_data->lastMapping);
    if (memory_obj_should_close(this) || retval)
        closefds(private_data);

    return retval;
}

static int linux_read_fn(const struct memory_access_obj *this, u8 *buffer, u64 offset, size_t length)
{
    return copy_mmap(this, buffer, offset, length, READ_MMAP);
}

static int linux_write_fn(const struct memory_access_obj *this, u8 *buffer, u64 offset, size_t length)
{
    fnprintf("THIS: %p\n", this);
    return copy_mmap(this, buffer, offset, length, WRITE_MMAP);
}

static void linux_free(struct memory_access_obj *this)
{
    struct linux_data *private_data = (struct linux_data *)this->private_data;

    if (private_data->filename)
    {
        free(private_data->filename);
        private_data->filename = 0;
    }

    closefds(private_data);

    free(private_data);
    this->private_data = 0;
    this->initialized=0;
}

static void linux_cleanup(struct memory_access_obj *this)
{
    struct linux_data *private_data = (struct linux_data *)this->private_data;

    closefds(private_data);

    private_data->mem_errno = 0;
    private_data->rw = 0;
}

__internal void init_mem_struct_filename(struct memory_access_obj *m, const char *fn)
{
    struct linux_data *private_data = (struct linux_data *)calloc(1, sizeof(struct linux_data));

    // must be power of 2, >= getpagesize()
    if (getpagesize() > 4096)
        private_data->mappingSize = getpagesize();  // 64k
    else
        private_data->mappingSize = getpagesize() * 16;  // 64k
    fnprintf("mappingSize(%lld)\n", private_data->mappingSize);

    private_data->lastMappedOffset = -1;
    private_data->filename = (char *)calloc(1, strlen(fn) + 1);
    private_data->rw = 0;
    strcat(private_data->filename, fn);

    m->private_data = private_data;
    m->free = linux_free;
    m->read_fn = linux_read_fn;
    m->write_fn = linux_write_fn;
    m->cleanup = linux_cleanup;
    m->close = 1;
    m->initialized = 1;
}

__internal void init_mem_struct(struct memory_access_obj *m)
{
   init_mem_struct_filename(m, "/dev/mem");
}




