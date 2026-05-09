# PRIMARY KEY, UNIQUE, and DROP COLUMN
CREATE TABLE users(id PRIMARY KEY, email UNIQUE, name);
INSERT INTO users VALUES (1, ada.example, Ada), (2, grace.example, Grace);
SCHEMA;
SELECT id, email, name FROM users ORDER BY id;
ALTER TABLE users DROP COLUMN name;
SCHEMA;
SELECT id, email FROM users ORDER BY id;
