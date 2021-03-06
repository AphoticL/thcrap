/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Additional JSON-related functions.
  */

#include "thcrap.h"

json_t* json_decref_safe(json_t *json)
{
	if(json && json->refcount != (size_t)-1 && --json->refcount == 0) {
        json_delete(json);
		return NULL;
	}
	return json;
}

size_t json_hex_value(json_t *val)
{
	const char *str = json_string_value(val);
	if(str) {
		return str_address_value(str, NULL);
	}
	return (size_t)json_integer_value(val);
}

int json_array_set_new_expand(json_t *arr, size_t ind, json_t *value)
{
	size_t arr_size = json_array_size(arr);
	if(ind >= arr_size) {
		int ret = 0;
		size_t i;
		for(i = arr_size; i <= ind; i++) {
			ret = json_array_append_new(arr, value);
		}
		return ret;
	} else {
		return json_array_set_new(arr, ind, value);
	}
}
int json_array_set_expand(json_t *arr, size_t ind, json_t *value)
{
	return json_array_set_new_expand(arr, ind, json_incref(value));
}

size_t json_array_get_hex(json_t *arr, const size_t ind)
{
	json_t *val = json_array_get(arr, ind);
	if(val) {
		size_t ret = json_hex_value(val);
		if(json_is_string(val)) {
			// Rewrite the JSON value
			json_array_set_new(arr, ind, json_integer(ret));
		}
		return ret;
	}
	return 0;
}

const char* json_array_get_string(const json_t *arr, const size_t ind)
{
	return json_string_value(json_array_get(arr, ind));
}

const char* json_array_get_string_safe(const json_t *arr, const size_t ind)
{
	const char *ret = json_array_get_string(arr, ind);
	if(!ret) {
		ret = "";
	}
	return ret;
}

json_t* json_array_from_wchar_array(int argc, const wchar_t *wargv[])
{
	json_t *ret = NULL;
	int i;

	if(!argc || !wargv) {
		return ret;
	}

	ret = json_array();
	for(i = 0; i < argc; i++) {
		const wchar_t *arg = wargv[i];
		UTF8_DEC(arg);
		UTF8_CONV(arg);
		json_array_append_new(ret, json_string(arg_utf8));
		UTF8_FREE(arg);
	}
	return ret;
}

size_t json_flex_array_size(const json_t *json)
{
	return json ? (json_is_array(json) ? json_array_size(json) : 1) : 0;
}

json_t *json_flex_array_get(json_t *flarr, size_t ind)
{
	return json_is_array(flarr) ? json_array_get(flarr, ind) : flarr;
}

const char* json_flex_array_get_string_safe(json_t *flarr, size_t ind)
{
	if(json_is_array(flarr)) {
		const char *ret = json_array_get_string(flarr, ind);
		return ret ? ret : "";
	} else if(json_is_string(flarr)) {
		return ind == 0 ? json_string_value(flarr) : "";
	}
	return NULL;
}

json_t* json_object_get_create(json_t *object, const char *key, json_type type)
{
	json_t *ret = json_object_get(object, key);
	if(!ret && object) {
		// This actually results in nicer assembly than using the ternary operator!
		json_t *new_obj = NULL;
		switch(type) {
			case JSON_OBJECT:
				new_obj = json_object();
				break;
			case JSON_ARRAY:
				new_obj = json_array();
				break;
		}
		json_object_set_new(object, key, new_obj);
		return new_obj;
	}
	return ret;
}

json_t* json_object_numkey_get(const json_t *object, const json_int_t key)
{
	char key_str[64];
	snprintf(key_str, 64, "%lld", key);
	return json_object_get(object, key_str);
}

size_t json_object_get_hex(json_t *object, const char *key)
{
	json_t *val = json_object_get(object, key);
	if(val) {
		size_t ret = json_hex_value(val);
		if(json_is_string(val)) {
			 // Rewrite the JSON value
			json_object_set_new_nocheck(object, key, json_integer(ret));
		}
		return ret;
	}
	return 0;
}

const char* json_object_get_string(const json_t *object, const char *key)
{
	if(!key) {
		return NULL;
	}
	return json_string_value(json_object_get(object, key));
}

json_t* json_object_merge(json_t *old_obj, json_t *new_obj)
{
	const char *key;
	json_t *new_val;

	if(!old_obj || !new_obj) {
		return old_obj;
	}
	if(!json_is_object(old_obj) || !json_is_object(new_obj)) {
		json_decref(old_obj);
		return json_incref(new_obj);
	}
	json_object_foreach(new_obj, key, new_val) {
		json_t *old_val = json_object_get(old_obj, key);
		if(json_is_object(old_val) && json_is_object(new_val)) {
			// Recursion!
			json_object_merge(old_val, new_val);
		} else {
			json_object_set_nocheck(old_obj, key, new_val);
		}
	}
	return old_obj;
}

static int __cdecl object_key_compare_keys(const void *key1, const void *key2)
{
	return strcmp(*(const char **)key1, *(const char **)key2);
}

json_t* json_object_get_keys_sorted(const json_t *object)
{
	json_t *ret = NULL;
	if(object) {
		size_t size = json_object_size(object);
		VLA(const char*, keys, size);
		size_t i;
		void *iter = json_object_iter((json_t *)object);

		if(!keys) {
			return NULL;
		}

		i = 0;
		while(iter) {
			keys[i] = json_object_iter_key(iter);
			iter = json_object_iter_next((json_t *)object, iter);
			i++;
		}

		qsort((void*)keys, size, sizeof(const char *), object_key_compare_keys);

		ret = json_array();
		for(i = 0; i < size; i++) {
			json_array_append_new(ret, json_string(keys[i]));
		}
		VLA_FREE(keys);
	}
	return ret;
}

json_t* json_loadb_report(const char *buffer, size_t buflen, size_t flags, const char *source)
{
	const unsigned char utf8_bom[] = {0xef, 0xbb, 0xbf};
	const size_t utf8_bom_len = sizeof(utf8_bom);
	json_error_t error;
	json_t *ret;

	if(!buffer || !buflen) {
		return NULL;
	}
	// Skip UTF-8 byte order mark, if there
	if(buflen > utf8_bom_len && !memcmp(buffer, utf8_bom, utf8_bom_len)) {
		buffer += utf8_bom_len;
		buflen -= utf8_bom_len;
	}
	ret = json_loadb(buffer, buflen, JSON_DISABLE_EOF_CHECK, &error);
	if(!ret) {
		log_mboxf(NULL, MB_OK | MB_ICONSTOP,
			"JSON parsing error:\n"
			"\n"
			"\t%s\n"
			"\n"
			"(%s%sline %d, column %d)",
			error.text, source ? source : "", source ? ", " : "", error.line, error.column
		);
	}
	return ret;
}

json_t* json_load_file_report(const char *json_fn)
{
	size_t json_size;
	char *json_buffer = (char*)file_read(json_fn, &json_size);
	json_t *json = json_loadb_report(json_buffer, json_size, JSON_DISABLE_EOF_CHECK, json_fn);
	SAFE_FREE(json_buffer);
	return json;
}

static int __cdecl dump_to_log(const char *buffer, size_t size, void *data)
{
	log_nprint(buffer, size);
	return 0;
}

int json_dump_log(const json_t *json, size_t flags)
{
	return json_dump_callback(json, dump_to_log, NULL, flags);
}
