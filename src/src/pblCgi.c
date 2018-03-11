/*
 pblCgi.c - C Common Gateway Interface functions.

 Copyright (c) 2018 Peter Graf. All rights reserved.

 This file is part of PBL - The Program Base Library.
 PBL is free software.

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 For more information on the Program Base Library or Peter Graf,
 please see: http://www.mission-base.com/.

 $Log: pblCgi.c,v $
 Revision 1.40  2018/03/11 00:23:25  peter
 Cleanup of split to list handling

 Revision 1.39  2018/03/10 21:49:21  peter
 Integration with ArvosDirectoryService

 Revision 1.38  2018/03/10 16:22:21  peter
 More work on cgi functions

 Revision 1.37  2018/02/23 23:20:24  peter
 Started to work on the cgi code


 */

 /*
  * Make sure "strings <exe> | grep Id | sort -u" shows the source file versions
  */
char* pblCgi_c_id = "$Id: pblCgi.c,v 1.40 2018/03/11 00:23:25 peter Exp $";

#include <stdio.h>
#include <memory.h>

#ifndef __APPLE__
#include <malloc.h>
#endif

#include <stdlib.h>

#include "pbl.h"
#include "pblCgi.h"

/*****************************************************************************/
/* #defines                                                                  */
/*****************************************************************************/
#define PBL_CGI_MAX_SIZE_OF_BUFFER_ON_STACK		(4 * 1024)
#define PBL_CGI_MAX_QUERY_PARAMETERS_COUNT	    128
#define PBL_CGI_MAX_POST_INPUT_LEN              (1024 * 1024)

/*****************************************************************************/
/* Variables                                                                 */
/*****************************************************************************/

FILE * pblCgiTraceFile = NULL;

static char * contentType = NULL;
static void pblCgiSetContentType(char * type)
{
	if (!contentType)
	{
		contentType = type;

		printf("Content-Type: %s\n\n", contentType);
		PBL_CGI_TRACE("Content-Type: %s\n", contentType);
	}
}

static char pblCgiCheckChar(char c)
{
	if (c < ' ' || c == '<')
	{
		return ' ';
	}
	return c;
}

static char * pblCgiDecode(char * source)
{
	static char * tag = "pblCgiDecode";
	char * sourcePtr = source;
	char * destinationPtr;
	char * destination;
	char buffer[3];
	int i;

	destinationPtr = destination = pbl_malloc(tag, strlen(source) + 1);
	if (!destinationPtr)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	while (*sourcePtr)
	{
		if (*sourcePtr == '+')
		{
			*destinationPtr++ = ' ';
			sourcePtr++;
			continue;
		}

		if ((*sourcePtr == '%') && *(sourcePtr + 1) && *(sourcePtr + 2)
			&& isxdigit((int)(*(sourcePtr + 1))) && isxdigit((int)(*(sourcePtr + 2))))
		{
			pblCgiStrNCpy(buffer, sourcePtr + 1, 3);
			buffer[2] = '\0';
#ifdef WIN32
			sscanf_s(buffer, "%x", &i);
#else
			sscanf(buffer, "%x", &i);
#endif
			*destinationPtr++ = (char)pblCgiCheckChar(i);
			sourcePtr += 3;
		}
		else
		{
			*destinationPtr++ = pblCgiCheckChar(*sourcePtr++);
			continue;
		}
	}
	*destinationPtr = '\0';
	return destination;
}

/**
* Reads a two column file into a string map.
*
* The format is
*
*   # Comment
*   key   value
*
*   If the parameter map is NULL, a new string map is created.
*   The pointer to the map used is returned.
*
* @return PblMap * retPtr != NULL: The pointer to the map.
*/
PblMap * pblCgiFileToMap(PblMap * map, char * filePath)
{
	static char * tag = "pblCgiFileToMap";

	if (!map)
	{
		if (!(map = pblMapNewHashMap()))
		{
			pblCgiExitOnError("Failed to create a map, pbl_errno = %d\n", pbl_errno);
		}
	}

	FILE * stream = pblCgiFopen(filePath, "r");
	char line[PBL_CGI_MAX_LINE_LENGTH + 1];

	while ((fgets(line, sizeof(line) - 1, stream)))
	{
		char * ptr = line;

		while (*ptr && isspace(*ptr))
		{
			ptr++;
		}

		if (!*ptr)
		{
			continue;
		}

		if (*ptr == '#')
		{
			continue;
		}

		char * key = ptr;
		while (*ptr && (!isspace(*ptr)))
		{
			ptr++;
		}

		if (*ptr)
		{
			*ptr++ = '\0';
		}

		char * value = pblCgiStrTrim(ptr);

		if (pblMapAddStrStr(map, key, value) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
	}
	fclose(stream);

	return map;
}

static PblMap * queryMap = NULL;
static void pblCgiSetQueryValue(char * key, char * value)
{
	static char * tag = "pblCgiSetQueryValue";

	if (!queryMap)
	{
		if (!(queryMap = pblMapNewHashMap()))
		{
			pblCgiExitOnError("Failed to create a map, pbl_errno = %d\n", pbl_errno);
		}
	}
	if (!key || !*key)
	{
		return;
	}
	if (!value)
	{
		value = "";
	}

	if (pblMapAddStrStr(queryMap, key, value) < 0)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}
	PBL_CGI_TRACE("In %s=%s", key, value);
}

