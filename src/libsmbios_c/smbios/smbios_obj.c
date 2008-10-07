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

// system
#include <stdlib.h>
#include <string.h>

// public
#include "smbios_c/memory.h"
#include "smbios_c/obj/smbios.h"
#include "smbios_c/smbios.h"
#include "smbios_c/types.h"

// private
#include "smbios_impl.h"

// forward declarations

// static vars
static struct smbios_table singleton; // auto-init to 0

struct smbios_table *smbios_table_factory(int flags, ...)
{
    struct smbios_table *toReturn = 0;

    dbg_printf("DEBUG: smbios_table_factory()\n");

    if (flags==SMBIOS_DEFAULTS)
        flags = SMBIOS_GET_SINGLETON;

    if (flags & SMBIOS_GET_SINGLETON)
        toReturn = &singleton;
    else
        toReturn = (struct smbios_table *)calloc(1, sizeof(struct smbios_table));

    if (toReturn->initialized)
        goto out;

    init_smbios_struct(toReturn);

    do_smbios_fixups(toReturn);

out:
    return toReturn;
}


void smbios_table_free(struct smbios_table *m)
{
    if (m != &singleton)
        _smbios_table_free(m);

    // can do special cleanup for singleton, but none necessary atm
}


struct smbios_struct *smbios_table_get_next_struct(const struct smbios_table *table, const struct smbios_struct *cur)
{
    const u8 *data = 0;

    //If we are called on an uninitialized smbiosBuffer, return 0;
    if (0 == table->table || (cur && 0x7f == cur->type))
        goto out1;

    data = (u8*)table->table;

    // cur == 0, that means we return the first struct
    if (0 == cur)
        goto out1;

    // start out at the end of the cur structure.
    // The only things that sits between us and the next struct
    // are the strings for the cur structure.
    data = (const u8 *)(cur) + smbios_struct_get_length(cur);

    // skip past strings at the end of the formatted structure,
    // go until we hit double NULL "\0"
    // add a check to make sure we don't walk off the buffer end
    // for broken BIOSen.
    // The (3) is to take into account the deref at the end "data[0] ||
    // data[1]", and to take into account the "data += 2" on the next line.
    while (((data - (u8*)table->table) < (table->tep.dmi.table_length - 3)) && (*data || data[1]))
        data++;

    // ok, skip past the actual double null.
    data += 2;

    // add code specifically to work around crap bios implementations
    // that do not have the _required_ 0x7f end-of-table entry
    //   note: (4) == sizeof a std header.
    if ( (data - (u8*)table->table) > (table->tep.dmi.table_length - 4))
    {
        // really should output some nasty message here... This is very
        // broken
        data = 0;
        goto out1;
    }

out1:
    return (struct smbios_struct *)data;
}

struct smbios_struct *smbios_table_get_next_struct_by_type(const struct smbios_table *table, const struct smbios_struct *cur, u8 type)
{
    do {
        cur = smbios_table_get_next_struct(table, cur);
        if (cur && cur->type == type)
            break;
    } while ( cur );
    return (struct smbios_struct *)cur;
}

struct smbios_struct *smbios_table_get_next_struct_by_handle(const struct smbios_table *table, const struct smbios_struct *cur, u16 handle)
{
    do {
        cur = smbios_table_get_next_struct(table, cur);
        if (cur && cur->handle == handle)
            break;
    } while ( cur );
    return (struct smbios_struct *)cur;
}


u8 smbios_struct_get_type(const struct smbios_struct *s)
{
    return s->type;
}

u8 smbios_struct_get_length(const struct smbios_struct *s)
{
    return s->length;
}

u16 smbios_struct_get_handle(const struct smbios_struct *s)
{
    return s->handle;
}

int smbios_struct_get_data(const struct smbios_struct *s, void *dest, u8 offset, size_t len)
{
    int retval = -1;

    dbg_printf("smbios_struct_get_data(%p, %p, %d, %ld)\n", s, dest, offset, len);

    if (offset > smbios_struct_get_length(s))
        goto out;

    if( offset + len < offset ) // attempt to wraparound... :(
        goto out;

    if( offset + len > smbios_struct_get_length(s) )
        goto out;

    retval = 0;
    memcpy(dest, (const u8 *)(s)+offset, len);

out:
    return retval;
}

