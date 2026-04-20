-- mariadb-fractalsql UDF registration.
--
-- Prerequisite: fractalsql.so (Linux) or fractalsql.dll (Windows) is
-- in MariaDB's plugin_dir. The release packages handle this for you:
--   Linux:   /usr/lib/mysql/plugin/fractalsql.so
--   Windows: C:\Program Files\MariaDB <VER>\lib\plugin\fractalsql.dll
--
-- Run once, as a user with CREATE FUNCTION privilege:
--   mysql -u root -p < sql/install_udf.sql
--
-- Or from an interactive session:
--   SOURCE /usr/share/mariadb-fractalsql/install_udf.sql;
--
-- A single fractalsql.so covers all supported MariaDB majors
-- (10.6 / 10.11 / 11.4 LTS, 12.2 rolling) — the UDF ABI is stable
-- across them.

DROP FUNCTION IF EXISTS fractal_search;
DROP FUNCTION IF EXISTS fractalsql_edition;
DROP FUNCTION IF EXISTS fractalsql_version;

-- fractal_search(vector_csv, query_csv, k, params) -> JSON STRING
CREATE FUNCTION fractal_search    RETURNS STRING SONAME 'fractalsql.so';

-- fractalsql_edition() -> 'Community'
CREATE FUNCTION fractalsql_edition RETURNS STRING SONAME 'fractalsql.so';

-- fractalsql_version() -> '1.0.0'
CREATE FUNCTION fractalsql_version RETURNS STRING SONAME 'fractalsql.so';

-- Verify installation:
--   SELECT name, dl FROM mysql.func;
--   SELECT fractalsql_edition(), fractalsql_version();
--
-- Example call + JSON_EXTRACT slice:
--
--   SET @corpus = '[[1,0,0],[0,1,0],[0,0,1],[0.5,0.5,0]]';
--   SET @query  = '[0.6,0.6,0]';
--   SET @params = '{"iterations":30,"population_size":50,"walk":0.5}';
--
--   SELECT
--     JSON_EXTRACT(r, '$.best_point')    AS best_point,
--     JSON_EXTRACT(r, '$.top_k[0].idx')  AS top_hit,
--     JSON_EXTRACT(r, '$.top_k[0].dist') AS top_dist
--   FROM (SELECT fractal_search(@corpus, @query, 3, @params) AS r) t;
