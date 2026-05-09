# NULLs, defaults, NOT NULL, parenthesized filters, multi-row INSERT, import-created tables, and LEFT JOIN
IMPORT imported FROM 'tests/sql/cases/06-import-create.tsv' CREATE TABLE;
SELECT id, label FROM imported ORDER BY id;
CREATE TABLE accounts(id NOT NULL, name NOT NULL DEFAULT unknown, note DEFAULT NULL, qty DEFAULT 7);
INSERT INTO accounts (id, name) VALUES (1, Ada), (2, Bob), (3, Cara);
INSERT INTO accounts (id, name, note, qty) VALUES (4, Dana, '', 0);
SELECT id, name, note, qty FROM accounts WHERE note IS NULL OR (qty = 0 AND note IS EMPTY) ORDER BY id;
SELECT id FROM accounts WHERE (name = Ada OR name = Bob) AND note IS NULL ORDER BY id;
UPDATE accounts SET note = ready WHERE id = 1 OR (id = 2 AND name = Bob);
SELECT id, note FROM accounts WHERE note IS NOT NULL ORDER BY id;
CREATE TABLE memberships(user_id, group_id);
CREATE TABLE groups(id, label);
INSERT INTO memberships VALUES (1, 1), (2, 2);
INSERT INTO groups VALUES (1, admin);
SELECT a.name AS name, g.label AS group_label FROM accounts a LEFT JOIN memberships m ON a.id = m.user_id LEFT JOIN groups g ON m.group_id = g.id ORDER BY a.id;