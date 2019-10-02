CREATE SCHEMA upgrade_distributed_table_before;
SET search_path TO upgrade_distributed_table_before, public;
SET citus.replication_model TO streaming;

-- set sync intervals to less than 15s so wait_until_metadata_sync never times out
ALTER SYSTEM SET citus.metadata_sync_interval TO 3000;
ALTER SYSTEM SET citus.metadata_sync_retry_interval TO 500;
SELECT pg_reload_conf();

CREATE FUNCTION wait_until_metadata_sync(timeout INTEGER DEFAULT 15000)
    RETURNS void
    LANGUAGE C STRICT
    AS 'citus';

CREATE TABLE t(a int);
SELECT create_distributed_table('t', 'a');
INSERT INTO t SELECT * FROM generate_series(1, 5);

CREATE TYPE tc1 AS (a int, b int);
CREATE TABLE t1 (a int PRIMARY KEY, b tc1);
SELECT create_distributed_table('t1','a');

-- We store the index of distribution column and here we check that the distribution
-- column index does not change after an upgrade if we drop a column that comes before the
-- distribution column. The index information is in partkey column of pg_dist_partition table.
CREATE TABLE t_ab(a int, b int);
SELECT create_distributed_table('t_ab', 'b');
INSERT INTO t_ab VALUES (1, 11);
INSERT INTO t_ab VALUES (2, 22);
INSERT INTO t_ab VALUES (3, 33);

ALTER TABLE t_ab DROP a;

CREATE FUNCTION count_values(a int) RETURNS int AS
$$
    DECLARE
       cnt int := 0;
    BEGIN
        SELECT count(*) INTO cnt FROM upgrade_distributed_table_before.t_ab WHERE t_ab.b = $1;
        RETURN cnt;
    END;
$$ LANGUAGE plpgsql;
SELECT create_distributed_function('count_values(int)', '$1', colocate_with:='t_ab');
SELECT wait_until_metadata_sync();
SET client_min_messages TO DEBUG1;
SELECT bool_and(metadatasynced) FROM pg_dist_node;
SELECT count_values(11);
SELECT count_values(12);
