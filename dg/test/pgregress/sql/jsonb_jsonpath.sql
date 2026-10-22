select jsonb '{"a": 12}' @? '$';
select jsonb '{"a": 12}' @? '1';
select jsonb '{"a": 12}' @? '$.a.b';
select jsonb '{"a": 12}' @? '$.b';
select jsonb '{"a": 12}' @? '$.a + 2';
select jsonb '{"a": 12}' @? '$.b + 2';
select jsonb '{"a": {"a": 12}}' @? '$.a.a';
select jsonb '{"a": {"a": 12}}' @? '$.*.a';
select jsonb '{"b": {"a": 12}}' @? '$.*.a';
select jsonb '{"b": {"a": 12}}' @? '$.*.b';
select jsonb '{"b": {"a": 12}}' @? 'strict $.*.b';
select jsonb '{}' @? '$.*';
select jsonb '{"a": 1}' @? '$.*';
select jsonb '{"a": {"b": 1}}' @? 'lax $.**{1}';
select jsonb '{"a": {"b": 1}}' @? 'lax $.**{2}';
select jsonb '{"a": {"b": 1}}' @? 'lax $.**{3}';
select jsonb '[]' @? '$[*]';
select jsonb '[1]' @? '$[*]';
select jsonb '[1]' @? '$[1]';
select jsonb '[1]' @? 'strict $[1]';
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1]', 'strict $[1]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1]', 'strict $[1]', silent => true);
select jsonb '[1]' @? 'lax $[10000000000000000]';
select jsonb '[1]' @? 'strict $[10000000000000000]';
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1]', 'lax $[10000000000000000]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1]', 'strict $[10000000000000000]');
select jsonb '[1]' @? '$[0]';
select jsonb '[1]' @? '$[0.3]';
select jsonb '[1]' @? '$[0.5]';
select jsonb '[1]' @? '$[0.9]';
select jsonb '[1]' @? '$[1.2]';
select jsonb '[1]' @? 'strict $[1.2]';
select jsonb '{"a": [1,2,3], "b": [3,4,5]}' @? '$ ? (@.a[*] >  @.b[*])';
select jsonb '{"a": [1,2,3], "b": [3,4,5]}' @? '$ ? (@.a[*] >= @.b[*])';
select jsonb '{"a": [1,2,3], "b": [3,4,"5"]}' @? '$ ? (@.a[*] >= @.b[*])';
select jsonb '{"a": [1,2,3], "b": [3,4,"5"]}' @? 'strict $ ? (@.a[*] >= @.b[*])';
select jsonb '{"a": [1,2,3], "b": [3,4,null]}' @? '$ ? (@.a[*] >= @.b[*])';
select jsonb '1' @? '$ ? ((@ == "1") is unknown)';
select jsonb '1' @? '$ ? ((@ == 1) is unknown)';
select jsonb '[{"a": 1}, {"a": 2}]' @? '$[0 to 1] ? (@.a > 1)';

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_exists('[{"a": 1}, {"a": 2}, 3]', 'lax $[*].a', silent => false);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_exists('[{"a": 1}, {"a": 2}, 3]', 'lax $[*].a', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_exists('[{"a": 1}, {"a": 2}, 3]', 'strict $[*].a', silent => false);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_exists('[{"a": 1}, {"a": 2}, 3]', 'strict $[*].a', silent => true);

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'lax $.a');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $.a');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $.*');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $.a', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $.*', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'lax $.a');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.a', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', 'lax $.a');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', 'strict $.a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', 'strict $.a', silent => true);

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $[1]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $[*]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $[1]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $["a"]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $[1]', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'strict $[*]', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $[1]', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $["a"]', silent => true);

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 12, "b": {"a": 13}}', '$.a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 12, "b": {"a": 13}}', '$.b');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 12, "b": {"a": 13}}', '$.*');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 12, "b": {"a": 13}}', 'lax $.*.a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[*].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[*].*');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[0].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[1].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[2].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[0,1].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[0 to 10].a');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}]', 'lax $[0 to 10 / 0].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[12, {"a": 13}, {"b": 14}, "ccc", true]', '$[2.5 - 1 to $.size() - 2]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'lax $[0]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', 'lax $[*]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1]', 'lax $[0]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1]', 'lax $[*]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', 'lax $[*]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', 'strict $[*].a');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', 'strict $[*].a', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$[last]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$[last ? (exists(last))]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $[last]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $[last]', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1]', '$[last]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', '$[last]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', '$[last - 1]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', '$[last ? (@.type() == "number")]');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', '$[last ? (@.type() == "string")]');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', '$[last ? (@.type() == "string")]', silent => true);

