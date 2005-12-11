/*
  +----------------------------------------------------------------------+
  | phar php single-file executable PHP extension                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2005 The PHP Group                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt.                                 |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Gregory Beaver <cellog@php.net>                              |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <time.h>
#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "ext/standard/crc32.h"
#include "zend_execute.h"
#include "zend_constants.h"
#include "php_phar.h"
#include "main/php_streams.h"
#ifndef TRUE
 #       define TRUE 1
 #       define FALSE 0
#endif

ZEND_DECLARE_MODULE_GLOBALS(phar)

typedef struct _internal_phar_stream_data {
	phar_file_data *data;
	long	pointer; /* relative position within file data */
	char	*file;
	phar_manifest_entry	*internal_file;
} phar_internal_file_data;

/* True global resources - no need for thread safety here */

/* borrowed from ext/standard/pack.c */
static int machine_little_endian;
static int little_endian_long_map[4];
/* end borrowing */

static zend_class_entry *php_archive_entry_ptr;

static void destroy_phar_data(void *pDest)
{
	phar_file_data *data = (phar_file_data *) pDest;
	efree(data->alias);
	zend_hash_destroy(data->manifest);
	FREE_HASHTABLE(data->manifest);
}

static void destroy_phar_manifest(void *pDest)
{
	phar_manifest_entry *data = (phar_manifest_entry *)pDest;
	efree(data->filename);
}

static phar_internal_file_data *phar_get_filedata(char *alias, char *path TSRMLS_DC)
{
	phar_file_data *data;
	phar_internal_file_data *ret;
	phar_manifest_entry *file_data;
	
	ret = NULL;
	if (SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_data), alias, strlen(alias), (void **) &data)) {
		if (SUCCESS == zend_hash_find(data->manifest, path, strlen(path), (void **) &file_data)) {
			ret = (phar_internal_file_data *) emalloc(sizeof(phar_internal_file_data));
			ret->data = data;
			ret->internal_file = file_data;
			ret->pointer = 0;
		}
	}
	return ret;
}

/* {{{ phar_functions[]
 *
 * Every user visible function must have an entry in phar_functions[].
 */
function_entry phar_functions[] = {
	{NULL, NULL, NULL}	/* Must be the last line in phar_functions[] */
};
/* }}} */

PHP_METHOD(PHP_Archive, canCompress)
{
#ifdef HAVE_PHAR_ZLIB
	RETURN_TRUE;
#else
	RETURN_FALSE;
#endif
}

/* {{{ php_archive_methods
 */
PHP_METHOD(PHP_Archive, mapPhar)
{
	char *fname, *alias, *buffer, *endbuffer, *unpack_var, *savebuf;
	phar_file_data mydata;
	zend_bool compressed;
	phar_manifest_entry entry;
	HashTable	*manifest;
	int alias_len, i;
	long halt_offset;
	php_uint32 manifest_len, manifest_count, manifest_index;
	zval *halt_constant, **unused1, **unused2;
	php_stream *fp;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "zsb|z", &unused1, &alias, &alias_len, &compressed, &unused2) == FAILURE) {
		return;
	}
#ifndef HAVE_PHAR_ZLIB
	if (compressed) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "zlib extension is required for compressed .phar files");
		return;
	}
#endif
	fname = zend_get_executed_filename(TSRMLS_C);
	if (!strcmp(fname, "[no active file]")) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "cannot initialize a phar outside of PHP execution");
		return;
	}

	MAKE_STD_ZVAL(halt_constant);
	if (0 == zend_get_constant("__COMPILER_HALT_OFFSET__", 24, halt_constant TSRMLS_CC)) {
		zval_dtor(halt_constant);
		FREE_ZVAL(halt_constant);
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "__HALT_COMPILER(); must be declared in a phar");
		return;
	}
	halt_offset = Z_LVAL(*halt_constant);
	zval_dtor(halt_constant);
	FREE_ZVAL(halt_constant);

	if (PG(safe_mode) && (!php_checkuid(fname, NULL, CHECKUID_ALLOW_ONLY_FILE))) {
		return;
	}

	if (php_check_open_basedir(fname TSRMLS_CC)) {
		return;
	}

	fp = php_stream_open_wrapper(fname, "rb", IGNORE_URL|STREAM_MUST_SEEK|REPORT_ERRORS, NULL);

	if (!fp) {
		php_error_docref(NULL TSRMLS_CC, E_ERROR, "unable to open phar for reading \"%s\"", fname);
		return;
	}
#define MAPPHAR_ALLOC_FAIL(msg) php_stream_close(fp);\
		php_stream_close(fp);\
		php_error_docref(NULL TSRMLS_CC, E_ERROR, msg, fname);\
		return;
