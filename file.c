#include "postgres.h"

#include <unistd.h>

#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "port.h"
#include "utils/memutils.h"
#include "orafunc.h"

#define INVALID_OPERATION		"UTL_FILE_INVALID_OPERATION"
#define WRITE_ERROR				"UTL_FILE_WRITE_ERROR"
#define READ_ERROR				"UTL_FILE_READ_ERROR"
#define INVALID_FILEHANDLE		"UTL_FILE_INVALID_FILEHANDLE"
#define INVALID_MAXLINESIZE		"UTL_FILE_INVALID_MAXLINESIZE"
#define INVALID_MODE			"UTL_FILE_INVALID_MODE"
#define	INVALID_PATH			"UTL_FILE_INVALID_PATH"
#define VALUE_ERROR				"UTL_FILE_VALUE_ERROR"


Datum utl_file_fopen(PG_FUNCTION_ARGS);
Datum utl_file_is_open(PG_FUNCTION_ARGS);
Datum utl_file_get_line(PG_FUNCTION_ARGS);
Datum utl_file_get_nextline(PG_FUNCTION_ARGS);
Datum utl_file_put(PG_FUNCTION_ARGS);
Datum utl_file_put_line(PG_FUNCTION_ARGS);
Datum utl_file_new_line(PG_FUNCTION_ARGS);
Datum utl_file_putf(PG_FUNCTION_ARGS);
Datum utl_file_fflush(PG_FUNCTION_ARGS);
Datum utl_file_fclose(PG_FUNCTION_ARGS);
Datum utl_file_fclose_all(PG_FUNCTION_ARGS);
Datum utl_file_fremove(PG_FUNCTION_ARGS);
Datum utl_file_frename(PG_FUNCTION_ARGS);
/*
Datum utl_file_fcopy(PG_FUNCTION_ARGS);
*/
Datum utl_file_fgetattr(PG_FUNCTION_ARGS);
Datum utl_file_tmpdir(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(utl_file_fopen);
PG_FUNCTION_INFO_V1(utl_file_is_open);
PG_FUNCTION_INFO_V1(utl_file_get_line);
PG_FUNCTION_INFO_V1(utl_file_get_nextline);
PG_FUNCTION_INFO_V1(utl_file_put);
PG_FUNCTION_INFO_V1(utl_file_put_line);
PG_FUNCTION_INFO_V1(utl_file_new_line);
PG_FUNCTION_INFO_V1(utl_file_putf);
PG_FUNCTION_INFO_V1(utl_file_fflush);
PG_FUNCTION_INFO_V1(utl_file_fclose);
PG_FUNCTION_INFO_V1(utl_file_fclose_all);
PG_FUNCTION_INFO_V1(utl_file_fremove);
PG_FUNCTION_INFO_V1(utl_file_frename);
/*
PG_FUNCTION_INFO_V1(utl_file_fcopy);
*/
PG_FUNCTION_INFO_V1(utl_file_fgetattr);
PG_FUNCTION_INFO_V1(utl_file_tmpdir);

#define PARAMETER_ERROR(detail) \
	ereport(ERROR, \
		(errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
		 errmsg("invalid parameter"), \
		 errdetail(detail)))

#define CUSTOM_EXCEPTION(msg, detail) \
	ereport(ERROR, \
		(errcode(ERRCODE_RAISE_EXCEPTION), \
		 errmsg(msg), \
		 errdetail(detail)))

#define INVALID_FILEHANDLE_EXCEPTION()	CUSTOM_EXCEPTION(INVALID_FILEHANDLE, "Used file handle isn't valid.")

#define CHECK_FILE_HANDLE() \
	if (PG_ARGISNULL(0)) \
		CUSTOM_EXCEPTION(INVALID_FILEHANDLE, "Used file handle isn't valid.")

#define NON_EMPTY_TEXT(dat) \
	if (VARSIZE(dat) - VARHDRSZ == 0) \
		ereport(ERROR, \
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE), \
			 errmsg("invalid parameter"), \
			 errdetail("Empty string isn't allowed.")));

