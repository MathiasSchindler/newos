# Basic schema, explicit inserts, ALTER TABLE, UPDATE, DROP
CREATE TABLE IF NOT EXISTS people(id, name, city);
CREATE TABLE IF NOT EXISTS people(id, name, city);
INSERT INTO people (name, id) VALUES (Ada, 1);
INSERT INTO people VALUES (2, Alan, London);
SELECT id, name, city FROM people ORDER BY id;
ALTER TABLE people ADD COLUMN status;
SELECT id, status FROM people ORDER BY id;
UPDATE people SET status = active WHERE id = 1 OR name = Alan;
SELECT id, status FROM people ORDER BY id;
SCHEMA;
DROP TABLE IF EXISTS missing;
DROP TABLE people;
SCHEMA;
