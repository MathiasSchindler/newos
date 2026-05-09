# Multi-statement execution, aliases, IN/NOT/IS EMPTY, arithmetic updates, and TSV import/export
CREATE TABLE items(id, name, qty, note); INSERT INTO items VALUES (1, alpha, 10, ''); INSERT INTO items VALUES (2, beta, 20, keep); INSERT INTO items VALUES (3, gamma, 30, ''); UPDATE items SET qty = qty + 5 WHERE id IN (1,2); UPDATE items SET qty = qty - 10 WHERE name NOT IN (alpha, beta) AND note IS EMPTY; SELECT id, qty FROM items WHERE id IN (1,2,3) ORDER BY id;
SELECT id, name FROM items WHERE note IS EMPTY OR name NOT LIKE 'b%' ORDER BY id;
CREATE TABLE users(id, name);
CREATE TABLE orders(user_id, total);
INSERT INTO users VALUES (1, Ada);
INSERT INTO users VALUES (2, Alan);
INSERT INTO orders VALUES (1, 30);
INSERT INTO orders VALUES (2, 15);
SELECT u.name AS customer, o.total AS total FROM users u JOIN orders AS o ON u.id = o.user_id WHERE o.total IN (15,30) ORDER BY total DESC;
CREATE TABLE colors(id, label);
IMPORT colors FROM 'tests/sql/cases/05-import.tsv';
EXPORT colors TO 'tests/tmp/05-export.tsv';
CREATE TABLE copied_colors(id, label);
IMPORT copied_colors FROM 'tests/tmp/05-export.tsv';
SELECT id, label FROM copied_colors ORDER BY id;