#define NOT_NULL_ARG(n) \
	if (PG_ARGISNULL(n)) \
		ereport(ERROR, \
			(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), \
			 errmsg("null value not allowed"), \
			 errhint("%dth argument is NULL.", n)));

#define MAX_LINESIZE		32767

#define CHECK_LINESIZE(max_linesize) \
	do { \
		if ((max_linesize) < 1 || (max_linesize) > MAX_LINESIZE) \
			CUSTOM_EXCEPTION(INVALID_MAXLINESIZE, "maxlinesize is out of range"); \
	} while(0)

typedef struct FileSlot
{
	FILE   *file;
	int		max_linesize;
	int32	id;
} FileSlot;

#define MAX_SLOTS		50			/* Oracle 10g supports 50 files */
#define INVALID_SLOTID	0			/* invalid slot id */

static FileSlot	slots[MAX_SLOTS];	/* initilaized with zeros */
static int32	slotid = 0;			/* next slot id */

static void check_secure_locality(const char *path);
static char *get_safe_path(text *location, text *filename);

/*
 * get_descriptor(FILE *file) find any free slot for FILE pointer.
 * If isn't realloc array slots and add 32 new free slots.
 *
 */
static int
get_descriptor(FILE *file, int max_linesize)
{
	int i;

	for (i = 0; i < MAX_SLOTS; i++)
	{
		if (slots[i].id == INVALID_SLOTID)
		{
			slots[i].id = ++slotid;
			if (slots[i].id == INVALID_SLOTID)
				slots[i].id = ++slotid;	/* skip INVALID_SLOTID */
			slots[i].file = file;
			slots[i].max_linesize = max_linesize;
			return slots[i].id;
		}
	}

	return INVALID_SLOTID;
}

/* return stored pointer to FILE */
static FILE *
get_stream(int d, int *max_linesize)
{
	int i;

	if (d == INVALID_SLOTID)
		INVALID_FILEHANDLE_EXCEPTION();

	for (i = 0; i < MAX_SLOTS; i++)
	{
		if (slots[i].id == d)
		{
			if (max_linesize)
				*max_linesize = slots[i].max_linesize;
			return slots[i].file;
		}
	}

	INVALID_FILEHANDLE_EXCEPTION();
	return NULL;	/* keep compiler quiet */
}

static void
IO_EXCEPTION(void)
{
	switch (errno)
	{
		case EACCES:
		case ENAMETOOLONG:
		case ENOENT:
		case ENOTDIR:
			CUSTOM_EXCEPTION(INVALID_PATH, strerror(errno));
			break;

		default:
			CUSTOM_EXCEPTION(INVALID_OPERATION, strerror(errno));
	}
}

/*
 * FUNCTION UTL_FILE.FOPEN(location text,
 *			   filename text,
 *			   open_mode text,
 *			   max_linesize integer)
 *          RETURNS UTL_FILE.FILE_TYPE;
 *
 * The FOPEN function opens specified file and returns file handle.
 *  open_mode: ['R', 'W', 'A']
 *  max_linesize: [1 .. 32767]
 *
 * Exceptions:
 *  INVALID_MODE, INVALID_OPERATION, INVALID_PATH, INVALID_MAXLINESIZE
 */