const char *smbios_struct_get_string_from_offset(const struct smbios_struct *s, u8 offset)
{
    u8 strnum = 0;
    const char *retval = 0;

    dbg_printf("smbios_struct_get_string_from_offset()\n");

    if (smbios_struct_get_data(s, &strnum, offset, sizeof(strnum)) >= 0)
    {
        dbg_printf("string offset: %d  which: %d\n", offset, strnum);
        retval = smbios_struct_get_string_number(s, strnum);
    }

    dbg_printf("string: %s\n", retval);
    return retval;
}

const char *smbios_struct_get_string_number(const struct smbios_struct *s, u8 which)
{
    const char *string_pointer = 0;
    const char *retval = 0;

    dbg_printf("smbios_struct_get_string_number(%p, %d)\n", s, which);

    if (!which)     //strings are numbered beginning with 1
        goto out;

    string_pointer = (const char *)(s);

    // start out at the end of the header. This is where
    // the first string starts
    string_pointer += smbios_struct_get_length(s);

    for (; which > 1; which--)
    {
        string_pointer += strlen (string_pointer) + 1;

        // if it is still '\0', that means we are
        // at the end of this item and should stop.
        // user gave us a bad index
        if( ! *string_pointer )
            goto out;
    }

    retval = string_pointer;


out:
    return retval;
}

// visitor pattern
void smbios_table_walk(struct smbios_table *table, void (*fn)(const struct smbios_struct *, void *userdata), void *userdata)
{
    const struct smbios_struct *s = smbios_table_get_next_struct(table, 0);
    do {
        fn(s, userdata);
        s = smbios_table_get_next_struct(table, s);
    }while(s);
    smbios_table_free(table);
}


/**************************************************
 *
 * Internal functions
 *
 **************************************************/

void __internal _smbios_table_free(struct smbios_table *this)
{
    memset(&this->tep, 0, sizeof(this->tep));

    free(this->table);
    this->table = 0;

    this->initialized=0;

    free(this);
}

void __internal init_smbios_struct(struct smbios_table *m)
{
    m->initialized = 1;

    dbg_printf("DEBUG: smbios_table_factory()\n");

    // smbios efi strategy
    if (smbios_get_table_efi(m) >= 0)
        return;

    // smbios memory strategy
    if (smbios_get_table_memory(m) >= 0)
        return;

    // smbios WMI strategy (windows only)
    if (smbios_get_table_wmi(m) >= 0)
        return;

    // smbios firmware tables strategy (windows only)
    if (smbios_get_table_firm_tables(m) >= 0)
        return;
}



// validate the smbios table entry point
bool __internal validate_dmi_tep( const struct dmi_table_entry_point *dmiTEP, bool strict )
{
    // This code checks for the following:
    //       entry point structure checksum : As per the specs
    //       anchor string : As per the specs
    bool retval = true;

    u8 checksum = 0;
    const u8 *ptr = (const u8*)(dmiTEP);
    // don't overrun dmiTEP if BIOS is buggy... (note sizeof() test here)
    //      added especially to deal with buggy Intel BIOS.
    for( unsigned int i = 0; i < sizeof(*dmiTEP); ++i )
        // stupid stuff to avoid MVC++ .NET runtime exception check for cast to different size
        checksum = (checksum + ptr[i]) & 0xFF;

    if(memcmp(dmiTEP->anchor,"_DMI_",5)!=0) // Checking intermediate anchor string
        retval = false;  // validation failed

    if(checksum) // Checking entry point structure checksum
        retval = false;  // validation failed

    return retval;
}