--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$');
--dg-differs: another error: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$ ? (@.a < $value)');
--dg-differs: another error: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$ ? (@.a < $value)', '1');
--dg-differs: another error: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$ ? (@.a < $value)', '[{"value" : 13}]');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$ ? (@.a < $value)', '{"value" : 13}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$ ? (@.a < $value)', '{"value" : 8}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('{"a": 10}', '$.a ? (@ < $value)', '{"value" : 13}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[10,11,12,13,14,15]', '$[*] ? (@ < $value)', '{"value" : 13}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[10,11,12,13,14,15]', '$[0,1] ? (@ < $x.value)', '{"x": {"value" : 13}}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[10,11,12,13,14,15]', '$[0 to 2] ? (@ < $value)', '{"value" : 15}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[1,"1",2,"2",null]', '$[*] ? (@ == "1")');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[1,"1",2,"2",null]', '$[*] ? (@ == $value)', '{"value" : "1"}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[1,"1",2,"2",null]', '$[*] ? (@ == $value)', '{"value" : null}');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[1, "2", null]', '$[*] ? (@ != null)');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[1, "2", null]', '$[*] ? (@ == null)');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('{}', '$ ? (@ == @)');
--dg-skip: function ?(...) does not exist
--dg select * from jsonb_path_query('[]', 'strict $ ? (@ == @)');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{0}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{0 to last}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{1}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{1 to last}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{2}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{2 to last}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{3 to last}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{last}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{0}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{1}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{0 to last}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{1 to last}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"b": 1}}', 'lax $.**{1 to 2}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**{0}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**{1}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**{0 to last}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**{1 to last}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**{1 to 2}.b ? (@ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": {"c": {"b": 1}}}', 'lax $.**{2 to 3}.b ? (@ > 0)');

select jsonb '{"a": {"b": 1}}' @? '$.**.b ? ( @ > 0)';
select jsonb '{"a": {"b": 1}}' @? '$.**{0}.b ? ( @ > 0)';
select jsonb '{"a": {"b": 1}}' @? '$.**{1}.b ? ( @ > 0)';
select jsonb '{"a": {"b": 1}}' @? '$.**{0 to last}.b ? ( @ > 0)';
select jsonb '{"a": {"b": 1}}' @? '$.**{1 to last}.b ? ( @ > 0)';
select jsonb '{"a": {"b": 1}}' @? '$.**{1 to 2}.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**{0}.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**{1}.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**{0 to last}.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**{1 to last}.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**{1 to 2}.b ? ( @ > 0)';
select jsonb '{"a": {"c": {"b": 1}}}' @? '$.**{2 to 3}.b ? ( @ > 0)';

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": {"x": 2}}', '$.g ? (exists (@.x))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": {"x": 2}}', '$.g ? (exists (@.y))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": {"x": 2}}', '$.g ? (exists (@.x ? (@ >= 2) ))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'lax $.g ? (exists (@.x))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'lax $.g ? (exists (@.x + "3"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'lax $.g ? ((exists (@.x + "3")) is unknown)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'strict $.g[*] ? (exists (@.x))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'strict $.g[*] ? ((exists (@.x)) is unknown)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'strict $.g ? (exists (@[*].x))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"g": [{"x": 2}, {"y": 3}]}', 'strict $.g ? ((exists (@[*].x)) is unknown)');

--test ternary logic
--dg-skip: function ?(...) does not exist
--dg select
--dg 	x, y,
--dg 	jsonb_path_query(
--dg 		'[true, false, null]',
--dg 		'$[*] ? (@ == true  &&  ($x == true && $y == true) ||
--dg 				 @ == false && !($x == true && $y == true) ||
--dg 				 @ == null  &&  ($x == true && $y == true) is unknown)',
--dg 		jsonb_build_object('x', x, 'y', y)
--dg 	) as "x && y"
--dg from
--dg 	(values (jsonb 'true'), ('false'), ('"null"')) x(x),
--dg 	(values (jsonb 'true'), ('false'), ('"null"')) y(y);

--dg-skip: function ?(...) does not exist
--dg select
--dg 	x, y,
--dg 	jsonb_path_query(
--dg 		'[true, false, null]',
--dg 		'$[*] ? (@ == true  &&  ($x == true || $y == true) ||
--dg 				 @ == false && !($x == true || $y == true) ||
--dg 				 @ == null  &&  ($x == true || $y == true) is unknown)',
--dg 		jsonb_build_object('x', x, 'y', y)
--dg 	) as "x || y"
--dg from
--dg 	(values (jsonb 'true'), ('false'), ('"null"')) x(x),
--dg 	(values (jsonb 'true'), ('false'), ('"null"')) y(y);

select jsonb '{"a": 1, "b":1}' @? '$ ? (@.a == @.b)';
select jsonb '{"c": {"a": 1, "b":1}}' @? '$ ? (@.a == @.b)';
select jsonb '{"c": {"a": 1, "b":1}}' @? '$.c ? (@.a == @.b)';
select jsonb '{"c": {"a": 1, "b":1}}' @? '$.c ? ($.c.a == @.b)';
select jsonb '{"c": {"a": 1, "b":1}}' @? '$.* ? (@.a == @.b)';
select jsonb '{"a": 1, "b":1}' @? '$.** ? (@.a == @.b)';
select jsonb '{"c": {"a": 1, "b":1}}' @? '$.** ? (@.a == @.b)';

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"c": {"a": 2, "b":1}}', '$.** ? (@.a == 1 + 1)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"c": {"a": 2, "b":1}}', '$.** ? (@.a == (1 + 1))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"c": {"a": 2, "b":1}}', '$.** ? (@.a == @.b + 1)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"c": {"a": 2, "b":1}}', '$.** ? (@.a == (@.b + 1))');
select jsonb '{"c": {"a": -1, "b":1}}' @? '$.** ? (@.a == - 1)';
select jsonb '{"c": {"a": -1, "b":1}}' @? '$.** ? (@.a == -1)';
select jsonb '{"c": {"a": -1, "b":1}}' @? '$.** ? (@.a == -@.b)';
select jsonb '{"c": {"a": -1, "b":1}}' @? '$.** ? (@.a == - @.b)';
select jsonb '{"c": {"a": 0, "b":1}}' @? '$.** ? (@.a == 1 - @.b)';
select jsonb '{"c": {"a": 2, "b":1}}' @? '$.** ? (@.a == 1 - - @.b)';
select jsonb '{"c": {"a": 0, "b":1}}' @? '$.** ? (@.a == 1 - +@.b)';
select jsonb '[1,2,3]' @? '$ ? (+@[*] > +2)';
select jsonb '[1,2,3]' @? '$ ? (+@[*] > +3)';
select jsonb '[1,2,3]' @? '$ ? (-@[*] < -2)';
select jsonb '[1,2,3]' @? '$ ? (-@[*] < -3)';
select jsonb '1' @? '$ ? ($ > 0)';