/**
* Get the value for an iteration given for the key in the query.
*/
char * pblCgiQueryValueForIteration(char * key, int iteration)
{
	static char * tag = "pblCgiQueryValueForIteration";

	if (!queryMap)
	{
		return "";
	}
	if (!key || !*key)
	{
		pblCgiExitOnError("%s: Empty key not allowed!\n", tag, pbl_errstr);
	}
	if (iteration >= 0)
	{
		char * iterationKey = pblCgiSprintf("%s_%d", key, iteration);
		char * value = pblMapGetStr(queryMap, iterationKey);
		PBL_FREE(iterationKey);
		return value ? value : "";
	}
	char * value = pblMapGetStr(queryMap, key);
	return value ? value : "";
}

/**
* Get the value given for the key in the query.
*/
char * pblCgiQueryValue(char * key)
{
	return pblCgiQueryValueForIteration(key, -1);
}

void pblCgiTrace(const char * format, ...)
{
	if (!pblCgiTraceFile)
	{
		return;
	}

	va_list args;
	va_start(args, format);

	char buffer[PBL_CGI_MAX_SIZE_OF_BUFFER_ON_STACK + 1];
	int rc = vsnprintf(buffer, sizeof(buffer) - 1, format, args);
	va_end(args);

	if (rc < 0)
	{
		pblCgiExitOnError("Printing of format '%s' and size %lu failed with errno=%d\n", format, sizeof(buffer) - 1,
			errno);
	}
	buffer[sizeof(buffer) - 1] = '\0';

	char * now = pblCgiStrFromTime(time((time_t*)0));
	fputs(now, pblCgiTraceFile);
	PBL_FREE(now);

#ifndef _WIN32
	fprintf(pblCgiTraceFile, " %d: ", getpid());
#endif

	fputs(buffer, pblCgiTraceFile);
	fputs("\n", pblCgiTraceFile);
	fflush(pblCgiTraceFile);
}

/**
 * Print an error message and exit the program.
 */
void pblCgiExitOnError(const char * format, ...)
{
	pblCgiSetContentType("text/html");

	printf(
		"<!DOCTYPE html>\n"
		"<html>\n"
		"<head>\n"
		"<title>Arvos-App Server</title>\n"
		"</head>\n"
		"<body>\n"
		"<h1>\n"
		"PBL CGI Test\n"
		"</h1>\n"
		"<p><hr> <p>\n"
		"<h1>An error occurred</h1>\n");

	char * scriptName = pblCgiGetEnv("SCRIPT_NAME");
	if (!scriptName || !*scriptName)
	{
		scriptName = "unknown";
	}

	printf("<p>While accessing the script '%s'.\n", scriptName);
	printf("<p><b>\n");

	va_list args;
	va_start(args, format);

	char buffer[PBL_CGI_MAX_SIZE_OF_BUFFER_ON_STACK + 1];
	int rc = vsnprintf(buffer, sizeof(buffer) - 1, format, args);
	va_end(args);

	if (rc < 0)
	{
		printf("Printing of format '%s' and size %lu failed with errno=%d\n",
			format, sizeof(buffer) - 1, errno);
	}
	else
	{
		buffer[sizeof(buffer) - 1] = '\0';
		printf("%s", buffer);
		PBL_CGI_TRACE("%s", buffer);
	}

	printf("</b>\n");
	printf("<p>Please click your browser's back button to continue.\n");
	printf("<p><hr> <p>\n");
	printf(
		"<small>Copyright &copy; 2018 - Tamiko Thiel and Peter Graf</small>\n");
	printf("</body></HTML>\n");

	PBL_CGI_TRACE("%s exit(-1)", scriptName);
	exit(-1);
}

/**
 * Like sprintf, copies the value to the heap.
 */
char * pblCgiSprintf(const char * format, ...)
{
	va_list args;
	va_start(args, format);

	char buffer[PBL_CGI_MAX_SIZE_OF_BUFFER_ON_STACK + 1];
	int rc = vsnprintf(buffer, sizeof(buffer) - 1, format, args);
	va_end(args);

	if (rc < 0)
	{
		pblCgiExitOnError("Printing of format '%s' and size %lu failed with errno=%d\n", format, sizeof(buffer) - 1,
			errno);
	}
	buffer[sizeof(buffer) - 1] = '\0';
	return pblCgiStrDup(buffer);
}

/**
 * Tests whether a NULL terminated string array contains a string
 */
int pblCgiStrArrayContains(char ** array, char * string)
{
	for (int i = 0;; i++)
	{
		char * ptr = array[i];
		if (!ptr)
		{
			break;
		}
		if (pblCgiStrEquals(ptr, string))
		{
			return i;
		}
	}
	return -1;
}

/**
 * Copies at most n bytes, the result is always 0 terminated
 */