// validate the smbios table entry point
bool __internal validate_smbios_tep( const struct smbios_table_entry_point *tempTEP, bool strict)
{
    // This code checks for the following:
    //       entry point structure checksum : As per the specs
    //       smbios major version : As per the specs
    //       Intermediate anchor string : As per the specs
    //
    // This code does not check the following:
    //      intermediate checksum: the main checksum covers the
    //      entire area
    //          and should be sufficient, plus there is a
    //          possibility for
    //          BIOS bugs in this area.
    //
    //      minor version: according to the spec, this parser should
    //      work
    //          with any change in minor version. The spec says this
    //          parser
    //          will break if major version changes, so we check
    //          that.
    //

    bool retval = true;

    u8 checksum = 0;
    const u8 *ptr = (const u8*)(tempTEP);
    // don't overrun tempTEP if BIOS is buggy... (note sizeof() test here)
    //      added especially to deal with buggy Intel BIOS.
    for( unsigned int i = 0; (i < (unsigned int)(tempTEP->eps_length)) && (i < sizeof(*tempTEP)); ++i )
        // stupid stuff to avoid MVC++ .NET runtime exception check for cast to different size
        checksum = (checksum + ptr[i]) & 0xFF;

    validate_dmi_tep( &(tempTEP->dmi), strict);

    if(checksum) // Checking entry point structure checksum
        retval = false;  // validation failed

    if(tempTEP->major_ver!=0x02)     // Checking smbios major version
        retval = false;  // validation failed

    // Entry Point Length field is at least 0x1f.
    if(tempTEP->eps_length < 0x0f)
        retval = false;  // validation failed

    return retval;
}


int __internal smbios_get_tep_memory(struct smbios_table *table, bool strict)
{
    int retval = 1;

    unsigned long fp = E_BLOCK_START;

    // tell the memory subsystem that it can optimize here and
    // keep memory open while we scan rather than open/close/open/close/...
    // for each fillBuffer() call
    memory_suggest_leave_open();

    struct smbios_table_entry_point tempTEP;
    memset(&tempTEP, 0, sizeof(tempTEP));
    while ( (fp + sizeof(tempTEP)) < F_BLOCK_END)
    {
        memory_read(&tempTEP, fp, sizeof(tempTEP));

        // search for promising looking headers
        // first, look for old-style DMI header
        if (memcmp (&tempTEP, "_DMI_", 5) == 0)
        {
            dbg_printf("Found _DMI_ anchor. Trying to parse legacy DMI structure.\n");
            struct dmi_table_entry_point *dmiTEP = (struct dmi_table_entry_point *)(&tempTEP);
            memmove(&(tempTEP.dmi), &dmiTEP, sizeof(struct dmi_table_entry_point));
            // fake the rest of the smbios table entry point...
            tempTEP.major_ver=2;
            tempTEP.minor_ver=0;
            if(validate_dmi_tep(dmiTEP, strict))
                break;
        }

        // then, look for new-style smbios header. This will always
        // occur before _DMI_ in memory
        if ((memcmp (&tempTEP, "_SM_", 4) == 0))
        {
            dbg_printf("Found _SM_ anchor. Trying to parse legacy DMI structure.\n");
            if(validate_smbios_tep(&tempTEP, strict))
                break;
        }

        fp += 16;
    }

    // dont need memory optimization anymore
    memory_suggest_close();

    // bad stuff happened if we got to here and fp > 0xFFFFFL
    if ((fp + sizeof(tempTEP)) >= F_BLOCK_END)
    {
        retval = 0;
        goto out;
    }

    memcpy( &table->tep, &tempTEP, sizeof(table->tep) );

out:
    return retval;
}


int __internal smbios_get_table_memory(struct smbios_table *m)
{
    int retval = -1; //fail

    dbg_printf("DEBUG: smbios_get_table_memory()\n");

    if (!smbios_get_tep_memory(m, false))
        goto out;

    size_t len = m->tep.dmi.table_length;
    m->table = (struct table*)calloc(1, len);
    retval = memory_read(m->table, m->tep.dmi.table_address, len);
    if (retval == 0)
        goto out;

    // fail
    free(m->table);
    m->table = 0;

out:
    return retval;
}
