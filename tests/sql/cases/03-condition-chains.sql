# WHERE chains, BETWEEN, LIKE, UPDATE filters, and DELETE filters
CREATE TABLE people(id, name, city, score);
INSERT INTO people VALUES (1, Ada, London, 10);
INSERT INTO people VALUES (2, Alan, London, 20);
INSERT INTO people VALUES (3, Grace, Arlington, 30);
INSERT INTO people VALUES (4, Adele, Paris, 40);
SELECT id, name FROM people WHERE city = London AND score BETWEEN 10 AND 20 ORDER BY id;
SELECT id, name FROM people WHERE name LIKE 'Ad%' OR city = Arlington ORDER BY id;
UPDATE people SET city = UK WHERE name LIKE 'A%' AND score < 30;
SELECT id, name, city FROM people WHERE city = UK OR score >= 40 ORDER BY id;
DELETE FROM people WHERE id BETWEEN 1 AND 2 OR name LIKE 'Grace';
SELECT id, name FROM people ORDER BY id;
