# SQL

## NAME

sql - small file-backed SQL database tool

## SYNOPSIS

```
sql DBFILE [SQL]
sql DBFILE < SCRIPT
sql -h
sql --help
```

## DESCRIPTION

`sql` opens a small database stored in `DBFILE`, executes one or more SQL statements, prints query output to standard output, and writes changes back to the same file. If `SQL` arguments are provided, they are joined with spaces and executed as the statement text. If no SQL argument is provided, statement text is read from standard input.

The database file uses the project-local `SQS1` text format. A missing database file starts as an empty database and is created when a changing statement is saved.

Statements are separated with semicolons. Values can be written as bare words or quoted strings. Query output and TSV import/export use tab-separated text.

## CURRENT CAPABILITIES

- `CREATE TABLE`, including `IF NOT EXISTS`, typed columns, `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, and `DEFAULT`
- `INSERT INTO`, with optional column lists and multi-row `VALUES`
- `SELECT`, including aliases, `DISTINCT`, expressions with `+`, `-`, and `||`, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`, and `OFFSET`
- inner joins, `LEFT JOIN`, `RIGHT JOIN`, `FULL JOIN`, `NATURAL JOIN`, `ON`, and `USING`
- aggregate functions `COUNT`, `SUM`, `AVG`, `TOTAL`, `MIN`, `MAX`, `FIRST`, `LAST`, and `GROUP_CONCAT`
- `UPDATE`, including literal assignments and simple column arithmetic
- `DELETE FROM`, `DROP TABLE`, `SCHEMA`, and `ALTER TABLE`
- `ALTER TABLE` rename table, rename column, add column, and drop column
- `IMPORT` and `EXPORT` with TSV by default and optional `CSV`
- `TEXT`, `INTEGER`, and `REAL` column types
- `NULL` values, `IS NULL`, `IS EMPTY`, `LIKE`, `IN`, `BETWEEN`, `AND`, `OR`, and `NOT`

## OPTIONS

- `DBFILE` database file to open or create
- `SQL` statement text to execute; when omitted, `sql` reads statement text from standard input
- `-h`, `--help` show usage

## DATA FORMAT

The on-disk database is a newline-oriented `SQS1` text file managed by `sql`. It is intended to be stable enough for the tool to read and write, but users should prefer SQL statements, `IMPORT`, and `EXPORT` rather than hand-editing the database file.

`IMPORT TABLE FROM PATH` reads a header row followed by data rows. By default fields are tab-separated. Add `CSV` to read comma-separated fields with double-quoted CSV escaping. Add `CREATE TABLE` to create a table from the header row.

`EXPORT TABLE TO PATH` writes a header row and all table rows. Add `CSV` to write comma-separated output. Use `-` as the path to write exported data to standard output.

## CAPACITY AND MEMORY USE

Database and query collections grow on demand through the runtime allocator. Small databases do not preallocate large table, row, column, result, join, condition, grouping, or ordering arrays.

Explicit byte-oriented limits are:

- SQL statement text and import lines up to 1 MiB each
- table and column names up to 31 bytes
- values up to 511 bytes each

Table metadata, row value slots, INSERT and UPDATE scratch space, SELECT parse state, source and join state, condition trees, result rows, statement text, import lines, and the value arena grow through the runtime allocator. Collection failure therefore depends on available memory or the representable 32-bit database offsets rather than a smaller hidden item count.

For plain `SELECT ... LIMIT` queries without ordering, grouping, aggregates, `DISTINCT`, or `HAVING`, row collection stops once enough rows have been found for the requested offset and limit. Queries that need whole-result visibility still collect the full result before applying the final output step.

## LIMITATIONS

- this is a compact project-local SQL subset, not SQLite or a full SQL implementation
- statement text, import lines, names, individual values, and the 32-bit value arena have explicit size limits; collection counts otherwise depend on available memory
- values are stored as text; `INTEGER` and `REAL` columns validate numeric input and comparisons use numeric ordering when both sides parse as numbers
- there are no transactions, indexes persisted on disk, foreign keys, views, triggers, subqueries, prepared statements, or concurrent writers
- `ALTER TABLE DROP COLUMN` refuses to drop the final remaining column
- import and export are intended for simple TSV or CSV data and do not implement every dialect variation
- string matching and identifier handling are byte-oriented rather than locale-aware

## EXAMPLES

```
sql notes.db "CREATE TABLE notes(id INTEGER PRIMARY KEY, text TEXT);"
sql notes.db "INSERT INTO notes VALUES (1, hello);"
sql notes.db "SELECT id, text FROM notes ORDER BY id;"
printf 'CREATE TABLE sales(region, amount); INSERT INTO sales VALUES (East, 10); SELECT region, SUM(amount) FROM sales GROUP BY region;\n' | sql sales.db
sql people.db "IMPORT people FROM 'people.csv' CSV CREATE TABLE;"
sql people.db "EXPORT people TO '-' CSV;"
sql people.db "SCHEMA;"
```

## JSON Output

JSON mode limitation: full structured output for this tool is not implemented yet. Until a tool-specific event schema is added, callers should treat normal stdout as the documented text or binary output and use `--json` only where the implementation accepts it for shared usage and diagnostic events. See `json-output` for the common envelope and compatibility rules.

## SEE ALSO

awk, bc, sqlite, sh