-- arithmetic errors
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,0,3]', '$[*] ? (2 / @ > 0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,0,3]', '$[*] ? ((2 / @ > 0) is unknown)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('0', '1 / $');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('0', '1 / $ + 2');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('0', '-(3 + 1 % $)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$ + "2"');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1, 2]', '3 * $');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"a"', '-$');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1,"2",3]', '+$');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', '$ + "2"', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1, 2]', '3 * $', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"a"', '-$', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,"2",3]', '+$', silent => true);
select jsonb '["1",2,0,3]' @? '-$[*]';
select jsonb '[1,"2",0,3]' @? '-$[*]';
select jsonb '["1",2,0,3]' @? 'strict -$[*]';
select jsonb '[1,"2",0,3]' @? 'strict -$[*]';

-- unwrapping of operator arguments in lax mode
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": [2]}', 'lax $.a * 3');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": [2]}', 'lax $.a + 3');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": [2, 3, 4]}', 'lax -$.a');
-- should fail
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{"a": [1, 2]}', 'lax $.a * 3');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": [1, 2]}', 'lax $.a * 3', silent => true);

-- any key on arrays with and without unwrapping.
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": [1,2,3], "b": [3,4,5]}', '$.*');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3]', '$.*');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3,{"b": [3,4,5]}]', 'lax $.*');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1,2,3,{"b": [3,4,5]}]', 'strict $.*');
select jsonb_path_query('[1,2,3,{"b": [3,4,5]}]', 'strict $.*', NULL, true);
select jsonb '{"a": [1,2,3], "b": [3,4,5]}' @? '$.*';
select jsonb '[1,2,3]' @? '$.*';
select jsonb '[1,2,3,{"b": [3,4,5]}]' @? 'lax $.*';
select jsonb '[1,2,3,{"b": [3,4,5]}]' @? 'strict $.*';

-- extension: boolean expressions
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('2', '$ > 1');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('2', '$ <= 1');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('2', '$ == "2"');
select jsonb '2' @? '$ == "2"';

