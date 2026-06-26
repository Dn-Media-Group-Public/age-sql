#!/bin/bash
set -e

export PATH="/usr/lib/postgresql/16/bin:$PATH"
PGDATA=/tmp/pgdata

gosu postgres initdb -D "$PGDATA"
gosu postgres pg_ctl -D "$PGDATA" -l /tmp/pg.log start -w

gosu postgres psql -v ON_ERROR_STOP=1 \
    -c "CREATE EXTENSION age; CREATE EXTENSION age_sql;" \
    -f /smoke.sql

echo "All smoke tests passed."

gosu postgres pg_ctl -D "$PGDATA" stop
