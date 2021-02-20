
#include <stdio.h>

#include "lustre.h"

int parse_hai_cb(struct hsm_action_item *hai, void *arg) {
	(void)arg;
	(void)hai;
	return 0;
}

int main() {
	int rc;

	rc = parse_active_requests(0, parse_hai_cb, NULL);
	printf("got %d items\n", rc);

	return 0;
}