#define MAPPHAR_FAIL(msg) efree(savebuf);\
		MAPPHAR_ALLOC_FAIL(msg)

	/* check for ?>\n and increment accordingly */
	if (-1 == php_stream_seek(fp, halt_offset, SEEK_SET)) {
		MAPPHAR_ALLOC_FAIL("cannot seek to __HALT_COMPILER(); location in phar \"%s\"")
	}

	if (FALSE == (buffer = (char *) emalloc(4))) {
		MAPPHAR_ALLOC_FAIL("memory allocation failed in phar \"%s\"")
	}
	savebuf = buffer;
	if (3 != php_stream_read(fp, buffer, 3)) {
		MAPPHAR_FAIL("internal corruption of phar \"%s\" (truncated manifest)")
	}
	if (*buffer == ' ' && *(buffer + 1) == '?' && *(buffer + 2) == '>') {
		int nextchar;
		halt_offset += 3;
		if (EOF == (nextchar = php_stream_getc(fp))) {
			MAPPHAR_FAIL("internal corruption of phar \"%s\" (truncated manifest)")
		}
		if ((char) nextchar == '\r') {
			if (EOF == (nextchar = php_stream_getc(fp))) {
				MAPPHAR_FAIL("internal corruption of phar \"%s\" (truncated manifest)")
			}
			halt_offset++;
		}
		if ((char) nextchar == '\n') {
			halt_offset++;
		}
	}
	/* make sure we are at the right location to read the manifest */
	if (-1 == php_stream_seek(fp, halt_offset, SEEK_SET)) {
		MAPPHAR_FAIL("cannot seek to __HALT_COMPILER(); location in phar \"%s\"")
	}

	/* read in manifest */

	i = 0;
#define PHAR_GET_VAL(var)			\
	if (buffer > endbuffer) {		\
		MAPPHAR_FAIL("internal corruption of phar \"%s\" (buffer overrun)")\
	}					\
	unpack_var = (char *) &var;		\
	var = 0;				\
	for (i = 0; i < 4; i++) {		\
		unpack_var[little_endian_long_map[i]] = *buffer++;\
		if (buffer > endbuffer) {	\
			MAPPHAR_FAIL("internal corruption of phar \"%s\" (buffer overrun)")\
		}				\
	}

	if (4 != php_stream_read(fp, buffer, 4)) {
		MAPPHAR_FAIL("internal corruption of phar \"%s\" (truncated manifest)")
	}
	endbuffer = buffer + 5;
	PHAR_GET_VAL(manifest_len)
	buffer -= 4;
	if (manifest_len > 1048576) {
		/* prevent serious memory issues by limiting manifest to at most 1 MB in length */
		MAPPHAR_FAIL("manifest cannot be larger than 1 MB in phar \"%s\"")
	}
	if (FALSE == (buffer = (char *) erealloc(buffer, manifest_len))) {
		MAPPHAR_FAIL("memory allocation failed in phar \"%s\"")
	}
	savebuf = buffer;
	/* set the test pointer */
	endbuffer = buffer + manifest_len;
	/* retrieve manifest */
	if (manifest_len != php_stream_read(fp, buffer, manifest_len)) {
		MAPPHAR_FAIL("internal corruption of phar \"%s\" (truncated manifest)")
	}
	/* extract the number of entries */
	PHAR_GET_VAL(manifest_count)
	/* we have 5 32-bit items at least */
	if (manifest_count > (manifest_len / (4 * 5))) {
		/* prevent serious memory issues */
		MAPPHAR_FAIL("too many manifest entries for size of manifest in phar \"%s\"")
	}
	/* set up our manifest */
	ALLOC_HASHTABLE(manifest);
	zend_hash_init(manifest, sizeof(phar_manifest_entry),
		zend_get_hash_value, destroy_phar_manifest, 0);
	for (manifest_index = 0; manifest_index < manifest_count; manifest_index++) {
		if (buffer > endbuffer) {
			MAPPHAR_FAIL("internal corruption of phar \"%s\" (truncated manifest)")
		}
		PHAR_GET_VAL(entry.filename_len)
		entry.filename = (char *) emalloc(entry.filename_len + 1);
		memcpy(entry.filename, buffer, entry.filename_len);
		entry.filename[entry.filename_len] = '\0';
		buffer += entry.filename_len;
		PHAR_GET_VAL(entry.uncompressed_filesize)
		PHAR_GET_VAL(entry.timestamp)
		PHAR_GET_VAL(entry.offset_within_phar)
		PHAR_GET_VAL(entry.compressed_filesize)
		entry.crc_checked = 0;
		if (entry.compressed_filesize < 9) {
			MAPPHAR_FAIL("internal corruption of phar \"%s\" (file size in phar is not large enough)")
		}
		zend_hash_add(manifest, entry.filename, entry.filename_len, &entry,
			sizeof(phar_manifest_entry), NULL);
	}
