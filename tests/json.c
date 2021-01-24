#include <jansson.h>
#include <stdio.h>
#include <unistd.h>

int main() {
	json_t *val, *obj;
	char foo[] = { 'f', 'o', 'o', 0x12, 0x32, 0x62, 0x0, 'm' , 0x0};
	val = json_stringn(foo, sizeof(foo));
	obj = json_object();
	json_object_set_new(obj, "str", val);
	char *s = json_dumps(obj, JSON_INDENT(2));
	json_decref(obj);
	printf("%s\n", s);

	obj = json_loads(s, JSON_ALLOW_NUL, NULL);
	free(s);
	val = json_object_get(obj, "str");
	if (!val) {
		printf("key not found\n");
		return 1;
	}
	printf("%zd\n", json_string_length(val));
	fflush(stdout);
	write(1, json_string_value(val), json_string_length(val));
	printf("\n");
	json_decref(obj);

	return 0;
}
