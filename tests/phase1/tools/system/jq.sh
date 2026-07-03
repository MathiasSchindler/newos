#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/tools/system/common.inc"

phase1_setup jq

cat > "$WORK_DIR/sample.json" <<'JSON'
{"name":"root","x":4,"y":2,"flag":true,"path":"/usr/local/bin","code":"Ada42","count_string":"42","users":[{"name":"Ada","active":true,"score":3,"tags":["math","code"]},{"name":"Grace","active":false,"score":5,"tags":["code"]}],"matrix":[[1,2],[3,4]],"nums":[3,1,2],"obj":{"b":2,"a":1},"json":"{\"ok\":true,\"n\":7}","meta":{"count":2,"ok":true},"weird-key":{"inner.value":"ok"},"escaped":"line\nnext","heart":"\u2665","word":"hé"}
JSON

assert_text_equals "$("${TEST_BIN_DIR}/jq" '.' "$WORK_DIR/sample.json")" "$(cat "$WORK_DIR/sample.json")" "jq identity filter changed the input slice"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name' "$WORK_DIR/sample.json")" '"root"' "jq object field selection failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[1].name' "$WORK_DIR/sample.json")" '"Grace"' "jq array index plus field selection failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.matrix[1][0]' "$WORK_DIR/sample.json")" '3' "jq nested array index selection failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '."weird-key"["inner.value"]' "$WORK_DIR/sample.json")" '"ok"' "jq quoted key selection failed"

names=$("${TEST_BIN_DIR}/jq" -r '.users[].name' "$WORK_DIR/sample.json")
expected_names=$(printf 'Ada\nGrace')
assert_text_equals "$names" "$expected_names" "jq array iteration plus raw output failed"

escaped=$("${TEST_BIN_DIR}/jq" -r '.escaped' "$WORK_DIR/sample.json")
expected_escaped=$(printf 'line\nnext')
assert_text_equals "$escaped" "$expected_escaped" "jq raw output did not decode newline escapes"

heart=$("${TEST_BIN_DIR}/jq" -r '.heart' "$WORK_DIR/sample.json")
expected_heart=$(printf '\342\231\245')
assert_text_equals "$heart" "$expected_heart" "jq raw output did not decode Unicode escapes"

pipe_names=$("${TEST_BIN_DIR}/jq" -r '.users[] | .name' "$WORK_DIR/sample.json")
assert_text_equals "$pipe_names" "$expected_names" "jq pipe filter failed"

comma_values=$("${TEST_BIN_DIR}/jq" '.name, .meta.count' "$WORK_DIR/sample.json")
expected_comma=$(printf '"root"\n2')
assert_text_equals "$comma_values" "$expected_comma" "jq comma filter failed"

selected=$("${TEST_BIN_DIR}/jq" -r '.users[] | select(.active) | .name' "$WORK_DIR/sample.json")
assert_text_equals "$selected" 'Ada' "jq select truthy filter failed"

selected_compare=$("${TEST_BIN_DIR}/jq" -r '.users[] | select(.name == "Grace") | .name' "$WORK_DIR/sample.json")
assert_text_equals "$selected_compare" 'Grace' "jq select equality predicate failed"

mapped=$("${TEST_BIN_DIR}/jq" '.users | map(.name)' "$WORK_DIR/sample.json")
assert_text_equals "$mapped" '["Ada","Grace"]' "jq map filter failed"

assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | length' "$WORK_DIR/sample.json")" '2' "jq array length failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta | length' "$WORK_DIR/sample.json")" '2' "jq object length failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.word | length' "$WORK_DIR/sample.json")" '2' "jq string length failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta | keys' "$WORK_DIR/sample.json")" '["count","ok"]' "jq object keys failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | keys' "$WORK_DIR/sample.json")" '[0,1]' "jq array keys failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta | has("ok")' "$WORK_DIR/sample.json")" 'true' "jq object has true case failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta | has("missing")' "$WORK_DIR/sample.json")" 'false' "jq object has false case failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | has(1)' "$WORK_DIR/sample.json")" 'true' "jq array has true case failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | type' "$WORK_DIR/sample.json")" '"array"' "jq type filter failed"

