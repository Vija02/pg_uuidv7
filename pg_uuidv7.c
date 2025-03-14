#include "postgres.h"

#include "fmgr.h"
#include "port/pg_bswap.h"
#include "utils/uuid.h"
#include "utils/timestamp.h"

#include <time.h>

/*
 * Monkey-patch the clock_gettime function for windows
 * 
 * Thanks to https://stackoverflow.com/questions/5404277/porting-clock-gettime-to-windows
 */
#ifdef WIN32
LARGE_INTEGER
getFILETIMEoffset()
{
	SYSTEMTIME s;
	FILETIME f;
	LARGE_INTEGER t;

	s.wYear = 1970;
	s.wMonth = 1;
	s.wDay = 1;
	s.wHour = 0;
	s.wMinute = 0;
	s.wSecond = 0;
	s.wMilliseconds = 0;
	SystemTimeToFileTime(&s, &f);
	t.QuadPart = f.dwHighDateTime;
	t.QuadPart <<= 32;
	t.QuadPart |= f.dwLowDateTime;
	return (t);
}

int clock_gettime(int X, struct timeval *tv)
{
	LARGE_INTEGER t;
	FILETIME f;
	double microseconds;
	static LARGE_INTEGER offset;
	static double frequencyToMicroseconds;
	static int initialized = 0;
	static BOOL usePerformanceCounter = 0;

	if (!initialized)
	{
		LARGE_INTEGER performanceFrequency;
		initialized = 1;
		usePerformanceCounter = QueryPerformanceFrequency(&performanceFrequency);
		if (usePerformanceCounter)
		{
			QueryPerformanceCounter(&offset);
			frequencyToMicroseconds = (double)performanceFrequency.QuadPart / 1000000.;
		}
		else
		{
			offset = getFILETIMEoffset();
			frequencyToMicroseconds = 10.;
		}
	}
	if (usePerformanceCounter)
		QueryPerformanceCounter(&t);
	else
	{
		GetSystemTimeAsFileTime(&f);
		t.QuadPart = f.dwHighDateTime;
		t.QuadPart <<= 32;
		t.QuadPart |= f.dwLowDateTime;
	}

	t.QuadPart -= offset.QuadPart;
	microseconds = (double)t.QuadPart / frequencyToMicroseconds;
	t.QuadPart = microseconds;
	tv->tv_sec = t.QuadPart / 1000000;
	tv->tv_usec = t.QuadPart % 1000000;
	return (0);
}
#define CLOCK_REALTIME 0
#endif

/*
 * Number of microseconds between unix and postgres epoch
 */
#define EPOCH_DIFF_USECS ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * USECS_PER_DAY)

PG_MODULE_MAGIC;

/* DLL Export if building with MSVC */
#ifdef WIN32
PGDLLEXPORT Datum uuid_generate_v7(PG_FUNCTION_ARGS);
#endif

PG_FUNCTION_INFO_V1(uuid_generate_v7);

Datum uuid_generate_v7(PG_FUNCTION_ARGS)
{
	pg_uuid_t *uuid = palloc(UUID_LEN);
	struct timespec ts;
	uint64_t tms;

	/*
	 * Set first 48 bits to unix epoch timestamp
	 */
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not get CLOCK_REALTIME")));

	tms = ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);
	tms = pg_hton64(tms << 16);
	memcpy(&uuid->data[0], &tms, 6);

	if (!pg_strong_random(&uuid->data[6], UUID_LEN - 6))
		ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not generate random values")));

	/*
	 * Set magic numbers for a "version 7" UUID, see
	 * https://www.ietf.org/archive/id/draft-ietf-uuidrev-rfc4122bis-00.html#name-uuid-version-7
	 */
	uuid->data[6] = (uuid->data[6] & 0x0f) | 0x70; /* 4 bit version [0111] */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80; /* 2 bit variant [10]   */

	PG_RETURN_UUID_P(uuid);
}

static uint64_t uuid_v7_to_uint64(pg_uuid_t *uuid)
{
	uint64_t ts;

	memcpy(&ts, &uuid->data[0], 6);
	ts = pg_ntoh64(ts) >> 16;
	ts = 1000 * ts - EPOCH_DIFF_USECS;

	return ts;
}

/* DLL Export if building with MSVC */
#ifdef WIN32
PGDLLEXPORT Datum uuid_v7_to_timestamptz(PG_FUNCTION_ARGS);
#endif

PG_FUNCTION_INFO_V1(uuid_v7_to_timestamptz);

Datum uuid_v7_to_timestamptz(PG_FUNCTION_ARGS)
{
	pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
	uint64_t ts = uuid_v7_to_uint64(uuid);

	PG_RETURN_TIMESTAMPTZ(ts);
}

/* DLL Export if building with MSVC */
#ifdef WIN32
PGDLLEXPORT Datum uuid_v7_to_timestamp(PG_FUNCTION_ARGS);
#endif

PG_FUNCTION_INFO_V1(uuid_v7_to_timestamp);

Datum uuid_v7_to_timestamp(PG_FUNCTION_ARGS)
{
	pg_uuid_t *uuid = PG_GETARG_UUID_P(0);
	uint64_t ts = uuid_v7_to_uint64(uuid);

	PG_RETURN_TIMESTAMP(ts);
}

static Datum uuid_uint64_to_v7(uint64_t ts, bool zero)
{
	pg_uuid_t *uuid = palloc(UUID_LEN);
	uint64_t tms;

	tms = (ts + EPOCH_DIFF_USECS) / 1000;
	tms = pg_hton64(tms << 16);
	memcpy(&uuid->data[0], &tms, 6);

	if (zero)
		memset(&uuid->data[6], 0, UUID_LEN - 6);
	else if (!pg_strong_random(&uuid->data[6], UUID_LEN - 6))
		ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("could not generate random values")));

	/*
	 * Set magic numbers for a "version 7" UUID, see
	 * https://www.ietf.org/archive/id/draft-ietf-uuidrev-rfc4122bis-00.html#name-uuid-version-7
	 */
	uuid->data[6] = (uuid->data[6] & 0x0f) | 0x70; /* 4 bit version [0111] */
	uuid->data[8] = (uuid->data[8] & 0x3f) | 0x80; /* 2 bit variant [10]   */

	PG_RETURN_UUID_P(uuid);
}

/* DLL Export if building with MSVC */
#ifdef WIN32
PGDLLEXPORT Datum uuid_timestamptz_to_v7(PG_FUNCTION_ARGS);
#endif

PG_FUNCTION_INFO_V1(uuid_timestamptz_to_v7);

Datum uuid_timestamptz_to_v7(PG_FUNCTION_ARGS)
{
	TimestampTz ts = PG_GETARG_TIMESTAMPTZ(0);
	bool zero = false;

	if (!PG_ARGISNULL(1))
		zero = PG_GETARG_BOOL(1);

	return uuid_uint64_to_v7(ts, zero);
}

/* DLL Export if building with MSVC */
#ifdef WIN32
PGDLLEXPORT Datum uuid_timestamp_to_v7(PG_FUNCTION_ARGS);
#endif

PG_FUNCTION_INFO_V1(uuid_timestamp_to_v7);

Datum uuid_timestamp_to_v7(PG_FUNCTION_ARGS)
{
	Timestamp ts = PG_GETARG_TIMESTAMP(0);
	bool zero = false;

	if (!PG_ARGISNULL(1))
		zero = PG_GETARG_BOOL(1);

	return uuid_uint64_to_v7(ts, zero);
}