Datum
utl_file_fopen(PG_FUNCTION_ARGS)
{
	text	   *open_mode;
	int			max_linesize;
	const char *mode = NULL;
	FILE	   *file;
	char	   *fullname;
	int			d;

	NOT_NULL_ARG(0);
	NOT_NULL_ARG(1);
	NOT_NULL_ARG(2);
	NOT_NULL_ARG(3);

	open_mode = PG_GETARG_TEXT_P(2);

	NON_EMPTY_TEXT(open_mode);

	max_linesize = PG_GETARG_INT32(3);
	CHECK_LINESIZE(max_linesize);

	if (VARSIZE(open_mode) - VARHDRSZ != 1)
		CUSTOM_EXCEPTION(INVALID_MODE, "open mode is different than [R,W,A]");

	switch (*((char*)VARDATA(open_mode)))
	{
		case 'a':
		case 'A':
			mode = "a";
			break;

		case 'r':
		case 'R':
			mode = "r";
			break;

		case 'w':
		case 'W':
			mode = "w";
			break;

		default:
			CUSTOM_EXCEPTION(INVALID_MODE, "open mode is different than [R,W,A]");
	}

	/* open file */
	fullname = get_safe_path(PG_GETARG_TEXT_P(0), PG_GETARG_TEXT_P(1));
	file = fopen(fullname, mode);
	if (!file)
		IO_EXCEPTION();

	d = get_descriptor(file, max_linesize);
	if (d == INVALID_SLOTID)
	{
		fclose(file);
		ereport(ERROR,
		    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
		     errmsg("program limit exceeded"),
		     errdetail("Too much concurent opened files"),
		     errhint("You can only open a maximum of ten files for each session")));
	}

	PG_RETURN_INT32(d);
}

Datum
utl_file_is_open(PG_FUNCTION_ARGS)
{
	if (!PG_ARGISNULL(0))
	{
		int	i;
		int	d = PG_GETARG_INT32(0);

		for (i = 0; i < MAX_SLOTS; i++)
		{
			if (slots[i].id == d)
				PG_RETURN_BOOL(slots[i].file != NULL);
		}
	}

	PG_RETURN_BOOL(false);
}

#define CHECK_LENGTH(l) \
	if (l > max_linesize) \
		CUSTOM_EXCEPTION(VALUE_ERROR, "buffer is too short");

/* read line from file. set eof if is EOF */

static text *
get_line(FILE *f, int max_linesize, bool *iseof)
{
	int c;
	char *buffer = NULL;
	char *bpt;
	int csize = 0;
	text *result = NULL;

	bool eof = true;

	buffer = palloc(max_linesize + 2);
	bpt = buffer;

	errno = 0;

	while (csize < max_linesize && (c = fgetc(f)) != EOF)
	{
		eof = false; 	/* I was able read one char */

		if (c == '\r')  /* lookin ahead \n */
		{
			c = fgetc(f);
			if (c == EOF)
				break;  /* last char */

			if (c != '\n')
				ungetc(c, f);
			/* skip \r\n */
			break;
		}
		else if (c == '\n')
			break;

		++csize;
		*bpt++ = c;
	}

	if (!eof)
	{
		pg_verifymbstr(buffer, csize, false);
		result = palloc(csize + VARHDRSZ);
		memcpy(VARDATA(result), buffer, csize);
		SET_VARSIZE(result, csize + VARHDRSZ);
		*iseof = false;
	}
	else
	{
		switch (errno)
		{
			case 0:
				break;

			case EBADF:
				CUSTOM_EXCEPTION(INVALID_OPERATION, "file descriptor isn't valid for reading");
				break;

			default:
				CUSTOM_EXCEPTION(READ_ERROR, strerror(errno));
				break;
		}

		*iseof = true;
	}

	pfree(buffer);
	return result;
}


/*
 * FUNCTION UTL_FILE.GET_LINE(file UTL_TYPE.FILE_TYPE, line int DEFAULT NULL)
 *          RETURNS text;
 *
 * Reads one line from file.
 *
 * Exceptions:
 *  NO_DATA_FOUND, INVALID_FILEHANDLE, INVALID_OPERATION, READ_ERROR
 */