recursive_types=$("${TEST_BIN_DIR}/jq" '.. | type' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/head" -n 4)
expected_recursive_types=$(printf '"object"\n"string"\n"number"\n"number"')
assert_text_equals "$recursive_types" "$expected_recursive_types" "jq recursive descent order changed"

assert_text_equals "$("${TEST_BIN_DIR}/jq" '.x + 3 * .y' "$WORK_DIR/sample.json")" '10' "jq arithmetic precedence failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name + "-user"' "$WORK_DIR/sample.json")" '"root-user"' "jq string concatenation failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '"user=\(.name),count=\(.meta.count)"' "$WORK_DIR/sample.json")" '"user=root,count=2"' "jq string interpolation failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name | contains("oo")' "$WORK_DIR/sample.json")" 'true' "jq contains string filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name | startswith("ro")' "$WORK_DIR/sample.json")" 'true' "jq startswith string filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name | endswith("ot")' "$WORK_DIR/sample.json")" 'true' "jq endswith string filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.path | split("/")' "$WORK_DIR/sample.json")" '["","usr","local","bin"]' "jq split string filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | map(.name) | join(",")' "$WORK_DIR/sample.json")" '"Ada,Grace"' "jq join string filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name | ascii_upcase' "$WORK_DIR/sample.json")" '"ROOT"' "jq ascii_upcase filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.code | test("[A-Z][a-z]+[0-9]+")' "$WORK_DIR/sample.json")" 'true' "jq regex test filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.code | match("[0-9]+")' "$WORK_DIR/sample.json")" '{"offset":3,"length":2,"string":"42"}' "jq regex match filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.code | capture("[A-Z][a-z]+")' "$WORK_DIR/sample.json")" '{"match":"Ada"}' "jq regex capture filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.code | sub("[0-9]+"; "")' "$WORK_DIR/sample.json")" '"Ada"' "jq regex sub filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.path | gsub("/"; ":")' "$WORK_DIR/sample.json")" '":usr:local:bin"' "jq regex gsub filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name | @json' "$WORK_DIR/sample.json")" '"\"root\""' "jq @json formatting filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.x >= 4 and .y <= 2 and .x != .y' "$WORK_DIR/sample.json")" 'true' "jq ordering comparison and boolean filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'null < false and false < 0 and 0 < "a" and "a" < [] and [] < {}' "$WORK_DIR/sample.json")" 'true' "jq type ordering comparison failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.x < .y or not false' "$WORK_DIR/sample.json")" 'true' "jq boolean or/not filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'if .flag then .name else "no" end' "$WORK_DIR/sample.json")" '"root"' "jq conditional filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[] | if .active then .name else empty end' "$WORK_DIR/sample.json")" '"Ada"' "jq empty filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'try .missing.required catch "fallback"' "$WORK_DIR/sample.json")" '"fallback"' "jq try/catch filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.name | tonumber?' "$WORK_DIR/sample.json")" '' "jq optional suffix should suppress filter errors"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.missing // "fallback"' "$WORK_DIR/sample.json")" '"fallback"' "jq fallback operator failed on missing field"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta.missing?' "$WORK_DIR/sample.json")" 'null' "jq optional object access failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[9]? // "none"' "$WORK_DIR/sample.json")" '"none"' "jq optional array access plus fallback failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[-1].name' "$WORK_DIR/sample.json")" '"Grace"' "jq negative array index failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[:-1] | map(.score)' "$WORK_DIR/sample.json")" '[3]' "jq array slice with negative bound failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '[.name, .meta.count, .x + 1]' "$WORK_DIR/sample.json")" '["root",2,5]' "jq array construction failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '{who: .name, next: .x + 1}' "$WORK_DIR/sample.json")" '{"who":"root","next":5}' "jq object construction failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '{name, (.meta.count | tostring): .x}' "$WORK_DIR/sample.json")" '{"name":"root","2":4}' "jq object shorthand and computed key construction failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta.count as $n | $n + 10' "$WORK_DIR/sample.json")" '12' "jq variable binding failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '. as {name: $n, users: [$first, $second], meta: {count: $c}} | [$n, $first.name, $second.name, $c]' "$WORK_DIR/sample.json")" '["root","Ada","Grace",2]' "jq destructuring binding failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'def inc($v): $v + 1; inc(.x)' "$WORK_DIR/sample.json")" '5' "jq user-defined function failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'def add($a; $b): $a + $b; add(.x; .y)' "$WORK_DIR/sample.json")" '6' "jq multi-argument function failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'def addn($x): $x + $n; .x as $n | addn(3)' "$WORK_DIR/sample.json")" '7' "jq function access to active binding failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta.count = 7' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '.meta.count')" '7' "jq path assignment failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[0].score |= . + 10' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '.users[0].score')" '13' "jq path update failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.created.deep = .name' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '.created.deep')" '"root"' "jq missing object path creation failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users[3].name = "Linus"' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '.users[3].name')" '"Linus"' "jq missing array path creation failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.alias, .meta.alias = .name' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '[.alias, .meta.alias]')" '["root","root"]' "jq multiple assignment targets failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.meta.count, .x |= . + 1' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '[.meta.count, .x]')" '[3,5]' "jq multiple update targets failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.new.count |= . // 0 + 1' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '.new.count')" '1' "jq missing update path creation failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'del(.obj.b, .users[0].score)' "$WORK_DIR/sample.json" | "${TEST_BIN_DIR}/jq" '[.obj, .users[0]]')" '[{"a":1},{"name":"Ada","active":true,"tags":["math","code"]}]' "jq del filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.obj | to_entries' "$WORK_DIR/sample.json")" '[{"key":"b","value":2},{"key":"a","value":1}]' "jq to_entries failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.obj | to_entries | from_entries' "$WORK_DIR/sample.json")" '{"b":2,"a":1}' "jq from_entries failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.obj | with_entries(.value |= . + 10)' "$WORK_DIR/sample.json")" '{"b":12,"a":11}' "jq with_entries failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | sort' "$WORK_DIR/sample.json")" '[1,2,3]' "jq sort failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | sort_by(.active) | map(.name)' "$WORK_DIR/sample.json")" '["Grace","Ada"]' "jq sort_by failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | add' "$WORK_DIR/sample.json")" '6' "jq add aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | first' "$WORK_DIR/sample.json")" '3' "jq first aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | last' "$WORK_DIR/sample.json")" '2' "jq last aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | min' "$WORK_DIR/sample.json")" '1' "jq min aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | max' "$WORK_DIR/sample.json")" '3' "jq max aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | unique' "$WORK_DIR/sample.json")" '[1,2,3]' "jq unique aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | min_by(.score)' "$WORK_DIR/sample.json")" '{"name":"Ada","active":true,"score":3,"tags":["math","code"]}' "jq min_by aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | max_by(.score)' "$WORK_DIR/sample.json")" '{"name":"Grace","active":false,"score":5,"tags":["code"]}' "jq max_by aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | unique_by(.active) | map(.name)' "$WORK_DIR/sample.json")" '["Grace","Ada"]' "jq unique_by aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.users | group_by(.active) | map(map(.name))' "$WORK_DIR/sample.json")" '[["Grace"],["Ada"]]' "jq group_by aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.matrix | flatten' "$WORK_DIR/sample.json")" '[1,2,3,4]' "jq flatten aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | any(. > 2)' "$WORK_DIR/sample.json")" 'true' "jq any aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.nums | all(. > 0)' "$WORK_DIR/sample.json")" 'true' "jq all aggregator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'range(1; 5; 2)' "$WORK_DIR/sample.json")" "$(printf '1\n3')" "jq range generator failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.path | index("local")' "$WORK_DIR/sample.json")" '5' "jq index filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.path | indices("/")' "$WORK_DIR/sample.json")" '[0,4,10]' "jq indices filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'reduce .nums[] as $n (0; . + $n)' "$WORK_DIR/sample.json")" '6' "jq reduce filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" 'foreach .nums[] as $n (0; . + $n)' "$WORK_DIR/sample.json")" "$(printf '3\n4\n6')" "jq foreach filter failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.count_string | tonumber' "$WORK_DIR/sample.json")" '42' "jq tonumber failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.json | fromjson | .n' "$WORK_DIR/sample.json")" '7' "jq fromjson failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" '.obj | tojson' "$WORK_DIR/sample.json")" '"{\"b\":2,\"a\":1}"' "jq tojson failed"

cat > "$WORK_DIR/math.jq" <<'JQ'
def bump($v): $v + 1;
JQ
include_filter="include \"$WORK_DIR/math.jq\"; bump(.x)"
import_filter="import \"$WORK_DIR/math.jq\" as math; bump(.y)"
assert_text_equals "$("${TEST_BIN_DIR}/jq" "$include_filter" "$WORK_DIR/sample.json")" '5' "jq include directive failed"
assert_text_equals "$("${TEST_BIN_DIR}/jq" "$import_filter" "$WORK_DIR/sample.json")" '3' "jq import directive failed"

missing_status=0
"${TEST_BIN_DIR}/jq" '.users[9]' "$WORK_DIR/sample.json" > "$WORK_DIR/missing.out" 2>&1 || missing_status=$?
assert_exit_code "$missing_status" 1 "jq out-of-range array index should fail in the current subset"
assert_file_contains "$WORK_DIR/missing.out" 'filter failed' "jq failure diagnostic did not mention filter failure"