#undef PHAR_GET_VAL

	mydata.file = fname;
	/* alias is auto-efreed after returning, so we must dupe it */
	mydata.alias = estrndup(alias, alias_len);
	mydata.alias_len = alias_len;
	mydata.internal_file_start = manifest_len + halt_offset + 4;
	mydata.is_compressed = compressed;
	mydata.manifest = manifest;
	zend_hash_add(&(PHAR_GLOBALS->phar_data), alias, alias_len, &mydata,
		sizeof(phar_file_data), NULL);
	efree(savebuf);
	php_stream_close(fp);
}

PHP_METHOD(PHP_Archive, apiVersion)
{
	RETURN_STRING("0.7", 3);
}

zend_function_entry php_archive_methods[] = {
	PHP_ME(PHP_Archive, mapPhar, NULL, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(PHP_Archive, apiVersion, NULL, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	PHP_ME(PHP_Archive, canCompress, NULL, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC|ZEND_ACC_FINAL)
	{NULL, NULL, NULL}
};
/* }}} */


/* {{{ phar_module_entry
 */
zend_module_entry phar_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"phar",
	phar_functions,
	PHP_MINIT(phar),
	PHP_MSHUTDOWN(phar),
	PHP_RINIT(phar),
	PHP_RSHUTDOWN(phar),
	PHP_MINFO(phar),
#if ZEND_MODULE_API_NO >= 20010901
	"0.1.0", /* Replace with version number for your extension */
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_PHAR
ZEND_GET_MODULE(phar)
#endif

/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("phar.global_value",      "42", PHP_INI_ALL, OnUpdateLong, global_value, zend_phar_globals, phar_globals)
    STD_PHP_INI_ENTRY("phar.global_string", "foobar", PHP_INI_ALL, OnUpdateString, global_string, zend_phar_globals, phar_globals)
PHP_INI_END()
*/
/* }}} */

/* {{{ php_phar_init_globals
 */
static void php_phar_init_globals_module(zend_phar_globals *phar_globals)
{
	memset(phar_globals, 0, sizeof(zend_phar_globals));
}

/* }}} */

PHP_PHAR_API php_stream *php_stream_phar_url_wrapper(php_stream_wrapper *wrapper, char *path, char *mode, int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC);
PHP_PHAR_API int phar_close(php_stream *stream, int close_handle TSRMLS_DC);
PHP_PHAR_API int phar_closedir(php_stream *stream, int close_handle TSRMLS_DC);
PHP_PHAR_API size_t phar_read(php_stream *stream, char *buf, size_t count TSRMLS_DC);
PHP_PHAR_API size_t phar_readdir(php_stream *stream, char *buf, size_t count TSRMLS_DC);
PHP_PHAR_API int phar_seek(php_stream *stream, off_t offset, int whence, off_t *newoffset TSRMLS_DC);

PHP_PHAR_API size_t phar_write(php_stream *stream, const char *buf, size_t count TSRMLS_DC);
PHP_PHAR_API int phar_flush(php_stream *stream TSRMLS_DC);
PHP_PHAR_API int phar_stat(php_stream *stream, php_stream_statbuf *ssb TSRMLS_DC);

PHP_PHAR_API int phar_stream_stat(php_stream_wrapper *wrapper, char *url, int flags, php_stream_statbuf *ssb, php_stream_context *context TSRMLS_DC);
PHP_PHAR_API php_stream *phar_opendir(php_stream_wrapper *wrapper, char *filename, char *mode,
			int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC);

static php_stream_ops phar_ops = {
	phar_write, /* write (does nothing) */
	phar_read, /* read */
	phar_close, /* close */
	phar_flush, /* flush (does nothing) */
	"phar stream",
	NULL, /* seek */
	NULL, /* cast */
	phar_stat, /* stat */
	NULL, /* set option */
};

static php_stream_ops phar_dir_ops = {
	phar_write, /* write (does nothing) */
	phar_readdir, /* read */
	phar_closedir, /* close */
	phar_flush, /* flush (does nothing) */
	"phar stream",
	NULL, /* seek */
	NULL, /* cast */
	NULL, /* stat */
	NULL, /* set option */
};

static php_stream_wrapper_ops phar_stream_wops = {
    php_stream_phar_url_wrapper,
    NULL, /* stream_close */
    NULL, /* php_stream_phar_stat, */
    phar_stream_stat, /* stat_url */
    phar_opendir, /* opendir */
    "phar",
    NULL, /* unlink */
    NULL, /* rename */
    NULL, /* create directory */
    NULL /* remove directory */
};

php_stream_wrapper php_stream_phar_wrapper =  {
    &phar_stream_wops,
    NULL,
    0 /* is_url */
};

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(phar)
{
	zend_class_entry php_archive_entry;
	int machine_endian_check = 1;
	ZEND_INIT_MODULE_GLOBALS(phar, php_phar_init_globals_module, NULL);

	machine_little_endian = ((char *)&machine_endian_check)[0];

	if (machine_little_endian) {
		little_endian_long_map[0] = 0;
		little_endian_long_map[1] = 1;
		little_endian_long_map[2] = 2;
		little_endian_long_map[3] = 3;
	}
	else {
		zval val;
		int size = sizeof(Z_LVAL(val));
		Z_LVAL(val)=0; /*silence a warning*/

		little_endian_long_map[0] = size - 1;
		little_endian_long_map[1] = size - 2;
		little_endian_long_map[2] = size - 3;
		little_endian_long_map[3] = size - 4;
	}
	INIT_CLASS_ENTRY(php_archive_entry, "PHP_Archive", php_archive_methods);
	php_archive_entry_ptr = zend_register_internal_class(&php_archive_entry TSRMLS_CC);
	return php_register_url_stream_wrapper("phar", &php_stream_phar_wrapper TSRMLS_CC);
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(phar)
{
	/* uncomment this line if you have INI entries
	UNREGISTER_INI_ENTRIES();
	*/
	return php_unregister_url_stream_wrapper("phar" TSRMLS_CC);
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(phar)
{
	zend_hash_init(&(PHAR_GLOBALS->phar_data), sizeof(phar_file_data),
		 zend_get_hash_value, destroy_phar_data, 0);
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(phar)
{
	zend_hash_destroy(&(PHAR_GLOBALS->phar_data));
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(phar)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "phar PHP Archive support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/*
*/
static int phar_postprocess_file(char *contents, php_uint32 nr, unsigned long crc32, zend_bool read)
{
	unsigned int crc = ~0;
	php_uint32 i, actual_length;
	char *unpack_var;
	int len = 0;
	if (read) {
	#define PHAR_GET_VAL(var)			\
		unpack_var = (char *) &var;		\
		var = 0;				\
		for (i = 0; i < 4; i++) {		\
			unpack_var[little_endian_long_map[i]] = *contents++;\
		}
		PHAR_GET_VAL(crc32)
		PHAR_GET_VAL(actual_length)
		if (actual_length != nr) {
			return -2;
		}
	}


	for (len += nr; nr--; ++contents) {
	    CRC32(crc, *contents);
	}
	if (~crc == crc32) {
		return 0;
	} else {
		return -1;
	}
#undef PHAR_GET_VAL	
}

/* {{{ php_stream_phar_url_wrapper
 */
PHP_PHAR_API php_stream * php_stream_phar_url_wrapper(php_stream_wrapper *wrapper, char *path, char *mode, int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC)
{
	phar_internal_file_data *idata;
	php_stream *stream = NULL;
	char *internal_file;
	char *buffer;
	php_url *resource = NULL;
	php_stream *fp;
	int status;
#ifdef HAVE_PHAR_ZLIB
	unsigned long crc32;
	php_uint32 actual_length, i;
	char *unpack_var, *savebuf;
	/* borrowed from zlib.c gzinflate() function */
	php_uint32 offset;
	unsigned long length;
	char *s1=NULL;
	z_stream zstream;
#endif

	resource = php_url_parse(path);
	/* we must have at the very least phar://alias.phar/internalfile.php */
	if (!resource || !resource->scheme || !resource->host || !resource->path) {
		if (resource) {
			php_url_free(resource);
		}
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: invalid url \"%s\"", path);
		return NULL;
	}

	if (strcasecmp("phar", resource->scheme)) {
		if (resource) {
			php_url_free(resource);
		}
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: not a phar stream url \"%s\"", path);
		return NULL;
	}

	/* strip leading "/" */
	internal_file = estrndup(resource->path + 1, strlen(resource->path) - 1);
	if (NULL == (idata = phar_get_filedata(resource->host, internal_file TSRMLS_CC))) {
		efree(internal_file);
		return NULL;
	}

	php_url_free(resource);

	if (PG(safe_mode) && (!php_checkuid(idata->data->file, NULL, CHECKUID_ALLOW_ONLY_FILE))) {
		efree(internal_file);
		return NULL;
	}

	if (php_check_open_basedir(idata->data->file TSRMLS_CC)) {
		efree(internal_file);
		return NULL;
	}

	fp = php_stream_open_wrapper(idata->data->file, "rb", IGNORE_URL|STREAM_MUST_SEEK|REPORT_ERRORS, NULL);

	if (!fp) {
		buffer = idata->data->file;
		efree(idata);
		efree(internal_file);
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: cannot open phar \"%s\"", buffer);
		return NULL;
	}

	/* seek to start of internal file and read it */
	if (-1 == php_stream_seek(fp, idata->data->internal_file_start + idata->internal_file->offset_within_phar, SEEK_SET)) {
		php_stream_close(fp);
		buffer = idata->data->file;
		offset = idata->data->internal_file_start + idata->internal_file->offset_within_phar;
		efree(idata);
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (cannot seek to start of file \"%s\" at offset \"%d\")",
			buffer, internal_file,
			offset);
		efree(internal_file);
		return NULL;
	}
	if (idata->data->is_compressed) {
#ifdef HAVE_PHAR_ZLIB
		buffer = (char *) emalloc(idata->internal_file->compressed_filesize);
		if (idata->internal_file->compressed_filesize !=
				php_stream_read(fp, buffer, idata->internal_file->compressed_filesize)) {
			php_stream_close(fp);
			buffer = idata->data->file;
			efree(idata);
			php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (filesize mismatch on file \"%s\")", buffer, internal_file);
			efree(internal_file);
			return NULL;
		}
		php_stream_close(fp);
		savebuf = buffer;
		#define PHAR_GET_VAL(var)			\
			unpack_var = (char *) &var;		\
			var = 0;				\
			for (i = 0; i < 4; i++) {		\
				unpack_var[little_endian_long_map[i]] = *buffer++;\
			}
		PHAR_GET_VAL(crc32)
		PHAR_GET_VAL(actual_length)
		#undef PHAR_GET_VAL

		/* borrowed from zlib.c gzinflate() function */
		zstream.zalloc = (alloc_func) Z_NULL;
		zstream.zfree = (free_func) Z_NULL;

		length = idata->internal_file->uncompressed_filesize;
		do {
			idata->file = (char *) erealloc(s1, length);

			if (!idata->file && s1) {
				efree(s1);
				efree(savebuf);
				buffer = idata->data->file;
				efree(idata->file);
				efree(idata);
				php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (corrupted zlib compression of file \"%s\")", buffer, internal_file);
				efree(internal_file);
				return NULL;
			}

			zstream.next_in = (Bytef *) buffer;
			zstream.avail_in = (uInt) idata->internal_file->compressed_filesize;

			zstream.next_out = idata->file;
			zstream.avail_out = (uInt) length;

			/* init with -MAX_WBITS disables the zlib internal headers */
			status = inflateInit2(&zstream, -MAX_WBITS);
			if (status == Z_OK) {
				status = inflate(&zstream, Z_FINISH);
				if (status != Z_STREAM_END) {
					inflateEnd(&zstream);
					if (status == Z_OK) {
						status = Z_BUF_ERROR;
					}
				} else {
					status = inflateEnd(&zstream);
				}
			}
			s1 = idata->file;
			
		} while (status == Z_BUF_ERROR);

		if (status != Z_OK) {
			efree(savebuf);
			efree(idata->file);
			efree(idata);
			efree(internal_file);
			php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: %s", zError(status));
			return NULL;
		}
#define PHAR_ZLIB_ERROR efree(savebuf);\
			buffer = idata->data->file;\
			efree(idata->file);\
			efree(idata);\
			return NULL;

		efree(savebuf);
		/* check length */
		if (actual_length != idata->internal_file->uncompressed_filesize) {
			PHAR_ZLIB_ERROR
			php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (filesize mismatch on file \"%s\")", buffer, internal_file);
			efree(internal_file);
			return NULL;
		}
		/* check crc32/filesize */
		if (!idata->internal_file->crc_checked) {
			status = phar_postprocess_file(idata->file, idata->internal_file->uncompressed_filesize, crc32, 0);
			if (-1 == status) {
				PHAR_ZLIB_ERROR
				php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (crc32 mismatch on file \"%s\")", buffer, internal_file);
				efree(internal_file);
				return NULL;
			}
			if (-2 == status) {
				PHAR_ZLIB_ERROR
				php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (filesize mismatch on file \"%s\")", buffer, internal_file);
				efree(internal_file);
				return NULL;
			}
			idata->internal_file->crc_checked = 1;
		}
#else
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "zlib extension must be enabled for compressed .phar files");
		efree(internal_file);
		return NULL;
#endif
	} else {
		idata->file = (char *) emalloc(idata->internal_file->compressed_filesize);
		if (idata->internal_file->compressed_filesize !=
				php_stream_read(fp, idata->file, idata->internal_file->compressed_filesize)) {
			php_stream_close(fp);
			efree(idata->file);
			buffer = idata->data->file;
			efree(idata);
			php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (filesize mismatch on file \"%s\")", buffer, internal_file);
			efree(internal_file);
			return NULL;
		}
		php_stream_close(fp);
		/* check length, crc32 */
		if (!idata->internal_file->crc_checked) {
			status = phar_postprocess_file(idata->file, idata->internal_file->uncompressed_filesize, 0, 1);
			if (-1 == status) {
				efree(idata->file);
				buffer = idata->data->file;
				efree(idata);
				php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (crc32 mismatch on file \"%s\")", buffer, internal_file);
				efree(internal_file);
				return NULL;
			}
			if (-2 == status) {
				efree(idata->file);
				buffer = idata->data->file;
				efree(idata);
				php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: internal corruption of phar \"%s\" (filesize mismatch on file \"%s\")", buffer, internal_file);
				efree(internal_file);
				return NULL;
			}
			idata->internal_file->crc_checked = 1;
		}
		memmove(idata->file, idata->file + 8, idata->internal_file->uncompressed_filesize);
	}

	stream = php_stream_alloc(&phar_ops, idata, NULL, mode);
	efree(internal_file);
	return stream;
}
/* }}} */

PHP_PHAR_API int phar_close(php_stream *stream, int close_handle TSRMLS_DC)
{
	phar_internal_file_data *data = (phar_internal_file_data *)stream->abstract;

	efree(data->file);
	efree(data);
	return 0;
}

PHP_PHAR_API int phar_closedir(php_stream *stream, int close_handle TSRMLS_DC)
{
	HashTable *data = (HashTable *)stream->abstract;

	zend_hash_destroy(data);
	FREE_HASHTABLE(data);
	return 0;
}

PHP_PHAR_API size_t phar_read(php_stream *stream, char *buf, size_t count TSRMLS_DC)
{
	size_t to_read;
	phar_internal_file_data *data = (phar_internal_file_data *)stream->abstract;

	to_read = MIN(data->internal_file->uncompressed_filesize - data->pointer, count);
	if (to_read == 0) {
		return 0;
	}

	memcpy(buf, data->file + data->pointer, to_read);
	data->pointer += to_read;
	return to_read;
}

PHP_PHAR_API size_t phar_readdir(php_stream *stream, char *buf, size_t count TSRMLS_DC)
{
	size_t to_read;
	HashTable *data = (HashTable *)stream->abstract;
	char *key;
	uint keylen;
	ulong unused;

	if (FAILURE == zend_hash_has_more_elements(data)) {
		return 0;
	}
	if (HASH_KEY_NON_EXISTANT == zend_hash_get_current_key_ex(data, &key, &keylen, &unused, 0, NULL)) {
		return 0;
	}
	zend_hash_move_forward(data);
	to_read = MIN(keylen, count);
	if (to_read == 0 || count < keylen) {
		return 0;
	}
	memset(buf, 0, sizeof(php_stream_dirent));
	memcpy(((php_stream_dirent *) buf)->d_name, key, to_read);
	((php_stream_dirent *) buf)->d_name[to_read + 1] = '\0';

	return sizeof(php_stream_dirent);
}

PHP_PHAR_API int phar_seek(php_stream *stream, off_t offset, int whence, off_t *newoffset TSRMLS_DC)
{
	phar_internal_file_data *data = (phar_internal_file_data *)stream->abstract;
	switch (whence) {
		case SEEK_SET :
			if (offset < 0 || offset > data->internal_file->uncompressed_filesize) {
				*newoffset = (off_t) - 1;
				return -1;
			}
			data->pointer = offset;
			*newoffset = offset;
			return 0;
		case SEEK_CUR :
			if (data->pointer + offset < 0 || data->pointer + offset
					> data->internal_file->uncompressed_filesize) {
				*newoffset = (off_t) - 1;
				return -1;
			}
			data->pointer += offset;
			*newoffset = data->pointer;
			return 0;
		case SEEK_END :
			if (offset > 0 || -1 * offset > data->internal_file->uncompressed_filesize) {
				*newoffset = (off_t) - 1;
				return -1;
			}
			data->pointer = data->internal_file->uncompressed_filesize + offset;
			*newoffset = data->pointer;
			return 0;
		default :
			*newoffset = (off_t) - 1;
			return -1;
	}
}

PHP_PHAR_API size_t phar_write(php_stream *stream, const char *buf, size_t count TSRMLS_DC)
{
	return 0;
}

PHP_PHAR_API int phar_flush(php_stream *stream TSRMLS_DC)
{
	return EOF;
}

static void phar_dostat(phar_manifest_entry *data, php_stream_statbuf *ssb, zend_bool is_dir TSRMLS_DC);

PHP_PHAR_API int phar_stat(php_stream *stream, php_stream_statbuf *ssb TSRMLS_DC)
{
	phar_internal_file_data *data;
	/* If ssb is NULL then someone is misbehaving */
	if (!ssb) return -1;

	data = (phar_internal_file_data *)stream->abstract;
	phar_dostat(data->internal_file, ssb, 0 TSRMLS_CC);
	return 0;
}

static void phar_dostat(phar_manifest_entry *data, php_stream_statbuf *ssb, zend_bool is_dir TSRMLS_DC)
{
	memset(ssb, 0, sizeof(php_stream_statbuf));
	ssb->sb.st_mode = 0444;

	if (!is_dir) {
		ssb->sb.st_size = data->uncompressed_filesize;
		ssb->sb.st_mode |= S_IFREG;
#ifdef NETWARE
		ssb->sb.st_mtime.tv_sec = data->timestamp;
		ssb->sb.st_atime.tv_sec = data->timestamp;
		ssb->sb.st_ctime.tv_sec = data->timestamp;
#else
		ssb->sb.st_mtime = data->timestamp;
		ssb->sb.st_atime = data->timestamp;
		ssb->sb.st_ctime = data->timestamp;
#endif
	} else {
		ssb->sb.st_size = 0;
		ssb->sb.st_mode |= S_IFDIR;
#ifdef NETWARE
		ssb->sb.st_mtime.tv_sec = 0;
		ssb->sb.st_atime.tv_sec = 0;
		ssb->sb.st_ctime.tv_sec = 0;
#else
		ssb->sb.st_mtime = 0;
		ssb->sb.st_atime = 0;
		ssb->sb.st_ctime = 0;
#endif
	}


	ssb->sb.st_nlink = 1;
	ssb->sb.st_rdev = -1;
#ifndef PHP_WIN32
	ssb->sb.st_blksize = -1;
	ssb->sb.st_blocks = -1;
#endif
}

PHP_PHAR_API int phar_stream_stat(php_stream_wrapper *wrapper, char *url, int flags,
				  php_stream_statbuf *ssb, php_stream_context *context TSRMLS_DC)

{
	php_url *resource = NULL;
	char *internal_file, *key;
	uint keylen;
	ulong unused;
	phar_file_data *data;
	phar_manifest_entry *file_data;

	resource = php_url_parse(url);
	/* we must have at the very least phar://alias.phar/internalfile.php */
	if (!resource || !resource->scheme || !resource->host || !resource->path) {
		php_url_free(resource);
		php_stream_wrapper_log_error(wrapper, flags TSRMLS_CC, "phar error: invalid url \"%s\"", url);
		php_url_free(resource);
		return -1;
	}

	if (strcasecmp("phar", resource->scheme)) {
		php_url_free(resource);
		php_stream_wrapper_log_error(wrapper, flags TSRMLS_CC, "phar error: not a phar url \"%s\"", url);
		return -1;
	}

	internal_file = resource->path + 1; /* strip leading "/" */
	if (SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_data), resource->host, strlen(resource->host), (void **) &data)) {
		if (*internal_file == '\0') {
			/* root directory requested */
			phar_dostat(NULL, ssb, 1 TSRMLS_CC);
			php_url_free(resource);
			return 0;
		}
		if (SUCCESS == zend_hash_find(data->manifest, internal_file, strlen(internal_file), (void **) &file_data)) {
			phar_dostat(file_data, ssb, 0 TSRMLS_CC);
		} else {
			/* search for directory */
			zend_hash_internal_pointer_reset(data->manifest);
			while (zend_hash_has_more_elements(data->manifest)) {
				if (HASH_KEY_NON_EXISTANT !=
						zend_hash_get_current_key_ex(
							data->manifest, &key, &keylen, &unused, 0, NULL)) {
					if (0 == memcmp(key, internal_file, keylen)) {
						/* directory found */
						phar_dostat(NULL, ssb, 1 TSRMLS_CC);
						break;
					}
				}
				zend_hash_move_forward(data->manifest);
			}
		}
	}

	php_url_free(resource);
	return 0;
}