char * pblCgiStrNCpy(char *dest, char *string, size_t n)
{
	char * ptr = dest;
	while (--n && (*ptr++ = *string++))
		;

	if (*ptr)
	{
		*ptr = '\0';
	}
	return dest;
}

static char * pblCgiStrTrimStart(char * string)
{
	for (; *string; string++)
	{
		if (!isspace(*string))
		{
			return string;
		}
	}
	return string;
}

static void pblCgiStrTrimEnd(char * string)
{
	if (!string)
	{
		return;
	}

	char * ptr = (string + strlen(string)) - 1;
	while (ptr >= string)
	{
		if (isspace(*ptr))
		{
			*ptr-- = '\0';
		}
		else
		{
			return;
		}
	}
}

/**
 * Trim the string, remove blanks at start and end.
 */
char * pblCgiStrTrim(char * string)
{
	pblCgiStrTrimEnd(string);
	return pblCgiStrTrimStart(string);
}

/**
 * Test if a string is NULL or white space only
 */
int pblCgiStrIsNullOrWhiteSpace(char * string)
{
	if (string)
	{
		for (; *string; string++)
		{
			if (!isspace(*string))
			{
				return 0;
			}
		}
	}
	return 1;
}

char * pblCgiStrRangeDup(char * start, char * end)
{
	static char * tag = "pblCgiRangedup";

	start = pblCgiStrTrimStart(start);
	int length = end - start;
	if (length > 0)
	{
		char * value = pbl_memdup(tag, start, length + 1);
		if (!value)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		value[length] = '\0';
		pblCgiStrTrimEnd(value);
		return value;
	}
	return pblCgiStrDup("");
}

/**
 * String equals, handles NULLs.
 */
int pblCgiStrEquals(char * s1, char * s2)
{
	return !pblCgiStrCmp(s1, s2);
}

/**
 * Like strcmp, handles NULLs.
 */
int pblCgiStrCmp(char * s1, char * s2)
{
	if (!s1)
	{
		if (!s2)
		{
			return 0;
		}
		return -1;
	}
	if (!s2)
	{
		return 1;
	}
	return strcmp(s1, s2);
}

/**
 * Like strdup.
 *
 * If an error occurs, the program exits with an error message.
 */
char * pblCgiStrDup(char * string)
{
	static char * tag = "pblCgiStrDup";

	if (!string)
	{
		string = "";
	}
	string = pbl_strdup(tag, string);
	if (!string)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}
	return string;
}

/**
 * Like strcat, but the memory for the target is allocated.
 *
 * If an error occurs, the program exits with an error message.
 */
char * pblCgiStrCat(char * s1, char * s2)
{
	static char * tag = "pblCgiStrCat";
	char * result = NULL;
	size_t len1 = 0;
	size_t len2 = 0;

	if (!s1 || !(*s1))
	{
		if (!s2 || !(*s2))
		{
			return pblCgiStrDup("");
		}
		return pblCgiStrDup(s2);
	}

	if (!s2 || !(*s2))
	{
		return pblCgiStrDup(s1);
	}

	len1 = strlen(s1);
	len2 = strlen(s2);
	result = pbl_malloc(tag, len1 + len2 + 1);
	if (!result)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	memcpy(result, s1, len1);
	memcpy(result + len1, s2, len2 + 1);
	return result;
}

/**
 * Return a malloced time string.
 *
 * If an error occurs, the program exits with an error message.
 */
