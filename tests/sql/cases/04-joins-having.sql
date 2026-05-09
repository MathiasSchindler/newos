# Joins, qualified columns, aliases, grouped aggregates, and HAVING chains
CREATE TABLE users(id, name);
CREATE TABLE orders(user_id, total, status);
INSERT INTO users VALUES (1, Ada);
INSERT INTO users VALUES (2, Alan);
INSERT INTO orders VALUES (1, 10, open);
INSERT INTO orders VALUES (1, 20, paid);
INSERT INTO orders VALUES (2, 5, open);
INSERT INTO orders VALUES (3, 7, open);
SELECT users.name AS customer, orders.total AS total FROM users JOIN orders ON users.id = orders.user_id WHERE orders.total >= 10 ORDER BY customer, total DESC;
SELECT users.name AS customer, count(*) AS order_count, sum(orders.total) AS total FROM users JOIN orders ON users.id = orders.user_id GROUP BY users.name HAVING sum(orders.total) BETWEEN 20 AND 40 OR count(*) = 1 ORDER BY total DESC;
SELECT users.name AS customer, orders.user_id AS user_id, orders.total AS total FROM users RIGHT JOIN orders ON users.id = orders.user_id ORDER BY user_id, total;