select jsonb '2' @@ '$ > 1';
select jsonb '2' @@ '$ <= 1';
select jsonb '2' @@ '$ == "2"';
select jsonb '2' @@ '1';
select jsonb '{}' @@ '$';
select jsonb '[]' @@ '$';
select jsonb '[1,2,3]' @@ '$[*]';
select jsonb '[]' @@ '$[*]';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_match('[[1, true], [2, false]]', 'strict $[*] ? (@[0] > $x) [1]', '{"x": 1}');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_match('[[1, true], [2, false]]', 'strict $[*] ? (@[0] < $x) [1]', '{"x": 2}');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_match('[{"a": 1}, {"a": 2}, 3]', 'lax exists($[*].a)', silent => false);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_match('[{"a": 1}, {"a": 2}, 3]', 'lax exists($[*].a)', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_match('[{"a": 1}, {"a": 2}, 3]', 'strict exists($[*].a)', silent => false);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_match('[{"a": 1}, {"a": 2}, 3]', 'strict exists($[*].a)', silent => true);


--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null,1,true,"a",[],{}]', '$.type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null,1,true,"a",[],{}]', 'lax $.type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null,1,true,"a",[],{}]', '$[*].type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', 'null.type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', 'true.type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '(123).type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '"123".type()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 2}', '($.a - 5).abs() + 10');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 2.5}', '-($.a * $.a).floor() % 4.3');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1, 2, 3]', '($[*] > 2) ? (@ == true)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1, 2, 3]', '($[*] > 3).type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1, 2, 3]', '($[*].a > 3).type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1, 2, 3]', 'strict ($[*].a > 3).type()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[1,null,true,"11",[],[1],[1,2,3],{},{"a":1,"b":2}]', 'strict $[*].size()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,null,true,"11",[],[1],[1,2,3],{},{"a":1,"b":2}]', 'strict $[*].size()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[1,null,true,"11",[],[1],[1,2,3],{},{"a":1,"b":2}]', 'lax $[*].size()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[0, 1, -2, -3.4, 5.6]', '$[*].abs()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[0, 1, -2, -3.4, 5.6]', '$[*].floor()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[0, 1, -2, -3.4, 5.6]', '$[*].ceiling()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[0, 1, -2, -3.4, 5.6]', '$[*].ceiling().abs()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[0, 1, -2, -3.4, 5.6]', '$[*].ceiling().abs().type()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[{},1]', '$[*].keyvalue()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[{},1]', '$[*].keyvalue()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.keyvalue()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{"a": 1, "b": [1, 2], "c": {"a": "bbb"}}', '$.keyvalue()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[{"a": 1, "b": [1, 2]}, {"c": {"a": "bbb"}}]', '$[*].keyvalue()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[{"a": 1, "b": [1, 2]}, {"c": {"a": "bbb"}}]', 'strict $.keyvalue()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[{"a": 1, "b": [1, 2]}, {"c": {"a": "bbb"}}]', 'lax $.keyvalue()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[{"a": 1, "b": [1, 2]}, {"c": {"a": "bbb"}}]', 'strict $.keyvalue().a');
select jsonb '{"a": 1, "b": [1, 2]}' @? 'lax $.keyvalue()';
select jsonb '{"a": 1, "b": [1, 2]}' @? 'lax $.keyvalue().key';

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.double()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.double()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.double()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.double()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.double()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.double()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.double()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1e1000', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"nan"', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"NaN"', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.double()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.double()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.double()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.double()', silent => true);

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.abs()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.floor()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.2"', '$.ceiling()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.abs()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.floor()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1.2"', '$.ceiling()', silent => true);

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('["", "a", "abc", "abcabc"]', '$[*] ? (@ starts with "abc")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('["", "a", "abc", "abcabc"]', 'strict $ ? (@[*] starts with "abc")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('["", "a", "abd", "abdabc"]', 'strict $ ? (@[*] starts with "abc")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('["abc", "abcabc", null, 1]', 'strict $ ? (@[*] starts with "abc")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('["abc", "abcabc", null, 1]', 'strict $ ? ((@[*] starts with "abc") is unknown)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[[null, 1, "abc", "abcabc"]]', 'lax $ ? (@[*] starts with "abc")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[[null, 1, "abd", "abdabc"]]', 'lax $ ? ((@[*] starts with "abc") is unknown)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "abd", "abdabc"]', 'lax $[*] ? ((@ starts with "abc") is unknown)');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "abc", "abd", "aBdC", "abdacb", "babc", "adc\nabc", "ab\nadc"]', 'lax $[*] ? (@ like_regex "^ab.*c")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "abc", "abd", "aBdC", "abdacb", "babc", "adc\nabc", "ab\nadc"]', 'lax $[*] ? (@ like_regex "^ab.*c" flag "i")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "abc", "abd", "aBdC", "abdacb", "babc", "adc\nabc", "ab\nadc"]', 'lax $[*] ? (@ like_regex "^ab.*c" flag "m")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "abc", "abd", "aBdC", "abdacb", "babc", "adc\nabc", "ab\nadc"]', 'lax $[*] ? (@ like_regex "^ab.*c" flag "s")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "a\b", "a\\b", "^a\\b$"]', 'lax $[*] ? (@ like_regex "a\\b" flag "q")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "a\b", "a\\b", "^a\\b$"]', 'lax $[*] ? (@ like_regex "a\\b" flag "")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "a\b", "a\\b", "^a\\b$"]', 'lax $[*] ? (@ like_regex "^a\\b$" flag "q")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "a\b", "a\\b", "^a\\b$"]', 'lax $[*] ? (@ like_regex "^a\\B$" flag "q")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "a\b", "a\\b", "^a\\b$"]', 'lax $[*] ? (@ like_regex "^a\\B$" flag "iq")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[null, 1, "a\b", "a\\b", "^a\\b$"]', 'lax $[*] ? (@ like_regex "^a\\b$" flag "")');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"bogus"', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34"', '$.datetime("aaa")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"aaaa"', '$.datetime("HH24")');

select jsonb '"10-03-2017"' @? '$.datetime("dd-mm-yyyy")';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017"', '$.datetime("dd-mm-yyyy")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017"', '$.datetime("dd-mm-yyyy").type()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '$.datetime("dd-mm-yyyy")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '$.datetime("dd-mm-yyyy").type()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '       $.datetime("dd-mm-yyyy HH24:MI").type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 +05:20"', '$.datetime("dd-mm-yyyy HH24:MI TZH:TZM").type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.datetime("HH24:MI:SS").type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:20"', '$.datetime("HH24:MI:SS TZH:TZM").type()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017T12:34:56"', '$.datetime("dd-mm-yyyy\"T\"HH24:MI:SS")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017t12:34:56"', '$.datetime("dd-mm-yyyy\"T\"HH24:MI:SS")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34:56"', '$.datetime("dd-mm-yyyy\"T\"HH24:MI:SS")');

-- Test .bigint()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.bigint()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.bigint()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.bigint()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.bigint()', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1e1000', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"nan"', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"NaN"', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.bigint()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.bigint()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"123"', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.83', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234567890123', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1234567890123"', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12345678901234567890', '$.bigint()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12345678901234567890"', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"+123"', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-123', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-123"', '$.bigint()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.bigint() * 2');