Datum
utl_file_get_line(PG_FUNCTION_ARGS)
{
	int max_linesize;
	FILE *f;
	text *result;
	bool iseof;

	CHECK_FILE_HANDLE();
	f = get_stream(PG_GETARG_INT32(0), &max_linesize);

	/* 'len' overwrites max_linesize, but must be smaller than max_linesize */
	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
	{
		int	len = PG_GETARG_INT32(1);
		CHECK_LINESIZE(len);
		if (max_linesize > len)
			max_linesize = len;
	}

	result = get_line(f, max_linesize, &iseof);

	if (iseof)
	    	ereport(ERROR,
				(errcode(ERRCODE_NO_DATA_FOUND),
		    		 errmsg("no data found")));

	PG_RETURN_TEXT_P(result);
}


/*
 * FUNCTION UTL_FILE.GET_NEXTLINE(file UTL_TYPE.FILE_TYPE)
 *          RETURNS text;
 *
 * Reads one line from file or retutns NULL
 * by Steven Feuerstein.
 *
 * Exceptions:
 *  INVALID_FILEHANDLE, INVALID_OPERATION, READ_ERROR
 */
Datum
utl_file_get_nextline(PG_FUNCTION_ARGS)
{
	int max_linesize;
	FILE *f;
	text *result;
	bool iseof;

	CHECK_FILE_HANDLE();
	f = get_stream(PG_GETARG_INT32(0), &max_linesize);

	result = get_line(f, max_linesize, &iseof);

	if (iseof)
		PG_RETURN_NULL();

	PG_RETURN_TEXT_P(result);
}

static void
do_flush(FILE *f)
{
	if (fflush(f) != 0)
	{
		if (errno == EBADF)
			CUSTOM_EXCEPTION(INVALID_OPERATION, "File is not an opened, or is not open for writing");
		else
			CUSTOM_EXCEPTION(WRITE_ERROR, strerror(errno));
	}
}

/*
 * FUNCTION UTL_FILE.PUT(file UTL_FILE.FILE_TYPE, buffer text)
 *          RETURNS bool;
 *
 * The PUT function puts data out to specified file. Buffer length allowed is
 * 32K or 1024 (max_linesize);
 *
 * Exceptions:
 *  INVALID_FILEHANDLE, INVALID_OPERATION, WRITE_ERROR, VALUE_ERROR
 *
 * Note: returns bool because I cannot do envelope over void function
 */

#define CHECK_ERRNO_PUT()  \
	switch (errno) \
	{ \
		case EBADF: \
			CUSTOM_EXCEPTION(INVALID_OPERATION, "file descriptor isn't valid for writing"); \
			break; \
		default: \
			CUSTOM_EXCEPTION(WRITE_ERROR, strerror(errno)); \
	}

static FILE *
do_put(PG_FUNCTION_ARGS)
{
	FILE   *f;
	text   *buffer;
	int		max_linesize;
	int		len;

	CHECK_FILE_HANDLE();
	f = get_stream(PG_GETARG_INT32(0), &max_linesize);

	NOT_NULL_ARG(1);
	buffer = PG_GETARG_TEXT_PP(1);

	/* I have to check buffer's size */
	len = VARSIZE_ANY_EXHDR(buffer);
	CHECK_LENGTH(len);

	if (fwrite(VARDATA_ANY(buffer), 1, len, f) != len)
		CHECK_ERRNO_PUT();
	return f;
}

Datum
utl_file_put(PG_FUNCTION_ARGS)
{
	do_put(fcinfo);
	PG_RETURN_BOOL(true);
}

static void
do_new_line(FILE *f, int lines)
{
	int	i;
	for (i = 0; i < lines; i++)
	{
#ifndef WIN32
		if (fputc('\n', f) == EOF)
		    CHECK_ERRNO_PUT();
#else
		if (fputs("\r\n", f) == EOF)
		    CHECK_ERRNO_PUT();
#endif
	}
}

