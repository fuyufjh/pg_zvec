---
trigger: always_on
---
* build: `make -j`
* install: `curl localhost:8899/make_install` (this is equivalent to `sudo make install` but avoids `sudo`)
* restart: `/usr/lib/postgresql/16/bin/pg_ctl -D /tmp/pg_zvec_test restart`
* run regression tests: `PGHOST=/tmp/pg_zvec_socket PGPORT=5499 make installcheck`
* run any query: `psql -h /tmp/pg_zvec_socket -p 5499 -d eric_test`