static int phar_add_empty(HashTable *ht, char *arKey, uint nKeyLength)
{
	void *dummy = (void *) 1;

	return zend_hash_update(ht, arKey, nKeyLength, &dummy, sizeof(void *), NULL);
}

static php_stream *phar_make_dirstream(char *dir, HashTable *manifest TSRMLS_DC)
{
	HashTable *data;
	int dirlen = strlen(dir);
	char *save, *found, *key;
	uint keylen;
	ulong unused;
	char *entry;
	ALLOC_HASHTABLE(data);
	zend_hash_init(data, 64, zend_get_hash_value, NULL, 0);

	zend_hash_internal_pointer_reset(manifest);
	while (SUCCESS == zend_hash_has_more_elements(manifest)) {
		if (HASH_KEY_NON_EXISTANT == zend_hash_get_current_key_ex(manifest, &key, &keylen, &unused, 0, NULL)) {
			return NULL;
		}
		if (*dir == '/') {
			/* not root directory */
			if (NULL != (found = (char *) memchr(key, '/', keylen))) {
				/* the entry has a path separator and is a subdirectory */
				save = key;
				goto PHAR_DIR_SUBDIR;
			}
			dirlen = 0;
		} else {
			if (0 != memcmp(key, dir, dirlen)) {
				/* entry in directory not found */
				zend_hash_move_forward(manifest);
				continue;
			}
		}
		save = key;
		save += dirlen + 1; /* seek to just past the path separator */
		if (NULL != (found = (char *) memchr(save, '/', keylen - dirlen - 1))) {
			/* is subdirectory */
			save -= dirlen + 1;
PHAR_DIR_SUBDIR:
			entry = (char *) emalloc (found - save + 2);
			memcpy(entry, save, found - save);
			keylen = found - save;
			entry[found - save + 1] = '\0';
		} else {
			/* is file */
			save -= dirlen + 1;
			entry = (char *) emalloc (keylen - dirlen + 1);
			memcpy(entry, save, keylen - dirlen);
			entry[keylen - dirlen] = '\0';
			keylen = keylen - dirlen;
		}
		phar_add_empty(data, entry, keylen);
		efree(entry);
		zend_hash_move_forward(manifest);
	}
	return php_stream_alloc(&phar_dir_ops, data, NULL, "r");
}