-- Test .boolean()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.boolean()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.boolean()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.boolean()', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1e1000', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"nan"', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"NaN"', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.boolean()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.boolean()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.boolean()', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"100"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('false', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('0', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-1', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('100', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"0"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"true"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"false"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"TRUE"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"FALSE"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"yes"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"NO"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"T"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"f"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"y"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"N"', '$.boolean()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.boolean().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.boolean().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"Yes"', '$.boolean().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_array('[1, "yes", false]', '$[*].boolean()');

-- Test .date()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.date()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"bogus"', '$.date()');

select jsonb '"2023-08-15"' @? '$.date()';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.date()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.date().type()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:30"', '$.date()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.date()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.date()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.date()'); -- should work

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.date(2)');

-- Test .decimal()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.decimal()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.decimal()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.decimal()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.decimal()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1e1000', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"nan"', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"NaN"', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.decimal()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.decimal()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.decimal()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"123"', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12345678901234567890', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12345678901234567890"', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"+12.3"', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-12.3', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-12.3"', '$.decimal()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12.3', '$.decimal() * 2');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(6, 1)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(6, 2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(6, 2)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(4, 6)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(0, 6)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(1001, 6)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(+6, +2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(+6, -2)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(-6, +2)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(6, -1001)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(6, 1001)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-1234.5678', '$.decimal(+6, -2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('0.0123456', '$.decimal(1,2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('0.0012345', '$.decimal(2,4)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-0.00123456', '$.decimal(2,-4)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12.3', '$.decimal(12345678901,1)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12.3', '$.decimal(1,12345678901)');
-- An out-of-range precision or scale does not fail in silent mode.
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(0, 6)', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12345.678', '$.decimal(1001, 6)', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(-6, +2)', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(6, -1001)', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234.5678', '$.decimal(6, 1001)', silent => true);
select '1234.5678'::jsonb @? '$.decimal(0)';

-- Test .integer()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.integer()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.integer()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.integer()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.integer()', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1e1000', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"nan"', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"NaN"', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.integer()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.integer()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"123"', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.83', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('12345678901', '$.integer()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12345678901"', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"+123"', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-123', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-123"', '$.integer()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.integer() * 2');

-- Test .number()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.number()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.number()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.number()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.number()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1e1000', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"nan"', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"NaN"', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.number()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"inf"', '$.number()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-inf"', '$.number()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('123', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"123"', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12345678901234567890', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12345678901234567890"', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"+12.3"', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('-12.3', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"-12.3"', '$.number()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('12.3', '$.number() * 2');

-- Test .string()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.string()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.string()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.string()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.string()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.string()', silent => true);
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1.23', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1.23"', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1.23aaa"', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('1234', '$.string().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[2, true]', '$.string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_array('[1.23, "yes", false]', '$[*].string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_array('[1.23, "yes", false]', '$[*].string().type()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +5:30"', '$.timestamp().string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +5:30"', '$.timestamp().string()'); -- should work
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp_tz().string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56"', '$.timestamp_tz().string()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +5:30"', '$.timestamp_tz().string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp().string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +5:30"', '$.time_tz().string()');
-- this timetz usage will absorb the UTC offset of the current timezone setting
begin;
set local timezone = 'UTC-10';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"12:34:56"', '$.time_tz().string()');
rollback;
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.time().string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.date().string()');

-- .string() does not react to timezone or datestyle
begin;
set local timezone = 'UTC';
set local datestyle = 'German';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +5:30"', '$.timestamp_tz().string()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp().string()');
rollback;

-- Test .time()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.time()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.time()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.time()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.time()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.time()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.time()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"bogus"', '$.time()');

select jsonb '"12:34:56"' @? '$.time()';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.time()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.time().type()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.time()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:30"', '$.time()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"12:34:56 +05:30"', '$.time()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.time()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(-1)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(2.0)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(12345678901)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(5)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789"', '$.time(10)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789012"', '$.time(8)');

-- Test .time_tz()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.time_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"bogus"', '$.time_tz()');

select jsonb '"12:34:56 +05:30"' @? '$.time_tz()';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:30"', '$.time_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:30"', '$.time_tz().type()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.time_tz()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(-1)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(2.0)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(12345678901)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(5)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789 +05:30"', '$.time_tz(10)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56.789012 +05:30"', '$.time_tz(8)');

-- Test .timestamp()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.timestamp()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"bogus"', '$.timestamp()');

select jsonb '"2023-08-15 12:34:56"' @? '$.timestamp()';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp().type()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.timestamp()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:30"', '$.timestamp()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(-1)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(2.0)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(12345678901)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(5)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789"', '$.timestamp(10)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789012"', '$.timestamp(8)');

-- Test .timestamp_tz()
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('null', '$.timestamp_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('true', '$.timestamp_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('1', '$.timestamp_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('[]', '$.timestamp_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('[]', 'strict $.timestamp_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('{}', '$.timestamp_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"bogus"', '$.timestamp_tz()');

select jsonb '"2023-08-15 12:34:56 +05:30"' @? '$.timestamp_tz()';
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp_tz().type()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15"', '$.timestamp_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15"', '$.timestamp_tz()'); -- should work
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.timestamp_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56 +05:30"', '$.timestamp_tz()');

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(-1)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(2.0)');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(12345678901)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(0)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(2)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(5)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789 +05:30"', '$.timestamp_tz(10)');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56.789012 +05:30"', '$.timestamp_tz(8)');


set time zone '+00';

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.time()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.time()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.time_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"12:34:56"', '$.time_tz()'); -- should work
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.timestamp()'); -- should work
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56"', '$.timestamp_tz()'); -- should work

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '$.datetime("dd-mm-yyyy HH24:MI")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '$.datetime("dd-mm-yyyy HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 +05"', '$.datetime("dd-mm-yyyy HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 -05"', '$.datetime("dd-mm-yyyy HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 +05:20"', '$.datetime("dd-mm-yyyy HH24:MI TZH:TZM")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 -05:20"', '$.datetime("dd-mm-yyyy HH24:MI TZH:TZM")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34"', '$.datetime("HH24:MI")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34"', '$.datetime("HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 +05"', '$.datetime("HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 -05"', '$.datetime("HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 +05:20"', '$.datetime("HH24:MI TZH:TZM")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 -05:20"', '$.datetime("HH24:MI TZH:TZM")');

set time zone '+10';

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.time()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.time()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.timestamp()'); -- should work
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56"', '$.timestamp_tz()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56"', '$.timestamp_tz()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp_tz()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '$.datetime("dd-mm-yyyy HH24:MI")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34"', '$.datetime("dd-mm-yyyy HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 +05"', '$.datetime("dd-mm-yyyy HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 -05"', '$.datetime("dd-mm-yyyy HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 +05:20"', '$.datetime("dd-mm-yyyy HH24:MI TZH:TZM")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"10-03-2017 12:34 -05:20"', '$.datetime("dd-mm-yyyy HH24:MI TZH:TZM")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34"', '$.datetime("HH24:MI")');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"12:34"', '$.datetime("HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 +05"', '$.datetime("HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 -05"', '$.datetime("HH24:MI TZH")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 +05:20"', '$.datetime("HH24:MI TZH:TZM")');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34 -05:20"', '$.datetime("HH24:MI TZH:TZM")');

set time zone default;

--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.time()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.time()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.time_tz()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz('"2023-08-15 12:34:56 +05:30"', '$.timestamp()'); -- should work
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2023-08-15 12:34:56 +05:30"', '$.timestamp_tz()');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56+3"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56+3"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56+3:10"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56+3:10"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10T12:34:56+3:10"', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10t12:34:56+3:10"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10 12:34:56.789+3:10"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10T12:34:56.789+3:10"', '$.datetime()');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10t12:34:56.789+3:10"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10T12:34:56.789EST"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"2017-03-10T12:34:56.789Z"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56+3"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56+3"', '$.datetime()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56+3:10"', '$.datetime().type()');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"12:34:56+3:10"', '$.datetime()');

set time zone '+00';

-- date comparison
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "12:34:56", "01:02:03+04", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ == "10.03.2017".datetime("dd.mm.yyyy"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "12:34:56", "01:02:03+04", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ >= "10.03.2017".datetime("dd.mm.yyyy"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "12:34:56", "01:02:03+04", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ <  "10.03.2017".datetime("dd.mm.yyyy"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "12:34:56", "01:02:03+04", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ == "10.03.2017".datetime("dd.mm.yyyy"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "12:34:56", "01:02:03+04", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ >= "10.03.2017".datetime("dd.mm.yyyy"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "12:34:56", "01:02:03+04", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ <  "10.03.2017".datetime("dd.mm.yyyy"))');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ == "2017-03-10".date())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ >= "2017-03-10".date())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].datetime() ? (@ <  "2017-03-10".date())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].date() ? (@ == "2017-03-10".date())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].date() ? (@ >= "2017-03-10".date())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].date() ? (@ <  "2017-03-10".date())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].date() ? (@ == "2017-03-10".date())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].date() ? (@ >= "2017-03-10".date())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10", "2017-03-11", "2017-03-09", "2017-03-10 00:00:00", "2017-03-10 12:34:56", "2017-03-10 01:02:03+04", "2017-03-10 03:00:00+03"]',
--dg 	'$[*].date() ? (@ <  "2017-03-10".date())');

-- time comparison
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ == "12:35".datetime("HH24:MI"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ >= "12:35".datetime("HH24:MI"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ <  "12:35".datetime("HH24:MI"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ == "12:35".datetime("HH24:MI"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ >= "12:35".datetime("HH24:MI"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ <  "12:35".datetime("HH24:MI"))');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ == "12:35:00".time())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ >= "12:35:00".time())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].datetime() ? (@ <  "12:35:00".time())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].time() ? (@ == "12:35:00".time())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].time() ? (@ >= "12:35:00".time())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].time() ? (@ <  "12:35:00".time())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00.123", "12:35:00.123", "12:36:00.1123", "12:35:00.1123+00", "12:35:00.123+01", "13:35:00.123+01", "2017-03-10 12:35:00.1", "2017-03-10 12:35:00.123+01"]',
--dg 	'$[*].time(2) ? (@ >= "12:35:00.123".time(2))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].time() ? (@ == "12:35:00".time())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].time() ? (@ >= "12:35:00".time())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00", "12:35:00", "12:36:00", "12:35:00+00", "12:35:00+01", "13:35:00+01", "2017-03-10 12:35:00", "2017-03-10 12:35:00+01"]',
--dg 	'$[*].time() ? (@ <  "12:35:00".time())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00.123", "12:35:00.123", "12:36:00.1123", "12:35:00.1123+00", "12:35:00.123+01", "13:35:00.123+01", "2017-03-10 12:35:00.1", "2017-03-10 12:35:00.123+01"]',
--dg 	'$[*].time(2) ? (@ >= "12:35:00.123".time(2))');


-- timetz comparison
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ == "12:35 +1".datetime("HH24:MI TZH"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ >= "12:35 +1".datetime("HH24:MI TZH"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ <  "12:35 +1".datetime("HH24:MI TZH"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ == "12:35 +1".datetime("HH24:MI TZH"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ >= "12:35 +1".datetime("HH24:MI TZH"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10", "2017-03-10 12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ <  "12:35 +1".datetime("HH24:MI TZH"))');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ == "12:35:00 +1".time_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ >= "12:35:00 +1".time_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].datetime() ? (@ <  "12:35:00 +1".time_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].time_tz() ? (@ == "12:35:00 +1".time_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].time_tz() ? (@ >= "12:35:00 +1".time_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].time_tz() ? (@ <  "12:35:00 +1".time_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["12:34:00.123+01", "12:35:00.123+01", "12:36:00.1123+01", "12:35:00.1123+02", "12:35:00.123-02", "10:35:00.123", "11:35:00.1", "12:35:00.123", "2017-03-10 12:35:00.123 +1"]',
--dg 	'$[*].time_tz(2) ? (@ >= "12:35:00.123 +1".time_tz(2))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].time_tz() ? (@ == "12:35:00 +1".time_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].time_tz() ? (@ >= "12:35:00 +1".time_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00+01", "12:35:00+01", "12:36:00+01", "12:35:00+02", "12:35:00-02", "10:35:00", "11:35:00", "12:35:00", "2017-03-10 12:35:00 +1"]',
--dg 	'$[*].time_tz() ? (@ <  "12:35:00 +1".time_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["12:34:00.123+01", "12:35:00.123+01", "12:36:00.1123+01", "12:35:00.1123+02", "12:35:00.123-02", "10:35:00.123", "11:35:00.1", "12:35:00.123", "2017-03-10 12:35:00.123 +1"]',
--dg 	'$[*].time_tz(2) ? (@ >= "12:35:00.123 +1".time_tz(2))');

-- timestamp comparison
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ == "10.03.2017 12:35".datetime("dd.mm.yyyy HH24:MI"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ >= "10.03.2017 12:35".datetime("dd.mm.yyyy HH24:MI"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ < "10.03.2017 12:35".datetime("dd.mm.yyyy HH24:MI"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ == "10.03.2017 12:35".datetime("dd.mm.yyyy HH24:MI"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ >= "10.03.2017 12:35".datetime("dd.mm.yyyy HH24:MI"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ < "10.03.2017 12:35".datetime("dd.mm.yyyy HH24:MI"))');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].datetime() ? (@ == "2017-03-10 12:35:00".timestamp())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].datetime() ? (@ >= "2017-03-10 12:35:00".timestamp())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].datetime() ? (@ < "2017-03-10 12:35:00".timestamp())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp() ? (@ == "2017-03-10 12:35:00".timestamp())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp() ? (@ >= "2017-03-10 12:35:00".timestamp())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp() ? (@ < "2017-03-10 12:35:00".timestamp())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00.123", "2017-03-10 12:35:00.123", "2017-03-10 12:36:00.1123", "2017-03-10 12:35:00.1123+01", "2017-03-10 13:35:00.123+01", "2017-03-10 12:35:00.1-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp(2) ? (@ >= "2017-03-10 12:35:00.123".timestamp(2))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp() ? (@ == "2017-03-10 12:35:00".timestamp())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp() ? (@ >= "2017-03-10 12:35:00".timestamp())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00", "2017-03-10 12:35:00", "2017-03-10 12:36:00", "2017-03-10 12:35:00+01", "2017-03-10 13:35:00+01", "2017-03-10 12:35:00-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp() ? (@ < "2017-03-10 12:35:00".timestamp())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00.123", "2017-03-10 12:35:00.123", "2017-03-10 12:36:00.1123", "2017-03-10 12:35:00.1123+01", "2017-03-10 13:35:00.123+01", "2017-03-10 12:35:00.1-01", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp(2) ? (@ >= "2017-03-10 12:35:00.123".timestamp(2))');

-- timestamptz comparison
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ == "10.03.2017 12:35 +1".datetime("dd.mm.yyyy HH24:MI TZH"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ >= "10.03.2017 12:35 +1".datetime("dd.mm.yyyy HH24:MI TZH"))');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ < "10.03.2017 12:35 +1".datetime("dd.mm.yyyy HH24:MI TZH"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ == "10.03.2017 12:35 +1".datetime("dd.mm.yyyy HH24:MI TZH"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ >= "10.03.2017 12:35 +1".datetime("dd.mm.yyyy HH24:MI TZH"))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11", "12:34:56", "12:34:56+01"]',
--dg 	'$[*].datetime() ? (@ < "10.03.2017 12:35 +1".datetime("dd.mm.yyyy HH24:MI TZH"))');

--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].datetime() ? (@ == "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].datetime() ? (@ >= "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].datetime() ? (@ < "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz() ? (@ == "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz() ? (@ >= "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz() ? (@ < "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-differs: another error: function ?(...) does not exist
--dg select jsonb_path_query(
--dg 	'["2017-03-10 12:34:00.123+01", "2017-03-10 12:35:00.123+01", "2017-03-10 12:36:00.1123+01", "2017-03-10 12:35:00.1123+02", "2017-03-10 12:35:00.123-02", "2017-03-10 10:35:00.123", "2017-03-10 11:35:00.1", "2017-03-10 12:35:00.123", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz(2) ? (@ >= "2017-03-10 12:35:00.123 +1".timestamp_tz(2))');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz() ? (@ == "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz() ? (@ >= "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00+01", "2017-03-10 12:35:00+01", "2017-03-10 12:36:00+01", "2017-03-10 12:35:00+02", "2017-03-10 12:35:00-02", "2017-03-10 10:35:00", "2017-03-10 11:35:00", "2017-03-10 12:35:00", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz() ? (@ < "2017-03-10 12:35:00 +1".timestamp_tz())');
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query_tz(
--dg 	'["2017-03-10 12:34:00.123+01", "2017-03-10 12:35:00.123+01", "2017-03-10 12:36:00.1123+01", "2017-03-10 12:35:00.1123+02", "2017-03-10 12:35:00.123-02", "2017-03-10 10:35:00.123", "2017-03-10 11:35:00.1", "2017-03-10 12:35:00.123", "2017-03-10", "2017-03-11"]',
--dg 	'$[*].timestamp_tz(2) ? (@ >= "2017-03-10 12:35:00.123 +1".timestamp_tz(2))');


-- overflow during comparison
--dg-skip: function ?(...) does not exist
--dg select jsonb_path_query('"1000000-01-01"', '$.datetime() > "2020-01-01 12:00:00".datetime()'::jsonpath);

set time zone default;

-- jsonpath operators

--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query('[{"a": 1}, {"a": 2}]', '$[*]');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query('[{"a": 1}, {"a": 2}]', '$[*] ? (@.a > 10)');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_query('[{"a": 1}]', '$undefined_var');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query('[{"a": 1}]', 'false');

--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_query_array('[{"a": 1}, {"a": 2}, {}]', 'strict $[*].a');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_array('[{"a": 1}, {"a": 2}]', '$[*].a');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_array('[{"a": 1}, {"a": 2}]', '$[*].a ? (@ == 1)');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_array('[{"a": 1}, {"a": 2}]', '$[*].a ? (@ > 10)');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_array('[{"a": 1}, {"a": 2}, {"a": 3}, {"a": 5}]', '$[*].a ? (@ > $min && @ < $max)', vars => '{"min": 1, "max": 4}');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_array('[{"a": 1}, {"a": 2}, {"a": 3}, {"a": 5}]', '$[*].a ? (@ > $min && @ < $max)', vars => '{"min": 3, "max": 4}');

--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}, {}]', 'strict $[*].a');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}, {}]', 'strict $[*].a', silent => true);
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}]', '$[*].a');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}]', '$[*].a ? (@ == 1)');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}]', '$[*].a ? (@ > 10)');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}, {"a": 3}, {"a": 5}]', '$[*].a ? (@ > $min && @ < $max)', vars => '{"min": 1, "max": 4}');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}, {"a": 2}, {"a": 3}, {"a": 5}]', '$[*].a ? (@ > $min && @ < $max)', vars => '{"min": 3, "max": 4}');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}]', '$undefined_var');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_query_first('[{"a": 1}]', 'false');

SELECT jsonb '[{"a": 1}, {"a": 2}]' @? '$[*].a ? (@ > 1)';
SELECT jsonb '[{"a": 1}, {"a": 2}]' @? '$[*] ? (@.a > 2)';
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_exists('[{"a": 1}, {"a": 2}]', '$[*].a ? (@ > 1)');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_exists('[{"a": 1}, {"a": 2}, {"a": 3}, {"a": 5}]', '$[*] ? (@.a > $min && @.a < $max)', vars => '{"min": 1, "max": 4}');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_exists('[{"a": 1}, {"a": 2}, {"a": 3}, {"a": 5}]', '$[*] ? (@.a > $min && @.a < $max)', vars => '{"min": 3, "max": 4}');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_exists('[{"a": 1}]', '$undefined_var');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_exists('[{"a": 1}]', 'false');

--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('true', '$', silent => false);
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('false', '$', silent => false);
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('null', '$', silent => false);
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('1', '$', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('1', '$', silent => false);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('"a"', '$', silent => false);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('{}', '$', silent => false);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('[true]', '$', silent => false);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('{}', 'lax $.a', silent => false);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('{}', 'strict $.a', silent => false);
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('{}', 'strict $.a', silent => true);
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('[true, true]', '$[*]', silent => false);
SELECT jsonb '[{"a": 1}, {"a": 2}]' @@ '$[*].a > 1';
SELECT jsonb '[{"a": 1}, {"a": 2}]' @@ '$[*].a > 2';
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('[{"a": 1}, {"a": 2}]', '$[*].a > 1');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT jsonb_path_match('[{"a": 1}]', '$undefined_var');
--dg-skip: function ?(...) does not exist
--dg SELECT jsonb_path_match('[{"a": 1}]', 'false');

-- test string comparison (Unicode codepoint collation)
--dg-skip: function ?(...) does not exist
--dg WITH str(j, num) AS
--dg (
--dg 	SELECT jsonb_build_object('s', s), num
--dg 	FROM unnest('{"", "a", "ab", "abc", "abcd", "b", "A", "AB", "ABC", "ABc", "ABcD", "B"}'::text[]) WITH ORDINALITY AS a(s, num)
--dg )
--dg SELECT
--dg 	s1.j, s2.j,
--dg 	jsonb_path_query_first(s1.j, '$.s < $s', vars => s2.j) lt,
--dg 	jsonb_path_query_first(s1.j, '$.s <= $s', vars => s2.j) le,
--dg 	jsonb_path_query_first(s1.j, '$.s == $s', vars => s2.j) eq,
--dg 	jsonb_path_query_first(s1.j, '$.s >= $s', vars => s2.j) ge,
--dg 	jsonb_path_query_first(s1.j, '$.s > $s', vars => s2.j) gt
--dg FROM str s1, str s2
--dg ORDER BY s1.num, s2.num;