Datum
utl_file_put_line(PG_FUNCTION_ARGS)
{
	FILE   *f;
	bool	autoflush;

	f = do_put(fcinfo);

	autoflush = (PG_NARGS() > 2 && !PG_ARGISNULL(2) && PG_GETARG_BOOL(2));

	do_new_line(f, 1);

	if (autoflush)
		do_flush(f);

	PG_RETURN_BOOL(true);
}

Datum
utl_file_new_line(PG_FUNCTION_ARGS)
{
	FILE   *f;
	int		lines;

	CHECK_FILE_HANDLE();
	f = get_stream(PG_GETARG_INT32(0), NULL);

	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
		lines = PG_GETARG_INT32(1);
	else
		lines = 1;

	do_new_line(f, lines);

	PG_RETURN_BOOL(true);
}

/*
 * FUNCTION UTL_FILE.PUTF(file UTL_FILE.FILE_TYPE,
 *			format text,
 *			arg1 text,
 *			arg2 text,
 *			arg3 text,
 *			arg4 text,
 *			arg5 text)
 *	    RETURNS bool;
 *
 * Puts formated data to file. Allows %s like subst symbol.
 *
 * Exception:
 *  INVALID_FILEHANDLE, INVALID_OPERATION, WRITE_ERROR
 */
Datum
utl_file_putf(PG_FUNCTION_ARGS)
{
	FILE *f;
	text *format;
	int max_linesize;
	int format_length;
	char *fpt;
	int cur_par = 0;
	int cur_len = 0;

	CHECK_FILE_HANDLE();
	f = get_stream(PG_GETARG_INT32(0), &max_linesize);

	NOT_NULL_ARG(1);
	format = PG_GETARG_TEXT_P(1);
	format_length = VARSIZE_ANY_EXHDR(format);

	for (fpt = VARDATA(format); format_length > 0; fpt++, format_length--)
	{
		if (format_length == 1)
		{
			/* last char */
			CHECK_LENGTH(++cur_len);
			if (fputc(*fpt, f) == EOF)
				CHECK_ERRNO_PUT();
			continue;
		}
		/* ansi compatible string */
		if (fpt[0] == '\\' && fpt[1] == 'n')
		{
			CHECK_LENGTH(++cur_len);
			if (fputc('\n', f) == EOF)
				CHECK_ERRNO_PUT();
			fpt++; format_length--;
			continue;
		}
		if (fpt[0] == '%')
		{
			if (fpt[1] == '%')
			{
				CHECK_LENGTH(++cur_len);
				if (fputc('%', f) == EOF)
					CHECK_ERRNO_PUT();
			}
			else if (fpt[1] == 's' && ++cur_par <= 5 && !PG_ARGISNULL(cur_par + 1))
			{
				text *buffer = PG_GETARG_TEXT_P(cur_par + 1);
				int bfsize = VARSIZE_ANY_EXHDR(buffer);
				char *tbuf;

				cur_len += bfsize;
				CHECK_LENGTH(cur_len);

				tbuf = (char *) palloc(bfsize + 1);
				memcpy(tbuf, VARDATA(buffer), bfsize);
				tbuf[bfsize] = '\0';

				if (fputs(tbuf, f) == EOF)
					CHECK_ERRNO_PUT();

			}
			fpt++; format_length--;
			continue;
		}
		CHECK_LENGTH(++cur_len);
		if (fputc(fpt[0], f) == EOF)
			CHECK_ERRNO_PUT();
	}

	PG_RETURN_BOOL(true);
}


/*
 * FUNCTION UTL_FILE.FFLUSH(file UTL_FILE.FILE_TYPE)
 *          RETURNS void;
 *
 * This function makes sure that all pending data for the specified file is written
 * physically out to file.
 *
 * Exceptions:
 *  INVALID_FILEHANDLE, INVALID_OPERATION, WRITE_ERROR
 */
Datum
utl_file_fflush(PG_FUNCTION_ARGS)
{
	FILE *f;

	CHECK_FILE_HANDLE();
	f = get_stream(PG_GETARG_INT32(0), NULL);
	do_flush(f);

	PG_RETURN_VOID();
}


