/* Stub for when PostgreSQL (libpq) is not available.
   Compiled only when COOKBOOK_HAS_POSTGRES is NOT defined. */

#ifndef COOKBOOK_HAS_POSTGRES

#include "cookbook_db.h"
#include <stdio.h>

cookbook_db *cookbook_db_open_postgres(const char *conninfo) {
    (void)conninfo;
    fprintf(stderr, "cookbook: PostgreSQL backend not available "
            "(built without libpq)\n");
    return NULL;
}

#endif /* !COOKBOOK_HAS_POSTGRES */
