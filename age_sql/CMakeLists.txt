cmake_minimum_required(VERSION 3.14)
project(age_sql C)

find_program(PG_CONFIG pg_config REQUIRED)

execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --sharedir
    OUTPUT_VARIABLE PG_SHAREDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --cflags_sl
    OUTPUT_VARIABLE PG_CFLAGS_SL OUTPUT_STRIP_TRAILING_WHITESPACE)

add_library(age_sql MODULE
    src/age_sql.c
    src/regexp.c
    src/sql_exec.c
)

# AGE often doesn't install headers into PG_INCLUDEDIR_SERVER.
# If the build can't find utils/agtype.h, point this at AGE's src/include:
#   cmake -DAGE_INCLUDEDIR=/path/to/age/src/include ...
set(AGE_INCLUDEDIR "${PG_INCLUDEDIR_SERVER}" CACHE PATH "Apache AGE headers (default: same as PG server headers)")

target_include_directories(age_sql PRIVATE ${PG_INCLUDEDIR_SERVER} ${AGE_INCLUDEDIR})
separate_arguments(PG_CFLAGS_SL_LIST UNIX_COMMAND "${PG_CFLAGS_SL}")
target_compile_options(age_sql PRIVATE ${PG_CFLAGS_SL_LIST})

set_target_properties(age_sql PROPERTIES
    PREFIX ""
    OUTPUT_NAME "age_sql"
)

install(TARGETS age_sql LIBRARY DESTINATION ${PG_PKGLIBDIR})
install(FILES age_sql.control DESTINATION ${PG_SHAREDIR}/extension)
install(FILES sql/age_sql--1.0.sql DESTINATION ${PG_SHAREDIR}/extension)
