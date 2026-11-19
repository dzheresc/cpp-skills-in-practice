---
name: "postgres-by-design"
description: "Use when designing, writing, reviewing or tuning PostgreSQL schemas, SQL or PL/pgSQL. Applies Kyte-style principles (design for performance, prepared statements, set-based SQL, measure first) adapted to Postgres."
---

# Postgres by Design (Tom Kyte's *Effective Oracle by Design* adapted to PostgreSQL)

The philosophy carries over from Kyte. The mechanics differ because Postgres has a different MVCC design, a process-per-connection model, and no optimizer hints in core. Defaults and features change between major versions, so check the user's version (`SELECT version();`) and flag advice that depends on it.

## Core stance
- **Design for performance; don't tune it in later.** The data model, the indexes and the access patterns matter far more than postgresql.conf.
- **Measure, don't guess.** Every claim gets a reproducible test case: DDL, data from `generate_series`, both approaches, and `EXPLAIN (ANALYZE, BUFFERS)` output.
- **"It depends"**, and the job is to say what it depends on.
- **Use the database as a database.** Use constraints, transactions, window functions, CTEs, and PL/pgSQL where they fit.
- **Read the docs.** The official manual is excellent; start with the MVCC, Indexes and Performance Tips chapters.

## Performance toolkit
- `EXPLAIN (ANALYZE, BUFFERS, SETTINGS)`: compare estimated rows with actual rows. A large mismatch usually points to the root cause.
- `pg_stat_statements` shows the top SQL by total time, calls and rows. Install it from day one.
- `auto_explain` and `log_min_duration_statement` capture slow plans in production.
- `pg_stat_activity` plus wait events show what sessions are doing now. `pg_locks` shows who is blocking whom.
- `pg_stat_user_tables` and `pg_stat_user_indexes` show sequential versus index scans, dead tuples, the last (auto)vacuum and analyze, and unused indexes.
- `pgbench` with custom scripts is the A/B harness, the Postgres counterpart of Kyte's runstats. Compare TPS, latency and buffer counts.
- Instrument the application: set `application_name` per service and tag queries with comments where tooling supports it.

## Statement processing
- **Parameterize everything**: server-side prepared statements or the driver's extended protocol. Literals mean replanning every time and SQL injection risk.
- Understand the plan cache. After 5 executions a prepared statement may switch from custom plans to a generic plan. If skewed data makes that plan bad, consider `plan_cache_mode`.
- Connection poolers: PgBouncer in transaction mode historically broke named prepared statements (newer versions support protocol-level ones). Verify this for the user's setup.
- There are no hints in core. Fix statistics, indexes or the query itself. `pg_hint_plan` is a last-resort extension.

## SQL
- **If it can be one SQL statement, make it one.** Then PL/pgSQL, then application code. Avoid slow-by-slow row loops and N+1 queries from ORMs.
- Prefer `INSERT ... ON CONFLICT`, `MERGE` (PG15+), `UPDATE ... FROM`, `DELETE ... USING`, `RETURNING`, data-modifying CTEs, window functions, `LATERAL`, and `COPY` for bulk loads.
- CTEs are inlined since PG12 unless you write `MATERIALIZED`. Use that keyword deliberately as an optimization fence.
- Statistics: `ANALYZE`, a per-column `STATISTICS` target, and `CREATE STATISTICS` (dependencies, ndistinct, MCV) for correlated columns.
- A sequential scan is not automatically evil, and an index is not automatically good.

