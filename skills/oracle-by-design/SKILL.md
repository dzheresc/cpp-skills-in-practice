---
name: "oracle-by-design"
description: "Use when designing, writing, reviewing or tuning Oracle schemas, SQL or PL/SQL — applies Tom Kyte's Effective Oracle by Design principles: design for performance, bind variables, set-based SQL, measure first."
---

# Oracle by Design (after Tom Kyte, *Effective Oracle by Design*, Oracle Press 2003)

The book is from the 9iR2 era. The philosophy still holds; specific features and defaults may have moved on. When advice depends on a version, check the user's version and say so.

## Core stance
- **Design for performance; don't tune it in later.** Most performance comes from the data model and the access patterns, not from instance parameters.
- **Measure, don't guess.** Every claim gets a test case: build it, run it, show the numbers. Prefer "here's the proof" to "I think".
- **The answer is usually "it depends"**, and the job is to say what it depends on.
- **Use the database as a database**, not as a bit bucket. Exploit what you paid for: constraints, read consistency, analytics, PL/SQL.
- **DBAs and developers are one team.** Know the architecture you code against.
- **Read the documentation** (Concepts Guide first).

## Performance toolkit (use before opining)
- SQL trace + TKPROF (extended trace with waits/binds) for what a session really did.
- AUTOTRACE / DBMS_XPLAN for plans *and* actual row counts; compare estimates to actuals.
- A runstats-style harness: capture latches, stats and elapsed time for approach A vs B; report the latch difference, because latches limit scalability.
- AWR/ASH (Statspack in the book's era) for system-level views.
- Instrument application code (DBMS_APPLICATION_INFO, module/action, debug logging that can be enabled in production).

## Statement processing
- **Always use bind variables** in OLTP. Literals cause hard parses, latch contention on the shared pool, and SQL injection risk. CURSOR_SHARING=FORCE is a band-aid, not a fix.
- Parse once, execute many. Don't close and reparse cursors in a loop; static SQL in PL/SQL caches cursors for you.
- Understand hard vs soft parse vs "softer" (session cursor cache).

## SQL
- **If you can do it in one SQL statement, do it.** Otherwise PL/SQL; otherwise Java/C; avoid slow-by-slow row processing.
- Prefer set operations, MERGE, multi-table insert, and analytic functions over procedural loops and self-joins.
- Know the CBO: gather representative stats, check cardinality estimates, understand why a plan was chosen (10053 trace) before using hints.
- Full scans are not evil; indexes are not always good.

## PL/SQL
- Static SQL by default (dependency tracking, compile-time checks, cursor caching). Dynamic SQL only when needed, and always with binds.
- Use BULK COLLECT / FORALL when row-at-a-time is unavoidable.
- Package everything; avoid standalone procedures.
- Exception handling: catch only what you can handle. `WHEN OTHERS` that doesn't re-raise is a bug.
- **Don't commit in a loop** (ORA-01555, broken atomicity, slower). Commit when the transaction is logically complete.
- Autonomous transactions only for genuine independent work (e.g. error logging), never to dodge mutating-table errors.

## Schema design
- **Let the database enforce integrity**: PK, FK, CHECK, NOT NULL, UNIQUE. App-side checks are racy under concurrency.
- **Use the correct datatype**: dates in DATE/TIMESTAMP, numbers in NUMBER. Strings-for-everything breaks the optimizer and the data.
- Avoid generic EAV / "one table to hold anything" designs.
- Pick physical structures for the access pattern: heap, IOT, hash/B*-tree clusters, partitioning, compression, function-based indexes, materialized views.
- Index foreign keys that are updated/deleted through the parent (unindexed FKs cause locking problems).
- Optimize for how data is read, not just how it is written.

## Architecture & administration
- Dedicated server vs shared server: choose deliberately; use connection pooling in the middle tier.
- Understand read consistency, undo and redo before designing batch jobs.
- Use the database's built-ins (e.g. SPFILE, locally managed tablespaces, automatic undo) rather than hand-rolled schemes.
- Back up and *test recovery*. Know flashback options for user errors.

## How to answer with this skill
1. Restate the requirement and the Oracle version.
2. Point out design-level issues first (model, datatypes, constraints, binds, row-by-row code).
3. Propose the set-based / built-in alternative.
4. Provide a small reproducible test case (CREATE TABLE, data load, both approaches, timing/stats) the user can run.
5. Flag anything version-dependent.