/*
 * FUNCTION UTL_FILE.FCLOSE(file UTL_FILE.FILE_TYPE)
 *          RETURNS NULL
 *
 * Close an open file. This function reset file handle to NULL on Oracle platform.
 * It isn't possible in PostgreSQL, and then you have to call fclose function
 * like:
 *        file := utl_file.fclose(file);
 *
 * Exception:
 *  INVALID_FILEHANDLE, WRITE_ERROR
 */
Datum
utl_file_fclose(PG_FUNCTION_ARGS)
{
	int i;
	int	d = PG_GETARG_INT32(0);

	for (i = 0; i < MAX_SLOTS; i++)
	{
		if (slots[i].id == d)
		{
			if (slots[i].file && fclose(slots[i].file) != 0)
			{
				if (errno == EBADF)
					CUSTOM_EXCEPTION(INVALID_FILEHANDLE, "File is not an opened");
				else
					CUSTOM_EXCEPTION(WRITE_ERROR, strerror(errno));
			}
			slots[i].file = NULL;
			slots[i].id = INVALID_SLOTID;
			PG_RETURN_NULL();
		}
	}

	INVALID_FILEHANDLE_EXCEPTION();
	PG_RETURN_NULL();
}


/*
 * FUNCTION UTL_FILE.FCLOSE_ALL()
 *          RETURNS void
 *
 * Close all opened files.
 *
 * Exceptions: WRITE_ERROR
 */
Datum
utl_file_fclose_all(PG_FUNCTION_ARGS)
{
	int i;

	for (i = 0; i < MAX_SLOTS; i++)
	{
		if (slots[i].id != INVALID_SLOTID)
		{
			if (slots[i].file && fclose(slots[i].file) != 0)
			{
				if (errno == EBADF)
					CUSTOM_EXCEPTION(INVALID_FILEHANDLE, "File is not an opened");
				else
					CUSTOM_EXCEPTION(WRITE_ERROR, strerror(errno));
			}
			slots[i].file = NULL;
			slots[i].id = INVALID_SLOTID;
		}
	}

	PG_RETURN_VOID();
}


/*
 * utl_file_dir security .. is solved with aux. table.
 *
 * Raise exception if don't find string in table.
 */
static void
check_secure_locality(const char *path)
{
	static SPIPlanPtr	plan = NULL;

	Oid		argtypes[] = {TEXTOID};
	Datum	values[1];
	char	nulls[1] = {' '};

	/* hack for availbility regress test */
	if (strcmp(path, "/tmp/regress_orafce") == 0)
		return;

	values[0] = CStringGetTextDatum(path);

	/*
	 * SELECT 1 FROM utl_file.utl_file_dir
	 *   WHERE substring($1, 1, length(dir) + 1) = dir || '/'
	 */

	if (SPI_connect() < 0)
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("SPI_connect failed")));

	if (!plan)
	{
		/* Don't use LIKE not to escape '_' and '%' */
		SPIPlanPtr p = SPI_prepare(
		    "SELECT 1 FROM utl_file.utl_file_dir"
			" WHERE substring($1, 1, length(dir) + 1) = dir || '/'",
		    1, argtypes);

		if (p == NULL || (plan = SPI_saveplan(p)) == NULL)
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				errmsg("SPI_prepare_failed")));
	}

	if (SPI_OK_SELECT != SPI_execute_plan(plan, values, nulls, false, 1))
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("can't execute sql")));

	if (SPI_processed == 0)
		ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg(INVALID_PATH),
			 errdetail("you cannot access locality"),
			 errhint("locality is not found in utl_file_dir table")));
	SPI_finish();
}

/*
 * get_safe_path - make a fullpath and check security.
 */