## Transactions and MVCC (where Postgres differs most from Oracle)
- No undo segments. Old row versions stay in the table as dead tuples until VACUUM removes them. **Updates create new tuple versions**, which causes bloat and write amplification.
- **Long-running and idle-in-transaction sessions block cleanup** across the whole cluster. Set `idle_in_transaction_session_timeout` and watch `backend_xmin`.
- Keep autovacuum healthy and tune it per table for hot tables. Monitor transaction-ID wraparound with `age(datfrozenxid)`.
- HOT updates avoid touching indexes when no indexed column changes and the page has room. A lower `fillfactor` helps update-heavy tables, and indexing fewer columns helps too.
- Commit when the logical unit of work is complete. The nuance versus Kyte: very large deletes and updates are often better done in batches in Postgres (procedures can `COMMIT` since PG11) to limit bloat, lock duration and replication lag. Decide on purpose and measure.
- The default isolation level is READ COMMITTED, with a fresh snapshot for each statement. `REPEATABLE READ` and `SERIALIZABLE` (SSI) need retry logic for serialization failures (40001).
- Don't use the database as a queue without `FOR UPDATE SKIP LOCKED`.

## PL/pgSQL
- Static SQL inside functions gets automatic plan caching. For dynamic SQL use `EXECUTE format('... %I ...', ident) USING params`, never string-concatenated values.
- Prefer plain SQL functions, which can be inlined, for simple logic. Mark volatility (`IMMUTABLE`, `STABLE`, `VOLATILE`) and `PARALLEL SAFE` correctly.
- A `BEGIN ... EXCEPTION` block starts a subtransaction: it costs something and consumes subxact IDs. Don't put one inside a hot loop. Catch only what you can handle, and a `WHEN OTHERS` that swallows the error is a bug.
- Return sets (`RETURNS TABLE`, `RETURN QUERY`) instead of looping row by row in the application.
- Triggers are fine for integrity and auditing. Avoid hiding business logic in trigger chains.

## Schema design
- **Let the database enforce integrity**: PK, FK, UNIQUE, CHECK, NOT NULL, domains, and exclusion constraints (e.g. no overlapping ranges).
- **Postgres does NOT create indexes on foreign key columns.** Index them when you join on them or delete or update the parent. Otherwise you get sequential scans and lock pain.
- **Use the correct type**: `timestamptz` (not `timestamp` for instants), `numeric` for money, `text` with CHECK constraints instead of arbitrary `varchar(n)`, `uuid`, range types, `boolean`, and identity columns instead of `serial`.
- Use `jsonb` for genuinely schemaless attributes, not as a replacement for a relational model. Avoid EAV.
- Choose physical design for the access pattern:
  - B-tree with `INCLUDE` for covering and index-only scans (keep the visibility map fresh through vacuum).
  - Partial indexes and expression indexes.
  - GIN (jsonb, arrays, full-text search, trigram), GiST/SP-GiST (ranges, geometry), and BRIN (huge append-only, naturally ordered tables).
  - Declarative partitioning for retention and partition pruning, not as a default.
  - Materialized views (`REFRESH ... CONCURRENTLY` needs a unique index).
  - There are no index-organized tables. `CLUSTER` is a one-off reorder.
- Every index slows writes and can block HOT updates, so drop unused ones.

## Architecture and operations
- Each connection is a process. **Pool connections** (PgBouncer or an application pool) and keep `max_connections` modest.
- Memory: `shared_buffers` works together with the OS page cache. `work_mem` applies per sort or hash node, per connection, so size it carefully.
- Schema changes: take locks deliberately. Use `lock_timeout`, `CREATE INDEX CONCURRENTLY`, and `NOT VALID` followed by `VALIDATE CONSTRAINT`.
- Backups: use WAL archiving with PITR (pgBackRest, Barman, or `pg_basebackup`), and **test restores**. There is no flashback, so recovery from user error means PITR or a delayed replica.
- Understand replication (streaming or logical) and replica lag before routing reads to replicas.

## How to answer with this skill
1. Restate the requirement and the Postgres version.
2. Point out design-level issues first: data model, types, constraints, missing FK indexes, unparameterized SQL, row-by-row code, long transactions.
3. Propose the set-based or built-in alternative.
4. Give a small reproducible test (DDL, `generate_series` data, both approaches, `EXPLAIN (ANALYZE, BUFFERS)` or `pgbench`).
5. Flag version-dependent features and Oracle-to-Postgres differences when the user comes from Oracle.