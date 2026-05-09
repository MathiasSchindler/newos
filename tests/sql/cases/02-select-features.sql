# SELECT projection, DISTINCT, multi-key order, LIMIT comma, aliases, and aggregates
CREATE TABLE sales(region, item, amount, optional);
INSERT INTO sales VALUES (East, beta, 10, yes);
INSERT INTO sales VALUES (East, alpha, 2, yes);
INSERT INTO sales (region, item, amount) VALUES (West, gamma, 30);
INSERT INTO sales VALUES (West, delta, 4, yes);
SELECT region, item, amount FROM sales ORDER BY region ASC, amount DESC;
SELECT item FROM sales ORDER BY amount LIMIT 1,2;
SELECT DISTINCT region FROM sales ORDER BY region;
SELECT region AS place, count(*) AS rows, count(optional) AS filled, sum(amount) AS total, avg(amount) AS avg_amount, min(amount) AS low, max(amount) AS high FROM sales GROUP BY region HAVING count(*) >= 1 AND avg(amount) >= 6 ORDER BY rows DESC, place;
