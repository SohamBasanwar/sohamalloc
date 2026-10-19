-- Allocation-heavy sqlite3 workload (in-memory database, run as
--   sqlite3 :memory: < sqlite_workload.sql
-- ).  The sqlite3 CLI uses the system malloc (SQLITE_CONFIG_MALLOC default),
-- so the preloaded allocator serves every page-cache page, row buffer, sorter
-- run and string temporary.  Scale is controlled by the row count in the
-- recursive CTE below; bench/run_bench.sh substitutes @ROWS@.
PRAGMA cache_size = -65536;
PRAGMA temp_store = MEMORY;
CREATE TABLE t(id INTEGER PRIMARY KEY, grp INTEGER, name TEXT, payload BLOB, score REAL);
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < @ROWS@)
INSERT INTO t SELECT x, x % 997, printf('name-%08d-%016x', x, (x * 2654435761) % 4294967296),
                     randomblob(16 + (x % 300)), (x * 7919) % 10007 / 13.0 FROM c;
CREATE INDEX t_name ON t(name);
CREATE INDEX t_grp_score ON t(grp, score);
SELECT count(*), sum(length(payload)) FROM t;
SELECT grp, count(*), avg(score), length(group_concat(substr(name, 6, 4), ',')) FROM t GROUP BY grp ORDER BY 3 DESC LIMIT 5;
SELECT count(*) FROM (SELECT name FROM t ORDER BY upper(name) DESC);
SELECT count(*) FROM t a JOIN t b ON a.grp = b.grp AND b.id = a.id + 997 WHERE a.score > 300;
UPDATE t SET name = name || '-' || printf('%x', id * 31), payload = zeroblob(length(payload) / 2) WHERE id % 3 = 0;
DELETE FROM t WHERE id % 5 = 0;
CREATE TABLE u AS SELECT grp, json_group_array(json_object('id', id, 'n', name)) AS j FROM t GROUP BY grp;
SELECT sum(json_array_length(j)) FROM u;
SELECT count(DISTINCT substr(name, 1, 12)) FROM t;
REINDEX;
SELECT count(*) FROM t WHERE name LIKE 'name-0001%';