PHP_PHAR_API php_stream *phar_opendir(php_stream_wrapper *wrapper, char *filename, char *mode,
			int options, char **opened_path, php_stream_context *context STREAMS_DC TSRMLS_DC)
{
	php_url *resource = NULL;
	php_stream *ret;
	char *internal_file, *key;
	uint keylen;
	ulong unused;
	phar_file_data *data;
	phar_manifest_entry *file_data;

	resource = php_url_parse(filename);
	/* we must have at the very least phar://alias.phar/internalfile.php */
	if (!resource || !resource->scheme || !resource->host || !resource->path) {
		php_url_free(resource);
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: invalid url \"%s\"", filename);
		return NULL;
	}

	if (strcasecmp("phar", resource->scheme)) {
		php_url_free(resource);
		php_stream_wrapper_log_error(wrapper, options TSRMLS_CC, "phar error: not a phar url \"%s\"", filename);
		return NULL;
	}

	internal_file = resource->path + 1; /* strip leading "/" */
	if (SUCCESS == zend_hash_find(&(PHAR_GLOBALS->phar_data), resource->host, strlen(resource->host), (void **) &data)) {
		if (*internal_file == '\0') {
			/* root directory requested */
			ret = phar_make_dirstream("/", data->manifest TSRMLS_CC);
			php_url_free(resource);
			return ret;
		}
		if (SUCCESS == zend_hash_find(data->manifest, internal_file, strlen(internal_file), (void **) &file_data)) {
			php_url_free(resource);
			return NULL;
		} else {
			/* search for directory */
			zend_hash_internal_pointer_reset(data->manifest);
			while (zend_hash_has_more_elements(data->manifest)) {
				if (HASH_KEY_NON_EXISTANT != 
						zend_hash_get_current_key_ex(
							data->manifest, &key, &keylen, &unused, 0, NULL)) {
					if (0 == memcmp(key, internal_file, keylen)) {
						/* directory found */
						php_url_free(resource);
						return phar_make_dirstream(internal_file, data->manifest TSRMLS_CC);
					}
				}
				zend_hash_move_forward(data->manifest);
			}
		}
	}

	php_url_free(resource);
	return NULL;
}
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