char * pblCgiStrFromTime(time_t t)
{
	struct tm *tm;

#ifdef _WIN32

	struct tm windowsTm;
	tm = &windowsTm;
	localtime_s(tm, (time_t *) &(t));
	
#else

	tm = localtime((time_t *) &(t));

#endif
	return pblCgiSprintf("%02d.%02d.%02d %02d:%02d:%02d", (tm->tm_year + 1900) % 100, tm->tm_mon + 1, tm->tm_mday,
		tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/**
 * Like split.
 *
 * If an error occurs, the program exits with an error message.
 *
 * The results should be defined as:
 *
 * 	char * results[size + 1];
 */
int pblCgiStrSplit(char * string, char * splitString, size_t size, char * results[])
{
	unsigned int index = 0;
	if (size < 1)
	{
		return index;
	}

	size_t length = strlen(splitString);
	results[0] = NULL;

	char * ptr = string;

	for (;;)
	{
		if (index > size - 1)
		{
			return index;
		}

		char * ptr2 = strstr(ptr, splitString);
		if (!ptr2)
		{
			results[index] = pblCgiStrDup(ptr);
			results[++index] = NULL;

			return index;
		}
		results[index] = pblCgiStrRangeDup(ptr, ptr2);
		results[++index] = NULL;

		ptr = ptr2 + length;
	}
	return index;
}

/**
* Split a string to a list.
*/
PblList * pblCgiStrSplitToList(char * string, char * splitString)
{
	static char * tag = "pblCgiSplitToList";
	PblList * list = pblListNewArrayList();
	if (!list)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	size_t length = strlen(splitString);
	char * ptr = string;

	for (;;)
	{
		char * ptr2 = strstr(ptr, splitString);
		if (!ptr2)
		{
			if (pblListAdd(list, pblCgiStrDup(ptr)) < 0)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
			break;
		}

		if (pblListAdd(list, pblCgiStrRangeDup(ptr, ptr2)) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		ptr = ptr2 + length;
	}
	return list;
}

static const char *pblCgiHexDigits = "0123456789abcdef";

/**
 * Convert a buffer to a hex byte string
 *
 * If an error occurs, the program exits with an error message.
 *
 * return The malloced hex string.
 */
char * pblCgiStrToHexFromBuffer(unsigned char * buffer, size_t length)
{
	static char * tag = "pblCgiStrToHexFromBuffer";
	char * hexString = pbl_malloc(tag, 2 * length + 1);
	if (!hexString)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	unsigned char c;
	int stringIndex = 0;
	for (unsigned int i = 0; i < length; i++)
	{
		hexString[stringIndex++] = pblCgiHexDigits[((c = buffer[i]) >> 4) & 0x0f];
		hexString[stringIndex++] = pblCgiHexDigits[c & 0x0f];
	}
	hexString[stringIndex] = 0;

	return hexString;
}

/**
 * Get the cookie, if any is given.
 */
char * pblCgiGetCoockie(char * cookieKey, char * cookieTag)
{
	char * cookie = pblCgiStrDup(pblCgiGetEnv("HTTP_COOKIE"));
	if (cookie && *cookie)
	{
		cookie = strstr(cookie, cookieTag);
		if (cookie)
		{
			cookie += strlen(cookieTag);
		}
	}
	else
	{
		cookie = pblCgiQueryValue(cookieKey);
	}

	if (!cookie || !*cookie)
	{
		return NULL;
	}
	for (char * ptr = cookie; *ptr; ptr++)
	{
		if (!strchr(pblCgiHexDigits, *ptr))
		{
			*ptr = '\0';
			break;
		}
	}
	return cookie;
}

/**
* Like fopen, needed for Windows port
*/
FILE * pblCgiFopen(char * filePath, char * openType)
{
	static char * tag = "pblCgiFopen";
	FILE * stream;

#ifdef WIN32

	errno_t err = fopen_s(&stream, filePath, openType);
	if (err != 0)
	{
		pblCgiExitOnError("%s: Cannot open file '%s', err=%d, errno=%d\n", tag,
			filePath, err, errno);
	}

#else

	if (!(stream = fopen(filePath, openType)))
	{
		pblCgiExitOnError("%s: Cannot open file '%s', errno=%d\n", tag, filePath, errno);
	}

#endif
	return stream;
}

/**
 * Like getenv, needed for Windows port
 */
char * pblCgiGetEnv(char * name)
{
#ifdef WIN32

	char *value;
	size_t len;
	_dupenv_s(&value, &len, name);
	return value;

#else

	return getenv(name);

#endif
}

/**
 * Reads in GET or POST query data, converts it to non-escaped text,
 * and saves each parameter in the query map.
 *
 *  If there's no variables that indicates GET or POST, it is assumed
 *  that the CGI script was started from the command line, specifying
 *  the query string like
 *
 *      script 'key1=value1&key2=value2'
 *
 */
void pblCgiParseQuery(int argc, char * argv[])
{
	static char * tag = "pblCgiParseQuery";
	char * ptr = NULL;
	char * queryString = NULL;

	ptr = pblCgiGetEnv("REQUEST_METHOD");
	if (!ptr || !*ptr)
	{
		if (argc < 2)
		{
			pblCgiExitOnError("%s: test usage: %s querystring\n", tag, argv[0]);
		}
		queryString = pblCgiStrDup(argv[1]);
	}
	else if (!strcmp(ptr, "GET"))
	{
		ptr = pblCgiGetEnv("QUERY_STRING");
		if (!ptr || !*ptr)
		{
			queryString = pblCgiStrDup("");
		}
		else
		{
			queryString = pblCgiStrDup(ptr);
		}
	}
	else if (!strcmp(ptr, "POST"))
	{
		size_t length = 0;

		ptr = pblCgiGetEnv("QUERY_STRING");
		if (!ptr || !*ptr)
		{
			queryString = pblCgiStrDup("");
		}
		else
		{
			queryString = pblCgiStrCat(ptr, "&");
			length = strlen(queryString);
		}

		ptr = pblCgiGetEnv("CONTENT_LENGTH");
		if (ptr && *ptr)
		{
			int contentLength = atoi(ptr);
			if (contentLength > 0)
			{
				if (length + contentLength >= PBL_CGI_MAX_POST_INPUT_LEN)
				{
					pblCgiExitOnError("%s: POST input too long, %d bytes\n", tag,
						length + contentLength);
				}

				queryString = realloc(queryString, length + contentLength + 1);
				if (!queryString)
				{
					pblCgiExitOnError("%s: Out of memory\n", tag);
				}

				int c;
				ptr = queryString + length;
				while (contentLength-- > 0)
				{
					if ((c = getchar()) == EOF)
					{
						break;
					}
					*ptr++ = c;
				}
				*ptr = '\0';
			}
		}
	}
	else
	{
		pblCgiExitOnError("%s: Unknown REQUEST_METHOD '%s'\n", tag, ptr);
	}

	PBL_CGI_TRACE("In %s", queryString);

	char * keyValuePairs[PBL_CGI_MAX_QUERY_PARAMETERS_COUNT + 1];
	char * keyValuePair[2 + 1];

	pblCgiStrSplit(queryString, "&", PBL_CGI_MAX_QUERY_PARAMETERS_COUNT, keyValuePairs);
	for (int i = 0; keyValuePairs[i]; i++)
	{
		pblCgiStrSplit(keyValuePairs[i], "=", 2, keyValuePair);
		if (keyValuePair[0] && keyValuePair[1] && !keyValuePair[2])
		{
			char * key = pblCgiDecode(keyValuePair[0]);
			char * value = pblCgiDecode(keyValuePair[1]);

			pblCgiSetQueryValue(pblCgiStrTrim(key), pblCgiStrTrim(value));

			PBL_FREE(key);
			PBL_FREE(value);
		}

		PBL_FREE(keyValuePair[0]);
		PBL_FREE(keyValuePair[1]);
		PBL_FREE(keyValuePairs[i]);
	}

	PBL_FREE(queryString);
}

static char * pblCgiReplaceLowerThan(char * string, char *ptr2)
{
	static char * tag = "pblCgiReplaceLowerThan";
	char * ptr = string;

	PblStringBuilder * stringBuilder = pblStringBuilderNew();
	if (!stringBuilder)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	for (int i = 0; i < 1000000; i++)
	{
		size_t length = ptr2 - ptr;
		if (length > 0)
		{
			if (pblStringBuilderAppendStrN(stringBuilder, length, ptr) == ((size_t)-1))
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
		}

		if (pblStringBuilderAppendStr(stringBuilder, "&lt;") == ((size_t)-1))
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		ptr = ptr2 + 1;
		ptr2 = strstr(ptr, "<");
		if (!ptr2)
		{
			if (pblStringBuilderAppendStr(stringBuilder, ptr) == ((size_t)-1))
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}

			char * result = pblStringBuilderToString(stringBuilder);
			if (!result)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
			pblStringBuilderFree(stringBuilder);
			return result;
		}
	}
	pblCgiExitOnError("%s: There are more than 1000000 '<' characters to be replaced in one string.\n", tag);
	return NULL;
}

static char * pblCgiReplaceVariable(char * string, int iteration)
{
	static char * tag = "pblCgiReplaceVariable";
	static char * startPattern = "<!--?";
	static char * startPattern2 = "<?";

	int startPatternLength;
	char * endPattern;
	int endPatternLength;

	char * ptr = strchr(string, '<');
	if (!ptr)
	{
		return string;
	}

	size_t length = ptr - string;

	char * ptr2 = strstr(ptr, startPattern);
	if (!ptr2)
	{
		ptr2 = strstr(ptr, startPattern2);
		if (!ptr2)
		{
			return string;
		}
		else
		{
			startPatternLength = 2;
			endPattern = ">";
			endPatternLength = 1;
		}
	}
	else
	{
		char * ptr3 = strstr(ptr, startPattern2);
		if (ptr3 && ptr3 < ptr2)
		{
			ptr2 = ptr3;
			startPatternLength = 2;
			endPattern = ">";
			endPatternLength = 1;
		}
		else
		{
			startPatternLength = 5;
			endPattern = "-->";
			endPatternLength = 3;
		}
	}

	PblStringBuilder * stringBuilder = pblStringBuilderNew();
	if (!stringBuilder)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	if (length > 0)
	{
		if (pblStringBuilderAppendStrN(stringBuilder, length, string) == ((size_t)-1))
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
	}

	char * result;

	for (int i = 0; i < 1000000; i++)
	{
		length = ptr2 - ptr;
		if (length > 0)
		{
			if (pblStringBuilderAppendStrN(stringBuilder, length, ptr) == ((size_t)-1))
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
		}

		ptr = ptr2 + startPatternLength;
		ptr2 = strstr(ptr, endPattern);
		if (!ptr2)
		{
			result = pblStringBuilderToString(stringBuilder);
			if (!result)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
			pblStringBuilderFree(stringBuilder);
			return result;
		}

		char * key = pblCgiStrRangeDup(ptr, ptr2);
		char * value = NULL;
		if (iteration >= 0)
		{
			value = pblCgiValueForIteration(key, iteration);
		}
		if (!value)
		{
			value = pblCgiValue(key);
		}
		PBL_FREE(key);

		if (value && *value)
		{
			char * pointer = strstr(value, "<");
			if (!pointer)
			{
				if (pblStringBuilderAppendStr(stringBuilder, value) == ((size_t)-1))
				{
					pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
				}
			}
			else
			{
				value = pblCgiReplaceLowerThan(value, pointer);
				if (value && *value)
				{
					if (pblStringBuilderAppendStr(stringBuilder, value) == ((size_t)-1))
					{
						pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
					}
				}
				PBL_FREE(value);
			}
		}
		ptr = ptr2 + endPatternLength;
		ptr2 = strstr(ptr, startPattern);
		if (!ptr2)
		{
			ptr2 = strstr(ptr, startPattern2);
			if (!ptr2)
			{
				if (pblStringBuilderAppendStr(stringBuilder, ptr) == ((size_t)-1))
				{
					pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
				}
				result = pblStringBuilderToString(stringBuilder);
				if (!result)
				{
					pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
				}
				pblStringBuilderFree(stringBuilder);
				return result;
			}
			else
			{
				startPatternLength = 2;
				endPattern = ">";
				endPatternLength = 1;
			}
		}
		else
		{
			char * ptr3 = strstr(ptr, startPattern2);
			if (ptr3 && ptr3 < ptr2)
			{
				ptr2 = ptr3;
				startPatternLength = 2;
				endPattern = ">";
				endPatternLength = 1;
			}
			else
			{
				startPatternLength = 5;
				endPattern = "-->";
				endPatternLength = 3;
			}
		}
	}
	pblCgiExitOnError("%s: There are more than 1000000 variables defined in one string.\n", tag);
	return NULL;
}

static char * pblCgiPrintStr(char * string, FILE * outStream, int iteration);

static char * pblCgiSkip(char * string, char * skipKey, FILE * outStream, int iteration)
{
	string = pblCgiReplaceVariable(string, -1);

	for (;;)
	{
		char * ptr = strstr(string, "<!--#ENDIF");
		if (!ptr)
		{
			return skipKey;
		}
		ptr += 10;

		char *ptr2 = strstr(ptr, "-->");
		if (ptr2)
		{
			char * key = pblCgiStrRangeDup(ptr, ptr2);
			if (!strcmp(skipKey, key))
			{
				PBL_FREE(skipKey);
				PBL_FREE(key);
				return pblCgiPrintStr(ptr2 + 3, outStream, iteration);
			}

			PBL_FREE(key);
			string = ptr2 + 3;
			continue;
		}
		break;
	}
	return skipKey;
}

static char * pblCgiPrintStr(char * string, FILE * outStream, int iteration)
{
	string = pblCgiReplaceVariable(string, iteration);

	for (;;)
	{
		char * ptr = strstr(string, "<!--#");
		if (!ptr)
		{
			fputs(string, outStream);
			return NULL;
		}

		while (string < ptr)
		{
			fputc(*string++, outStream);
		}

		if (!memcmp(string, "<!--#IFDEF", 10))
		{
			ptr += 10;

			char *ptr2 = strstr(ptr, "-->");
			if (ptr2)
			{
				char * key = pblCgiStrRangeDup(ptr, ptr2);
				if (!pblCgiValue(key) && !pblCgiValueForIteration(key, iteration))
				{
					return pblCgiSkip(ptr2 + 1, key, outStream, iteration);
				}
				PBL_FREE(key);
				string = ptr2 + 3;
				continue;
			}
			return NULL;
		}
		if (!memcmp(string, "<!--#IFNDEF", 11))
		{
			ptr += 11;

			char *ptr2 = strstr(ptr, "-->");
			if (ptr2)
			{
				char * key = pblCgiStrRangeDup(ptr, ptr2);
				if (pblCgiValue(key) || pblCgiValueForIteration(key, iteration))
				{
					return pblCgiSkip(ptr2 + 1, key, outStream, iteration);
				}
				PBL_FREE(key);
				string = ptr2 + 3;
				continue;
			}
			return NULL;
		}
		if (!memcmp(string, "<!--#ENDIF", 10))
		{
			ptr += 7;

			char *ptr2 = strstr(ptr, "-->");
			if (ptr2)
			{
				string = ptr2 + 3;
				continue;
			}
			return NULL;
		}
		fputs(string, outStream);
		break;
	}
	return NULL;
}

static char * pblCgiHandleFor(PblList * list, char * line, char * forKey)
{
	static char * tag = "pblCgiHandleFor";
	char * ptr = strstr(line, "<!--#");
	if (!ptr)
	{
		if (pblListAdd(list, pblCgiStrDup(line)) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		return forKey;
	}
	if (!memcmp(ptr, "<!--#ENDFOR", 11))
	{
		if (ptr > line)
		{
			int length = ptr - line;
			char * value = pbl_memdup(tag, line, length + 1);
			if (!value)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
			value[length] = '\0';

			if (pblListAdd(list, value) < 0)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
		}
		ptr += 11;

		char * ptr2 = strstr(ptr, "-->");
		if (ptr2)
		{
			char * key = pblCgiStrRangeDup(ptr, ptr2);
			if (!strcmp(forKey, key))
			{
				PBL_FREE(key);
				return NULL;
			}
			PBL_FREE(key);
		}
	}
	else
	{
		if (pblListAdd(list, pblCgiStrDup(line)) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
	}
	return forKey;
}

static PblList * pblCgiReadFor(char * inputLine, char * forKey, FILE * stream)
{
	static char * tag = "pblCgiReadFor";
	char line[PBL_CGI_MAX_LINE_LENGTH + 1];

	PblList * list = pblListNewLinkedList();
	if (!list)
	{
		pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
	}

	if (!pblCgiStrIsNullOrWhiteSpace(inputLine))
	{
		if (!pblCgiHandleFor(list, inputLine, forKey))
		{
			return list;
		}
	}

	while ((fgets(line, sizeof(line) - 1, stream)))
	{
		if (!pblCgiHandleFor(list, line, forKey))
		{
			break;
		}
	}
	return list;
}

static void pblCgiPrintFor(PblList * lines, char * forKey, FILE * outStream)
{
	static char * tag = "pblCgiPrintFor";
	int hasNext;

	for (unsigned long iteration = 0; 1; iteration++)
	{
		if (!pblCgiValueForIteration(forKey, iteration))
		{
			return;
		}

		char * skipKey = NULL;
		PblIterator iteratorBuffer;
		PblIterator * iterator = &iteratorBuffer;

		if (pblIteratorInit((PblCollection *)lines, iterator) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		while ((hasNext = pblIteratorHasNext(iterator)) > 0)
		{
			char * line = (char*)pblIteratorNext(iterator);
			if (line == (void*)-1)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
			if (line)
			{
				char * ptr = strstr(line, "<!--#");
				if (!ptr)
				{
					if (!skipKey)
					{
						fputs(pblCgiReplaceVariable(line, iteration), outStream);
					}
					continue;
				}

				if (skipKey)
				{
					skipKey = pblCgiSkip(line, skipKey, outStream, iteration);
					continue;
				}
				skipKey = pblCgiPrintStr(line, outStream, iteration);
			}
		}
		if (hasNext < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
	}
}

/**
* Print a template
*/
void pblCgiPrint(char * directory, char * fileName, char * contentType)
{
	static char * tag = "pblCgiPrint";

	PBL_CGI_TRACE("Directory=%s", directory);
	PBL_CGI_TRACE("FileName=%s", fileName);
	PBL_CGI_TRACE("ContentType=%s", contentType);

	char * filePath = pblCgiStrCat(directory, fileName);
	char * skipKey = NULL;
	FILE * stream = pblCgiFopen(filePath, "r");
	PBL_FREE(filePath);

	if (contentType)
	{
		pblCgiSetContentType(contentType);
	}

	char line[PBL_CGI_MAX_LINE_LENGTH + 1];

	while ((fgets(line, sizeof(line) - 1, stream)))
	{
		char * start = line;
		char * ptr = strstr(line, "<!--#");
		if (!ptr)
		{
			if (!skipKey)
			{
				fputs(pblCgiReplaceVariable(line, -1), stdout);
			}
			continue;
		}

		if (skipKey)
		{
			skipKey = pblCgiSkip(line, skipKey, stdout, -1);
			continue;
		}

		if (!memcmp(ptr, "<!--#INCLUDE", 12))
		{
			while (start < ptr)
			{
				fputc(*start++, stdout);
			}
			ptr += 12;

			char * ptr2 = strstr(ptr, "-->");
			if (ptr2)
			{
				char * includeFileName = pblCgiStrRangeDup(ptr, ptr2);
				pblCgiPrint(directory, includeFileName, NULL);
				PBL_FREE(includeFileName);

				skipKey = pblCgiPrintStr(ptr2 + 3, stdout, -1);
			}
			continue;
		}
		if (!memcmp(ptr, "<!--#FOR", 8))
		{
			while (start < ptr)
			{
				fputc(*start++, stdout);
			}
			ptr += 8;

			char * ptr2 = strstr(ptr, "-->");
			if (ptr2)
			{
				char * forKey = pblCgiStrRangeDup(ptr, ptr2);
				PblList * lines = pblCgiReadFor(ptr2 + 3, forKey, stream);
				if (lines)
				{
					pblCgiPrintFor(lines, forKey, stdout);
					while (pblListSize(lines))
					{
						char * p = pblListPop(lines);
						if ((void*)-1 == p)
						{
							pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
						}
						PBL_FREE(p);
					}
					pblListFree(lines);
				}
				PBL_FREE(forKey);
			}
			continue;
		}
		while (start < ptr)
		{
			fputc(*start++, stdout);
		}
		skipKey = pblCgiPrintStr(ptr, stdout, -1);
	}
	fclose(stream);
}

/**
* Create a new empty map
*/
PblMap * pblCgiNewMap(void)
{
	PblMap * map = pblMapNewHashMap();
	if (!map)
	{
		pblCgiExitOnError("Failed to create a map, pbl_errno = %d\n", pbl_errno);
	}
	return map;
}

/**
* Test if a map is empty
*/
int pblCgiMapIsEmpty(PblMap * map)
{
	return 0 == pblMapSize(map);
}

/**
* Release the memory used by a map
*/
void pblCgiMapFree(PblMap * map)
{
	if (map)
	{
		pblMapFree(map);
	}
}

static PblMap * valueMap = NULL;

PblMap * pblCgiValueMap()
{
	if (!valueMap)
	{
		if (!(valueMap = pblMapNewHashMap()))
		{
			pblCgiExitOnError("Failed to create value map, pbl_errno = %d\n", pbl_errno);
		}
	}
	return valueMap;
}
/**
* Set a value for the given key.
*/
void pblCgiSetValue(char * key, char * value)
{
	pblCgiSetValueForIteration(key, value, -1);
}

/**
* Set a value for the given key for a loop iteration.
*/
void pblCgiSetValueForIteration(char * key, char * value, int iteration)
{
	pblCgiSetValueToMap(key, value, iteration, pblCgiValueMap());
}

/**
* Set a value for the given key for a loop iteration into a map.
*/
void pblCgiSetValueToMap(char * key, char * value, int iteration, PblMap * map)
{
	static char * tag = "pblCgiSetValueToMap";

	if (!key || !*key)
	{
		return;
	}
	if (!value)
	{
		value = "";
	}
	if (iteration >= 0)
	{
		char * iteratedKey = pblCgiSprintf("%s_%d", key, iteration);

		if (pblMapAddStrStr(map, iteratedKey, value) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		if (map == valueMap)
		{
			PBL_CGI_TRACE("Out %s=%s", iteratedKey, value);
		}

		PBL_FREE(iteratedKey);

		iteratedKey = pblCgiSprintf("IDX_%d", iteration);
		char * index = pblMapGetStr(map, iteratedKey);
		if (!index)
		{
			index = pblCgiSprintf("%d", iteration);
			if (pblMapAddStrStr(map, iteratedKey, index) < 0)
			{
				pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
			}
			if (map == valueMap)
			{
				PBL_CGI_TRACE("Out %s=%s", iteratedKey, index);
			}

			PBL_FREE(index);
		}
		PBL_FREE(iteratedKey);
	}
	else
	{
		if (pblMapAddStrStr(map, key, value) < 0)
		{
			pblCgiExitOnError("%s: pbl_errno = %d, message='%s'\n", tag, pbl_errno, pbl_errstr);
		}
		if (map == valueMap)
		{
			PBL_CGI_TRACE("Out %s=%s", key, value);
		}
	}
}

/**
* Clear the value for the given key.
*/
void pblCgiUnSetValue(char * key)
{
	pblCgiUnSetValueForIteration(key, -1);
}

/**
* Clear all values.
*/
void pblCgiClearValues()
{
	if (!valueMap)
	{
		return;
	}

	PBL_CGI_TRACE("Out cleared");
	pblMapClear(valueMap);
}

/**
* Clear the value for the given key for a loop iteration.
*/
void pblCgiUnSetValueForIteration(char * key, int iteration)
{
	if (!valueMap)
	{
		return;
	}
	pblCgiUnSetValueFromMap(key, iteration, valueMap);
}

/**
* Clear the value for the given key for a loop iteration from a map.
*/
void pblCgiUnSetValueFromMap(char * key, int iteration, PblMap * map)
{
	if (!key || !*key)
	{
		return;
	}

	if (iteration >= 0)
	{
		char * iteratedKey = pblCgiSprintf("%s_%d", key, iteration);

		pblMapUnmapStr(map, iteratedKey);
		PBL_CGI_TRACE("Del %s=", iteratedKey);
		PBL_FREE(iteratedKey);

	}
	else
	{
		pblMapUnmapStr(map, key);
		PBL_CGI_TRACE("Del %s=", key);
	}
}

/**
* Get the value for the given key.
*/
char * pblCgiValue(char * key)
{
	return pblCgiValueForIteration(key, -1);
}

static char * pblCgiDurationKey = PBL_CGI_KEY_DURATION;

/**
* Get the value for the given key for a loop iteration.
*/
char * pblCgiValueForIteration(char * key, int iteration)
{
	if (*key && *key == *pblCgiDurationKey && !strcmp(pblCgiDurationKey, key))
	{
		return pblCgiValueFromMap(key, iteration, valueMap);
	}

	if (!valueMap)
	{
		return NULL;
	}
	return pblCgiValueFromMap(key, iteration, valueMap);
}

/**
* Get the value for the given key for a loop iteration from a map.
*/
char * pblCgiValueFromMap(char * key, int iteration, PblMap * map)
{
	static char * tag = "pblCgiValueFromMap";

	//if (*key && *key == *pblCgiDurationKey && !strcmp(pblCgiDurationKey, key))
	//{
	//	struct timeval now;
	//	gettimeofday(&now, NULL);

	//	unsigned long duration = now.tv_sec * 1000000 + now.tv_usec;
	//	duration -= pblCgiStartTime.tv_sec * 1000000 + pblCgiStartTime.tv_usec;
	//	char * string = pblCgiSprintf("%lu", duration);
	//	PBL_CGI_TRACE("Duration=%s microseconds", string);
	//	return string;
	//}

	if (!key || !*key)
	{
		pblCgiExitOnError("%s: Empty key not allowed!\n", tag, pbl_errstr);
	}

	if (iteration >= 0)
	{
		char * iteratedKey = pblCgiSprintf("%s_%d", key, iteration);
		char * value = pblMapGetStr(map, iteratedKey);
		PBL_FREE(iteratedKey);
		return value;
	}

	return pblMapGetStr(map, key);
}