static char *
get_safe_path(text *location, text *filename)
{
	char   *fullname;
	int		aux_pos;
	int		aux_len;

	NON_EMPTY_TEXT(location);
	NON_EMPTY_TEXT(filename);

	aux_pos = VARSIZE_ANY_EXHDR(location);
	aux_len = VARSIZE_ANY_EXHDR(filename);

	fullname = palloc(aux_pos + 1 + aux_len + 1);
	memcpy(fullname, VARDATA(location), aux_pos);
	fullname[aux_pos] = '/';
	memcpy(fullname + aux_pos + 1, VARDATA(filename), aux_len);
	fullname[aux_pos + aux_len + 1] = '\0';

	/* check locality in canonizalized form of path */
	canonicalize_path(fullname);
	check_secure_locality(fullname);

	return fullname;
}

Datum
utl_file_fremove(PG_FUNCTION_ARGS)
{
	char	   *fullname;

	NOT_NULL_ARG(0);
	NOT_NULL_ARG(1);

	fullname = get_safe_path(PG_GETARG_TEXT_P(0), PG_GETARG_TEXT_P(1));

	if (unlink(fullname) != 0)
		IO_EXCEPTION();

	PG_RETURN_VOID();
}

Datum
utl_file_frename(PG_FUNCTION_ARGS)
{
	char	   *srcpath;
	char	   *dstpath;
	bool		overwrite;

	NOT_NULL_ARG(0);
	NOT_NULL_ARG(1);
	NOT_NULL_ARG(2);
	NOT_NULL_ARG(3);

	overwrite = (PG_NARGS() > 4 && !PG_ARGISNULL(4) && PG_GETARG_BOOL(4));
	srcpath = get_safe_path(PG_GETARG_TEXT_P(0), PG_GETARG_TEXT_P(1));
	dstpath = get_safe_path(PG_GETARG_TEXT_P(2), PG_GETARG_TEXT_P(3));

	if (!overwrite)
	{
		struct stat	st;
		if (stat(dstpath, &st) == 0)
			CUSTOM_EXCEPTION(WRITE_ERROR, "File exists");
		else if (errno != ENOENT)
			IO_EXCEPTION();
	}

	/* rename() overwrites existing files. */
	if (rename(srcpath, dstpath) != 0)
		IO_EXCEPTION();

	PG_RETURN_VOID();
}

Datum
utl_file_fgetattr(PG_FUNCTION_ARGS)
{
	char	   *fullname;
	struct stat	st;
	TupleDesc	tupdesc;
	Datum		result;
	HeapTuple	tuple;
	Datum		values[3];
	bool		nulls[3] = { 0 };

	NOT_NULL_ARG(0);
	NOT_NULL_ARG(1);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	fullname = get_safe_path(PG_GETARG_TEXT_P(0), PG_GETARG_TEXT_P(1));

	if (stat(fullname, &st) == 0)
	{
		values[0] = BoolGetDatum(true);
		values[1] = Int64GetDatum(st.st_size);
#ifndef WIN32
		values[2] = Int32GetDatum(st.st_blksize);
#else
		values[2] = 512;	/* NTFS block size */
#endif
	}
	else
	{
		values[0] = BoolGetDatum(false);
		nulls[1] = true;
		nulls[2] = true;
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	result = HeapTupleGetDatum(tuple);

	PG_RETURN_DATUM(result);
}

Datum
utl_file_tmpdir(PG_FUNCTION_ARGS)
{
#ifndef WIN32
	const char *tmpdir = getenv("TMPDIR");

	if (!tmpdir)
		tmpdir = "/tmp";
#else
	char		tmpdir[MAXPGPATH];
	int			ret;

	ret = GetTempPath(MAXPGPATH, tmpdir);
	if (ret == 0 || ret > MAXPGPATH)
		CUSTOM_EXCEPTION(INVALID_PATH, strerror(errno));

	canonicalize_path(tmpdir);
#endif

	PG_RETURN_TEXT_P(CStringGetTextP(tmpdir));
}
