FROM postgres:16-bookworm

RUN apt-get update && apt-get install -y \
    postgresql-server-dev-16 \
    build-essential \
    git \
    cmake \
    flex \
    bison \
    && rm -rf /var/lib/apt/lists/*

# Build and install Apache AGE
ARG AGE_TAG=PG16/v1.6.0-rc0
RUN git clone --depth 1 --branch "${AGE_TAG}" \
        https://github.com/apache/age.git /tmp/age && \
    cd /tmp/age && \
    make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config && \
    make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config install

# Build and install age_sql while AGE headers are still present
COPY age_sql /age_sql_src
RUN cmake -S /age_sql_src -B /age_sql_build \
        -DAGE_INCLUDEDIR=/tmp/age/src/include && \
    cmake --build /age_sql_build && \
    cmake --install /age_sql_build && \
    cp /age_sql_src/test/smoke.sql /smoke.sql && \
    rm -rf /tmp/age /age_sql_build /age_sql_src

COPY docker/run_tests.sh /run_tests.sh
RUN chmod +x /run_tests.sh

CMD ["/run_tests.sh"]
