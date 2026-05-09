# ALTER rename/defaults, CSV import/export, FULL JOIN, USING, and SELECT expressions
IMPORT people FROM 'tests/sql/cases/07-people.csv' CSV CREATE TABLE;
ALTER TABLE people RENAME COLUMN note TO memo;
ALTER TABLE people ADD COLUMN qty NOT NULL DEFAULT 5;
ALTER TABLE people RENAME TO contacts;
SELECT id, name || suffix AS full_name, qty + 2 AS qty_plus, qty - 3 AS qty_minus, memo FROM contacts ORDER BY id;
EXPORT contacts TO 'tests/tmp/07-contacts.csv' CSV;
IMPORT roundtrip FROM 'tests/tmp/07-contacts.csv' CSV CREATE TABLE;
SELECT id, name || suffix AS name FROM roundtrip ORDER BY id;
CREATE TABLE lefts(id, left_name);
CREATE TABLE rights(id, right_name);
INSERT INTO lefts VALUES (1, left_one), (2, left_two);
INSERT INTO rights VALUES (2, right_two), (3, right_three);
SELECT lefts.id AS left_id, left_name, rights.id AS right_id, right_name FROM lefts FULL JOIN rights USING (id) ORDER BY right_id;
