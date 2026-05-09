# Typed columns, signed decimal aggregates, extra aggregates, and NATURAL JOIN
CREATE TABLE metrics(name TEXT, amount REAL, qty INTEGER);
INSERT INTO metrics VALUES (refund, -1.5, -1);
INSERT INTO metrics VALUES (sale, 2.25, 2);
INSERT INTO metrics VALUES (bonus, 3, 3);
SELECT SUM(amount) AS sum_amount, AVG(amount) AS avg_amount, TOTAL(amount) AS total_amount, MIN(amount) AS min_amount, MAX(amount) AS max_amount FROM metrics;
SELECT FIRST(name) AS first_name, LAST(name) AS last_name, GROUP_CONCAT(name) AS names FROM metrics;
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE visits(id INTEGER, city TEXT);
INSERT INTO users VALUES (1, Ada);
INSERT INTO users VALUES (2, Alan);
INSERT INTO visits VALUES (1, Zurich);
INSERT INTO visits VALUES (2, London);
SELECT users.name AS name, visits.city AS city FROM users NATURAL JOIN visits ORDER BY name;
SCHEMA;