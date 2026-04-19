#!/bin/bash
set -e

echo "Testing bc with bignum support..."

# Test large number addition
result=$(./build/bc '12345678901234567890 + 98765432109876543210')
expected="111111111011111111100"
if [ "$result" != "$expected" ]; then
    echo "FAIL: Large addition - Expected $expected, got $result"
    exit 1
fi
echo "PASS: Large addition"

# Test power of 2^64
result=$(./build/bc '2^64')
expected="18446744073709551616"
if [ "$result" != "$expected" ]; then
    echo "FAIL: 2^64 - Expected $expected, got $result"
    exit 1
fi
echo "PASS: 2^64"

# Test power of 2^100
result=$(./build/bc '2^100')
expected="1267650600228229401496703205376"
if [ "$result" != "$expected" ]; then
    echo "FAIL: 2^100 - Expected $expected, got $result"
    exit 1
fi
echo "PASS: 2^100"

# Test large multiplication
result=$(./build/bc '999999999999 * 888888888888')
expected="888888888887111111111112"
if [ "$result" != "$expected" ]; then
    echo "FAIL: Large multiplication - Expected $expected, got $result"
    exit 1
fi
echo "PASS: Large multiplication"

echo "All bignum bc tests passed!"
