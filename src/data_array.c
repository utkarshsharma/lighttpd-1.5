#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "array.h"

static void data_array_free(data_unset *d) {
	data_array *ds = (data_array *)d;
	
	buffer_free(ds->key);
	array_free(ds->value);
	
	free(d);
}

static void data_array_reset(data_unset *d) {
	data_array *ds = (data_array *)d;
	
	/* reused array elements */
	buffer_reset(ds->key);
	array_reset(ds->value);
}

static int data_array_insert_dup(data_unset *dst, data_unset *src) {
	UNUSED(dst);

	src->free(src);
	
	return 0;
}

static void data_array_print(data_unset *d) {
	data_array *ds = (data_array *)d;
	
	printf("{%s:\n", ds->key->ptr);
	array_print(ds->value);
	printf("}");
}


data_array *data_array_init(void) {
	data_array *ds;
	
	ds = calloc(1, sizeof(*ds));
	
	ds->key = buffer_init();
	ds->value = array_init();
	
	ds->free = data_array_free;
	ds->reset = data_array_reset;
	ds->insert_dup = data_array_insert_dup;
	ds->print = data_array_print;
	ds->type = TYPE_ARRAY;
	
	return ds;
}